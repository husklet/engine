use std::ffi::OsString;

use crate::{extension::ExtensionSpec, Guest};

use super::{
    CheckpointSpec, CpuSpec, DebugSpec, EntropySpec, FilesystemSpec, GuestPlatform, IdentitySpec,
    NamespaceSpec, NetworkSpec, ObservabilitySpec, ProcessSpec, ResourceSpec, SecuritySpec,
    TimeSpec, TranslationCacheSpec,
};

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
