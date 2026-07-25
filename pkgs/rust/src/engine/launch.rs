use super::lowering::{Launch, ServiceLaunch};
use super::{
    ffi, wire, Arc, AsRawFd, CString, Child, Config, ConfigFile, Duration, Error, File, Guest,
    Instant, OpenOptions, OsStr, OsStrExt, Size, Stdio, EXECUTABLE,
};

pub(super) fn start(
    launch: Launch,
    io: crate::spec::ProcessIo,
    authorities: crate::extension::Authorities,
    resources: Vec<super::lowering::AllocatedResource>,
) -> Result<Child, Error> {
    start_channels(launch, io, authorities, resources, None)
}

/// Start with the checkpoint broker and trigger descriptors attached. They are borrowed for the duration of
/// the call and transferred to the engine with `SCM_RIGHTS`.
pub(super) fn start_channels(
    launch: Launch,
    io: crate::spec::ProcessIo,
    authorities: crate::extension::Authorities,
    resources: Vec<super::lowering::AllocatedResource>,
    channels: Option<(std::ffi::c_int, std::ffi::c_int)>,
) -> Result<Child, Error> {
    if let Some((checkpoint, trigger)) = channels {
        return start_full(
            launch.guest,
            &launch.config,
            launch.program,
            launch.arguments,
            (io.stdin, io.stdout, io.stderr),
            launch.terminal,
            launch.projections,
            launch.services.map(|services| (services, authorities)),
            resources,
            Some((checkpoint, trigger)),
        );
    }
    start_legacy(
        launch.guest,
        &launch.config,
        launch.program,
        launch.arguments,
        (io.stdin, io.stdout, io.stderr),
        launch.terminal,
        launch.projections,
        launch.services.map(|services| (services, authorities)),
        resources,
    )
}

#[allow(clippy::too_many_arguments)]
pub(super) fn start_legacy<I, S>(
    guest: Guest,
    config: &Config,
    program: impl AsRef<OsStr>,
    arguments: I,
    streams: (Stdio, Stdio, Stdio),
    terminal: Option<Size>,
    projections: Vec<crate::projection::Projection>,
    services: Option<(ServiceLaunch, crate::extension::Authorities)>,
    resources: Vec<super::lowering::AllocatedResource>,
) -> Result<Child, Error>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    start_full(
        guest,
        config,
        program,
        arguments,
        streams,
        terminal,
        projections,
        services,
        resources,
        None,
    )
}

#[allow(clippy::too_many_arguments)]
fn start_full<I, S>(
    guest: Guest,
    config: &Config,
    program: impl AsRef<OsStr>,
    arguments: I,
    streams: (Stdio, Stdio, Stdio),
    terminal: Option<Size>,
    projections: Vec<crate::projection::Projection>,
    services: Option<(ServiceLaunch, crate::extension::Authorities)>,
    resources: Vec<super::lowering::AllocatedResource>,
    channels: Option<(std::ffi::c_int, std::ffi::c_int)>,
) -> Result<Child, Error>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let mut argv = vec![program.as_ref().to_owned()];
    argv.extend(arguments.into_iter().map(|value| value.as_ref().to_owned()));
    if argv[0].as_bytes().is_empty() {
        return Err(Error::InvalidConfig("program must not be empty"));
    }
    let encoded = wire::encode(config, &argv, None)?;
    let domain = crate::Domain::from_identity(wire::domain(&encoded));
    let config = ConfigFile::create(&encoded)?;
    let executable = EXECUTABLE
        .get_or_init(|| {
            let path = std::env::current_exe().map_err(|error| error.to_string())?;
            CString::new(path.as_os_str().as_bytes())
                .map_err(|_| "current executable contains NUL".into())
        })
        .as_ref()
        .map_err(|message| Error::Distribution(message.clone()))?;
    let config_path = CString::new(config.path().as_os_str().as_bytes())
        .map_err(|_| Error::InvalidConfig("config path contains NUL"))?;
    // Compose rather than choose: gather whichever descriptors this launch requested — a provider
    // transport, a checkpoint broker and its trigger — and hand them to the engine in ONE combined call
    // alongside the process I/O (stdio or a PTY). Any subset may be present simultaneously.
    //
    // `_transport` keeps the provider-transport child socket alive across the activation call (its fd is
    // duplicated into the engine via SCM_RIGHTS during the call). `_server` keeps the provider dispatch
    // thread's join handle in scope for the whole function, exactly as `start_services` did: dropping the
    // handle only detaches the thread, but keeping it named documents and preserves that lifetime.
    let (transport_socket, _server) = if let Some((services, authority)) = services {
        let (child, server) = prepare_services(services, &authority)?;
        (Some(child), Some(server))
    } else {
        (None, None)
    };
    let transport = transport_socket.as_ref().map_or(-1, AsRawFd::as_raw_fd);
    let (checkpoint, trigger) =
        channels.map_or((-1, -1), |(checkpoint, trigger)| (checkpoint, trigger));

    let (process, stdin, stdout, stderr, terminal) = if let Some(size) = terminal {
        let (process, file) = ffi::start_combined(
            executable,
            guest_number(guest),
            &config_path,
            None,
            Some(size.native()),
            transport,
            checkpoint,
            trigger,
        )
        .map_err(native_error)?;
        let file = file.ok_or_else(|| native_error(5))?;
        (process, None, None, None, Some(crate::Terminal::new(file)))
    } else {
        let (input, stdin, input_child) = prepare(streams.0, true)?;
        let (output, stdout, output_child) = prepare(streams.1, false)?;
        let (error, stderr, error_child) = prepare(streams.2, false)?;
        let native = ffi::Streams {
            input,
            output,
            error,
        };
        let (process, _) = ffi::start_combined(
            executable,
            guest_number(guest),
            &config_path,
            Some(&native),
            None,
            transport,
            checkpoint,
            trigger,
        )
        .map_err(native_error)?;
        drop((input_child, output_child, error_child));
        (process, stdin, stdout, stderr, None)
    };
    Ok(Child {
        process: Some(process),
        _config: config,
        stdin,
        stdout,
        stderr,
        terminal,
        domain,
        completed: false,
        _projections: projections,
        _provider_resources: resources,
    })
}

fn prepare_services(
    launch: ServiceLaunch,
    authority: &crate::extension::Authorities,
) -> Result<(std::os::unix::net::UnixStream, std::thread::JoinHandle<()>), Error> {
    let handles = authority
        .provider(&launch.provider)
        .and_then(|provider| provider.handles.as_ref())
        .cloned()
        .ok_or(Error::InvalidConfig("missing provider handle authority"))?;
    let maximum_request = launch
        .registrations
        .iter()
        .map(|service| service.max_request_bytes)
        .max()
        .unwrap_or(1);
    let maximum_handles = u32::try_from(launch.registrations.len().saturating_mul(64))
        .unwrap_or(u32::MAX)
        .max(1);
    let payload = crate::service::encode_namespace_install(&launch.projections, 64, 4096)
        .map_err(|_| Error::InvalidConfig("invalid service namespace transaction"))?;
    let (parent, child) = std::os::unix::net::UnixStream::pair()?;
    let channel = Arc::new(
        crate::transport::Channel::from_stream(
            parent,
            crate::transport::TransportLimits {
                payload_bytes: maximum_request
                    .max(u32::try_from(payload.len()).unwrap_or(u32::MAX)),
                providers: 1,
            },
        )
        .map_err(|error| Error::Distribution(format!("provider transport: {error:?}")))?,
    );
    let dispatcher = Arc::new(crate::service::ProviderDispatcher::new(
        handles,
        &launch.registrations,
        launch.credentials,
        maximum_handles,
        maximum_request,
    ));
    let server = crate::service::ServiceServer::new(
        channel.clone(),
        dispatcher,
        64,
        Duration::from_secs(30),
    );
    let server = std::thread::spawn(move || {
        let startup = Instant::now() + Duration::from_secs(5);
        if channel.accept_handshake(1, startup) == Ok(1)
            && channel.install_namespace(payload, startup).is_ok()
        {
            let _ = server.run(Instant::now() + Duration::from_secs(24 * 60 * 60));
        }
    });
    Ok((child, server))
}

fn prepare(value: Stdio, input: bool) -> Result<(i32, Option<File>, Option<File>), Error> {
    match value {
        Stdio::Inherit => Ok((-1, None, None)),
        Stdio::Null => {
            let file = OpenOptions::new()
                .read(input)
                .write(!input)
                .open("/dev/null")?;
            Ok((file.as_raw_fd(), None, Some(file)))
        }
        Stdio::Piped => {
            let (read, write) = ffi::pipe_pair()?;
            let (parent, child) = if input { (write, read) } else { (read, write) };
            Ok((child.as_raw_fd(), Some(parent), Some(child)))
        }
    }
}
const fn guest_number(guest: Guest) -> u32 {
    match guest {
        Guest::Aarch64 => 1,
        Guest::X86_64 => 2,
    }
}
fn native_error(status: i32) -> Error {
    Error::Engine { status, detail: 0 }
}
