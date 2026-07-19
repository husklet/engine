//! Backend-independent live-control requests and result models.

use std::collections::BTreeSet;

use crate::api::extension::ProviderId;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SignalTarget {
    InitialProcess,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProcessInfo {
    pub host_id: u64,
    pub initial: bool,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ResourceUpdate {
    pub memory_bytes: Option<u64>,
    pub process_limit: Option<u32>,
    pub cpu_limit: Option<u32>,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct AttachRequest {
    pub streams: BTreeSet<AttachmentKind>,
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum AttachmentKind {
    Stdin,
    Stdout,
    Stderr,
    Terminal,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionHandle {
    pub provider: ProviderId,
}
