use std::io::Write;

use super::{
    ABI, MAGIC_REPLY, MAGIC_REQUEST, NAME_MAX, OP_CLAIM, OP_COMMIT, OP_DIGEST, OP_GROUP_ABORT,
    OP_GROUP_BEGIN, OP_GROUP_COMMIT, OP_GROUP_COUNT, OP_GROUP_PRESENT, OP_OBJECT_ABORT,
    OP_OBJECT_BEGIN, OP_OBJECT_FINISH, OP_OBJECT_TELL, OP_OBJECT_WRITE, OP_OBJECT_WRITE_AT,
    OP_SOURCE_LIST, OP_SOURCE_READ, OP_SOURCE_SIZE, OP_UNCLAIM, PAYLOAD_MAX, REPLY_BYTES,
    REQUEST_BYTES, STATUS_ERROR, STATUS_OK,
};

pub(super) struct Operation;

impl Operation {
    pub(super) const fn name(op: u32) -> &'static str {
        match op {
            OP_OBJECT_BEGIN => "OBJECT_BEGIN",
            OP_OBJECT_WRITE => "OBJECT_WRITE",
            OP_OBJECT_WRITE_AT => "OBJECT_WRITE_AT",
            OP_OBJECT_TELL => "OBJECT_TELL",
            OP_OBJECT_FINISH => "OBJECT_FINISH",
            OP_OBJECT_ABORT => "OBJECT_ABORT",
            OP_GROUP_BEGIN => "GROUP_BEGIN",
            OP_GROUP_COMMIT => "GROUP_COMMIT",
            OP_GROUP_ABORT => "GROUP_ABORT",
            OP_CLAIM => "CLAIM",
            OP_UNCLAIM => "UNCLAIM",
            OP_COMMIT => "COMMIT",
            OP_GROUP_PRESENT => "GROUP_PRESENT",
            OP_GROUP_COUNT => "GROUP_COUNT",
            OP_DIGEST => "DIGEST",
            OP_SOURCE_LIST => "SOURCE_LIST",
            OP_SOURCE_SIZE => "SOURCE_SIZE",
            OP_SOURCE_READ => "SOURCE_READ",
            _ => "UNKNOWN",
        }
    }
}

#[derive(Debug)]
pub(super) struct Request {
    pub(super) op: u32,
    pub(super) stream: u64,
    pub(super) offset: u64,
    pub(super) length: u64,
    pub(super) name_size: usize,
}

impl Request {
    pub(super) fn decode(bytes: &[u8; REQUEST_BYTES]) -> Option<Self> {
        let word =
            |at: usize| u32::from_ne_bytes(bytes[at..at + 4].try_into().ok().unwrap_or([0; 4]));
        let long =
            |at: usize| u64::from_ne_bytes(bytes[at..at + 8].try_into().ok().unwrap_or([0; 8]));
        if word(0) != MAGIC_REQUEST || word(4) != ABI {
            return None;
        }
        let name_size = word(40) as usize;
        let length = long(32);
        if name_size > NAME_MAX || length > PAYLOAD_MAX as u64 {
            return None;
        }
        Some(Self {
            op: word(8),
            stream: long(16),
            offset: long(24),
            length,
            name_size,
        })
    }

    pub(super) const fn carries_payload(&self) -> bool {
        self.length != 0 && self.op != OP_SOURCE_READ
    }
}

#[derive(Debug)]
pub(super) struct Reply {
    pub(super) status: i32,
    pub(super) value: u64,
    pub(super) payload: Vec<u8>,
}

impl Reply {
    pub(super) const fn status(status: i32) -> Self {
        Self {
            status,
            value: 0,
            payload: Vec::new(),
        }
    }

    pub(super) const fn ok() -> Self {
        Self::status(STATUS_OK)
    }

    pub(super) const fn error() -> Self {
        Self::status(STATUS_ERROR)
    }

    pub(super) const fn value(value: u64) -> Self {
        Self {
            status: STATUS_OK,
            value,
            payload: Vec::new(),
        }
    }

    pub(super) const fn payload(payload: Vec<u8>) -> Self {
        Self {
            status: STATUS_OK,
            value: 0,
            payload,
        }
    }

    pub(super) fn write(&self, channel: &mut crate::sys::Stream) -> std::io::Result<()> {
        let mut header = [0_u8; REPLY_BYTES];
        header[0..4].copy_from_slice(&MAGIC_REPLY.to_ne_bytes());
        header[4..8].copy_from_slice(&ABI.to_ne_bytes());
        header[8..12].copy_from_slice(&self.status.to_ne_bytes());
        header[16..24].copy_from_slice(&self.value.to_ne_bytes());
        header[24..32].copy_from_slice(&(self.payload.len() as u64).to_ne_bytes());
        channel.write_all(&header)?;
        if !self.payload.is_empty() {
            channel.write_all(&self.payload)?;
        }
        channel.flush()
    }
}
