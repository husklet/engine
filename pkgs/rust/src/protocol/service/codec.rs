use super::{
    input::{linux, protocol, put_i32, put_i64, put_u16, put_u32, put_u64, Input},
    Reply, Request, SeekWhence, ServiceFailure, ServiceStat,
};
use crate::api::extension::ServiceId;
use crate::provider::{Interest, LinuxError, Readiness, ReadyState};

const OPEN: u8 = 1;
const READ: u8 = 2;
pub(super) const WRITE: u8 = 3;
const SEEK: u8 = 4;
const STAT: u8 = 5;
const POLL: u8 = 6;
const CLOSE: u8 = 7;
const ERROR: u8 = 0xff;

/// Encodes one bounded service request.
///
/// # Errors
/// Returns a typed Linux or transport failure when values exceed the frozen contract.
pub fn encode_request(request: &Request, maximum: u32) -> Result<Vec<u8>, ServiceFailure> {
    let mut out = Vec::new();
    match request {
        Request::Open {
            service,
            read,
            write,
        } => {
            out.push(OPEN);
            put_u64(&mut out, service.0);
            out.push(u8::from(*read) | (u8::from(*write) << 1));
        }
        Request::Read {
            handle,
            offset,
            length,
        } => {
            if *length > maximum {
                return Err(linux(22, "service read exceeds request bound"));
            }
            out.push(READ);
            put_u64(&mut out, *handle);
            put_u64(&mut out, *offset);
            put_u32(&mut out, *length);
        }
        Request::Write {
            handle,
            offset,
            bytes,
        } => {
            let length = u32::try_from(bytes.len())
                .map_err(|_| linux(22, "service write exceeds protocol range"))?;
            if length > maximum {
                return Err(linux(22, "service write exceeds request bound"));
            }
            out.push(WRITE);
            put_u64(&mut out, *handle);
            put_u64(&mut out, *offset);
            put_u32(&mut out, length);
            out.extend(bytes);
        }
        Request::Seek {
            handle,
            offset,
            whence,
        } => {
            out.push(SEEK);
            put_u64(&mut out, *handle);
            put_i64(&mut out, *offset);
            out.push(match whence {
                SeekWhence::Start => 0,
                SeekWhence::Current => 1,
                SeekWhence::End => 2,
            });
        }
        Request::Stat { handle } => {
            out.push(STAT);
            put_u64(&mut out, *handle);
        }
        Request::Poll { handle, interest } => {
            out.push(POLL);
            put_u64(&mut out, *handle);
            out.push(
                u8::from(interest.readable)
                    | (u8::from(interest.writable) << 1)
                    | (u8::from(interest.priority) << 2),
            );
        }
        Request::Close { handle } => {
            out.push(CLOSE);
            put_u64(&mut out, *handle);
        }
    }
    Ok(out)
}

/// Decodes one bounded service request.
///
/// # Errors
/// Returns a typed failure for malformed or oversized input.
pub fn decode_request(bytes: &[u8], maximum: u32) -> Result<Request, ServiceFailure> {
    let mut input = Input::new(bytes);
    let tag = input.u8()?;
    let request = match tag {
        OPEN => {
            let service = ServiceId(input.u64()?);
            let access = input.u8()?;
            if access & !3 != 0 {
                return Err(protocol());
            }
            Request::Open {
                service,
                read: access & 1 != 0,
                write: access & 2 != 0,
            }
        }

        READ => {
            let handle = input.u64()?;
            let offset = input.u64()?;
            let length = input.u32()?;
            if length > maximum {
                return Err(linux(22, "service read exceeds request bound"));
            }
            Request::Read {
                handle,
                offset,
                length,
            }
        }
        WRITE => {
            let handle = input.u64()?;
            let offset = input.u64()?;
            let length = input.u32()?;
            if length > maximum {
                return Err(linux(22, "service write exceeds request bound"));
            }
            let payload = input.bytes(length as usize)?.to_vec();
            Request::Write {
                handle,
                offset,
                bytes: payload,
            }
        }
        SEEK => {
            let handle = input.u64()?;
            let offset = input.i64()?;
            let whence = match input.u8()? {
                0 => SeekWhence::Start,
                1 => SeekWhence::Current,
                2 => SeekWhence::End,
                _ => return Err(protocol()),
            };
            Request::Seek {
                handle,
                offset,
                whence,
            }
        }
        STAT => Request::Stat {
            handle: input.u64()?,
        },
        POLL => {
            let handle = input.u64()?;
            let value = input.u8()?;
            if value & !7 != 0 {
                return Err(protocol());
            }
            Request::Poll {
                handle,
                interest: Interest {
                    readable: value & 1 != 0,
                    writable: value & 2 != 0,
                    priority: value & 4 != 0,
                },
            }
        }
        CLOSE => Request::Close {
            handle: input.u64()?,
        },
        _ => return Err(protocol()),
    };
    input.finish()?;
    Ok(request)
}

/// Encodes one bounded service reply or Linux failure.
///
/// # Errors
/// Returns a typed failure when output exceeds the negotiated bound.
pub fn encode_reply(
    reply: &Result<Reply, ServiceFailure>,
    maximum: u32,
) -> Result<Vec<u8>, ServiceFailure> {
    let mut out = Vec::new();
    match reply {
        Err(ServiceFailure::Linux(error)) => {
            out.push(ERROR);
            put_i32(&mut out, error.errno);
            let context = error.context.as_bytes();
            let length = u16::try_from(context.len()).map_err(|_| protocol())?;
            put_u16(&mut out, length);
            out.extend(context);
        }

        Err(ServiceFailure::Transport(error)) => return Err(ServiceFailure::Transport(*error)),
        Ok(value) => match value {
            Reply::Opened { handle } => {
                out.push(OPEN);
                put_u64(&mut out, *handle);
            }

            Reply::Bytes(bytes) => {
                let length = u32::try_from(bytes.len()).map_err(|_| protocol())?;
                if length > maximum {
                    return Err(protocol());
                }
                out.push(READ);
                put_u32(&mut out, length);
                out.extend(bytes);
            }

            Reply::Written(value) => {
                out.push(WRITE);
                put_u32(&mut out, *value);
            }

            Reply::Offset(value) => {
                out.push(SEEK);
                put_u64(&mut out, *value);
            }

            Reply::Stat(value) => {
                out.push(STAT);
                put_u32(&mut out, value.mode);
                put_u32(&mut out, value.uid);
                put_u32(&mut out, value.gid);
                put_u64(&mut out, value.size);
            }

            Reply::Ready(value) => {
                out.push(POLL);
                let mut states = 0;
                for state in &value.states {
                    states |= match state {
                        ReadyState::Readable => 1,
                        ReadyState::Writable => 2,
                        ReadyState::Hangup => 4,
                        ReadyState::Error => 8,
                    };
                }
                out.push(states);
            }
            Reply::Closed => out.push(CLOSE),
        },
    }
    Ok(out)
}

/// Decodes one bounded service reply.
///
/// # Errors
/// Returns a typed failure for malformed or oversized input, or the encoded Linux failure.
pub fn decode_reply(bytes: &[u8], maximum: u32) -> Result<Reply, ServiceFailure> {
    let mut input = Input::new(bytes);
    let tag = input.u8()?;
    if tag == ERROR {
        let errno = input.i32()?;
        let length = input.u16()? as usize;
        let context = std::str::from_utf8(input.bytes(length)?)
            .map_err(|_| protocol())?
            .to_owned();
        input.finish()?;
        return Err(ServiceFailure::Linux(LinuxError { errno, context }));
    }
    let reply = match tag {
        OPEN => Reply::Opened {
            handle: input.u64()?,
        },
        READ => {
            let length = input.u32()?;
            if length > maximum {
                return Err(protocol());
            }
            Reply::Bytes(input.bytes(length as usize)?.to_vec())
        }

        WRITE => Reply::Written(input.u32()?),
        SEEK => Reply::Offset(input.u64()?),
        STAT => Reply::Stat(ServiceStat {
            mode: input.u32()?,
            uid: input.u32()?,
            gid: input.u32()?,
            size: input.u64()?,
        }),
        POLL => {
            let value = input.u8()?;
            if value & !15 != 0 {
                return Err(protocol());
            }
            let states = [
                (1, ReadyState::Readable),
                (2, ReadyState::Writable),
                (4, ReadyState::Hangup),
                (8, ReadyState::Error),
            ]
            .into_iter()
            .filter_map(|(bit, state)| (value & bit != 0).then_some(state))
            .collect();
            Reply::Ready(Readiness { states })
        }
        CLOSE => Reply::Closed,
        _ => return Err(protocol()),
    };
    input.finish()?;
    Ok(reply)
}
