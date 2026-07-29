use crate::model::extension::{Metadata, ServiceId};
use crate::provider::LinuxError;
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
    /// Executes one provider-defined ioctl over the command's bounded root argument.
    ///
    /// # Errors
    /// Returns a Linux error when the command is unknown or its argument is invalid.
    fn ioctl(&self, request: IoctlRequest) -> Result<IoctlResult, LinuxError> {
        let _ = request;
        Err(LinuxError {
            errno: 25,
            context: "provider handle ioctl is unsupported".into(),
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
    pub metadata: Metadata,
    pub size: u64,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlRequest {
    pub command: u64,
    pub argument: Vec<u8>,
    pub deadline: SystemTime,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlResult {
    pub value: i64,
    pub argument: Vec<u8>,
    pub writes: Vec<IoctlWrite>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IoctlWrite {
    pub address: u64,
    pub bytes: Vec<u8>,
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
