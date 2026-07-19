//! Side-effect-free preflight results and typed specification errors.

use std::{collections::BTreeSet, path::PathBuf};

use crate::{
    extension::{Feature, ProviderId, ServiceId},
    Version,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Validation {
    pub selected_extensions: Vec<SelectedExtension>,
    pub unavailable_optional_extensions: Vec<ProviderId>,
    pub degraded_features: Vec<DegradedFeature>,
    pub estimated_memory_bytes: u64,
    pub namespace_conflicts: Vec<NamespaceConflict>,
    pub resources: HostResourceEstimate,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SelectedExtension {
    pub provider: ProviderId,
    pub version: Version,
    pub features: BTreeSet<Feature>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DegradedFeature {
    pub provider: ProviderId,
    pub feature: Feature,
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
    Extension(ProviderId),
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
    Provider(ProviderId),
    Service(ServiceId),
    Limit { actual: u64, maximum: u64 },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SpecErrorCategory {
    Invalid,
    Unsupported,
    Conflict,
    Limit,
}

impl std::fmt::Display for SpecError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{}: {}", self.field, self.context)
    }
}

impl std::error::Error for SpecError {}
