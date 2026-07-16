use crate::{ffi, Error};

/// Durable identity for every native process descended from one engine launch.
///
/// Unlike a process identifier or process group, this remains valid after the
/// container init exits and across daemonizing forks and new sessions.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct Domain([u64; 2]);

impl Domain {
    pub(crate) const fn new(identity: [u64; 2]) -> Self {
        Self(identity)
    }

    /// Force-stops all live members. Calling this repeatedly is safe.
    ///
    /// # Errors
    /// Returns a native process-control failure if a verified member could not be stopped.
    pub fn terminate(self) -> Result<(), Error> {
        ffi::terminate_domain(self.0).map_err(|status| Error::Engine { status, detail: 0 })
    }
}
