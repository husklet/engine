//! Typed live-control models for a running machine.

use std::{collections::BTreeSet, sync::MutexGuard};

use crate::{extension::ProviderId, Machine, Terminal};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SignalTarget {
    InitialProcess,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Signal {
    Hangup,
    Interrupt,
    Quit,
    Kill,
    Terminate,
    User1,
    User2,
}

impl Signal {
    pub(crate) const fn host_number(self) -> i32 {
        match self {
            Self::Hangup => 1,
            Self::Interrupt => 2,
            Self::Quit => 3,
            Self::Kill => 9,
            Self::Terminate => 15,
            #[cfg(target_os = "linux")]
            Self::User1 => 10,
            #[cfg(target_os = "linux")]
            Self::User2 => 12,
            #[cfg(target_os = "macos")]
            Self::User1 => 30,
            #[cfg(target_os = "macos")]
            Self::User2 => 31,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProcessInfo {
    pub host_id: u64,
    pub initial: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ShutdownPolicy {
    Signal(Signal),
    Force,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ResourceUpdate {
    pub memory_bytes: Option<u64>,
    pub process_limit: Option<u32>,
    pub cpu_limit: Option<u32>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NetworkUpdate {
    pub replacement: crate::spec::NetworkSpec,
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

pub struct Attachment {
    pub stdin: Option<std::fs::File>,
    pub stdout: Option<std::fs::File>,
    pub stderr: Option<std::fs::File>,
    pub terminal: Option<Terminal>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionHandle {
    pub provider: ProviderId,
}

#[derive(Debug)]
pub struct EventStream {
    _private: (),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ControlError {
    pub category: ControlErrorCategory,
    pub operation: &'static str,
    pub context: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlErrorCategory {
    Unsupported,
    Invalid,
    Finished,
    Host,
    Engine,
}

impl ControlError {
    pub(crate) fn engine(operation: &'static str, error: &crate::Error) -> Self {
        Self {
            category: ControlErrorCategory::Engine,
            operation,
            context: error.to_string(),
        }
    }

    pub(crate) fn finished(operation: &'static str) -> Self {
        Self {
            category: ControlErrorCategory::Finished,
            operation,
            context: "the initial process has already completed".into(),
        }
    }

    #[must_use]
    pub fn unsupported(operation: &'static str) -> Self {
        Self {
            category: ControlErrorCategory::Unsupported,
            operation,
            context: "the current engine control transport does not implement this operation"
                .into(),
        }
    }
}

impl std::fmt::Display for ControlError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{}: {}", self.operation, self.context)
    }
}
impl std::error::Error for ControlError {}

/// Reference-counted machine pause. Dropping the final guard resumes execution.
#[must_use = "dropping the pause guard immediately resumes the machine"]
pub struct PauseGuard<'machine> {
    pub(crate) machine: &'machine Machine,
    pub(crate) active: bool,
}

impl PauseGuard<'_> {
    /// Explicitly releases this pause reference.
    ///
    /// # Errors
    /// Returns an engine control error if the final resume signal cannot be delivered.
    pub fn resume(mut self) -> Result<(), ControlError> {
        self.active = false;
        self.machine.release_pause(true)
    }
}

impl Drop for PauseGuard<'_> {
    fn drop(&mut self) {
        if self.active {
            let _ = self.machine.release_pause(false);
        }
    }
}

pub(crate) fn decrement(mut count: MutexGuard<'_, usize>) -> bool {
    debug_assert!(*count > 0);
    *count -= 1;
    *count == 0
}
