//! Live provider ports for HL engine implementations.
#![deny(unsafe_code)]

use std::{
    collections::{BTreeMap, BTreeSet},
    sync::Arc,
    time::{Duration, SystemTime},
};

use hl_engine_api::{
    extension::{
        ContractError, ExtensionCapability, ExtensionConfig, ExtensionLimits, ExtensionSelection,
        ExtensionSpec, Feature, Inheritance, MemoryRequirement, NamespaceEntry, Protections,
        ProviderId, ServiceId, ServiceRegistration, Sharing,
    },
    Version,
};

/// Launch-scoped authority for provider-backed open services.
///
/// Authorities are deliberately separate from [`ExtensionSpec`]: a declarative
/// machine specification remains cloneable and inspectable, while host authority
/// is granted explicitly at spawn and cannot leak into another launch.
#[derive(Default)]
pub struct HandlesAuthority {
    providers: BTreeMap<ProviderId, Arc<dyn Handles>>,
}

impl HandlesAuthority {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Grants one provider's handle authority to this launch.
    ///
    /// # Errors
    /// Returns [`ContractError::DuplicateProvider`] when the provider was
    /// already granted.
    pub fn grant(
        &mut self,
        provider: ProviderId,
        handles: Arc<dyn Handles>,
    ) -> Result<(), ContractError> {
        if self.providers.insert(provider, handles).is_some() {
            return Err(ContractError::DuplicateProvider);
        }
        Ok(())
    }

    /// Returns the authority granted for a provider, when present.
    #[must_use]
    pub fn handles(&self, provider: &ProviderId) -> Option<&Arc<dyn Handles>> {
        self.providers.get(provider)
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ProcessId(pub u64);
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ResourceId(pub u64);

/// A manifest and preparation boundary implemented outside the engine core.
pub trait ExtensionProvider: Send + Sync {
    fn manifest(&self) -> ExtensionManifest;
    /// Validates provider configuration and reserves resources for one launch.
    ///
    /// # Errors
    /// Returns a typed provider, validation, protocol, quota, cancellation, or timeout failure.
    fn prepare(
        &self,
        context: &PrepareContext,
        config: &ExtensionConfig,
    ) -> Result<PreparedExtension, ExtensionError>;
}

/// Negotiates versioned extension specifications before any provider is prepared.
pub trait Extensions: Send + Sync {
    fn capabilities(&self) -> Vec<ExtensionCapability>;
    /// Selects one compatible contract version and feature set.
    ///
    /// # Errors
    /// Returns a typed negotiation error when required contract elements cannot be selected.
    fn negotiate(&self, spec: &ExtensionSpec) -> Result<ExtensionSelection, ExtensionError>;
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionManifest {
    pub provider: ProviderId,
    pub versions: Vec<Version>,
    pub schema: String,
    pub features: BTreeSet<Feature>,
    pub limits: ExtensionLimits,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PrepareContext {
    pub process: ProcessId,
    pub deadline: SystemTime,
}

pub struct PreparedExtension {
    pub namespace: Vec<NamespaceEntry>,
    pub services: Vec<ServiceRegistration>,
    pub memory: Vec<MemoryRequirement>,
    pub lifecycle: Option<Box<dyn Lifecycle>>,
}

/// Transactional namespace installation port.
pub trait Namespace: Send + Sync {
    /// Validates and atomically installs all entries.
    ///
    /// # Errors
    /// Returns a Linux error without changing the namespace when validation or installation fails.
    fn install(&self, entries: Vec<NamespaceEntry>) -> Result<NamespaceGeneration, LinuxError>;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NamespaceGeneration(pub u64);

/// Opens provider resources. Guest descriptor numbers never cross this boundary.
pub trait Handles: Send + Sync {
    /// Opens one provider-backed open-file description.
    ///
    /// # Errors
    /// Returns the Linux error visible to the guest when opening fails.
    fn open(&self, request: OpenRequest) -> Result<Box<dyn OpenHandle>, LinuxError>;
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OpenRequest {
    pub service: ServiceId,
    pub access: OpenAccess,
    pub credentials: Credentials,
    pub deadline: SystemTime,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OpenAccess {
    pub read: bool,
    pub write: bool,
    pub nonblocking: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Credentials {
    pub uid: u32,
    pub gid: u32,
    pub groups: Vec<u32>,
}

pub trait OpenHandle: Send + Sync {
    /// Reads owned bytes from the open description.
    ///
    /// # Errors
    /// Returns a Linux I/O error on invalid or unsupported requests.
    fn read(&self, request: ReadRequest) -> Result<Vec<u8>, LinuxError>;
    /// Writes owned bytes to the open description.
    ///
    /// # Errors
    /// Returns a Linux I/O error on invalid or unsupported requests.
    fn write(&self, request: WriteRequest) -> Result<usize, LinuxError>;
    /// Changes the shared open-description offset.
    ///
    /// # Errors
    /// Returns a Linux error when the handle is not seekable or the resulting offset is invalid.
    fn seek(&self, request: SeekRequest) -> Result<u64, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 29,
            context: "provider handle is not seekable".into(),
        })
    }
    /// Returns provider-owned metadata for the open description.
    ///
    /// # Errors
    /// Returns a Linux error when metadata is unavailable.
    fn metadata(&self) -> Result<HandleMetadata, LinuxError> {
        Err(LinuxError {
            errno: 95,
            context: "provider handle metadata is unsupported".into(),
        })
    }
    /// Performs one bounded typed ioctl operation.
    ///
    /// # Errors
    /// Returns a Linux I/O error on invalid or unsupported requests.
    fn ioctl(&self, request: IoctlRequest) -> Result<IoctlReply, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 25,
            context: "provider handle does not support ioctl".into(),
        })
    }
    /// Creates a provider-backed mapping description.
    ///
    /// # Errors
    /// Returns a Linux error when the range or protections cannot be mapped.
    fn map(&self, request: MapRequest) -> Result<Mapping, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 19,
            context: "provider handle does not expose mappable memory".into(),
        })
    }
    /// Samples readiness for an interest set.
    ///
    /// # Errors
    /// Returns a Linux error when readiness cannot be observed.
    fn readiness(&self, interest: Interest) -> Result<Readiness, LinuxError>;
    /// Flushes pending writes.
    ///
    /// # Errors
    /// Returns a Linux error when pending data cannot be flushed.
    fn flush(&self) -> Result<(), LinuxError>;
    fn close(self: Box<Self>);
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ReadRequest {
    pub offset: Option<u64>,
    pub length: u32,
    pub deadline: SystemTime,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WriteRequest {
    pub offset: Option<u64>,
    pub bytes: Vec<u8>,
    pub deadline: SystemTime,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SeekRequest {
    pub offset: i64,
    pub origin: SeekOrigin,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SeekOrigin {
    Start,
    Current,
    End,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HandleMetadata {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlRequest {
    pub command: u64,
    pub input: Vec<u8>,
    pub output_capacity: u32,
    pub deadline: SystemTime,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlReply {
    pub result: i64,
    pub output: Vec<u8>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MapRequest {
    pub offset: u64,
    pub length: u64,
    pub protections: Protections,
    pub sharing: Sharing,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Mapping {
    pub resource: ResourceId,
    pub offset: u64,
    pub length: u64,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Interest {
    pub readable: bool,
    pub writable: bool,
    pub priority: bool,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Readiness {
    pub states: BTreeSet<ReadyState>,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum ReadyState {
    Readable,
    Writable,
    Hangup,
    Error,
}

pub trait Memory: Send + Sync {
    /// Allocates a provider resource matching the declared bounds.
    ///
    /// # Errors
    /// Returns a typed resource error for invalid, unsupported, exhausted, or failed allocations.
    fn allocate(&self, request: AllocationRequest) -> Result<HostResource, ResourceError>;
    /// Imports a validated opaque provider descriptor.
    ///
    /// # Errors
    /// Returns a typed resource error when the descriptor is incompatible or cannot be imported.
    fn import(&self, descriptor: &ResourceDescriptor) -> Result<HostResource, ResourceError>;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AllocationRequest {
    pub size: u64,
    pub alignment: u64,
    pub protections: Protections,
    pub sharing: Sharing,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourceDescriptor {
    pub provider: ProviderId,
    pub version: Version,
    pub bytes: Vec<u8>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HostResource {
    pub id: ResourceId,
    pub regions: Vec<Region>,
    pub handles: Vec<TransferHandle>,
    pub coherency: Coherency,
    pub inheritance: Inheritance,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Region {
    pub offset: u64,
    pub size: u64,
    pub protections: Protections,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferHandle(pub u64);
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Coherency {
    Coherent,
    Explicit,
}

pub trait Events: Send + Sync {
    /// Publishes a bounded provider readiness or completion event.
    ///
    /// # Errors
    /// Returns a Linux error when the queue is unavailable or full.
    fn publish(&self, event: ProviderEvent) -> Result<(), LinuxError>;
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ProviderEvent {
    pub resource: Option<ResourceId>,
    pub readiness: Option<Readiness>,
    pub sequence: u64,
}

pub trait Lifecycle: Send + Sync {
    /// Observes creation before the first guest instruction.
    ///
    /// # Errors
    /// Returns a typed extension error to abort process creation.
    fn process_created(&self, process: ProcessId) -> Result<(), ExtensionError>;
    /// Prepares resources before an emulated fork.
    ///
    /// # Errors
    /// Returns a typed extension error to veto the fork.
    fn fork_prepare(&self, parent: ProcessId, child: ProcessId) -> Result<(), ExtensionError>;
    /// Completes an ordered fork transition.
    ///
    /// # Errors
    /// Returns a typed extension error when resource inheritance cannot complete.
    fn fork_complete(&self, process: ProcessId, child: bool) -> Result<(), ExtensionError>;
    /// Observes exec after close-on-exec cleanup.
    ///
    /// # Errors
    /// Returns a typed extension error when surviving resources cannot transition.
    fn exec(&self, process: ProcessId, surviving: &[ResourceId]) -> Result<(), ExtensionError>;
    fn exit(&self, process: ProcessId);
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LinuxError {
    pub errno: i32,
    pub context: String,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourceError {
    pub category: ResourceErrorCategory,
    pub context: String,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResourceErrorCategory {
    Unsupported,
    Invalid,
    Quota,
    Host,
    Provider,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionError {
    pub category: ExtensionErrorCategory,
    pub context: String,
    pub retry_after: Option<Duration>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExtensionErrorCategory {
    Unsupported,
    Invalid,
    Protocol,
    Timeout,
    Cancelled,
    Quota,
    Provider,
}
