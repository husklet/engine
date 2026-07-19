//! Backend-independent contracts for HL engine implementations and callers.

pub mod checkpoint;
pub mod control;
pub mod extension;
pub mod observability;
pub mod spec;
mod types;

pub use types::{Access, Guest, Mount, Sandbox, Stdio};

/// Version of a negotiated engine or extension contract.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Version {
    pub major: u16,
    pub minor: u16,
}

impl Version {
    #[must_use]
    pub const fn new(major: u16, minor: u16) -> Self {
        Self { major, minor }
    }
}
