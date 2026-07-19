use crate::protocol::TransportError;
use crate::api::extension::ServiceId;
use crate::provider::{Interest, LinuxError, Readiness};
#[derive(Clone, Debug, Eq, PartialEq)]

pub enum Request {
    Open {
        service: ServiceId,
        read: bool,
        write: bool,
    },
    Read {
        handle: u64,
        offset: u64,
        length: u32,
    },
    Write {
        handle: u64,
        offset: u64,
        bytes: Vec<u8>,
    },
    Seek {
        handle: u64,
        offset: i64,
        whence: SeekWhence,
    },
    Stat {
        handle: u64,
    },
    Poll {
        handle: u64,
        interest: Interest,
    },
    Close {
        handle: u64,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SeekWhence {
    Start,
    Current,
    End,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Reply {
    Opened { handle: u64 },
    Bytes(Vec<u8>),
    Written(u32),
    Offset(u64),
    Stat(ServiceStat),
    Ready(Readiness),
    Closed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ServiceStat {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]

pub enum ServiceFailure {
    Linux(LinuxError),
    Transport(TransportError),
}

impl Request {
    #[must_use]
    pub fn handle(&self) -> u64 {
        match self {
            Self::Read { handle, .. }
            | Self::Write { handle, .. }
            | Self::Seek { handle, .. }
            | Self::Stat { handle }
            | Self::Poll { handle, .. }
            | Self::Close { handle } => *handle,
            Self::Open { .. } => 0,
        }
    }
}
