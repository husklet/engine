use crate::provider::{LinuxError, ResourceId};
use crate::api::extension::{Protections, ServiceId, Sharing};
use std::{collections::BTreeSet, time::SystemTime};

/// Opens provider resources. Guest descriptor numbers never cross this boundary.
pub trait Handles: Send + Sync {
    /// Opens one provider-backed open-file description.
    ///
    /// # Errors
    /// Returns the Linux error visible to the guest when opening fails.
    fn open(&self, request: OpenRequest) -> Result<Box<dyn OpenHandle>, LinuxError>;
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OpenRequest {
    pub service: ServiceId,
    pub access: OpenAccess,
    pub credentials: Credentials,
    pub deadline: SystemTime,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OpenAccess {
    pub read: bool,
    pub write: bool,
    pub nonblocking: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Credentials {
    pub uid: u32,
    pub gid: u32,
    pub groups: Vec<u32>,
}

pub trait OpenHandle: Send + Sync {
    /// Reads owned bytes from the open description.
    ///
    /// # Errors
    /// Returns a Linux I/O error on invalid or unsupported requests.
    fn read(&self, request: ReadRequest) -> Result<Vec<u8>, LinuxError>;
    /// Writes owned bytes to the open description.
    ///
    /// # Errors
    /// Returns a Linux I/O error on invalid or unsupported requests.
    fn write(&self, request: WriteRequest) -> Result<usize, LinuxError>;
    /// Changes the shared open-description offset.
    ///
    /// # Errors
    /// Returns a Linux error when the handle is not seekable or the resulting offset is invalid.
    fn seek(&self, request: SeekRequest) -> Result<u64, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 29,
            context: "provider handle is not seekable".into(),
        })
    }
    /// Returns provider-owned metadata for the open description.
    ///
    /// # Errors
    /// Returns a Linux error when metadata is unavailable.
    fn metadata(&self) -> Result<HandleMetadata, LinuxError> {
        Err(LinuxError {
            errno: 95,
            context: "provider handle metadata is unsupported".into(),
        })
    }
    /// Performs one bounded typed ioctl operation.
    ///
    /// # Errors
    /// Returns a Linux I/O error on invalid or unsupported requests.
    fn ioctl(&self, request: IoctlRequest) -> Result<IoctlReply, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 25,
            context: "provider handle does not support ioctl".into(),
        })
    }
    /// Creates a provider-backed mapping description.
    ///
    /// # Errors
    /// Returns a Linux error when the range or protections cannot be mapped.
    fn map(&self, request: MapRequest) -> Result<Mapping, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 19,
            context: "provider handle does not expose mappable memory".into(),
        })
    }
    /// Samples readiness for an interest set.
    ///
    /// # Errors
    /// Returns a Linux error when readiness cannot be observed.
    fn readiness(&self, interest: Interest) -> Result<Readiness, LinuxError>;
    /// Flushes pending writes.
    ///
    /// # Errors
    /// Returns a Linux error when pending data cannot be flushed.
    fn flush(&self) -> Result<(), LinuxError>;
    fn close(self: Box<Self>);
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ReadRequest {
    pub offset: Option<u64>,
    pub length: u32,
    pub deadline: SystemTime,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WriteRequest {
    pub offset: Option<u64>,
    pub bytes: Vec<u8>,
    pub deadline: SystemTime,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SeekRequest {
    pub offset: i64,
    pub origin: SeekOrigin,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SeekOrigin {
    Start,
    Current,
    End,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HandleMetadata {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlRequest {
    pub command: u64,
    pub input: Vec<u8>,
    pub output_capacity: u32,
    pub deadline: SystemTime,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlReply {
    pub result: i64,
    pub output: Vec<u8>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MapRequest {
    pub offset: u64,
    pub length: u64,
    pub protections: Protections,
    pub sharing: Sharing,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Mapping {
    pub resource: ResourceId,
    pub offset: u64,
    pub length: u64,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Interest {
    pub readable: bool,
    pub writable: bool,
    pub priority: bool,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Readiness {
    pub states: BTreeSet<ReadyState>,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum ReadyState {
    Readable,
    Writable,
    Hangup,
    Error,
}
