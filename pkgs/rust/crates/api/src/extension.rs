//! Declarative, backend-independent engine extension specifications.

use std::{collections::BTreeSet, path::PathBuf, sync::Arc};

use crate::Version;

/// Stable provider identity. Names are compared byte-for-byte and are not interpreted by the engine.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct ProviderId(String);

impl ProviderId {
    /// Creates a provider identity suitable for wire negotiation.
    ///
    /// # Errors
    /// Returns [`ContractError::InvalidIdentifier`] for an empty, oversized, or non-portable name.
    pub fn new(value: impl Into<String>) -> Result<Self, ContractError> {
        let value = value.into();
        if value.is_empty() || value.len() > 128 || !value.bytes().all(valid_identifier_byte) {
            return Err(ContractError::InvalidIdentifier);
        }
        Ok(Self(value))
    }

    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

const fn valid_identifier_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_')
}

/// One feature selected during extension negotiation.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Feature(String);

impl Feature {
    /// Creates one portable negotiated feature name.
    ///
    /// # Errors
    /// Returns [`ContractError::InvalidFeature`] for an empty, oversized, or non-portable name.
    pub fn new(value: impl Into<String>) -> Result<Self, ContractError> {
        let value = value.into();
        if value.is_empty() || value.len() > 64 || !value.bytes().all(valid_identifier_byte) {
            return Err(ContractError::InvalidFeature);
        }
        Ok(Self(value))
    }

    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

/// Engine support for one extension contract.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionCapability {
    pub provider: ProviderId,
    pub versions: Vec<Version>,
    pub features: BTreeSet<Feature>,
    pub hotplug: bool,
    pub limits: ExtensionLimits,
}

/// Declarative extension request carried by a launch specification.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionSpec {
    pub provider: ProviderId,
    pub version: Version,
    pub required: bool,
    pub required_features: BTreeSet<Feature>,
    pub optional_features: BTreeSet<Feature>,
    /// Manifest-schema-validated provider configuration.
    pub config: ExtensionConfig,
    pub namespace: Vec<NamespaceEntry>,
    pub services: Vec<ServiceRegistration>,
    pub memory: Vec<MemoryRequirement>,
    /// Ordered additions to the initial guest process environment.
    pub environment: Vec<(std::ffi::OsString, std::ffi::OsString)>,
}

/// Provider configuration tagged with the schema that validated it.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionConfig {
    pub schema: String,
    pub bytes: Vec<u8>,
}

impl ExtensionConfig {
    #[must_use]
    pub fn empty(schema: impl Into<String>) -> Self {
        Self {
            schema: schema.into(),
            bytes: Vec::new(),
        }
    }
}

/// Metadata common to projected namespace nodes.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Metadata {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
}

/// An entry installed atomically in the guest namespace.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum NamespaceEntry {
    Directory(DirectoryEntry),
    File(FileEntry),
    Symlink(SymlinkEntry),
    Device(DeviceEntry),
    HostBind(HostBindEntry),
    Socket(SocketEntry),
    Service(ServiceEntry),
}

impl NamespaceEntry {
    #[must_use]
    pub fn path(&self) -> &std::path::Path {
        match self {
            Self::Directory(value) => &value.path,
            Self::File(value) => &value.path,
            Self::Symlink(value) => &value.path,
            Self::Device(value) => &value.path,
            Self::HostBind(value) => &value.path,
            Self::Socket(value) => &value.path,
            Self::Service(value) => &value.path,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DirectoryEntry {
    pub path: PathBuf,
    pub metadata: Metadata,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FileEntry {
    pub path: PathBuf,
    pub metadata: Metadata,
    pub source: FileSource,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum FileSource {
    Immutable(Arc<[u8]>),
    /// Launch-private mutable bytes backed by one coherent open-file object.
    Mutable(Arc<[u8]>),
    Shared(SharedBytesId),
    Host(PathBuf),
    Generated(GeneratorId),
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct SharedBytesId(pub u64);

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct GeneratorId(pub u64);

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SymlinkEntry {
    pub path: PathBuf,
    pub target: PathBuf,
    pub uid: u32,
    pub gid: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DeviceKind {
    Character,
    Block,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DeviceEntry {
    pub path: PathBuf,
    pub metadata: Metadata,
    pub kind: DeviceKind,
    pub major: u32,
    pub minor: u32,
    pub service: Option<ServiceId>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BindAccess {
    ReadOnly,
    ReadWrite,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HostBindEntry {
    pub path: PathBuf,
    pub host: PathBuf,
    pub access: BindAccess,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SocketEntry {
    pub path: PathBuf,
    pub host: PathBuf,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServiceEntry {
    pub path: PathBuf,
    pub metadata: Metadata,
    pub service: ServiceId,
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct ServiceId(pub u64);

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServiceRegistration {
    pub id: ServiceId,
    pub operations: BTreeSet<HandleOperation>,
    pub max_request_bytes: u32,
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum HandleOperation {
    Read,
    Write,
    PositionedIo,
    Seek,
    Truncate,
    Metadata,
    Ioctl,
    Map,
    Poll,
    Transfer,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Sharing {
    Private,
    Shared,
    CopyOnWrite,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Inheritance {
    Retain,
    Duplicate,
    Invalidate,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MemoryRequirement {
    pub size: u64,
    pub alignment: u64,
    pub protections: Protections,
    pub sharing: Sharing,
    pub inheritance: Inheritance,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Protections {
    pub read: bool,
    pub write: bool,
    pub execute: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionSelection {
    pub provider: ProviderId,
    pub version: Version,
    pub features: BTreeSet<Feature>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExtensionLimits {
    pub namespace_entries: u32,
    pub services: u32,
    pub mappings: u32,
    pub queued_events: u32,
    pub request_bytes: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ContractError {
    InvalidIdentifier,
    InvalidFeature,
    DuplicateProvider,
}
