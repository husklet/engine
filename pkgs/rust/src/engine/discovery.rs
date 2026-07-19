use super::{
    handles_features, handles_provider, namespace_features, namespace_provider, BTreeSet,
    CheckpointCapabilities, CpuCapabilities, EngineCapabilities, EngineLimits, ExtensionCapability,
    FilesystemCapabilities, Guest, GuestPlatform, LinuxCapabilities, NetworkCapabilities,
    NetworkMode, Version,
};

#[allow(clippy::too_many_lines)]
pub(super) fn capabilities() -> EngineCapabilities {
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
        control: crate::spec::ControlCapabilities {
            operations: BTreeSet::from([
                crate::spec::ControlOperation::ProcessInventory,
                crate::spec::ControlOperation::Signal,
                crate::spec::ControlOperation::Pause,
                crate::spec::ControlOperation::Attach,
            ]),
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
                    mappings: 64,
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
            processes: 65_536,
        },
    }
}
