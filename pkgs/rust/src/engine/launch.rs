use super::lowering::{Launch, ServiceLaunch};
use super::{
    ffi, wire, Arc, AsRawFd, CString, Child, Config, ConfigFile, Duration, Error, File, Guest,
    HandlesAuthority, Instant, OpenOptions, OsStr, OsStrExt, Size, Stdio, EXECUTABLE,
};

pub(super) fn start(
    launch: Launch,
    io: crate::spec::ProcessIo,
    authority: HandlesAuthority,
) -> Result<Child, Error> {
    start_legacy(
        launch.guest,
        &launch.config,
        launch.program,
        launch.arguments,
        (io.stdin, io.stdout, io.stderr),
        launch.terminal,
        launch.projections,
        launch.services.map(|services| (services, authority)),
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
    services: Option<(ServiceLaunch, HandlesAuthority)>,
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
    let (process, stdin, stdout, stderr, terminal) = if let Some(size) = terminal {
        let (process, file) =
            ffi::start_terminal(executable, guest_number(guest), &config_path, size.native())
                .map_err(native_error)?;
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
        let process = if let Some((services, authority)) = services {
            start_services(
                executable,
                guest_number(guest),
                &config_path,
                &native,
                services,
                &authority,
            )?
        } else {
            ffi::start(executable, guest_number(guest), &config_path, &native)
                .map_err(native_error)?
        };
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
    })
}

fn start_services(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    streams: &ffi::Streams,
    launch: ServiceLaunch,
    authority: &HandlesAuthority,
) -> Result<ffi::Handle, Error> {
    let handles = authority
        .handles(&launch.provider)
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
    std::thread::spawn(move || {
        let startup = Instant::now() + Duration::from_secs(5);
        if channel.accept_handshake(1, startup) == Ok(1)
            && channel.install_namespace(payload, startup).is_ok()
        {
            let _ = server.run(Instant::now() + Duration::from_secs(24 * 60 * 60));
        }
    });
    ffi::start_with_transport(executable, guest, config, streams, &child).map_err(native_error)
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
