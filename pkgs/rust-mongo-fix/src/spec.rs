//! Typed discovery and launch-plane models.

use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::OsString,
    path::PathBuf,
};

use crate::{
    extension::ExtensionCapability, extension::ExtensionSpec, network, Domain, Guest, Sandbox,
    Size, Stdio,
};

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Version {
    pub major: u16,
    pub minor: u16,
}

impl Version {
    #[must_use]
    pub const fn new(major: u16, minor: u16) -> Self {
        Self { major, minor }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct EngineCapabilities {
    pub api: Version,
    pub guests: Vec<GuestPlatform>,
    pub cpu: CpuCapabilities,
    pub linux: LinuxCapabilities,
    pub filesystems: FilesystemCapabilities,
    pub networking: NetworkCapabilities,
    pub resources: ResourceCapabilities,
    pub time: TimeCapabilities,
    pub observability: ObservabilityCapabilities,
    pub debugging: DebugCapabilities,
    pub checkpoint: CheckpointCapabilities,
    pub extensions: Vec<ExtensionCapability>,
    pub limits: EngineLimits,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct GuestPlatform {
    pub os: GuestOs,
    pub architecture: Guest,
    pub endianness: Endianness,
    pub page_size: u32,
    pub minimum_linux_abi: Version,
}

impl GuestPlatform {
    #[must_use]
    pub const fn linux(architecture: Guest) -> Self {
        Self {
            os: GuestOs::Linux,
            architecture,
            endianness: Endianness::Little,
            page_size: 4096,
            minimum_linux_abi: Version::new(4, 14),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum GuestOs {
    Linux,
}
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Endianness {
    Little,
    Big,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CpuCapabilities {
    pub architectures: Vec<Guest>,
    pub page_sizes: Vec<u32>,
    pub maximum_cpus: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LinuxCapabilities {
    pub syscall_abi: Version,
    pub process_domains: bool,
    pub ptys: bool,
    pub descriptor_passing: bool,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FilesystemCapabilities {
    pub features: BTreeSet<FilesystemFeature>,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum FilesystemFeature {
    HostDirectories,
    Overlay,
    HostBinds,
    ReadOnlyRoot,
    ProjectedNamespace,
    CoherenceNotifications,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NetworkCapabilities {
    pub modes: BTreeSet<NetworkMode>,
    pub maximum_interfaces: u32,
    pub maximum_port_forwards: u32,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum NetworkMode {
    Host,
    None,
    Virtual,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointCapabilities {
    pub supported: bool,
    pub format: Option<Version>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourceCapabilities {
    pub launch_limits: BTreeSet<ResourceKind>,
    pub live_updates: BTreeSet<ResourceKind>,
    pub accounting: bool,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum ResourceKind {
    Memory,
    Processes,
    Threads,
    CpuCount,
    CpuQuota,
    CpuAffinity,
    OpenFiles,
    FileSize,
    LockedMemory,
    Stack,
    AddressSpace,
    Io,
    ExtensionBudget,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TimeCapabilities {
    pub host_time: bool,
    pub virtual_time: bool,
    pub deterministic_entropy: bool,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ObservabilityCapabilities {
    pub structured_events: bool,
    pub metrics: bool,
    pub tracing: bool,
    pub maximum_queue: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DebugCapabilities {
    pub operations: BTreeSet<DebugOperation>,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum DebugOperation {
    Registers,
    Memory,
    Breakpoints,
    Watchpoints,
    SingleStep,
    CoreDumps,
    TranslatedPc,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EngineLimits {
    pub path_bytes: u32,
    pub arguments: u32,
    pub environment_bytes: u32,
    pub namespace_entries: u32,
    pub projected_file_bytes: u64,
    pub extension_specs: u32,
    pub handles: u32,
    pub mappings: u32,
    pub mapped_bytes: u64,
    pub request_bytes: u32,
    pub queued_events: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MachineSpec {
    pub guest: GuestPlatform,
    pub cpu: CpuSpec,
    pub process: ProcessSpec,
    pub identity: IdentitySpec,
    pub filesystem: FilesystemSpec,
    pub namespaces: NamespaceSpec,
    pub network: NetworkSpec,
    pub resources: ResourceSpec,
    pub security: SecuritySpec,
    pub time: TimeSpec,
    pub entropy: EntropySpec,
    pub cache: TranslationCacheSpec,
    pub checkpoint: CheckpointSpec,
    pub observability: ObservabilitySpec,
    pub debug: DebugSpec,
    pub extensions: Vec<ExtensionSpec>,
}

impl MachineSpec {
    #[must_use]
    pub fn new(architecture: Guest, executable: impl Into<OsString>) -> Self {
        Self {
            guest: GuestPlatform::linux(architecture),
            cpu: CpuSpec::default(),
            process: ProcessSpec::new(executable),
            identity: IdentitySpec::default(),
            filesystem: FilesystemSpec::default(),
            namespaces: NamespaceSpec::default(),
            network: NetworkSpec::default(),
            resources: ResourceSpec::default(),
            security: SecuritySpec::default(),
            time: TimeSpec::default(),
            entropy: EntropySpec::default(),
            cache: TranslationCacheSpec::default(),
            checkpoint: CheckpointSpec::default(),
            observability: ObservabilitySpec::default(),
            debug: DebugSpec::default(),
            extensions: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct CpuSpec {
    pub count: Option<u32>,
    pub features: BTreeSet<String>,
    pub model: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ProcessSpec {
    pub executable: OsString,
    pub argv: Vec<OsString>,
    pub env: Vec<(OsString, OsString)>,
    pub cwd: OsString,
    pub umask: u32,
    pub terminal: Option<Size>,
    /// Existing process domain joined by an exec-like launch.
    pub domain: Option<Domain>,
}
impl ProcessSpec {
    #[must_use]
    pub fn new(executable: impl Into<OsString>) -> Self {
        let executable = executable.into();
        Self {
            argv: vec![executable.clone()],
            executable,
            env: Vec::new(),
            cwd: OsString::from("/"),
            umask: 0o022,
            terminal: None,
            domain: None,
        }
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct IdentitySpec {
    pub uid: Option<u32>,
    pub gid: Option<u32>,
    pub supplementary_groups: Vec<u32>,
    pub hostname: Option<OsString>,
    pub domain_name: Option<OsString>,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct FilesystemSpec {
    pub root: Option<TreeSource>,
    pub read_only: bool,
    pub mounts: Vec<crate::extension::HostBindEntry>,
    pub coherence: Option<CoherenceHandle>,
    pub ownership: Vec<InitialOwnership>,
}
/// Opaque channel used to notify the engine about externally changed filesystem identities.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct CoherenceHandle(PathBuf);

impl CoherenceHandle {
    /// Grants the engine access to a host-backed coherence notification channel.
    ///
    /// The current backend implements the channel using a generation file. Callers treat the
    /// returned value as an opaque capability rather than deriving guest-visible path policy from it.
    ///
    /// # Errors
    /// Returns a specification error when the granted host path is not absolute.
    pub fn from_host_file(path: impl Into<PathBuf>) -> Result<Self, SpecError> {
        let path = path.into();
        if !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0) {
            return Err(SpecError {
                category: SpecErrorCategory::Invalid,
                field: "filesystem.coherence".into(),
                resource: Some(SpecResource::Path(path)),
                context: "coherence host paths must be absolute".into(),
            });
        }
        Ok(Self(path))
    }

    pub(crate) fn host_path(&self) -> &std::path::Path {
        &self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct InitialOwnership {
    pub path: PathBuf,
    pub uid: u32,
    pub gid: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TreeSource {
    HostDirectory(PathBuf),
    ImageLayer(ImageLayerHandle),
    Overlay {
        lower: Vec<TreeSource>,
        upper: PathBuf,
        work: PathBuf,
    },
    Provider(crate::extension::ProviderId),
}
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ImageLayerHandle(pub u64);

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct NamespaceSpec {
    pub mount: IsolationMode,
    pub pid: IsolationMode,
    pub uts: IsolationMode,
    pub ipc: IsolationMode,
    pub network: IsolationMode,
    pub user: IsolationMode,
    pub cgroup: IsolationMode,
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub enum IsolationMode {
    #[default]
    Private,
    Host,
    Shared(NamespaceHandle),
}
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct NamespaceHandle(pub u64);

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NetworkSpec {
    pub mode: NetworkMode,
    pub namespace: Option<network::Namespace>,
    pub interfaces: Vec<network::Interface>,
    pub port_forwards: Vec<network::Rule>,
    pub external_listeners: bool,
}
impl Default for NetworkSpec {
    fn default() -> Self {
        Self {
            mode: NetworkMode::Host,
            namespace: None,
            interfaces: Vec::new(),
            port_forwards: Vec::new(),
            external_listeners: false,
        }
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ResourceSpec {
    pub memory_reservation_bytes: Option<u64>,
    pub memory_bytes: Option<u64>,
    pub process_limit: Option<u32>,
    pub thread_limit: Option<u32>,
    pub cpu_limit: Option<u32>,
    pub cpu_quota_micros: Option<u64>,
    pub cpu_affinity: BTreeSet<u32>,
    pub open_files: Option<u32>,
    pub file_size_bytes: Option<u64>,
    pub locked_memory_bytes: Option<u64>,
    pub stack_bytes: Option<u64>,
    pub address_space_bytes: Option<u64>,
    pub io_read_bytes_per_second: Option<u64>,
    pub io_write_bytes_per_second: Option<u64>,
    pub extension_budgets: BTreeMap<crate::extension::ProviderId, u64>,
    pub accounting: AccountingSpec,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum AccountingSpec {
    #[default]
    Disabled,
    Process,
    Machine,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SecuritySpec {
    pub sandbox: Sandbox,
    pub trusted_guest: bool,
    pub executable_memory: bool,
    pub allowed_providers: BTreeSet<crate::extension::ProviderId>,
}
impl Default for SecuritySpec {
    fn default() -> Self {
        Self {
            sandbox: Sandbox::Disabled,
            trusted_guest: false,
            executable_memory: true,
            allowed_providers: BTreeSet::new(),
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TimeSpec {
    pub source: TimeSource,
    pub monotonic: MonotonicOrigin,
    pub timer_resolution_ns: Option<u64>,
    pub rate: TimeRate,
}
impl Default for TimeSpec {
    fn default() -> Self {
        Self {
            source: TimeSource::Host,
            monotonic: MonotonicOrigin::Host,
            timer_resolution_ns: None,
            rate: TimeRate::default(),
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TimeSource {
    Host,
    Offset(i64),
    Frozen(i64),
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum MonotonicOrigin {
    #[default]
    Host,
    Zero,
    Nanoseconds(u64),
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeRate {
    pub numerator: u32,
    pub denominator: u32,
}
impl Default for TimeRate {
    fn default() -> Self {
        Self {
            numerator: 1,
            denominator: 1,
        }
    }
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub enum EntropySpec {
    #[default]
    SecureHost,
    Deterministic(Vec<u8>),
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct TranslationCacheSpec {
    pub directory: Option<PathBuf>,
    pub budget_bytes: Option<u64>,
    pub policy: CachePolicy,
    pub identity: Vec<CacheIdentity>,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CachePolicy {
    #[default]
    Disabled,
    ReadWrite,
    ReadOnly,
    Refresh,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CacheIdentity {
    pub name: String,
    pub bytes: Vec<u8>,
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct CheckpointSpec {
    pub enabled: bool,
    pub mode: CheckpointMode,
    pub maximum_pause_ms: Option<u64>,
    pub incompatible_resources: IncompatibleResourcePolicy,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CheckpointMode {
    #[default]
    Full,
    Incremental,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum IncompatibleResourcePolicy {
    #[default]
    Refuse,
    Reconnect,
    DiscardOptional,
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ObservabilitySpec {
    pub lifecycle_events: bool,
    pub syscall_sampling: Option<u32>,
    pub metrics: bool,
    pub events: BTreeSet<EventKind>,
    pub metric_kinds: BTreeSet<MetricKind>,
    pub queue_capacity: Option<u32>,
    pub trace: Option<TraceSpec>,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum EventKind {
    Machine,
    Process,
    Thread,
    Exec,
    Signal,
    Fault,
    Syscall,
    Translation,
    Cache,
    Memory,
    Descriptor,
    Filesystem,
    Network,
    Provider,
    Checkpoint,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum MetricKind {
    Cpu,
    Memory,
    Translation,
    Cache,
    Syscall,
    Filesystem,
    Network,
    Provider,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TraceSpec {
    pub queue_capacity: u32,
    pub include_guest_time: bool,
    pub include_host_time: bool,
    pub privacy: TracePrivacy,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TracePrivacy {
    MetadataOnly,
    Arguments,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct DebugSpec {
    pub authorization: Option<DebugAuthorization>,
    pub operations: BTreeSet<DebugOperation>,
    pub breakpoints: u32,
    pub watchpoints: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DebugAuthorization {
    pub capability: Vec<u8>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProcessIo {
    pub stdin: Stdio,
    pub stdout: Stdio,
    pub stderr: Stdio,
}
impl Default for ProcessIo {
    fn default() -> Self {
        Self {
            stdin: Stdio::Inherit,
            stdout: Stdio::Inherit,
            stderr: Stdio::Inherit,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Validation {
    pub selected_extensions: Vec<SelectedExtension>,
    pub unavailable_optional_extensions: Vec<crate::extension::ProviderId>,
    pub degraded_features: Vec<DegradedFeature>,
    pub estimated_memory_bytes: u64,
    pub namespace_conflicts: Vec<NamespaceConflict>,
    pub resources: HostResourceEstimate,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SelectedExtension {
    pub provider: crate::extension::ProviderId,
    pub version: Version,
    pub features: BTreeSet<crate::extension::Feature>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DegradedFeature {
    pub provider: crate::extension::ProviderId,
    pub feature: crate::extension::Feature,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NamespaceConflict {
    pub path: PathBuf,
    pub first: NamespaceOwner,
    pub second: NamespaceOwner,
    pub disposition: ConflictDisposition,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum NamespaceOwner {
    Filesystem,
    Extension(crate::extension::ProviderId),
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConflictDisposition {
    InactiveOptional,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct HostResourceEstimate {
    pub memory_bytes: u64,
    pub extension_memory_bytes: u64,
    pub processes: u32,
    pub cpus: u32,
    pub namespace_entries: u32,
    pub services: u32,
    pub mappings: u32,
    pub event_queue: u32,
    pub cache_bytes: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpecError {
    pub category: SpecErrorCategory,
    pub field: String,
    pub resource: Option<SpecResource>,
    pub context: String,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SpecResource {
    Path(PathBuf),
    Provider(crate::extension::ProviderId),
    Service(crate::extension::ServiceId),
    Limit { actual: u64, maximum: u64 },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SpecErrorCategory {
    Invalid,
    Unsupported,
    Conflict,
    Limit,
}

#[derive(Debug)]
pub enum SpawnError {
    Spec(SpecError),
    Engine(crate::Error),
}

impl std::fmt::Display for SpecError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{}: {}", self.field, self.context)
    }
}
impl std::error::Error for SpecError {}
impl std::fmt::Display for SpawnError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Spec(error) => error.fmt(formatter),
            Self::Engine(error) => error.fmt(formatter),
        }
    }
}
impl std::error::Error for SpawnError {}
