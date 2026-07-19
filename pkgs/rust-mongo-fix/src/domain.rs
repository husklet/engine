use crate::{ffi, Error};

/// Durable identity for every native process descended from one engine launch.
///
/// Unlike a process identifier or process group, this remains valid after the
/// container init exits and across daemonizing forks and new sessions.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct Domain([u64; 2]);

impl Domain {
    pub(crate) const fn from_identity(identity: [u64; 2]) -> Self {
        Self(identity)
    }

    pub(crate) fn create() -> Result<Self, Error> {
        use std::io::Read as _;
        loop {
            let mut bytes = [0_u8; 16];
            std::fs::File::open("/dev/urandom")?.read_exact(&mut bytes)?;
            let identity = [
                u64::from_ne_bytes(bytes[..8].try_into().unwrap()),
                u64::from_ne_bytes(bytes[8..].try_into().unwrap()),
            ];
            if identity != [0, 0] {
                return Ok(Self(identity));
            }
        }
    }

    pub(crate) const fn identity(self) -> [u64; 2] {
        self.0
    }

    /// Force-stops all live members. Calling this repeatedly is safe.
    ///
    /// # Errors
    /// Returns a native process-control failure if a verified member could not be stopped.
    pub fn terminate(self) -> Result<(), Error> {
        ffi::terminate_domain(self.0).map_err(|status| Error::Engine { status, detail: 0 })
    }
}
