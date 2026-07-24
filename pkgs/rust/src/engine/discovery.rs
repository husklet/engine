use super::{
    handles_features, handles_provider, namespace_features, namespace_provider, BTreeSet,
    CheckpointCapabilities, CpuCapabilities, EngineCapabilities, EngineLimits, ExtensionCapability,
    FilesystemCapabilities, Guest, GuestPlatform, LinuxCapabilities, NetworkCapabilities,
    NetworkMode, Version,
};

/// Guest architectures whose checkpoint/restore path is supported through this
/// crate.
///
/// Both `Aarch64` and `X86_64` qualify. The x86_64 restore path previously
/// brought a process back with its file descriptors pointing at the launcher's
/// stdio instead of the captured ones (a guest that dup2'd fd 1 onto a file
/// wrote to the console after restore). The root cause was in the C backend:
/// `x86_number()` in `src/linux_abi/number.c` did not map x86 `dup2` (33) onto
/// canonical `dup3` (24), so the shared post-dispatch fd-publish switch never
/// registered a dup2'd descriptor in `proc_fdvis`, and checkpoint's fd-table
/// scan silently dropped it. With that mapping added, every checkpoint fixture
/// reachable from this API (`checkpoint-{tree,pipe,deleted,cycle}`) captures and
/// restores its descriptor table correctly on x86_64, matching aarch64.
///
/// This set is the ONLY place checkpoint guest support is decided: the launch
/// validator reads it, so a guest added or removed here changes what `validate`
/// accepts in the same breath.
pub(super) fn checkpoint_guests() -> BTreeSet<Guest> {
    BTreeSet::from([Guest::Aarch64, Guest::X86_64])
}

#[allow(clippy::too_many_lines)]
pub(super) fn capabilities() -> EngineCapabilities {
    let guest_fd_limit = crate::ffi::guest_fd_limit();
    EngineCapabilities {
        // Version of the typed launch model in this crate, a compile-time fact of the source
        // that defines it. It is deliberately NOT the C ABI number (`HL_ENGINE_ABI`), which
        // versions a different contract.
        api: Version::new(1, 0),
        // Both guest frontends are linked into the embedded archive unconditionally
        // (`cmake/Phase2Production.cmake` builds `src/core/target/{aarch64,x86_64}.c` into the
        // one library), so guest support is a compile-time fact of this build.
        guests: vec![
            GuestPlatform::linux(Guest::Aarch64),
            GuestPlatform::linux(Guest::X86_64),
        ],
        cpu: CpuCapabilities {
            architectures: vec![Guest::Aarch64, Guest::X86_64],
            // The Linux ABI layer serves a fixed 4 KiB guest page size.
            page_sizes: vec![4096],
            // The guest affinity mask is HL_LINUX_AFFINITY_BYTES (128) bytes wide
            // (`src/linux_abi/affinity.h`), so 1024 CPUs is the largest topology the guest ABI
            // can represent. The launch validator enforces this exact number.
            maximum_cpus: 1024,
        },
        linux: LinuxCapabilities {
            // The guest-visible kernel release reported by the engine's own uname(2)
            // implementation (`src/linux_abi/syscall/misc.c`, "6.1.0"). The previous 6.6 claim
            // contradicted what every guest actually observes.
            syscall_abi: Version::new(6, 1),
            // Process domains: the engine exports domain enumeration and termination
            // (`hl_activation_domain_processes` / `hl_activation_domain_terminate`) and
            // `Machine` drives them. Present in every build of this archive.
            process_domains: true,
            // PTYs: `hl_activation_start_terminal` / `hl_terminal_resize` are exported and
            // driven by `Command::terminal`. Present in every build of this archive.
            ptys: true,
            // Descriptor passing: the host transport passes descriptors over SCM_RIGHTS on both
            // supported hosts (`src/host/fork_wire.c`, `src/host/macos/host.c`) and
            // `hl_activation_start_with_transport` exposes it. Present in every build.
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
            // Exactly the resource kinds the launch lowering forwards to the engine
            // (`engine/lowering.rs`: memory, process, CPU-count options). Every other kind is
            // rejected by `validate_resources`, and the tests assert that agreement.
            launch_limits: BTreeSet::from([
                crate::spec::ResourceKind::Memory,
                crate::spec::ResourceKind::Processes,
                crate::spec::ResourceKind::CpuCount,
            ]),
            // `Machine::update_resources` is an unconditional "unsupported" (`machine.rs`).
            live_updates: BTreeSet::new(),
            // No accounting plane exists; `validate_resources` rejects any non-disabled
            // accounting selection.
            accounting: false,
        },
        time: crate::spec::TimeCapabilities {
            // `validate_time_entropy` accepts only the host time source and secure host
            // entropy, and rejects every virtual-time or deterministic-entropy selection.
            host_time: true,
            virtual_time: false,
            deterministic_entropy: false,
        },
        observability: crate::spec::ObservabilityCapabilities {
            // `Machine::events` is an unconditional "unsupported" and `validate_observability`
            // rejects any non-default observability spec, so no queue exists to bound.
            structured_events: false,
            metrics: false,
            tracing: false,
            maximum_queue: 0,
        },
        debugging: crate::spec::DebugCapabilities {
            // `validate_debug` rejects every selected debug operation.
            operations: BTreeSet::new(),
        },
        checkpoint: CheckpointCapabilities {
            guests: checkpoint_guests(),
            format: Some(Version::new(1, 0)),
        },
        control: crate::spec::ControlCapabilities {
            // Each operation names a `Machine` method backed by an exported engine entry point:
            // process inventory (`hl_activation_domain_processes`), signal delivery, pause and
            // resume (stop/continue signals), and attach. The unimplemented control methods
            // (`update_resources`, `update_network`, `hotplug`, `events`) are absent by design.
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
                // `Machine::hotplug` is an unconditional "unsupported": no provider can be
                // attached to a running machine in this build.
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
                // `Machine::hotplug` is an unconditional "unsupported": no provider can be
                // attached to a running machine in this build.
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
