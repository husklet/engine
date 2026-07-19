//! Engine discovery models and hard limits.

use std::collections::BTreeSet;

use crate::{extension::ExtensionCapability, Guest, Version};

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
