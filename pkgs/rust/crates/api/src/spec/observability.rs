//! Observability and debugger launch policy.

use std::collections::BTreeSet;

use super::DebugOperation;

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ObservabilitySpec {
    pub lifecycle_events: bool,
    pub syscall_sampling: Option<u32>,
    pub metrics: bool,
    pub events: BTreeSet<EventKind>,
    pub metric_kinds: BTreeSet<MetricKind>,
    pub queue_capacity: Option<u32>,
    pub trace: Option<TraceSpec>,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum EventKind {
    Machine,
    Process,
    Thread,
    Exec,
    Signal,
    Fault,
    Syscall,
    Translation,
    Cache,
    Memory,
    Descriptor,
    Filesystem,
    Network,
    Provider,
    Checkpoint,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum MetricKind {
    Cpu,
    Memory,
    Translation,
    Cache,
    Syscall,
    Filesystem,
    Network,
    Provider,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TraceSpec {
    pub queue_capacity: u32,
    pub include_guest_time: bool,
    pub include_host_time: bool,
    pub privacy: TracePrivacy,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TracePrivacy {
    MetadataOnly,
    Arguments,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct DebugSpec {
    pub authorization: Option<DebugAuthorization>,
    pub operations: BTreeSet<DebugOperation>,
    pub breakpoints: u32,
    pub watchpoints: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DebugAuthorization {
    pub capability: Vec<u8>,
}
