use crate::{
    extension::{BindAccess, ExtensionCapability, HandlesAuthority, ProviderId},
    ffi,
    runtime::ConfigFile,
    spec::{
        CheckpointCapabilities, CpuCapabilities, EngineCapabilities, EngineLimits,
        FilesystemCapabilities, GuestPlatform, LinuxCapabilities, MachineSpec, NetworkCapabilities,
        NetworkMode, ProcessIo, SpawnError, SpecError, SpecErrorCategory, TreeSource, Validation,
        Version,
    },
    wire, Child, Command as GuestCommand, Config, Error, Guest, Machine, Mount, Size, Stdio,
};
use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::{CString, OsStr},
    fs::{File, OpenOptions},
    os::fd::AsRawFd,
    os::unix::ffi::OsStrExt,
    sync::{Arc, OnceLock},
    time::{Duration, Instant},
};

static EXECUTABLE: OnceLock<Result<CString, String>> = OnceLock::new();

/// Entry point for constructing guest commands.
#[derive(Clone, Copy, Debug, Default)]
pub struct Engine;
impl Engine {
    #[must_use]
    pub const fn new() -> Self {
        Self
    }
    #[must_use]
    pub fn command(&self, guest: Guest, program: impl Into<std::ffi::OsString>) -> GuestCommand {
        GuestCommand::new(guest, program.into())
    }

    /// Reports the exact typed launch features implemented by this engine build.
    #[must_use]
    #[allow(clippy::too_many_lines)]
    pub fn capabilities(&self) -> EngineCapabilities {
        let guest_fd_limit = crate::ffi::guest_fd_limit();
        EngineCapabilities {
            api: Version::new(1, 0),
            guests: vec![
                GuestPlatform::linux(Guest::Aarch64),
                GuestPlatform::linux(Guest::X86_64),
            ],
            cpu: CpuCapabilities {
                architectures: vec![Guest::Aarch64, Guest::X86_64],
                page_sizes: vec![4096],
                maximum_cpus: u32::MAX,
            },
            linux: LinuxCapabilities {
                syscall_abi: Version::new(6, 6),
                process_domains: true,
                ptys: true,
                descriptor_passing: true,
            },
            filesystems: FilesystemCapabilities {
                features: BTreeSet::from([
                    crate::spec::FilesystemFeature::HostDirectories,
                    crate::spec::FilesystemFeature::Overlay,
                    crate::spec::FilesystemFeature::HostBinds,
                    crate::spec::FilesystemFeature::ProjectedNamespace,
                    crate::spec::FilesystemFeature::ReadOnlyRoot,
                    crate::spec::FilesystemFeature::CoherenceNotifications,
                ]),
            },
            networking: NetworkCapabilities {
                modes: BTreeSet::from([NetworkMode::Host, NetworkMode::None, NetworkMode::Virtual]),
                maximum_interfaces: 8,
                maximum_port_forwards: 32,
            },
            resources: crate::spec::ResourceCapabilities {
                launch_limits: BTreeSet::from([
                    crate::spec::ResourceKind::Memory,
                    crate::spec::ResourceKind::Processes,
                    crate::spec::ResourceKind::CpuCount,
                ]),
                live_updates: BTreeSet::new(),
                accounting: false,
            },
            time: crate::spec::TimeCapabilities {
                host_time: true,
                virtual_time: false,
                deterministic_entropy: false,
            },
            observability: crate::spec::ObservabilityCapabilities {
                structured_events: false,
                metrics: false,
                tracing: false,
                maximum_queue: 0,
            },
            debugging: crate::spec::DebugCapabilities {
                operations: BTreeSet::new(),
            },
            checkpoint: CheckpointCapabilities {
                supported: false,
                format: None,
            },
            extensions: vec![
                ExtensionCapability {
                    provider: namespace_provider(),
                    versions: vec![Version::new(1, 0)],
                    features: namespace_features(),
                    hotplug: false,
                    limits: crate::extension::ExtensionLimits {
                        namespace_entries: 4096,
                        services: 0,
                        mappings: 0,
                        queued_events: 0,
                        request_bytes: 0,
                    },
                },
                ExtensionCapability {
                    provider: handles_provider(),
                    versions: vec![Version::new(1, 0)],
                    features: handles_features(),
                    hotplug: false,
                    limits: crate::extension::ExtensionLimits {
                        namespace_entries: 64,
                        services: 64,
                        mappings: 0,
                        queued_events: 64,
                        request_bytes: 1024 * 1024,
                    },
                },
            ],
            limits: EngineLimits {
                path_bytes: 4096,
                arguments: 4096,
                environment_bytes: 1024 * 1024,
                namespace_entries: 4096,
                projected_file_bytes: 64 * 1024 * 1024,
                extension_specs: 64,
                handles: guest_fd_limit,
                mappings: 65_536,
                mapped_bytes: 1 << 40,
                request_bytes: 1024 * 1024,
                queued_events: 4096,
            },
        }
    }

    /// Performs the same typed preflight checks as [`Engine::spawn`] without host side effects.
    ///
    /// # Errors
    /// Returns a field-addressed error for invalid, conflicting, unsupported, or oversized input.
    pub fn validate(&self, spec: &MachineSpec) -> Result<Validation, SpecError> {
        validate_spec(&self.capabilities(), spec)
    }

    /// Starts a machine from the versioned typed launch model.
    ///
    /// # Errors
    /// Returns preflight validation or engine process-start failures.
    pub fn spawn(&self, spec: MachineSpec, io: ProcessIo) -> Result<Machine, SpawnError> {
        self.spawn_with_authority(spec, io, HandlesAuthority::new())
    }

    /// Starts a machine with launch-scoped authority for selected handle services.
    ///
    /// # Errors
    /// Returns a typed specification error when a selected service provider has
    /// no matching authority, or an engine error when activation fails.
    pub fn spawn_with_authority(
        &self,
        spec: MachineSpec,
        io: ProcessIo,
        authority: HandlesAuthority,
    ) -> Result<Machine, SpawnError> {
        self.validate(&spec).map_err(SpawnError::Spec)?;
        validate_authority(&spec, &authority).map_err(SpawnError::Spec)?;
        let (guest, config, program, arguments, terminal, projections, services) =
            lower(spec).map_err(SpawnError::Spec)?;
        Self::start(
            guest,
            &config,
            program,
            arguments,
            (io.stdin, io.stdout, io.stderr),
            terminal,
            projections,
            services.map(|services| (services, authority)),
        )
        .map(Machine::new)
        .map_err(SpawnError::Engine)
    }
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn start<I, S>(
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
}

fn namespace_provider() -> ProviderId {
    ProviderId::new("engine.namespace")
        .unwrap_or_else(|_| unreachable!("constant provider id is valid"))
}

fn handles_provider() -> ProviderId {
    ProviderId::new("engine.handles")
        .unwrap_or_else(|_| unreachable!("constant provider id is valid"))
}

fn handles_features() -> BTreeSet<crate::extension::Feature> {
    ["read", "write", "poll", "ofd-lifecycle"]
        .into_iter()
        .map(|name| crate::extension::Feature::new(name).unwrap_or_else(|_| unreachable!()))
        .collect()
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
        .get(&launch.provider)
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

fn namespace_features() -> BTreeSet<crate::extension::Feature> {
    [
        "directories",
        "host-bind-read-only",
        "immutable-files",
        "mutable-files",
        "symlinks",
    ]
    .into_iter()
    .map(|name| {
        crate::extension::Feature::new(name)
            .unwrap_or_else(|_| unreachable!("constant feature is valid"))
    })
    .collect()
}

// Keeping the complete preflight in one linear pass makes `validate` and `spawn` structurally
// incapable of drifting to different policy. Each rejected field is still named explicitly.
#[allow(clippy::too_many_lines)]
fn validate_spec(
    capabilities: &EngineCapabilities,
    spec: &MachineSpec,
) -> Result<Validation, SpecError> {
    if !capabilities.guests.contains(&spec.guest) {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "guest",
            "guest platform is not supported",
        ));
    }
    if spec.process.executable.as_encoded_bytes().is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process.executable",
            "executable must not be empty",
        ));
    }
    if spec.process.executable.as_encoded_bytes().contains(&0)
        || spec
            .process
            .argv
            .iter()
            .any(|value| value.as_encoded_bytes().contains(&0))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process",
            "executable and arguments must not contain NUL",
        ));
    }
    if spec.process.argv.is_empty() || spec.process.argv[0].as_encoded_bytes().is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process.argv",
            "argv and argv[0] must not be empty",
        ));
    }
    if spec.process.argv[0] != spec.process.executable {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "process.argv[0]",
            "this backend cannot select an argv[0] different from the executable",
        ));
    }
    let cwd = std::path::Path::new(&spec.process.cwd);
    if !cwd.is_absolute()
        || cwd.as_os_str().as_encoded_bytes().contains(&0)
        || cwd.as_os_str().as_encoded_bytes().len() > capabilities.limits.path_bytes as usize
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process.cwd",
            "working directory must be absolute",
        ));
    }
    if spec.process.umask != 0o022 {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "process.umask",
            "custom umask is not implemented by this backend",
        ));
    }
    let mut environment = BTreeSet::new();
    let mut environment_bytes = 0usize;
    for (name, value) in &spec.process.env {
        let name = name.as_encoded_bytes();
        let value = value.as_encoded_bytes();
        if name.is_empty()
            || name.contains(&b'=')
            || name.contains(&b'\n')
            || name.contains(&0)
            || value.contains(&b'\n')
            || value.contains(&0)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "process.env",
                "environment contains an invalid record",
            ));
        }
        if !environment.insert(name.to_vec()) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "process.env",
                "environment keys must be unique",
            ));
        }
        environment_bytes = environment_bytes.saturating_add(name.len() + value.len() + 2);
    }
    if environment_bytes > capabilities.limits.environment_bytes as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "process.env",
            "environment exceeds the engine limit",
        ));
    }
    if spec.process.argv.len() > capabilities.limits.arguments as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "process.argv",
            "argument count exceeds the engine limit",
        ));
    }
    if spec.cpu.count == Some(0) || spec.resources.cpu_limit == Some(0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "cpu.count",
            "CPU limits must be greater than zero",
        ));
    }
    if !spec.cpu.features.is_empty() || spec.cpu.model.is_some() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "cpu",
            "CPU feature and model selection is not implemented by this backend",
        ));
    }
    match &spec.filesystem.root {
        None => {}
        Some(TreeSource::HostDirectory(path))
            if path.is_absolute()
                && !path.as_os_str().as_encoded_bytes().contains(&0)
                && path.is_dir() => {}
        Some(TreeSource::HostDirectory(_)) => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "filesystem.root",
                "host-directory roots must be absolute existing directories",
            ))
        }
        Some(TreeSource::Overlay { lower, upper, work }) => {
            if lower.is_empty() || lower.len() > 8 {
                return Err(spec_error(
                    SpecErrorCategory::Limit,
                    "filesystem.root.lower",
                    "overlay roots require between one and eight lower layers",
                ));
            }
            let mut granted = Vec::with_capacity(lower.len() + 2);
            granted.push(("filesystem.root.upper", upper));
            granted.push(("filesystem.root.work", work));
            for (index, layer) in lower.iter().enumerate() {
                let TreeSource::HostDirectory(path) = layer else {
                    return Err(spec_error(
                        SpecErrorCategory::Unsupported,
                        format!("filesystem.root.lower[{index}]"),
                        "this backend accepts host-directory overlay lowers",
                    ));
                };
                granted.push(("filesystem.root.lower", path));
            }
            for (field, path) in &granted {
                if !path.is_absolute()
                    || path.as_os_str().as_encoded_bytes().contains(&0)
                    || !path.is_dir()
                {
                    return Err(spec_error(
                        SpecErrorCategory::Invalid,
                        *field,
                        "overlay paths must be absolute existing directories",
                    ));
                }
            }
            for left in 0..granted.len() {
                for right in left + 1..granted.len() {
                    if granted[left].1.starts_with(granted[right].1)
                        || granted[right].1.starts_with(granted[left].1)
                    {
                        return Err(spec_error(
                            SpecErrorCategory::Conflict,
                            "filesystem.root",
                            "overlay upper, work, and lower authorities must not overlap",
                        ));
                    }
                }
            }
        }
        Some(_) => {
            return Err(spec_error(
                SpecErrorCategory::Unsupported,
                "filesystem.root",
                "this backend accepts host-directory and host-backed overlay roots",
            ))
        }
    }
    let mut paths = BTreeMap::new();
    for mount in &spec.filesystem.mounts {
        validate_guest_path(
            &mount.path,
            capabilities.limits.path_bytes,
            "filesystem.mounts",
        )?;
        if paths.insert(&mount.path, "filesystem.mounts").is_some() {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "filesystem.mounts",
                "multiple entries project the same guest path",
            ));
        }
        if !mount.host.is_absolute() || !mount.host.exists() {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "filesystem.mounts",
                "bound host paths must be absolute and exist",
            ));
        }
        if [mount.host.as_os_str(), mount.path.as_os_str()]
            .iter()
            .any(|path| {
                path.as_encoded_bytes()
                    .iter()
                    .any(|byte| matches!(byte, b':' | b',' | 0))
            })
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "filesystem.mounts",
                crate::spec::SpecResource::Path(mount.path.clone()),
                "current host-bind transport cannot encode ':', ',', or NUL in paths",
            ));
        }
    }
    let mut ownership_paths = BTreeSet::new();
    for ownership in &spec.filesystem.ownership {
        validate_guest_path(
            &ownership.path,
            capabilities.limits.path_bytes,
            "filesystem.ownership",
        )?;
        if ownership.path == std::path::Path::new("/")
            || !ownership_paths.insert(&ownership.path)
            || ownership
                .path
                .as_os_str()
                .as_encoded_bytes()
                .iter()
                .any(|byte| matches!(byte, b'\n' | b'\t' | 0))
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "filesystem.ownership",
                crate::spec::SpecResource::Path(ownership.path.clone()),
                "ownership paths must be unique, non-root, and wire-safe",
            ));
        }
    }
    if spec
        .identity
        .uid
        .is_some_and(|value| value > i32::MAX as u32)
        || spec
            .identity
            .gid
            .is_some_and(|value| value > i32::MAX as u32)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "identity",
            "uid and gid exceed the current backend range",
        ));
    }
    if spec.identity.hostname.as_ref().is_some_and(|value| {
        value.as_encoded_bytes().contains(&0) || value.as_encoded_bytes().contains(&b'\n')
    }) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "identity.hostname",
            "hostname contains an unsupported byte",
        ));
    }
    if !spec.identity.supplementary_groups.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "identity.supplementary_groups",
            "supplementary groups are not implemented by this backend",
        ));
    }
    if spec.identity.domain_name.is_some() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "identity.domain_name",
            "domain name selection is not implemented by this backend",
        ));
    }
    validate_resources(spec, capabilities.limits.handles)?;
    validate_network(spec)?;
    if !namespace_defaults(&spec.namespaces) {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "namespaces",
            "custom namespace sharing modes are not implemented by this backend",
        ));
    }
    validate_checkpoint(spec)?;
    validate_time_entropy(spec)?;
    validate_cache(spec)?;
    validate_observability(spec, capabilities)?;
    validate_debug(spec)?;
    if spec.security.trusted_guest || !spec.security.executable_memory {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "security",
            "the selected trust or executable-memory policy is not implemented by this backend",
        ));
    }
    if spec.extensions.len() > capabilities.limits.extension_specs as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "extensions",
            "extension count exceeds the engine limit",
        ));
    }
    validate_extension_shapes(capabilities, spec)?;
    let available = capabilities
        .extensions
        .iter()
        .map(|item| (&item.provider, item))
        .collect::<BTreeMap<_, _>>();
    let mut unavailable = Vec::new();
    let mut selected_extensions = Vec::new();
    let mut degraded_features = Vec::new();
    for extension in &spec.extensions {
        if !spec.security.allowed_providers.is_empty()
            && !spec
                .security
                .allowed_providers
                .contains(&extension.provider)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "security.allowed_providers",
                "an extension provider is outside the launch allowlist",
            ));
        }
        let Some(capability) = available.get(&extension.provider) else {
            if extension.required {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "a required extension provider is unavailable",
                ));
            }
            unavailable.push(extension.provider.clone());
            continue;
        };
        if !capability.versions.contains(&extension.version) {
            if extension.required {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.version",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "a required extension contract version is unavailable",
                ));
            }
            unavailable.push(extension.provider.clone());
            continue;
        }
        let missing_required = extension
            .required_features
            .difference(&capability.features)
            .next();
        if missing_required.is_some() {
            if extension.required {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.required_features",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "a required extension feature is unavailable",
                ));
            }
            unavailable.push(extension.provider.clone());
            continue;
        }
        let selected = extension
            .required_features
            .union(&extension.optional_features)
            .filter(|feature| capability.features.contains(*feature))
            .cloned()
            .collect();
        degraded_features.extend(
            extension
                .optional_features
                .difference(&capability.features)
                .cloned()
                .map(|feature| crate::spec::DegradedFeature {
                    provider: extension.provider.clone(),
                    feature,
                }),
        );
        selected_extensions.push(crate::spec::SelectedExtension {
            provider: extension.provider.clone(),
            version: extension.version,
            features: selected,
        });
    }
    let active = selected_extensions
        .iter()
        .map(|extension| &extension.provider)
        .collect::<BTreeSet<_>>();
    validate_selected_runtime(spec, &active)?;
    let namespace_conflicts = namespace_conflicts(spec, &active)?;
    let resources = estimate_resources(spec, &active, capabilities)?;
    let estimated_memory_bytes = resources
        .memory_bytes
        .checked_add(resources.extension_memory_bytes)
        .ok_or_else(|| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.memory_bytes",
                "combined host memory estimate overflows u64",
            )
        })?;
    Ok(Validation {
        selected_extensions,
        unavailable_optional_extensions: unavailable,
        degraded_features,
        estimated_memory_bytes,
        namespace_conflicts,
        resources,
    })
}

fn validate_selected_runtime(
    spec: &MachineSpec,
    active: &BTreeSet<&ProviderId>,
) -> Result<(), SpecError> {
    use crate::extension::{FileSource, NamespaceEntry};
    for extension in &spec.extensions {
        if !active.contains(&extension.provider) {
            continue;
        }
        if extension.provider == handles_provider() {
            if !extension.memory.is_empty() {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.memory",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "provider memory is not implemented by handles contract v1",
                ));
            }
            let supported = BTreeSet::from([
                crate::extension::HandleOperation::Read,
                crate::extension::HandleOperation::Write,
                crate::extension::HandleOperation::Poll,
            ]);
            if extension
                .services
                .iter()
                .any(|service| !service.operations.is_subset(&supported))
            {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.services.operations",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "handles contract v1 advertises only read, write, poll, and engine-owned OFD lifecycle",
                ));
            }
            continue;
        }
        if extension.provider != namespace_provider() {
            continue;
        }
        if !extension.services.is_empty() || !extension.memory.is_empty() {
            return Err(resource_error(
                SpecErrorCategory::Unsupported,
                "extensions",
                crate::spec::SpecResource::Provider(extension.provider.clone()),
                "open services and provider memory have no runtime implementation",
            ));
        }
        for entry in &extension.namespace {
            if !matches!(
                entry,
                NamespaceEntry::Directory(_)
                    | NamespaceEntry::Symlink(_)
                    | NamespaceEntry::File(crate::extension::FileEntry {
                        source: FileSource::Immutable(_) | FileSource::Mutable(_),
                        ..
                    })
                    | NamespaceEntry::HostBind(crate::extension::HostBindEntry {
                        access: BindAccess::ReadOnly,
                        ..
                    })
            ) {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.namespace",
                    crate::spec::SpecResource::Path(entry.path().to_owned()),
                    "this projected namespace node kind has no runtime implementation",
                ));
            }
            if let NamespaceEntry::HostBind(bind) = entry {
                let metadata = std::fs::symlink_metadata(&bind.host).map_err(|_| {
                    resource_error(
                        SpecErrorCategory::Invalid,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(bind.host.clone()),
                        "projected host binds must name an existing host file or directory",
                    )
                })?;
                if metadata.file_type().is_symlink()
                    || !(metadata.file_type().is_file() || metadata.file_type().is_dir())
                {
                    return Err(resource_error(
                        SpecErrorCategory::Unsupported,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(bind.host.clone()),
                        "projected host binds support only regular files and directories",
                    ));
                }
            }
        }
    }
    Ok(())
}

fn validate_authority(spec: &MachineSpec, authority: &HandlesAuthority) -> Result<(), SpecError> {
    if spec.process.terminal.is_some()
        && spec
            .extensions
            .iter()
            .any(|extension| extension.provider == handles_provider())
    {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "process.terminal",
            "provider service activation with a controlling terminal is not implemented",
        ));
    }
    for extension in &spec.extensions {
        if extension.provider == handles_provider() && authority.get(&extension.provider).is_none()
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "extensions.authority",
                crate::spec::SpecResource::Provider(extension.provider.clone()),
                "selected handle services require launch-scoped Handles authority",
            ));
        }
    }
    Ok(())
}

#[derive(Clone)]
struct Projection {
    owner: crate::spec::NamespaceOwner,
    active: bool,
    directory: bool,
}

fn namespace_conflicts(
    spec: &MachineSpec,
    active: &BTreeSet<&crate::extension::ProviderId>,
) -> Result<Vec<crate::spec::NamespaceConflict>, SpecError> {
    use crate::spec::NamespaceOwner;

    let mut projections = BTreeMap::<std::path::PathBuf, Projection>::new();
    let mut conflicts = Vec::new();
    for mount in &spec.filesystem.mounts {
        projections.insert(
            mount.path.clone(),
            Projection {
                owner: NamespaceOwner::Filesystem,
                active: true,
                directory: false,
            },
        );
    }
    for extension in &spec.extensions {
        let projection_active = active.contains(&extension.provider);
        for entry in &extension.namespace {
            let projection = Projection {
                owner: NamespaceOwner::Extension(extension.provider.clone()),
                active: projection_active,
                directory: matches!(entry, crate::extension::NamespaceEntry::Directory(_)),
            };
            if let Some(first) = projections.insert(entry.path().to_owned(), projection.clone()) {
                record_namespace_conflict(entry.path(), &first, &projection, &mut conflicts)?;
            }
        }
    }
    for (path, projection) in &projections {
        for ancestor in path
            .ancestors()
            .skip(1)
            .take_while(|path| *path != std::path::Path::new("/"))
        {
            if let Some(parent) = projections.get(ancestor).filter(|parent| !parent.directory) {
                record_namespace_conflict(ancestor, parent, projection, &mut conflicts)?;
            }
        }
    }
    conflicts.sort_by(|left, right| left.path.cmp(&right.path));
    Ok(conflicts)
}

fn record_namespace_conflict(
    path: &std::path::Path,
    first: &Projection,
    second: &Projection,
    conflicts: &mut Vec<crate::spec::NamespaceConflict>,
) -> Result<(), SpecError> {
    if first.active && second.active {
        return Err(resource_error(
            SpecErrorCategory::Conflict,
            "namespaces",
            crate::spec::SpecResource::Path(path.to_owned()),
            "active namespace projections conflict",
        ));
    }
    conflicts.push(crate::spec::NamespaceConflict {
        path: path.to_owned(),
        first: first.owner.clone(),
        second: second.owner.clone(),
        disposition: crate::spec::ConflictDisposition::InactiveOptional,
    });
    Ok(())
}

fn estimate_resources(
    spec: &MachineSpec,
    active: &BTreeSet<&crate::extension::ProviderId>,
    capabilities: &EngineCapabilities,
) -> Result<crate::spec::HostResourceEstimate, SpecError> {
    let selected = spec
        .extensions
        .iter()
        .filter(|extension| active.contains(&extension.provider));
    let (mut extension_memory_bytes, mut namespace_entries, mut services, mut mappings) =
        (0_u64, spec.filesystem.mounts.len(), 0_usize, 0_usize);
    for extension in selected {
        for memory in &extension.memory {
            extension_memory_bytes =
                extension_memory_bytes
                    .checked_add(memory.size)
                    .ok_or_else(|| {
                        spec_error(
                            SpecErrorCategory::Limit,
                            "resources",
                            "resource estimate overflow",
                        )
                    })?;
        }
        namespace_entries = namespace_entries
            .checked_add(extension.namespace.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "resources",
                    "resource estimate overflow",
                )
            })?;
        services = services
            .checked_add(extension.services.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "resources",
                    "resource estimate overflow",
                )
            })?;
        mappings = mappings
            .checked_add(extension.memory.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "resources",
                    "resource estimate overflow",
                )
            })?;
    }
    let size = |value| {
        u64::try_from(value).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources",
                "resource estimate exceeds u64",
            )
        })
    };
    validate_estimate_bounds(
        size(namespace_entries)?,
        size(services)?,
        size(mappings)?,
        extension_memory_bytes,
        capabilities,
    )?;
    Ok(crate::spec::HostResourceEstimate {
        memory_bytes: spec.resources.memory_bytes.unwrap_or(0),
        extension_memory_bytes,
        processes: spec.resources.process_limit.unwrap_or(0),
        cpus: spec.resources.cpu_limit.or(spec.cpu.count).unwrap_or(0),
        namespace_entries: u32::try_from(namespace_entries).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.namespace_entries",
                "resource estimate exceeds u32",
            )
        })?,
        services: u32::try_from(services).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.services",
                "resource estimate exceeds u32",
            )
        })?,
        mappings: u32::try_from(mappings).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.mappings",
                "resource estimate exceeds u32",
            )
        })?,
        event_queue: event_queue(spec),
        cache_bytes: spec.cache.budget_bytes.unwrap_or(0),
    })
}

fn event_queue(spec: &MachineSpec) -> u32 {
    spec.observability
        .queue_capacity
        .or_else(|| {
            spec.observability
                .trace
                .as_ref()
                .map(|trace| trace.queue_capacity)
        })
        .unwrap_or(0)
}

fn validate_estimate_bounds(
    namespace_entries: u64,
    services: u64,
    mappings: u64,
    extension_memory_bytes: u64,
    capabilities: &EngineCapabilities,
) -> Result<(), SpecError> {
    let bounded = [
        (
            namespace_entries,
            u64::from(capabilities.limits.namespace_entries),
            "resources.namespace_entries",
        ),
        (
            services,
            u64::from(capabilities.limits.handles),
            "resources.services",
        ),
        (
            mappings,
            u64::from(capabilities.limits.mappings),
            "resources.mappings",
        ),
        (
            extension_memory_bytes,
            capabilities.limits.mapped_bytes,
            "resources.extension_memory_bytes",
        ),
    ];
    if let Some((actual, maximum, field)) = bounded
        .into_iter()
        .find(|(actual, maximum, _)| actual > maximum)
    {
        return Err(resource_error(
            SpecErrorCategory::Limit,
            field,
            crate::spec::SpecResource::Limit { actual, maximum },
            "selected extensions exceed a host resource limit",
        ));
    }
    Ok(())
}

fn validate_extension_shapes(
    capabilities: &EngineCapabilities,
    spec: &MachineSpec,
) -> Result<(), SpecError> {
    let mut providers = BTreeSet::new();
    let mut environment = spec
        .process
        .env
        .iter()
        .map(|(name, _)| name.as_encoded_bytes().to_vec())
        .collect::<BTreeSet<_>>();
    let mut environment_bytes = spec
        .process
        .env
        .iter()
        .map(|(name, value)| name.as_encoded_bytes().len() + value.as_encoded_bytes().len() + 2)
        .sum::<usize>();
    for (index, extension) in spec.extensions.iter().enumerate() {
        if !providers.insert(&extension.provider) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].provider"),
                "a provider may appear only once per launch",
            ));
        }
        if extension.config.schema.is_empty()
            || extension.config.schema.len() > 256
            || extension.config.bytes.len() > capabilities.limits.request_bytes as usize
        {
            return Err(spec_error(
                SpecErrorCategory::Limit,
                format!("extensions[{index}].config"),
                "extension configuration schema or bytes exceed engine bounds",
            ));
        }
        if extension
            .required_features
            .intersection(&extension.optional_features)
            .next()
            .is_some()
        {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].features"),
                "the same feature cannot be both required and optional",
            ));
        }
        validate_extension_namespace(capabilities, extension, index)?;
        validate_extension_services(capabilities, extension, index)?;
        validate_extension_memory(capabilities, extension, index)?;
        validate_extension_environment(extension, index, &mut environment, &mut environment_bytes)?;
    }
    if environment_bytes > capabilities.limits.environment_bytes as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "extensions.environment",
            "combined guest environment exceeds the engine limit",
        ));
    }
    Ok(())
}

fn validate_extension_environment(
    extension: &crate::extension::ExtensionSpec,
    index: usize,
    environment: &mut BTreeSet<Vec<u8>>,
    environment_bytes: &mut usize,
) -> Result<(), SpecError> {
    for (name, value) in &extension.environment {
        let name_bytes = name.as_encoded_bytes();
        let value_bytes = value.as_encoded_bytes();
        if name_bytes.is_empty()
            || name_bytes.contains(&b'=')
            || name_bytes.contains(&b'\n')
            || name_bytes.contains(&0)
            || value_bytes.contains(&b'\n')
            || value_bytes.contains(&0)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                format!("extensions[{index}].environment"),
                "extension environment contains an invalid record",
            ));
        }
        if !environment.insert(name_bytes.to_vec()) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].environment"),
                "guest environment keys must be unique across the complete launch",
            ));
        }
        *environment_bytes = environment_bytes
            .checked_add(name_bytes.len() + value_bytes.len() + 2)
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "extensions.environment",
                    "guest environment size overflow",
                )
            })?;
    }
    Ok(())
}

fn validate_extension_namespace(
    capabilities: &EngineCapabilities,
    extension: &crate::extension::ExtensionSpec,
    index: usize,
) -> Result<(), SpecError> {
    use crate::extension::{FileSource, NamespaceEntry};

    if extension.namespace.len() > capabilities.limits.namespace_entries as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].namespace"),
            "namespace entry count exceeds the engine limit",
        ));
    }
    let mut paths = BTreeMap::new();
    let mut projected_bytes = 0_u64;
    for entry in &extension.namespace {
        let field = format!("extensions[{index}].namespace");
        validate_guest_path(
            entry.path(),
            capabilities.limits.path_bytes,
            "extensions.namespace",
        )
        .map_err(|mut error| {
            error.field.clone_from(&field);
            error
        })?;
        if entry.path() == std::path::Path::new("/") {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                field,
                "extensions cannot replace the guest namespace root",
            ));
        }
        if paths.insert(entry.path(), entry).is_some() {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                field,
                "namespace transaction contains duplicate guest paths",
            ));
        }
        validate_namespace_entry(capabilities, entry, index)?;
        if let NamespaceEntry::File(file) = entry {
            if let FileSource::Immutable(bytes) | FileSource::Mutable(bytes) = &file.source {
                projected_bytes =
                    projected_bytes
                        .checked_add(bytes.len() as u64)
                        .ok_or_else(|| {
                            spec_error(
                                SpecErrorCategory::Limit,
                                format!("extensions[{index}].namespace"),
                                "projected file bytes overflow the engine limit",
                            )
                        })?;
                if projected_bytes > capabilities.limits.projected_file_bytes {
                    return Err(spec_error(
                        SpecErrorCategory::Limit,
                        format!("extensions[{index}].namespace"),
                        "combined projected file bytes exceed the engine limit",
                    ));
                }
            }
        }
    }
    for path in paths.keys() {
        for ancestor in path
            .ancestors()
            .skip(1)
            .take_while(|path| *path != std::path::Path::new("/"))
        {
            if let Some(entry) = paths.get(ancestor) {
                if !matches!(entry, NamespaceEntry::Directory(_)) {
                    return Err(spec_error(
                        SpecErrorCategory::Conflict,
                        format!("extensions[{index}].namespace"),
                        "a non-directory projected entry cannot contain descendants",
                    ));
                }
            }
        }
    }
    Ok(())
}

fn validate_namespace_entry(
    capabilities: &EngineCapabilities,
    entry: &crate::extension::NamespaceEntry,
    index: usize,
) -> Result<(), SpecError> {
    use crate::extension::{FileSource, NamespaceEntry};

    let field = format!("extensions[{index}].namespace");
    let metadata = match entry {
        NamespaceEntry::Directory(value) => Some(value.metadata),
        NamespaceEntry::File(value) => Some(value.metadata),
        NamespaceEntry::Device(value) => Some(value.metadata),
        NamespaceEntry::Service(value) => Some(value.metadata),
        NamespaceEntry::Symlink(value) => {
            let target = value.target.as_os_str().as_encoded_bytes();
            if target.is_empty()
                || target.contains(&0)
                || target.len() > capabilities.limits.path_bytes as usize
            {
                return Err(spec_error(
                    SpecErrorCategory::Invalid,
                    field,
                    "symlink target is empty or exceeds path bounds",
                ));
            }
            None
        }
        NamespaceEntry::HostBind(value) => {
            validate_host_path(&value.host, &field)?;
            None
        }
        NamespaceEntry::Socket(value) => {
            validate_host_path(&value.host, &field)?;
            None
        }
    };
    if metadata.is_some_and(|metadata| metadata.mode & !0o7777 != 0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "namespace modes contain unsupported file-type bits",
        ));
    }
    match entry {
        NamespaceEntry::File(value) => match &value.source {
            FileSource::Host(path) => validate_host_path(path, &field)?,
            FileSource::Shared(id) if id.0 == 0 => {
                return Err(spec_error(
                    SpecErrorCategory::Invalid,
                    field,
                    "shared-byte identity must be nonzero",
                ))
            }
            FileSource::Generated(id) if id.0 == 0 => {
                return Err(spec_error(
                    SpecErrorCategory::Invalid,
                    field,
                    "generator identity must be nonzero",
                ))
            }
            _ => {}
        },
        NamespaceEntry::Device(value) if value.major >= 4096 || value.minor >= (1 << 20) => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                field,
                "device major or minor exceeds Linux dev_t bounds",
            ))
        }
        NamespaceEntry::Service(value) if value.service.0 == 0 => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                field,
                "service identity must be nonzero",
            ))
        }
        _ => {}
    }
    Ok(())
}

fn validate_extension_services(
    capabilities: &EngineCapabilities,
    extension: &crate::extension::ExtensionSpec,
    index: usize,
) -> Result<(), SpecError> {
    use crate::extension::NamespaceEntry;

    let contract_limit = capabilities
        .extensions
        .iter()
        .find(|capability| capability.provider == extension.provider)
        .map_or(capabilities.limits.handles, |capability| {
            capability.limits.services
        });
    if extension.services.len() > contract_limit.min(capabilities.limits.handles) as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].services"),
            "service count exceeds the engine limit",
        ));
    }
    let mut services = BTreeSet::new();
    for service in &extension.services {
        if service.id.0 == 0 || !services.insert(service.id) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].services"),
                "service identities must be nonzero and unique",
            ));
        }
        if service.operations.is_empty()
            || service.max_request_bytes == 0
            || service.max_request_bytes > capabilities.limits.request_bytes
            || capabilities
                .extensions
                .iter()
                .find(|capability| capability.provider == extension.provider)
                .is_some_and(|capability| {
                    service.max_request_bytes > capability.limits.request_bytes
                })
        {
            return Err(spec_error(
                SpecErrorCategory::Limit,
                format!("extensions[{index}].services"),
                "service operations or request bound are invalid",
            ));
        }
    }
    for entry in &extension.namespace {
        let service = match entry {
            NamespaceEntry::Service(value) => Some(value.service),
            NamespaceEntry::Device(value) => value.service,
            _ => None,
        };
        if service.is_some_and(|service| !services.contains(&service)) {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                format!("extensions[{index}].namespace"),
                "namespace entry references an unregistered service",
            ));
        }
    }
    Ok(())
}

fn validate_extension_memory(
    capabilities: &EngineCapabilities,
    extension: &crate::extension::ExtensionSpec,
    index: usize,
) -> Result<(), SpecError> {
    if extension.memory.len() > capabilities.limits.mappings as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].memory"),
            "mapping count exceeds the engine limit",
        ));
    }
    let mut total = 0_u64;
    for memory in &extension.memory {
        if memory.size == 0
            || memory.alignment == 0
            || !memory.alignment.is_power_of_two()
            || memory.alignment > (1 << 30)
            || !(memory.protections.read || memory.protections.write || memory.protections.execute)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                format!("extensions[{index}].memory"),
                "memory size, alignment, or protections are invalid",
            ));
        }
        total = total.checked_add(memory.size).ok_or_else(|| {
            spec_error(
                SpecErrorCategory::Limit,
                format!("extensions[{index}].memory"),
                "memory requirements overflow",
            )
        })?;
    }
    if total > capabilities.limits.mapped_bytes {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].memory"),
            "combined memory requirements exceed the engine limit",
        ));
    }
    Ok(())
}

fn validate_host_path(path: &std::path::Path, field: &str) -> Result<(), SpecError> {
    if !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0) {
        Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "host capability paths must be absolute and contain no NUL",
        ))
    } else {
        Ok(())
    }
}

fn namespace_defaults(namespaces: &crate::spec::NamespaceSpec) -> bool {
    use crate::spec::IsolationMode;
    matches!(namespaces.mount, IsolationMode::Private)
        && matches!(namespaces.pid, IsolationMode::Private)
        && matches!(namespaces.uts, IsolationMode::Private)
        && matches!(namespaces.ipc, IsolationMode::Private)
        && matches!(namespaces.network, IsolationMode::Private)
        && matches!(namespaces.user, IsolationMode::Private)
        && matches!(namespaces.cgroup, IsolationMode::Private)
}

fn validate_resources(spec: &MachineSpec, guest_fd_limit: u32) -> Result<(), SpecError> {
    let resources = &spec.resources;
    let values = [
        resources.memory_reservation_bytes,
        resources.memory_bytes,
        resources.file_size_bytes,
        resources.locked_memory_bytes,
        resources.stack_bytes,
        resources.address_space_bytes,
        resources.io_read_bytes_per_second,
        resources.io_write_bytes_per_second,
        resources.cpu_quota_micros,
    ];
    if values.into_iter().flatten().any(|value| value == 0)
        || [
            resources.process_limit,
            resources.thread_limit,
            resources.cpu_limit,
            resources.open_files,
        ]
        .into_iter()
        .flatten()
        .any(|value| value == 0)
        || resources
            .extension_budgets
            .values()
            .any(|value| *value == 0)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources",
            "explicit resource declarations must be greater than zero",
        ));
    }
    if resources
        .memory_reservation_bytes
        .zip(resources.memory_bytes)
        .is_some_and(|(reservation, limit)| reservation > limit)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources.memory_reservation_bytes",
            "memory reservation must not exceed the memory limit",
        ));
    }
    validate_open_files(resources.open_files, guest_fd_limit)?;
    if resources
        .cpu_affinity
        .iter()
        .any(|cpu| spec.cpu.count.is_some_and(|count| *cpu >= count))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources.cpu_affinity",
            "CPU affinity references a CPU outside the visible topology",
        ));
    }
    let providers = spec
        .extensions
        .iter()
        .map(|extension| &extension.provider)
        .collect::<BTreeSet<_>>();
    if resources
        .extension_budgets
        .keys()
        .any(|provider| !providers.contains(provider))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources.extension_budgets",
            "extension budgets must name an extension requested by this launch",
        ));
    }
    let unsupported = resources.memory_reservation_bytes.is_some()
        || resources.thread_limit.is_some()
        || resources.cpu_quota_micros.is_some()
        || !resources.cpu_affinity.is_empty()
        || resources.open_files.is_some()
        || resources.file_size_bytes.is_some()
        || resources.locked_memory_bytes.is_some()
        || resources.stack_bytes.is_some()
        || resources.address_space_bytes.is_some()
        || resources.io_read_bytes_per_second.is_some()
        || resources.io_write_bytes_per_second.is_some()
        || !resources.extension_budgets.is_empty()
        || resources.accounting != crate::spec::AccountingSpec::Disabled;
    if unsupported {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "resources",
            "one or more selected resource controls are not implemented by this backend",
        ));
    }
    Ok(())
}

fn validate_open_files(open_files: Option<u32>, guest_fd_limit: u32) -> Result<(), SpecError> {
    if open_files.is_some_and(|value| value > guest_fd_limit) {
        Err(spec_error(
            SpecErrorCategory::Limit,
            "resources.open_files",
            "open-file limit exceeds the host-backed guest descriptor ceiling",
        ))
    } else {
        Ok(())
    }
}

fn validate_checkpoint(spec: &MachineSpec) -> Result<(), SpecError> {
    let checkpoint = &spec.checkpoint;
    if checkpoint.maximum_pause_ms == Some(0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "checkpoint.maximum_pause_ms",
            "checkpoint pause bounds must be greater than zero",
        ));
    }
    if !checkpoint.enabled
        && (checkpoint.maximum_pause_ms.is_some()
            || checkpoint.mode != crate::spec::CheckpointMode::Full
            || checkpoint.incompatible_resources != crate::spec::IncompatibleResourcePolicy::Refuse)
    {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "checkpoint",
            "checkpoint options require checkpointing to be enabled",
        ));
    }
    if checkpoint.enabled {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "checkpoint.enabled",
            "checkpointing is not supported",
        ));
    }
    Ok(())
}

fn validate_time_entropy(spec: &MachineSpec) -> Result<(), SpecError> {
    if spec.time.rate.numerator == 0
        || spec.time.rate.denominator == 0
        || spec.time.timer_resolution_ns == Some(0)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "time",
            "time rate and timer resolution must be greater than zero",
        ));
    }
    if !matches!(spec.time.source, crate::spec::TimeSource::Host)
        || spec.time.monotonic != crate::spec::MonotonicOrigin::Host
        || spec.time.timer_resolution_ns.is_some()
        || spec.time.rate != crate::spec::TimeRate::default()
    {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "time",
            "virtual time selection is not implemented by this backend",
        ));
    }
    if matches!(&spec.entropy, crate::spec::EntropySpec::Deterministic(seed) if seed.is_empty()) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "entropy",
            "deterministic entropy requires a nonempty seed",
        ));
    }
    if !matches!(spec.entropy, crate::spec::EntropySpec::SecureHost) {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "entropy",
            "deterministic entropy is not implemented by this backend",
        ));
    }
    Ok(())
}

fn validate_cache(spec: &MachineSpec) -> Result<(), SpecError> {
    let cache = &spec.cache;
    if cache.budget_bytes == Some(0)
        || cache
            .identity
            .iter()
            .any(|item| item.name.is_empty() || item.bytes.is_empty())
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "cache",
            "cache budgets and identity components must be nonempty",
        ));
    }
    let mut identities = BTreeSet::new();
    let identity_bytes = cache.identity.iter().try_fold(0_usize, |total, item| {
        if !identities.insert(&item.name) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "cache.identity",
                "cache identity component names must be unique",
            ));
        }
        total
            .checked_add(item.name.len() + item.bytes.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "cache.identity",
                    "cache identity components overflow engine bounds",
                )
            })
    })?;
    if identity_bytes > 1024 * 1024 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "cache.identity",
            "cache identity components exceed the engine limit",
        ));
    }
    if cache
        .directory
        .as_ref()
        .is_some_and(|path| !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "cache.directory",
            "translation-cache host paths must be absolute and contain no NUL",
        ));
    }
    match cache.policy {
        crate::spec::CachePolicy::Disabled
            if cache.directory.is_some()
                || cache.budget_bytes.is_some()
                || !cache.identity.is_empty() =>
        {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "cache.policy",
                "cache settings require an enabled cache policy",
            ))
        }
        crate::spec::CachePolicy::ReadWrite if cache.directory.is_none() => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "cache.directory",
                "read-write cache policy requires a cache directory",
            ))
        }
        crate::spec::CachePolicy::ReadOnly | crate::spec::CachePolicy::Refresh => {
            return Err(spec_error(
                SpecErrorCategory::Unsupported,
                "cache.policy",
                "the selected cache policy is not implemented by this backend",
            ))
        }
        _ => {}
    }
    if cache.budget_bytes.is_some() || !cache.identity.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "cache",
            "cache budgets and caller identity components are not implemented by this backend",
        ));
    }
    Ok(())
}

fn validate_observability(
    spec: &MachineSpec,
    capabilities: &EngineCapabilities,
) -> Result<(), SpecError> {
    let value = &spec.observability;
    if value.syscall_sampling == Some(0)
        || value.syscall_sampling.is_some_and(|rate| rate > 1_000_000)
        || value.queue_capacity == Some(0)
        || value
            .queue_capacity
            .is_some_and(|size| size > capabilities.limits.queued_events)
        || value.trace.as_ref().is_some_and(|trace| {
            trace.queue_capacity == 0
                || trace.queue_capacity > capabilities.limits.queued_events
                || !(trace.include_guest_time || trace.include_host_time)
        })
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "observability",
            "sampling, queue, or trace bounds are invalid",
        ));
    }
    if !value.metrics && !value.metric_kinds.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "observability.metric_kinds",
            "metric kinds require metrics to be enabled",
        ));
    }
    if value != &crate::spec::ObservabilitySpec::default() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "observability",
            "structured observability selection is not implemented by this backend",
        ));
    }
    Ok(())
}

fn validate_debug(spec: &MachineSpec) -> Result<(), SpecError> {
    let value = &spec.debug;
    let selected = !value.operations.is_empty() || value.breakpoints != 0 || value.watchpoints != 0;
    if value.breakpoints > 65_536 || value.watchpoints > 65_536 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "debug",
            "debug point count exceeds the engine limit",
        ));
    }
    if selected && value.authorization.is_none() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "debug.authorization",
            "debug operations require explicit authorization",
        ));
    }
    if value
        .authorization
        .as_ref()
        .is_some_and(|value| value.capability.is_empty())
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "debug.authorization",
            "debug authorization capabilities must be nonempty",
        ));
    }
    if value.authorization.is_some() && !selected {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "debug",
            "debug authorization requires at least one selected operation",
        ));
    }
    if selected {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "debug",
            "debug control is not implemented by this backend",
        ));
    }
    Ok(())
}

fn validate_network(spec: &MachineSpec) -> Result<(), SpecError> {
    let network = &spec.network;
    if network.mode == NetworkMode::Host && spec.security.sandbox != crate::Sandbox::Disabled {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "network.mode",
            "host networking requires the host socket transport, which is unavailable in the selected sandbox",
        ));
    }
    let configured = network.namespace.is_some()
        || !network.interfaces.is_empty()
        || !network.port_forwards.is_empty()
        || network.external_listeners;
    match network.mode {
        NetworkMode::Host if configured => {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "network",
                "host networking cannot carry virtual network configuration",
            ))
        }
        NetworkMode::None
            if !network.interfaces.is_empty()
                || !network.port_forwards.is_empty()
                || network.external_listeners =>
        {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "network",
                "disabled networking cannot carry interfaces, forwards, or listeners",
            ))
        }
        NetworkMode::Virtual if network.namespace.is_none() => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "network.namespace",
                "virtual networking requires an opaque namespace identity",
            ))
        }
        _ => {}
    }
    if network.interfaces.len() > 8 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "network.interfaces",
            "at most eight interfaces are supported",
        ));
    }
    if network.port_forwards.len() > 32 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "network.port_forwards",
            "at most 32 port forwards are supported",
        ));
    }
    if network.external_listeners && network.port_forwards.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "network.external_listeners",
            "external listeners require at least one port forward",
        ));
    }
    Ok(())
}

fn validate_guest_path(
    path: &std::path::Path,
    maximum: u32,
    field: &'static str,
) -> Result<(), SpecError> {
    if !path.is_absolute() || path.as_os_str().as_encoded_bytes().len() > maximum as usize {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "guest paths must be absolute and within the engine path limit",
        ));
    }
    if path.as_os_str().as_encoded_bytes().contains(&0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "guest paths must not contain NUL",
        ));
    }
    if path
        .components()
        .any(|component| matches!(component, std::path::Component::ParentDir))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "guest paths must not escape through '..'",
        ));
    }
    Ok(())
}

type LoweredLaunch = (
    Guest,
    Config,
    std::ffi::OsString,
    Vec<std::ffi::OsString>,
    Option<Size>,
    Vec<crate::projection::Projection>,
    Option<ServiceLaunch>,
);

pub(crate) struct ServiceLaunch {
    provider: ProviderId,
    registrations: Vec<crate::extension::ServiceRegistration>,
    projections: Vec<crate::service::ServiceProjection>,
    credentials: crate::extension::Credentials,
}

#[allow(clippy::too_many_lines)]
fn lower(spec: MachineSpec) -> Result<LoweredLaunch, SpecError> {
    let services = lower_services(&spec);
    let mut config = Config::new()
        .working_dir(spec.process.cwd)
        .read_only_root(spec.filesystem.read_only)
        .sandbox(spec.security.sandbox);
    config = match spec.network.mode {
        NetworkMode::Host => config.host_network(true),
        NetworkMode::None => config.network(true),
        NetworkMode::Virtual => config,
    };
    match spec.filesystem.root {
        Some(TreeSource::HostDirectory(root)) => config = config.root(root),
        Some(TreeSource::Overlay { lower, upper, work }) => {
            let lower = lower
                .into_iter()
                .map(|layer| match layer {
                    TreeSource::HostDirectory(path) => path,
                    _ => unreachable!("validated overlay lower"),
                })
                .collect();
            config = config.overlay(lower, upper, work);
        }
        _ => {}
    }
    if let Some(uid) = spec.identity.uid {
        config = config.uid(i32::try_from(uid).map_err(|_| {
            spec_error(
                SpecErrorCategory::Invalid,
                "identity.uid",
                "uid exceeds the backend range",
            )
        })?);
    }
    if let Some(gid) = spec.identity.gid {
        config = config.gid(i32::try_from(gid).map_err(|_| {
            spec_error(
                SpecErrorCategory::Invalid,
                "identity.gid",
                "gid exceeds the backend range",
            )
        })?);
    }
    if let Some(hostname) = spec.identity.hostname {
        config = config.hostname(hostname);
    }
    if let Some(memory) = spec.resources.memory_bytes {
        config = config.memory_limit(memory);
    }
    if let Some(processes) = spec.resources.process_limit {
        config = config.process_limit(processes);
    }
    if let Some(cpus) = spec.resources.cpu_limit.or(spec.cpu.count) {
        config = config.cpu_limit(cpus);
    }
    if let Some(cache) = spec.cache.directory {
        config = config.translation_cache(cache);
    }
    if let Some(coherence) = spec.filesystem.coherence {
        config = config.filesystem_generation(coherence.host_path());
    }
    for (name, value) in spec.process.env {
        config = config.env(name, value);
    }
    for mount in spec.filesystem.mounts {
        config.mounts.push(match mount.access {
            BindAccess::ReadOnly => Mount::read_only(mount.host, mount.path),
            BindAccess::ReadWrite => Mount::read_write(mount.host, mount.path),
        });
    }
    let (mut config, projections) = lower_projections(config, &spec.extensions)?;
    for ownership in spec.filesystem.ownership {
        let relative = ownership.path.strip_prefix("/").map_err(|_| {
            spec_error(
                SpecErrorCategory::Invalid,
                "filesystem.ownership",
                "ownership paths must be absolute guest paths",
            )
        })?;
        config = config.owner(relative, ownership.uid, ownership.gid);
    }
    if let Some(domain) = spec.process.domain {
        config = config.domain(domain);
    }
    if let Some(namespace) = spec.network.namespace {
        config = config.network_namespace(namespace);
    }
    for interface in spec.network.interfaces {
        config = config.interface(interface);
    }
    for rule in spec.network.port_forwards {
        config = config.publish(rule);
    }
    config = config.publish_external(spec.network.external_listeners);
    let program = spec.process.executable;
    let arguments = spec.process.argv.into_iter().skip(1).collect();
    Ok((
        spec.guest.architecture,
        config,
        program,
        arguments,
        spec.process.terminal,
        projections,
        services,
    ))
}

fn lower_services(spec: &MachineSpec) -> Option<ServiceLaunch> {
    let extension = spec
        .extensions
        .iter()
        .find(|extension| extension.provider == handles_provider())?;
    let projections = extension
        .namespace
        .iter()
        .filter_map(|entry| match entry {
            crate::extension::NamespaceEntry::Service(value) => {
                Some(crate::service::ServiceProjection {
                    path: value.path.clone(),
                    service: value.service,
                    mode: value.metadata.mode,
                    uid: value.metadata.uid,
                    gid: value.metadata.gid,
                })
            }
            _ => None,
        })
        .collect();
    Some(ServiceLaunch {
        provider: extension.provider.clone(),
        registrations: extension.services.clone(),
        projections,
        credentials: crate::extension::Credentials {
            uid: spec.identity.uid.unwrap_or(0),
            gid: spec.identity.gid.unwrap_or(0),
            groups: spec.identity.supplementary_groups.clone(),
        },
    })
}

fn lower_projections(
    mut config: Config,
    extensions: &[crate::extension::ExtensionSpec],
) -> Result<(Config, Vec<crate::projection::Projection>), SpecError> {
    let entries = extensions
        .iter()
        .filter(|extension| extension.provider == namespace_provider())
        .flat_map(|extension| extension.namespace.iter())
        .collect::<Vec<_>>();
    if entries.is_empty() {
        return Ok((config, Vec::new()));
    }
    let materialized = entries
        .iter()
        .copied()
        .filter(|entry| !matches!(entry, crate::extension::NamespaceEntry::HostBind(_)))
        .collect::<Vec<_>>();
    let mut projections = Vec::new();
    if !materialized.is_empty() {
        let (projection, mounts) = crate::projection::Projection::materialize(&materialized)?;
        config.mounts.extend(mounts);
        projections.push(projection);
    }
    for entry in &entries {
        if let crate::extension::NamespaceEntry::HostBind(bind) = entry {
            config.mounts.push(Mount::read_only(&bind.host, &bind.path));
        }
    }
    for entry in entries {
        let (uid, gid) = match entry {
            crate::extension::NamespaceEntry::Directory(value) => {
                (value.metadata.uid, value.metadata.gid)
            }
            crate::extension::NamespaceEntry::File(value) => {
                (value.metadata.uid, value.metadata.gid)
            }
            crate::extension::NamespaceEntry::Symlink(value) => (value.uid, value.gid),
            _ => continue,
        };
        let relative = entry.path().strip_prefix("/").map_err(|_| {
            spec_error(
                SpecErrorCategory::Invalid,
                "extensions.namespace",
                "projection path must be absolute",
            )
        })?;
        config = config.owner(relative, uid, gid);
    }
    Ok((config, projections))
}

fn spec_error(
    category: SpecErrorCategory,
    field: impl Into<String>,
    context: impl Into<String>,
) -> SpecError {
    SpecError {
        category,
        field: field.into(),
        resource: None,
        context: context.into(),
    }
}

fn resource_error(
    category: SpecErrorCategory,
    field: impl Into<String>,
    resource: crate::spec::SpecResource,
    context: impl Into<String>,
) -> SpecError {
    SpecError {
        category,
        field: field.into(),
        resource: Some(resource),
        context: context.into(),
    }
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

#[cfg(test)]
mod typed_tests {
    use std::collections::BTreeSet;

    use crate::{
        extension::{
            ExtensionCapability, ExtensionConfig, ExtensionSpec, Feature, Inheritance,
            MemoryRequirement, Protections, ProviderId, Sharing,
        },
        spec::{NetworkMode, TreeSource, Version},
        Domain, Engine, Guest, MachineSpec, Sandbox,
    };

    fn requested(required_feature: Feature, optional_feature: Feature) -> ExtensionSpec {
        ExtensionSpec {
            provider: ProviderId::new("test.provider").unwrap(),
            version: Version::new(2, 0),
            required: true,
            required_features: BTreeSet::from([required_feature]),
            optional_features: BTreeSet::from([optional_feature]),
            config: ExtensionConfig::empty("test.provider/v2"),
            namespace: Vec::new(),
            services: Vec::new(),
            memory: vec![MemoryRequirement {
                size: 8192,
                alignment: 4096,
                protections: Protections {
                    read: true,
                    write: true,
                    execute: false,
                },
                sharing: Sharing::Shared,
                inheritance: Inheritance::Retain,
            }],
            environment: Vec::new(),
        }
    }

    #[test]
    fn negotiation_selects_required_and_degrades_optional_features() {
        let required = Feature::new("required").unwrap();
        let optional = Feature::new("optional").unwrap();
        let mut capabilities = Engine::new().capabilities();
        capabilities.extensions.push(ExtensionCapability {
            provider: ProviderId::new("test.provider").unwrap(),
            versions: vec![Version::new(2, 0)],
            features: BTreeSet::from([required.clone()]),
            hotplug: false,
            limits: crate::extension::ExtensionLimits {
                namespace_entries: 0,
                services: 0,
                mappings: 1,
                queued_events: 0,
                request_bytes: 0,
            },
        });
        let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
        spec.extensions.push(requested(required, optional.clone()));
        let validation = super::validate_spec(&capabilities, &spec).unwrap();
        assert_eq!(validation.selected_extensions.len(), 1);
        assert_eq!(validation.degraded_features[0].feature, optional);
        assert_eq!(validation.resources.extension_memory_bytes, 8192);
        assert_eq!(validation.resources.mappings, 1);
    }

    #[test]
    fn negotiation_rejects_a_missing_required_feature() {
        let required = Feature::new("required").unwrap();
        let optional = Feature::new("optional").unwrap();
        let mut capabilities = Engine::new().capabilities();
        capabilities.extensions.push(ExtensionCapability {
            provider: ProviderId::new("test.provider").unwrap(),
            versions: vec![Version::new(2, 0)],
            features: BTreeSet::new(),
            hotplug: false,
            limits: crate::extension::ExtensionLimits {
                namespace_entries: 0,
                services: 0,
                mappings: 1,
                queued_events: 0,
                request_bytes: 0,
            },
        });
        let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
        spec.extensions.push(requested(required, optional));
        assert_eq!(
            super::validate_spec(&capabilities, &spec)
                .unwrap_err()
                .field,
            "extensions.required_features"
        );
    }

    #[test]
    fn typed_lowering_preserves_the_frozen_legacy_wire_record() {
        let domain = Domain::from_identity([11, 22]);
        let root = std::path::PathBuf::from("/tmp/typed-wire-root");
        let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/echo");
        spec.process.argv.push("hello".into());
        spec.process.cwd = "/tmp".into();
        spec.process.env.push(("A".into(), "B".into()));
        spec.process.domain = Some(domain);
        spec.identity.uid = Some(12);
        spec.identity.gid = Some(34);
        spec.identity.hostname = Some("typed".into());
        spec.filesystem.root = Some(TreeSource::HostDirectory(root.clone()));
        spec.filesystem.read_only = true;
        spec.resources.memory_bytes = Some(4096);
        spec.resources.process_limit = Some(8);
        spec.resources.cpu_limit = Some(2);
        spec.security.sandbox = Sandbox::SentryOnly;
        spec.network.mode = NetworkMode::None;

        let (_, typed, program, arguments, _, _, _) = super::lower(spec).unwrap();
        let legacy = crate::Config::new()
            .root(root)
            .working_dir("/tmp")
            .env("A", "B")
            .domain(domain)
            .uid(12)
            .gid(34)
            .hostname("typed")
            .read_only_root(true)
            .memory_limit(4096)
            .process_limit(8)
            .cpu_limit(2)
            .sandbox(Sandbox::SentryOnly)
            .network(true);
        let mut argv = vec![program];
        argv.extend(arguments);
        assert_eq!(
            crate::wire::encode(&typed, &argv, None).unwrap(),
            crate::wire::encode(&legacy, &argv, None).unwrap()
        );
    }
}
