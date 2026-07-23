use crate::api::{
    extension::{
        ExtensionCapability, ExtensionConfig, ExtensionLimits, ExtensionSelection, ExtensionSpec,
        Feature, MemoryRequirement, NamespaceEntry, ProviderId, ServiceRegistration,
    },
    Version,
};
use crate::provider::{ExtensionError, Lifecycle, ProcessId};
use std::{collections::BTreeSet, time::SystemTime};

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
