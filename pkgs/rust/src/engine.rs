use crate::{
    extension::{Authorities, BindAccess, ExtensionCapability, HandlesAuthority, ProviderId},
    ffi,
    configfile::ConfigFile,
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

mod discovery;
mod launch;
pub(crate) mod lowering;
mod validation;

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
    pub fn capabilities(&self) -> EngineCapabilities {
        discovery::capabilities()
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
        self.spawn_with_authorities(spec, io, authority.into())
    }

    /// Starts a machine with the narrow provider ports granted for this launch.
    ///
    /// # Errors
    /// Returns a typed specification error for missing or excess authority, invalid provider
    /// resources, or an engine error when activation fails.
    pub fn spawn_with_authorities(
        &self,
        spec: MachineSpec,
        io: ProcessIo,
        authorities: Authorities,
    ) -> Result<Machine, SpawnError> {
        self.validate(&spec).map_err(SpawnError::Spec)?;
        validation::validate_authorities(&spec, &authorities).map_err(SpawnError::Spec)?;
        let resources = lowering::allocate_memory(&spec, &authorities).map_err(SpawnError::Spec)?;
        let launch = lower(spec).map_err(SpawnError::Spec)?;
        launch::start(launch, io, authorities, resources)
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
        services: Option<(lowering::ServiceLaunch, HandlesAuthority)>,
    ) -> Result<Child, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        launch::start_legacy(
            guest,
            config,
            program,
            arguments,
            streams,
            terminal,
            projections,
            services.map(|(launch, authority)| (launch, authority.into())),
            Vec::new(),
        )
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
    [
        "read",
        "write",
        "poll",
        "ofd-lifecycle",
        "memory-allocation",
        "devices",
    ]
    .into_iter()
    .map(|name| crate::extension::Feature::new(name).unwrap_or_else(|_| unreachable!()))
    .collect()
}

fn namespace_features() -> BTreeSet<crate::extension::Feature> {
    [
        "directories",
        "host-bind-read-only",
        "immutable-files",
        "mutable-files",
        "unix-sockets",
        "symlinks",
    ]
    .into_iter()
    .map(|name| {
        crate::extension::Feature::new(name)
            .unwrap_or_else(|_| unreachable!("constant feature is valid"))
    })
    .collect()
}

fn validate_spec(
    capabilities: &EngineCapabilities,
    spec: &MachineSpec,
) -> Result<Validation, SpecError> {
    validation::validate(capabilities, spec)
}

fn lower(spec: MachineSpec) -> Result<lowering::Launch, SpecError> {
    lowering::Launch::from_spec(spec)
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

        let launch = super::lower(spec).unwrap();
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
        let mut argv = vec![launch.program];
        argv.extend(launch.arguments);
        assert_eq!(
            crate::wire::encode(&launch.config, &argv, None).unwrap(),
            crate::wire::encode(&legacy, &argv, None).unwrap()
        );
    }
}
