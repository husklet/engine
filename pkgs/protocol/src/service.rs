use crate::TransportError;
use hl_engine_api::extension::ServiceId;
use hl_engine_provider::{Interest, LinuxError, Readiness, ReadyState};
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

const OPEN: u8 = 1;
const READ: u8 = 2;
pub const WRITE: u8 = 3;
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

pub fn put_u16(out: &mut Vec<u8>, value: u16) {
    out.extend(value.to_le_bytes());
}

pub fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend(value.to_le_bytes());
}

fn put_i32(out: &mut Vec<u8>, value: i32) {
    out.extend(value.to_le_bytes());
}

pub fn put_u64(out: &mut Vec<u8>, value: u64) {
    out.extend(value.to_le_bytes());
}

fn put_i64(out: &mut Vec<u8>, value: i64) {
    out.extend(value.to_le_bytes());
}

pub struct Input<'a> {
    bytes: &'a [u8],
    offset: usize,
}
impl<'a> Input<'a> {
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }
    pub fn bytes(&mut self, count: usize) -> Result<&'a [u8], ServiceFailure> {
        let end = self.offset.checked_add(count).ok_or_else(protocol)?;

        let value = self.bytes.get(self.offset..end).ok_or_else(protocol)?;
        self.offset = end;
        Ok(value)
    }

    fn u8(&mut self) -> Result<u8, ServiceFailure> {
        Ok(self.bytes(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16, ServiceFailure> {
        Ok(u16::from_le_bytes(
            self.bytes(2)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub fn u32(&mut self) -> Result<u32, ServiceFailure> {
        Ok(u32::from_le_bytes(
            self.bytes(4)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn i32(&mut self) -> Result<i32, ServiceFailure> {
        Ok(i32::from_le_bytes(
            self.bytes(4)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub fn u64(&mut self) -> Result<u64, ServiceFailure> {
        Ok(u64::from_le_bytes(
            self.bytes(8)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn i64(&mut self) -> Result<i64, ServiceFailure> {
        Ok(i64::from_le_bytes(
            self.bytes(8)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub fn finish(self) -> Result<(), ServiceFailure> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err(protocol())
        }
    }
}

fn linux(errno: i32, context: &str) -> ServiceFailure {
    ServiceFailure::Linux(LinuxError {
        errno,
        context: context.into(),
    })
}
fn protocol() -> ServiceFailure {
    ServiceFailure::Transport(TransportError::Malformed)
}
#[derive(Clone, Debug, Eq, PartialEq)]

pub struct ServiceProjection {
    pub path: std::path::PathBuf,
    pub service: ServiceId,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
}

/// Encodes one validated namespace installation transaction.
///
/// # Errors
/// Returns a typed failure for conflicts, invalid paths, or exceeded limits.
pub fn encode_namespace_install(
    entries: &[ServiceProjection],
    maximum_entries: u32,
    maximum_path: u32,
) -> Result<Vec<u8>, ServiceFailure> {
    validate_projections(entries, maximum_entries, maximum_path)?;
    let mut out = Vec::new();
    put_u32(
        &mut out,
        u32::try_from(entries.len()).map_err(|_| protocol())?,
    );
    for entry in entries {
        use std::os::unix::ffi::OsStrExt;
        let path = entry.path.as_os_str().as_bytes();
        put_u64(&mut out, entry.service.0);
        put_u32(&mut out, entry.mode);
        put_u32(&mut out, entry.uid);
        put_u32(&mut out, entry.gid);
        put_u16(&mut out, u16::try_from(path.len()).map_err(|_| protocol())?);
        out.extend(path);
    }
    Ok(out)
}

/// Decodes and validates one namespace installation transaction.
///
/// # Errors
/// Returns a typed failure for malformed input, conflicts, invalid paths, or exceeded limits.
pub fn decode_namespace_install(
    bytes: &[u8],
    maximum_entries: u32,
    maximum_path: u32,
) -> Result<Vec<ServiceProjection>, ServiceFailure> {
    use std::os::unix::ffi::OsStringExt;
    let mut input = Input::new(bytes);
    let count = input.u32()?;
    if count > maximum_entries {
        return Err(linux(7, "service projection count exceeds launch bound"));
    }
    let mut entries = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let service = ServiceId(input.u64()?);
        let mode = input.u32()?;
        let uid = input.u32()?;
        let gid = input.u32()?;
        let length = u32::from(input.u16()?);
        if length == 0 || length > maximum_path {
            return Err(linux(36, "service projection path exceeds launch bound"));
        }
        let path = std::ffi::OsString::from_vec(input.bytes(length as usize)?.to_vec()).into();
        entries.push(ServiceProjection {
            path,
            service,
            mode,
            uid,
            gid,
        });
    }
    input.finish()?;
    validate_projections(&entries, maximum_entries, maximum_path)?;
    Ok(entries)
}

fn validate_projections(
    entries: &[ServiceProjection],
    maximum_entries: u32,
    maximum_path: u32,
) -> Result<(), ServiceFailure> {
    use std::os::unix::ffi::OsStrExt;
    if entries.len() > maximum_entries as usize {
        return Err(linux(7, "service projection count exceeds launch bound"));
    }
    let mut paths = std::collections::BTreeSet::new();
    for entry in entries {
        let path = entry.path.as_os_str().as_bytes();
        if entry.service.0 == 0
            || entry.mode & !0o7777 != 0
            || path.is_empty()
            || path == b"/"
            || path.len() > maximum_path as usize
            || path.len() > u16::MAX as usize
            || path.contains(&0)
            || !entry.path.is_absolute()
            || entry.path.components().any(|component| {
                matches!(
                    component,
                    std::path::Component::ParentDir | std::path::Component::CurDir
                )
            })
            || !paths.insert(entry.path.clone())
        {
            return Err(linux(22, "invalid or conflicting service projection"));
        }
    }
    for path in &paths {
        if path
            .ancestors()
            .skip(1)
            .any(|ancestor| ancestor != std::path::Path::new("/") && paths.contains(ancestor))
        {
            return Err(linux(20, "service projection cannot contain descendants"));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frozen_codec_round_trips_requests_replies_and_errno() {
        let request = Request::Write {
            handle: 7,
            offset: 11,
            bytes: b"owned".to_vec(),
        };
        let encoded = encode_request(&request, 16).unwrap();
        assert_eq!(
            encoded,
            [
                vec![WRITE],
                7_u64.to_le_bytes().to_vec(),
                11_u64.to_le_bytes().to_vec(),
                5_u32.to_le_bytes().to_vec(),
                b"owned".to_vec(),
            ]
            .concat()
        );
        assert_eq!(decode_request(&encoded, 16).unwrap(), request);

        let ready = Reply::Ready(Readiness {
            states: [ReadyState::Readable, ReadyState::Hangup]
                .into_iter()
                .collect(),
        });
        let encoded = encode_reply(&Ok(ready.clone()), 16).unwrap();
        assert_eq!(decode_reply(&encoded, 16).unwrap(), ready);

        let failure = ServiceFailure::Linux(LinuxError {
            errno: 19,
            context: "provider unavailable".into(),
        });
        let encoded = encode_reply(&Err(failure.clone()), 64).unwrap();
        assert_eq!(decode_reply(&encoded, 64).unwrap_err(), failure);
    }

    #[test]
    fn codecs_reject_trailing_invalid_and_oversized_payloads() {
        let mut request = encode_request(&Request::Close { handle: 1 }, 8).unwrap();
        request.push(0);
        assert_eq!(decode_request(&request, 8).unwrap_err(), protocol());
        assert_eq!(decode_request(&[WRITE], 8).unwrap_err(), protocol());
        assert!(matches!(
            encode_request(
                &Request::Write {
                    handle: 1,
                    offset: 0,
                    bytes: vec![0; 9],
                },
                8,
            ),
            Err(ServiceFailure::Linux(LinuxError { errno: 22, .. }))
        ));
    }

    #[test]
    fn namespace_codec_enforces_transaction_bounds_and_normalization() {
        let entries = vec![ServiceProjection {
            path: "/run/provider".into(),
            service: ServiceId(9),
            mode: 0o660,
            uid: 10,
            gid: 20,
        }];
        let wire = encode_namespace_install(&entries, 4, 128).unwrap();
        assert_eq!(decode_namespace_install(&wire, 4, 128).unwrap(), entries);

        let conflicts = vec![
            ServiceProjection {
                path: "/run/provider".into(),
                service: ServiceId(1),
                mode: 0o600,
                uid: 0,
                gid: 0,
            },
            ServiceProjection {
                path: "/run/provider/child".into(),
                service: ServiceId(2),
                mode: 0o600,
                uid: 0,
                gid: 0,
            },
        ];
        assert!(matches!(
            encode_namespace_install(&conflicts, 4, 128),
            Err(ServiceFailure::Linux(LinuxError { errno: 20, .. }))
        ));

        let mut trailing = wire;
        trailing.push(0);
        assert_eq!(
            decode_namespace_install(&trailing, 4, 128).unwrap_err(),
            protocol()
        );
    }
}
