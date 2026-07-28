use crate::{
    configfile::ConfigFile,
    extension::{Authorities, BindAccess, ExtensionCapability, HandlesAuthority, ProviderId},
    ffi,
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

/// Which halves of the checkpoint lifecycle a caller-supplied store backs.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StoreDirection {
    /// The launch captures into the store.
    Capture,
    /// The launch restores from the store.
    Restore,
    /// The launch restores from the store and can capture back into it.
    Both,
}

impl StoreDirection {
    const fn captures(self) -> bool {
        matches!(self, Self::Capture | Self::Both)
    }
    const fn restores(self) -> bool {
        matches!(self, Self::Restore | Self::Both)
    }
}

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
        validation::validate(&self.capabilities(), spec)
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
        // The image only ever travels over a store channel, and this entry point creates none.
        if spec.checkpoint.enabled {
            return Err(SpawnError::Spec(crate::spec::SpecError {
                category: crate::spec::SpecErrorCategory::Conflict,
                field: "checkpoint".into(),
                resource: None,
                context: "checkpointing requires Engine::spawn_with_store".into(),
            }));
        }
        validation::validate_authorities(&spec, &authorities).map_err(SpawnError::Spec)?;
        let resources = lowering::allocate_memory(&spec, &authorities).map_err(SpawnError::Spec)?;
        let launch = lowering::Launch::from_spec(spec).map_err(SpawnError::Spec)?;
        launch::start(launch, io, authorities, resources)
            .map(Machine::new)
            .map_err(SpawnError::Engine)
    }

    /// Starts a machine whose checkpoint image is carried by a caller-supplied store.
    /// See [`crate::checkpoint_stream`] for why this needs a server rather than a callback.
    ///
    /// # Errors
    /// Returns a specification error for an invalid spec, or an engine error when the transport cannot be
    /// created or activation fails.
    pub fn spawn_with_store(
        &self,
        spec: MachineSpec,
        io: ProcessIo,
        store: Arc<dyn crate::CheckpointStore>,
        direction: StoreDirection,
    ) -> Result<Machine, SpawnError> {
        self.spawn_with_store_and_authorities(spec, io, store, direction, Authorities::default())
    }

    /// [`Engine::spawn_with_store`] with the narrow provider ports granted for this launch.
    ///
    /// # Errors
    /// As [`Engine::spawn_with_store`].
    pub fn spawn_with_store_and_authorities(
        &self,
        spec: MachineSpec,
        io: ProcessIo,
        store: Arc<dyn crate::CheckpointStore>,
        direction: StoreDirection,
        authorities: Authorities,
    ) -> Result<Machine, SpawnError> {
        let mut spec = spec;
        spec.checkpoint.enabled = true;
        spec.checkpoint.capture = direction.captures();
        spec.checkpoint.restore = direction.restores();
        self.validate(&spec).map_err(SpawnError::Spec)?;
        validation::validate_authorities(&spec, &authorities).map_err(SpawnError::Spec)?;
        let resources = lowering::allocate_memory(&spec, &authorities).map_err(SpawnError::Spec)?;
        let launch = lowering::Launch::from_spec(spec).map_err(SpawnError::Spec)?;
        let (broker, child) =
            ffi::Broker::pair().map_err(|error| SpawnError::Engine(Error::Io(error)))?;
        let trigger =
            ffi::Trigger::create().map_err(|error| SpawnError::Engine(Error::Io(error)))?;
        let server = Arc::new(crate::checkpoint_stream::SinkServer::new(store));
        let acceptor = crate::checkpoint_stream::SinkServer::start(&server, broker);
        let started = launch::start_channels(
            launch,
            io,
            authorities,
            resources,
            Some((child.raw(), trigger.raw())),
        );
        drop(child);
        match started {
            Ok(child) => Ok(Machine::with_store(child, server, trigger, acceptor)),
            Err(error) => {
                server.stop();
                Err(SpawnError::Engine(error))
            }
        }
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
        launch::start_full(
            guest,
            config,
            program,
            arguments,
            streams,
            terminal,
            projections,
            services.map(|(launch, authority)| (launch, authority.into())),
            Vec::new(),
            None,
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
        "metadata",
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
        spec::Version,
        Engine, Guest, MachineSpec,
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
        let validation = super::validation::validate(&capabilities, &spec).unwrap();
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
            super::validation::validate(&capabilities, &spec)
                .unwrap_err()
                .field,
            "extensions.required_features"
        );
    }
}
