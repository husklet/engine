//! Resource limits and accounting policy.

use std::collections::{BTreeMap, BTreeSet};

use crate::api::extension::ProviderId;

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ResourceSpec {
    pub memory_reservation_bytes: Option<u64>,
    pub memory_bytes: Option<u64>,
    pub process_limit: Option<u32>,
    pub thread_limit: Option<u32>,
    pub cpu_limit: Option<u32>,
    pub cpu_quota_micros: Option<u64>,
    pub cpu_affinity: BTreeSet<u32>,
    pub open_files: Option<u32>,
    pub file_size_bytes: Option<u64>,
    pub locked_memory_bytes: Option<u64>,
    pub stack_bytes: Option<u64>,
    pub address_space_bytes: Option<u64>,
    pub io_read_bytes_per_second: Option<u64>,
    pub io_write_bytes_per_second: Option<u64>,
    pub extension_budgets: BTreeMap<ProviderId, u64>,
    pub accounting: AccountingSpec,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum AccountingSpec {
    #[default]
    Disabled,
    Process,
    Machine,
}
