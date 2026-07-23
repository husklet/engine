//! Time, entropy, translation-cache, and checkpoint launch policy.

use std::path::PathBuf;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TimeSpec {
    pub source: TimeSource,
    pub monotonic: MonotonicOrigin,
    pub timer_resolution_ns: Option<u64>,
    pub rate: TimeRate,
}
impl Default for TimeSpec {
    fn default() -> Self {
        Self {
            source: TimeSource::Host,
            monotonic: MonotonicOrigin::Host,
            timer_resolution_ns: None,
            rate: TimeRate::default(),
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TimeSource {
    Host,
    Offset(i64),
    Frozen(i64),
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum MonotonicOrigin {
    #[default]
    Host,
    Zero,
    Nanoseconds(u64),
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeRate {
    pub numerator: u32,
    pub denominator: u32,
}
impl Default for TimeRate {
    fn default() -> Self {
        Self {
            numerator: 1,
            denominator: 1,
        }
    }
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub enum EntropySpec {
    #[default]
    SecureHost,
    Deterministic(Vec<u8>),
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct TranslationCacheSpec {
    pub directory: Option<PathBuf>,
    pub budget_bytes: Option<u64>,
    pub policy: CachePolicy,
    pub identity: Vec<CacheIdentity>,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CachePolicy {
    #[default]
    Disabled,
    ReadWrite,
    ReadOnly,
    Refresh,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CacheIdentity {
    pub name: String,
    pub bytes: Vec<u8>,
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct CheckpointSpec {
    pub enabled: bool,
    /// Arm this launch to write a native whole-process-tree checkpoint here.
    pub capture_directory: Option<PathBuf>,
    /// Restore a native whole-process-tree checkpoint from this directory.
    pub restore_directory: Option<PathBuf>,
    pub mode: CheckpointMode,
    pub maximum_pause_ms: Option<u64>,
    pub incompatible_resources: IncompatibleResourcePolicy,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CheckpointMode {
    #[default]
    Full,
    Incremental,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum IncompatibleResourcePolicy {
    #[default]
    Refuse,
    Reconnect,
    DiscardOptional,
}
