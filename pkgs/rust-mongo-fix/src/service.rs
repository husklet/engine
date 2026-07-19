use crate::{
    extension::{
        Credentials, HandleOperation, Handles, Interest, LinuxError, OpenAccess, OpenRequest,
        ReadRequest, Readiness, SeekOrigin, SeekRequest, ServiceId, ServiceRegistration,
        WriteRequest,
    },
    transport::{Channel, Frame, MessageType, TransportError},
};
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant, SystemTime},
};

#[derive(Clone, Debug, Eq, PartialEq)]

pub(crate) struct ServiceProjection {
    pub(crate) path: std::path::PathBuf,
    pub(crate) service: ServiceId,
    pub(crate) mode: u32,
    pub(crate) uid: u32,
    pub(crate) gid: u32,
}

pub(crate) fn encode_namespace_install(
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

pub(crate) fn decode_namespace_install(
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

#[derive(Clone, Debug, Eq, PartialEq)]

pub(crate) enum Request {
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
pub(crate) enum SeekWhence {
    Start,
    Current,
    End,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) enum Reply {
    Opened { handle: u64 },
    Bytes(Vec<u8>),
    Written(u32),
    Offset(u64),
    Stat(ServiceStat),
    Ready(Readiness),
    Closed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct ServiceStat {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
}

pub(crate) trait ServiceTransport: Send + Sync {
    fn request(
        &self,
        id: u64,
        request: Request,
        deadline: Instant,
    ) -> Result<Reply, ServiceFailure>;
    fn cancel(&self, id: u64);
}

#[derive(Clone, Debug, Eq, PartialEq)]

pub(crate) enum ServiceFailure {
    Linux(LinuxError),
    Transport(TransportError),
}

pub(crate) struct ProviderDispatcher {
    provider: Arc<dyn Handles>,
    services: BTreeMap<ServiceId, std::collections::BTreeSet<HandleOperation>>,
    credentials: Credentials,
    handles: Mutex<BTreeMap<u64, ActiveHandle>>,
    next_handle: AtomicU64,
    maximum_handles: u32,
    maximum_request: u32,
}

struct ActiveHandle {
    value: Box<dyn crate::extension::OpenHandle>,
    operations: std::collections::BTreeSet<HandleOperation>,
}

impl ProviderDispatcher {
    pub(crate) fn new(
        provider: Arc<dyn Handles>,
        services: &[ServiceRegistration],
        credentials: Credentials,
        maximum_handles: u32,
        maximum_request: u32,
    ) -> Self {
        Self {
            provider,
            services: services
                .iter()
                .map(|service| (service.id, service.operations.clone()))
                .collect(),
            credentials,
            handles: Mutex::new(BTreeMap::new()),
            next_handle: AtomicU64::new(1),
            maximum_handles,
            maximum_request,
        }
    }

    pub(crate) fn dispatch(
        &self,
        payload: &[u8],
        deadline: Instant,
    ) -> Result<Vec<u8>, ServiceFailure> {
        if Instant::now() >= deadline {
            return Err(ServiceFailure::Transport(TransportError::Timeout));
        }
        let request = match decode_request(payload, self.maximum_request) {
            Ok(request) => request,
            Err(error @ ServiceFailure::Linux(_)) => {
                return encode_reply(&Err(error), self.maximum_request)
            }
            Err(error) => return Err(error),
        };
        let reply = self.execute(request, deadline);
        encode_reply(&reply, self.maximum_request)
    }

    #[allow(clippy::too_many_lines)]
    fn execute(&self, request: Request, deadline: Instant) -> Result<Reply, ServiceFailure> {
        if Instant::now() >= deadline {
            return Err(ServiceFailure::Transport(TransportError::Timeout));
        }
        match request {
            Request::Open {
                service,
                read,
                write,
            } => {
                let operations = self
                    .services
                    .get(&service)
                    .ok_or_else(|| linux(19, "service is outside this launch authority"))?
                    .clone();
                if (read && !operations.contains(&HandleOperation::Read))
                    || (write && !operations.contains(&HandleOperation::Write))
                {
                    return Err(linux(13, "service access exceeds declared operations"));
                }
                let mut handles = self.handles.lock().map_err(|_| protocol())?;
                if handles.len() >= self.maximum_handles as usize {
                    return Err(linux(24, "provider handle quota exhausted"));
                }
                let handle = self
                    .provider
                    .open(OpenRequest {
                        service,
                        access: OpenAccess {
                            read,
                            write,
                            nonblocking: false,
                        },
                        credentials: self.credentials.clone(),
                        deadline: system_deadline(deadline),
                    })
                    .map_err(ServiceFailure::Linux)?;
                let id = self.next_handle.fetch_add(1, Ordering::Relaxed);
                if id == 0
                    || handles
                        .insert(
                            id,
                            ActiveHandle {
                                value: handle,
                                operations,
                            },
                        )
                        .is_some()
                {
                    return Err(ServiceFailure::Transport(TransportError::Quota));
                }
                Ok(Reply::Opened { handle: id })
            }
            Request::Close { handle } => {
                let value = self
                    .handles
                    .lock()
                    .map_err(|_| protocol())?
                    .remove(&handle)
                    .ok_or_else(|| linux(9, "unknown provider handle"))?;
                value.value.close();
                Ok(Reply::Closed)
            }
            request => {
                let handles = self.handles.lock().map_err(|_| protocol())?;
                let handle = handles
                    .get(&request.handle())
                    .ok_or_else(|| linux(9, "unknown provider handle"))?;
                match request {
                    Request::Read { offset, length, .. } => {
                        require(&handle.operations, HandleOperation::Read)?;
                        if offset != u64::MAX {
                            require(&handle.operations, HandleOperation::PositionedIo)?;
                        }
                        handle
                            .value
                            .read(ReadRequest {
                                offset: (offset != u64::MAX).then_some(offset),
                                length,
                                deadline: system_deadline(deadline),
                            })
                            .map(Reply::Bytes)
                            .map_err(ServiceFailure::Linux)
                    }
                    Request::Write { offset, bytes, .. } => {
                        require(&handle.operations, HandleOperation::Write)?;
                        if offset != u64::MAX {
                            require(&handle.operations, HandleOperation::PositionedIo)?;
                        }
                        handle
                            .value
                            .write(WriteRequest {
                                offset: (offset != u64::MAX).then_some(offset),
                                bytes,
                                deadline: system_deadline(deadline),
                            })
                            .and_then(|written| {
                                u32::try_from(written).map_err(|_| LinuxError {
                                    errno: 75,
                                    context: "provider write result exceeds protocol range".into(),
                                })
                            })
                            .map(Reply::Written)
                            .map_err(ServiceFailure::Linux)
                    }
                    Request::Seek { offset, whence, .. } => {
                        require(&handle.operations, HandleOperation::Seek)?;
                        handle
                            .value
                            .seek(SeekRequest {
                                offset,
                                origin: match whence {
                                    SeekWhence::Start => SeekOrigin::Start,
                                    SeekWhence::Current => SeekOrigin::Current,
                                    SeekWhence::End => SeekOrigin::End,
                                },
                            })
                            .map(Reply::Offset)
                            .map_err(ServiceFailure::Linux)
                    }
                    Request::Stat { .. } => {
                        require(&handle.operations, HandleOperation::Metadata)?;
                        handle
                            .value
                            .metadata()
                            .map(|value| {
                                Reply::Stat(ServiceStat {
                                    mode: value.mode,
                                    uid: value.uid,
                                    gid: value.gid,
                                    size: value.size,
                                })
                            })
                            .map_err(ServiceFailure::Linux)
                    }
                    Request::Poll { interest, .. } => {
                        require(&handle.operations, HandleOperation::Poll)?;
                        handle
                            .value
                            .readiness(interest)
                            .map(Reply::Ready)
                            .map_err(ServiceFailure::Linux)
                    }
                    Request::Open { .. } | Request::Close { .. } => unreachable!(),
                }
            }
        }
    }

    pub(crate) fn close_all(&self) {
        if let Ok(mut handles) = self.handles.lock() {
            for (_, handle) in std::mem::take(&mut *handles) {
                handle.value.close();
            }
        }
    }
}

impl Drop for ProviderDispatcher {
    fn drop(&mut self) {
        if let Ok(handles) = self.handles.get_mut() {
            for (_, handle) in std::mem::take(handles) {
                handle.value.close();
            }
        }
    }
}

impl Request {
    fn handle(&self) -> u64 {
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

fn system_deadline(deadline: Instant) -> SystemTime {
    SystemTime::now() + deadline.saturating_duration_since(Instant::now())
}

pub(crate) struct ServiceServer {
    channel: Arc<Channel>,
    dispatcher: Arc<ProviderDispatcher>,
    active: Arc<Mutex<BTreeMap<u64, Arc<AtomicBool>>>>,
    maximum_active: u32,
    request_timeout: Duration,
}

impl ServiceServer {
    pub(crate) fn new(
        channel: Arc<Channel>,
        dispatcher: Arc<ProviderDispatcher>,
        maximum_active: u32,
        request_timeout: Duration,
    ) -> Self {
        Self {
            channel,
            dispatcher,
            active: Arc::new(Mutex::new(BTreeMap::new())),
            maximum_active,
            request_timeout,
        }
    }

    pub(crate) fn run(&self, deadline: Instant) -> Result<(), TransportError> {
        loop {
            let frame = match self.channel.receive(deadline) {
                Ok(frame) => frame,
                Err(TransportError::PeerClosed) => {
                    self.cancel_all();
                    self.dispatcher.close_all();
                    return Ok(());
                }
                Err(error) => return Err(error),
            };
            match frame.kind {
                MessageType::Request | MessageType::Subscribe => self.start(frame)?,
                MessageType::Cancel | MessageType::Unsubscribe => self.cancel(frame.request_id)?,
                MessageType::Close => {
                    self.cancel_all();
                    self.dispatcher.close_all();
                    return Ok(());
                }
                _ => return Err(TransportError::Malformed),
            }
        }
    }

    fn start(&self, frame: Frame) -> Result<(), TransportError> {
        if frame.request_id == 0 {
            return Err(TransportError::Malformed);
        }
        let reply_kind = if frame.kind == MessageType::Subscribe {
            MessageType::ReadinessEvent
        } else {
            MessageType::Reply
        };
        let cancelled = Arc::new(AtomicBool::new(false));
        {
            let mut active = self.active.lock().map_err(|_| TransportError::Io)?;
            if active.len() >= self.maximum_active as usize {
                return Err(TransportError::Quota);
            }
            if active.insert(frame.request_id, cancelled.clone()).is_some() {
                return Err(TransportError::Malformed);
            }
        }
        let channel = self.channel.clone();
        let dispatcher = self.dispatcher.clone();
        let active = self.active.clone();
        let timeout = self.request_timeout;
        std::thread::spawn(move || {
            let deadline = Instant::now() + timeout;
            let reply = dispatcher.dispatch(&frame.payload, deadline);
            let was_cancelled = cancelled.load(Ordering::Acquire);
            if let Ok(mut values) = active.lock() {
                values.remove(&frame.request_id);
            }
            if !was_cancelled {
                if let Ok(payload) = reply {
                    let _ = channel.send(
                        &Frame {
                            kind: reply_kind,
                            request_id: frame.request_id,
                            features: 0,
                            payload,
                        },
                        deadline,
                    );
                }
            }
        });
        Ok(())
    }

    fn cancel(&self, request_id: u64) -> Result<(), TransportError> {
        if request_id == 0 {
            return Err(TransportError::Malformed);
        }
        if let Some(cancelled) = self
            .active
            .lock()
            .map_err(|_| TransportError::Io)?
            .get(&request_id)
        {
            cancelled.store(true, Ordering::Release);
        }
        Ok(())
    }

    fn cancel_all(&self) {
        if let Ok(active) = self.active.lock() {
            for cancelled in active.values() {
                cancelled.store(true, Ordering::Release);
            }
        }
    }
}

const OPEN: u8 = 1;
const READ: u8 = 2;
const WRITE: u8 = 3;
const SEEK: u8 = 4;
const STAT: u8 = 5;
const POLL: u8 = 6;
const CLOSE: u8 = 7;
const ERROR: u8 = 0xff;

pub(crate) fn encode_request(request: &Request, maximum: u32) -> Result<Vec<u8>, ServiceFailure> {
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

pub(crate) fn decode_request(bytes: &[u8], maximum: u32) -> Result<Request, ServiceFailure> {
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

pub(crate) fn encode_reply(
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
                        crate::extension::ReadyState::Readable => 1,
                        crate::extension::ReadyState::Writable => 2,
                        crate::extension::ReadyState::Hangup => 4,
                        crate::extension::ReadyState::Error => 8,
                    };
                }
                out.push(states);
            }
            Reply::Closed => out.push(CLOSE),
        },
    }
    Ok(out)
}

pub(crate) fn decode_reply(bytes: &[u8], maximum: u32) -> Result<Reply, ServiceFailure> {
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
                (1, crate::extension::ReadyState::Readable),
                (2, crate::extension::ReadyState::Writable),
                (4, crate::extension::ReadyState::Hangup),
                (8, crate::extension::ReadyState::Error),
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

fn put_u16(out: &mut Vec<u8>, value: u16) {
    out.extend(value.to_le_bytes());
}

fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend(value.to_le_bytes());
}

fn put_i32(out: &mut Vec<u8>, value: i32) {
    out.extend(value.to_le_bytes());
}

fn put_u64(out: &mut Vec<u8>, value: u64) {
    out.extend(value.to_le_bytes());
}

fn put_i64(out: &mut Vec<u8>, value: i64) {
    out.extend(value.to_le_bytes());
}

struct Input<'a> {
    bytes: &'a [u8],
    offset: usize,
}
impl<'a> Input<'a> {
    const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }
    fn bytes(&mut self, count: usize) -> Result<&'a [u8], ServiceFailure> {
        let end = self.offset.checked_add(count).ok_or_else(protocol)?;

        let value = self.bytes.get(self.offset..end).ok_or_else(protocol)?;
        self.offset = end;
        Ok(value)
    }

    fn u8(&mut self) -> Result<u8, ServiceFailure> {
        Ok(self.bytes(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, ServiceFailure> {
        Ok(u16::from_le_bytes(
            self.bytes(2)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn u32(&mut self) -> Result<u32, ServiceFailure> {
        Ok(u32::from_le_bytes(
            self.bytes(4)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn i32(&mut self) -> Result<i32, ServiceFailure> {
        Ok(i32::from_le_bytes(
            self.bytes(4)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn u64(&mut self) -> Result<u64, ServiceFailure> {
        Ok(u64::from_le_bytes(
            self.bytes(8)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn i64(&mut self) -> Result<i64, ServiceFailure> {
        Ok(i64::from_le_bytes(
            self.bytes(8)?.try_into().map_err(|_| protocol())?,
        ))
    }
    fn finish(self) -> Result<(), ServiceFailure> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err(protocol())
        }
    }
}

struct Description {
    handle: u64,
    offset: Mutex<u64>,
    transport: Arc<dyn ServiceTransport>,
    requests: Arc<AtomicU64>,
    maximum: u32,
}

impl Description {
    fn call(&self, request: Request, deadline: Instant) -> Result<Reply, ServiceFailure> {
        let id = self.requests.fetch_add(1, Ordering::Relaxed);
        let result = self.transport.request(id, request, deadline);
        if matches!(
            result,
            Err(ServiceFailure::Transport(TransportError::Timeout))
        ) {
            self.transport.cancel(id);
        }
        result
    }
}

impl Drop for Description {
    fn drop(&mut self) {
        let _ = self.call(
            Request::Close {
                handle: self.handle,
            },
            Instant::now() + Duration::from_secs(1),
        );
    }
}

#[derive(Clone)]

struct Descriptor {
    description: Arc<Description>,
    cloexec: bool,
}

pub(crate) struct Descriptors {
    values: BTreeMap<i32, Descriptor>,
    next: i32,
    requests: Arc<AtomicU64>,
    maximum_open: u32,
    maximum_request: u32,
}

impl Descriptors {
    pub(crate) fn new(maximum_descriptors: u32, maximum_request: u32) -> Self {
        Self {
            values: BTreeMap::new(),
            next: 3,
            requests: Arc::new(AtomicU64::new(1)),
            maximum_open: maximum_descriptors,
            maximum_request,
        }
    }

    pub(crate) fn open(
        &mut self,
        transport: Arc<dyn ServiceTransport>,
        service: ServiceId,
        read: bool,
        write: bool,
        cloexec: bool,
        deadline: Instant,
    ) -> Result<i32, ServiceFailure> {
        if self.values.len() >= self.maximum_open as usize {
            return Err(linux(24, "service descriptor quota exhausted"));
        }
        let id = self.requests.fetch_add(1, Ordering::Relaxed);
        let reply = transport.request(
            id,
            Request::Open {
                service,
                read,
                write,
            },
            deadline,
        );
        if matches!(
            reply,
            Err(ServiceFailure::Transport(TransportError::Timeout))
        ) {
            transport.cancel(id);
        }
        let reply = reply?;
        let Reply::Opened { handle } = reply else {
            return Err(protocol());
        };
        let fd = self.allocate()?;
        self.values.insert(
            fd,
            Descriptor {
                description: Arc::new(Description {
                    handle,
                    offset: Mutex::new(0),
                    transport,
                    requests: self.requests.clone(),
                    maximum: self.maximum_request,
                }),
                cloexec,
            },
        );
        Ok(fd)
    }

    pub(crate) fn dup(&mut self, fd: i32, cloexec: bool) -> Result<i32, ServiceFailure> {
        let description = self.get(fd)?.description.clone();
        let duplicate = self.allocate()?;
        self.values.insert(
            duplicate,
            Descriptor {
                description,
                cloexec,
            },
        );
        Ok(duplicate)
    }

    pub(crate) fn fork(&self) -> Self {
        Self {
            values: self.values.clone(),
            next: self.next,
            requests: self.requests.clone(),
            maximum_open: self.maximum_open,
            maximum_request: self.maximum_request,
        }
    }

    pub(crate) fn exec(&mut self) {
        self.values.retain(|_, descriptor| !descriptor.cloexec);
    }
    pub(crate) fn close(&mut self, fd: i32) -> Result<(), ServiceFailure> {
        self.values
            .remove(&fd)
            .map(|_| ())
            .ok_or_else(|| linux(9, "bad service descriptor"))
    }

    pub(crate) fn read(
        &self,
        fd: i32,
        length: u32,
        offset: Option<u64>,
        deadline: Instant,
    ) -> Result<Vec<u8>, ServiceFailure> {
        let description = &self.get(fd)?.description;
        if length > description.maximum {
            return Err(linux(22, "service read exceeds request bound"));
        }
        let at = offset.unwrap_or(*description.offset.lock().map_err(|_| protocol())?);
        let Reply::Bytes(bytes) = description.call(
            Request::Read {
                handle: description.handle,
                offset: at,
                length,
            },
            deadline,
        )?
        else {
            return Err(protocol());
        };
        if bytes.len() > length as usize {
            return Err(protocol());
        }
        if offset.is_none() {
            *description.offset.lock().map_err(|_| protocol())? =
                at.saturating_add(bytes.len() as u64);
        }
        Ok(bytes)
    }

    pub(crate) fn write(
        &self,
        fd: i32,
        bytes: &[u8],
        offset: Option<u64>,
        deadline: Instant,
    ) -> Result<u32, ServiceFailure> {
        let description = &self.get(fd)?.description;
        if bytes.len() > description.maximum as usize {
            return Err(linux(22, "service write exceeds request bound"));
        }
        let at = offset.unwrap_or(*description.offset.lock().map_err(|_| protocol())?);
        let Reply::Written(written) = description.call(
            Request::Write {
                handle: description.handle,
                offset: at,
                bytes: bytes.to_vec(),
            },
            deadline,
        )?
        else {
            return Err(protocol());
        };
        if written as usize > bytes.len() {
            return Err(protocol());
        }
        if offset.is_none() {
            *description.offset.lock().map_err(|_| protocol())? =
                at.saturating_add(u64::from(written));
        }
        Ok(written)
    }

    pub(crate) fn seek(
        &self,
        fd: i32,
        offset: i64,
        whence: SeekWhence,
        deadline: Instant,
    ) -> Result<u64, ServiceFailure> {
        let description = &self.get(fd)?.description;
        let Reply::Offset(value) = description.call(
            Request::Seek {
                handle: description.handle,
                offset,
                whence,
            },
            deadline,
        )?
        else {
            return Err(protocol());
        };
        *description.offset.lock().map_err(|_| protocol())? = value;
        Ok(value)
    }

    pub(crate) fn stat(&self, fd: i32, deadline: Instant) -> Result<ServiceStat, ServiceFailure> {
        let description = &self.get(fd)?.description;
        let Reply::Stat(value) = description.call(
            Request::Stat {
                handle: description.handle,
            },
            deadline,
        )?
        else {
            return Err(protocol());
        };
        Ok(value)
    }

    pub(crate) fn poll(
        &self,
        fd: i32,
        interest: Interest,
        deadline: Instant,
    ) -> Result<Readiness, ServiceFailure> {
        let description = &self.get(fd)?.description;
        let Reply::Ready(value) = description.call(
            Request::Poll {
                handle: description.handle,
                interest,
            },
            deadline,
        )?
        else {
            return Err(protocol());
        };
        Ok(value)
    }

    fn get(&self, fd: i32) -> Result<&Descriptor, ServiceFailure> {
        self.values
            .get(&fd)
            .ok_or_else(|| linux(9, "bad service descriptor"))
    }
    fn allocate(&mut self) -> Result<i32, ServiceFailure> {
        while self.values.contains_key(&self.next) {
            self.next = self
                .next
                .checked_add(1)
                .ok_or_else(|| linux(24, "descriptor range exhausted"))?;
        }
        let fd = self.next;
        self.next = self
            .next
            .checked_add(1)
            .ok_or_else(|| linux(24, "descriptor range exhausted"))?;
        Ok(fd)
    }
}

fn linux(errno: i32, context: &str) -> ServiceFailure {
    ServiceFailure::Linux(LinuxError {
        errno,
        context: context.into(),
    })
}
fn require(
    operations: &std::collections::BTreeSet<HandleOperation>,
    operation: HandleOperation,
) -> Result<(), ServiceFailure> {
    if operations.contains(&operation) {
        Ok(())
    } else {
        Err(linux(
            95,
            "service operation was not granted for this launch",
        ))
    }
}
fn protocol() -> ServiceFailure {
    ServiceFailure::Transport(TransportError::Malformed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::extension::{Readiness, ReadyState};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::time::Duration;

    struct LoopbackHandles {
        bytes: Arc<Mutex<Vec<u8>>>,
        closes: Arc<AtomicUsize>,
        delay: Duration,
    }

    struct LoopbackHandle {
        bytes: Arc<Mutex<Vec<u8>>>,
        closes: Arc<AtomicUsize>,
        delay: Duration,
    }

    impl Handles for LoopbackHandles {
        fn open(
            &self,
            request: OpenRequest,
        ) -> Result<Box<dyn crate::extension::OpenHandle>, LinuxError> {
            if request.service != ServiceId(77) {
                return Err(LinuxError {
                    errno: 19,
                    context: "unknown loopback service".into(),
                });
            }
            Ok(Box::new(LoopbackHandle {
                bytes: self.bytes.clone(),
                closes: self.closes.clone(),
                delay: self.delay,
            }))
        }
    }

    fn registrations() -> Vec<ServiceRegistration> {
        vec![ServiceRegistration {
            id: ServiceId(77),
            operations: std::collections::BTreeSet::from([
                HandleOperation::Read,
                HandleOperation::Write,
                HandleOperation::PositionedIo,
                HandleOperation::Seek,
                HandleOperation::Metadata,
                HandleOperation::Poll,
            ]),
            max_request_bytes: 1024,
        }]
    }

    impl crate::extension::OpenHandle for LoopbackHandle {
        fn read(&self, request: ReadRequest) -> Result<Vec<u8>, LinuxError> {
            std::thread::sleep(self.delay);
            let bytes = self.bytes.lock().unwrap();
            let start = usize::try_from(request.offset.unwrap_or(0)).map_err(|_| LinuxError {
                errno: 22,
                context: "offset".into(),
            })?;
            Ok(bytes
                .get(
                    start
                        ..start
                            .saturating_add(request.length as usize)
                            .min(bytes.len()),
                )
                .unwrap_or(&[])
                .to_vec())
        }
        fn write(&self, request: WriteRequest) -> Result<usize, LinuxError> {
            let mut bytes = self.bytes.lock().unwrap();
            let start = usize::try_from(request.offset.unwrap_or(0)).map_err(|_| LinuxError {
                errno: 22,
                context: "offset".into(),
            })?;
            let length = bytes.len().max(start.saturating_add(request.bytes.len()));
            bytes.resize(length, 0);
            bytes[start..start + request.bytes.len()].copy_from_slice(&request.bytes);
            Ok(request.bytes.len())
        }
        fn seek(&self, request: SeekRequest) -> Result<u64, LinuxError> {
            let base = match request.origin {
                SeekOrigin::Start | SeekOrigin::Current => 0,
                SeekOrigin::End => i64::try_from(self.bytes.lock().unwrap().len()).unwrap(),
            };
            u64::try_from(base + request.offset).map_err(|_| LinuxError {
                errno: 22,
                context: "negative offset".into(),
            })
        }
        fn metadata(&self) -> Result<crate::extension::HandleMetadata, LinuxError> {
            Ok(crate::extension::HandleMetadata {
                mode: 0o660,
                uid: 10,
                gid: 20,
                size: self.bytes.lock().unwrap().len() as u64,
            })
        }
        fn readiness(&self, _interest: Interest) -> Result<Readiness, LinuxError> {
            Ok(Readiness {
                states: [ReadyState::Readable, ReadyState::Writable]
                    .into_iter()
                    .collect(),
            })
        }
        fn flush(&self) -> Result<(), LinuxError> {
            Ok(())
        }
        fn close(self: Box<Self>) {
            self.closes.fetch_add(1, Ordering::Relaxed);
        }
    }

    struct Mock {
        bytes: Mutex<Vec<u8>>,
        closes: AtomicUsize,
        cancels: AtomicUsize,
        timeout: Mutex<bool>,
    }

    impl Mock {
        fn new() -> Self {
            Self {
                bytes: Mutex::new(b"abcdef".to_vec()),
                closes: AtomicUsize::new(0),
                cancels: AtomicUsize::new(0),
                timeout: Mutex::new(false),
            }
        }
    }

    impl ServiceTransport for Mock {
        fn request(
            &self,
            _id: u64,
            request: Request,
            _deadline: Instant,
        ) -> Result<Reply, ServiceFailure> {
            if *self.timeout.lock().unwrap() {
                return Err(ServiceFailure::Transport(TransportError::Timeout));
            }
            let mut bytes = self.bytes.lock().unwrap();
            Ok(match request {
                Request::Open { .. } => Reply::Opened { handle: 9 },
                Request::Read { offset, length, .. } => {
                    let start = usize::try_from(offset)
                        .map_err(|_| linux(22, "offset exceeds host range"))?;
                    Reply::Bytes(
                        bytes
                            .get(start..start.saturating_add(length as usize).min(bytes.len()))
                            .unwrap_or(&[])
                            .to_vec(),
                    )
                }
                Request::Write {
                    offset,
                    bytes: input,
                    ..
                } => {
                    let start = usize::try_from(offset)
                        .map_err(|_| linux(22, "offset exceeds host range"))?;
                    if bytes.len() < start + input.len() {
                        bytes.resize(start + input.len(), 0);
                    }
                    bytes[start..start + input.len()].copy_from_slice(&input);
                    Reply::Written(
                        u32::try_from(input.len())
                            .map_err(|_| linux(22, "write exceeds protocol range"))?,
                    )
                }
                Request::Seek { offset, whence, .. } => {
                    let base = match whence {
                        SeekWhence::Start | SeekWhence::Current => 0,
                        SeekWhence::End => i64::try_from(bytes.len())
                            .map_err(|_| linux(75, "size exceeds seek range"))?,
                    };
                    Reply::Offset(
                        u64::try_from(base + offset).map_err(|_| linux(22, "negative seek"))?,
                    )
                }
                Request::Stat { .. } => Reply::Stat(ServiceStat {
                    mode: 0o660,
                    uid: 1,
                    gid: 2,
                    size: bytes.len() as u64,
                }),
                Request::Poll { .. } => Reply::Ready(Readiness {
                    states: [ReadyState::Readable, ReadyState::Writable]
                        .into_iter()
                        .collect(),
                }),
                Request::Close { .. } => {
                    self.closes.fetch_add(1, Ordering::Relaxed);
                    Reply::Closed
                }
            })
        }

        fn cancel(&self, _id: u64) {
            self.cancels.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn deadline() -> Instant {
        Instant::now() + Duration::from_secs(1)
    }

    #[test]
    fn frozen_codec_roundtrips_owned_requests_replies_and_errno() {
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
                b"owned".to_vec()
            ]
            .concat()
        );
        assert_eq!(decode_request(&encoded, 16).unwrap(), request);

        let expected = Reply::Ready(Readiness {
            states: [ReadyState::Readable, ReadyState::Hangup]
                .into_iter()
                .collect(),
        });
        let reply = Ok(expected.clone());
        let encoded = encode_reply(&reply, 16).unwrap();
        assert_eq!(decode_reply(&encoded, 16).unwrap(), expected);

        let expected = ServiceFailure::Linux(LinuxError {
            errno: 19,
            context: "provider unavailable".into(),
        });
        let error = Err(expected.clone());
        let encoded = encode_reply(&error, 64).unwrap();
        assert_eq!(decode_reply(&encoded, 64).unwrap_err(), expected);
    }

    #[test]

    fn frozen_codec_rejects_trailing_invalid_and_oversized_payloads() {
        let mut request = encode_request(&Request::Close { handle: 1 }, 8).unwrap();
        request.push(0);
        assert_eq!(decode_request(&request, 8).unwrap_err(), protocol());
        assert!(matches!(
            decode_request(&[WRITE], 8),
            Err(ServiceFailure::Transport(TransportError::Malformed))
        ));
        assert!(matches!(
            encode_request(
                &Request::Write {
                    handle: 1,
                    offset: 0,
                    bytes: vec![0; 9]
                },
                8
            ),
            Err(ServiceFailure::Linux(LinuxError { errno: 22, .. }))
        ));
    }

    #[test]

    fn namespace_install_is_bounded_transactional_and_normalized() {
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
        let escaped = vec![ServiceProjection {
            path: "/run/../escape".into(),
            service: ServiceId(1),
            mode: 0o600,
            uid: 0,
            gid: 0,
        }];
        assert!(matches!(
            encode_namespace_install(&escaped, 4, 128),
            Err(ServiceFailure::Linux(LinuxError { errno: 22, .. }))
        ));
        let mut trailing = wire;
        trailing.push(0);
        assert_eq!(
            decode_namespace_install(&trailing, 4, 128).unwrap_err(),
            protocol()
        );
    }

    #[test]

    fn parent_dispatcher_owns_handles_enforces_quota_and_cleans_up() {
        let bytes = Arc::new(Mutex::new(b"abc".to_vec()));
        let closes = Arc::new(AtomicUsize::new(0));
        let dispatcher = ProviderDispatcher::new(
            Arc::new(LoopbackHandles {
                bytes,
                closes: closes.clone(),
                delay: Duration::ZERO,
            }),
            &registrations(),
            Credentials {
                uid: 10,
                gid: 20,
                groups: vec![20],
            },
            1,
            16,
        );
        let deadline = deadline();
        let open = encode_request(
            &Request::Open {
                service: ServiceId(77),
                read: true,
                write: true,
            },
            16,
        )
        .unwrap();
        let Reply::Opened { handle } =
            decode_reply(&dispatcher.dispatch(&open, deadline).unwrap(), 16).unwrap()
        else {
            panic!("open reply")
        };
        let second = dispatcher.dispatch(&open, deadline).unwrap();
        assert!(matches!(
            decode_reply(&second, 16),
            Err(ServiceFailure::Linux(LinuxError { errno: 24, .. }))
        ));
        let write = encode_request(
            &Request::Write {
                handle,
                offset: 1,
                bytes: b"XY".to_vec(),
            },
            16,
        )
        .unwrap();
        assert_eq!(
            decode_reply(&dispatcher.dispatch(&write, deadline).unwrap(), 16).unwrap(),
            Reply::Written(2)
        );
        let stat = encode_request(&Request::Stat { handle }, 16).unwrap();
        assert_eq!(
            decode_reply(&dispatcher.dispatch(&stat, deadline).unwrap(), 16).unwrap(),
            Reply::Stat(ServiceStat {
                mode: 0o660,
                uid: 10,
                gid: 20,
                size: 3
            })
        );
        assert_eq!(
            dispatcher.dispatch(&stat, Instant::now()).unwrap_err(),
            ServiceFailure::Transport(TransportError::Timeout)
        );
        drop(dispatcher);
        assert_eq!(closes.load(Ordering::Relaxed), 1);
    }

    fn assert_readiness_subscription(client: &Channel, handle: u64) {
        client
            .send(
                &Frame {
                    kind: MessageType::Subscribe,
                    request_id: 42,
                    features: 0,
                    payload: encode_request(
                        &Request::Poll {
                            handle,
                            interest: Interest {
                                readable: true,
                                writable: false,
                                priority: false,
                            },
                        },
                        16,
                    )
                    .unwrap(),
                },
                deadline(),
            )
            .unwrap();
        let event = client.receive(deadline()).unwrap();
        assert_eq!(event.kind, MessageType::ReadinessEvent);
        assert_eq!(event.request_id, 42);
        assert!(matches!(
            decode_reply(&event.payload, 16).unwrap(),
            Reply::Ready(_)
        ));
        client
            .send(
                &Frame {
                    kind: MessageType::Unsubscribe,
                    request_id: 42,
                    features: 0,
                    payload: vec![],
                },
                deadline(),
            )
            .unwrap();
    }

    #[test]
    fn concurrent_server_correlates_requests_and_revokes_handles_on_close() {
        let bytes = Arc::new(Mutex::new(b"abc".to_vec()));
        let closes = Arc::new(AtomicUsize::new(0));
        let dispatcher = Arc::new(ProviderDispatcher::new(
            Arc::new(LoopbackHandles {
                bytes,
                closes: closes.clone(),
                delay: Duration::ZERO,
            }),
            &registrations(),
            Credentials {
                uid: 10,
                gid: 20,
                groups: vec![],
            },
            2,
            16,
        ));
        let (server_channel, client) =
            Channel::pair(crate::transport::TransportLimits::default()).unwrap();
        let server = ServiceServer::new(
            Arc::new(server_channel),
            dispatcher,
            2,
            Duration::from_secs(1),
        );
        let thread =
            std::thread::spawn(move || server.run(Instant::now() + Duration::from_secs(2)));
        client
            .send(
                &Frame {
                    kind: MessageType::Request,
                    request_id: 41,
                    features: 0,
                    payload: encode_request(
                        &Request::Open {
                            service: ServiceId(77),
                            read: true,
                            write: true,
                        },
                        16,
                    )
                    .unwrap(),
                },
                deadline(),
            )
            .unwrap();
        let opened = client.receive(deadline()).unwrap();
        assert_eq!(opened.request_id, 41);
        let Reply::Opened { handle } = decode_reply(&opened.payload, 16).unwrap() else {
            panic!("open reply was not an owned provider handle")
        };
        assert_readiness_subscription(&client, handle);
        client
            .send(
                &Frame {
                    kind: MessageType::Close,
                    request_id: 0,
                    features: 0,
                    payload: vec![],
                },
                deadline(),
            )
            .unwrap();
        thread.join().unwrap().unwrap();
        assert_eq!(closes.load(Ordering::Relaxed), 1);
    }

    #[test]

    fn concurrent_server_observes_cancel_while_provider_request_runs() {
        let dispatcher = Arc::new(ProviderDispatcher::new(
            Arc::new(LoopbackHandles {
                bytes: Arc::new(Mutex::new(b"abc".to_vec())),
                closes: Arc::new(AtomicUsize::new(0)),
                delay: Duration::from_millis(100),
            }),
            &registrations(),
            Credentials {
                uid: 0,
                gid: 0,
                groups: vec![],
            },
            2,
            16,
        ));
        let (server_channel, client) =
            Channel::pair(crate::transport::TransportLimits::default()).unwrap();
        let server = ServiceServer::new(
            Arc::new(server_channel),
            dispatcher,
            2,
            Duration::from_secs(1),
        );
        let thread =
            std::thread::spawn(move || server.run(Instant::now() + Duration::from_secs(2)));
        client
            .send(
                &Frame {
                    kind: MessageType::Request,
                    request_id: 1,
                    features: 0,
                    payload: encode_request(
                        &Request::Open {
                            service: ServiceId(77),
                            read: true,
                            write: false,
                        },
                        16,
                    )
                    .unwrap(),
                },
                deadline(),
            )
            .unwrap();
        let Reply::Opened { handle } =
            decode_reply(&client.receive(deadline()).unwrap().payload, 16).unwrap()
        else {
            panic!("open reply")
        };
        client
            .send(
                &Frame {
                    kind: MessageType::Request,
                    request_id: 2,
                    features: 0,
                    payload: encode_request(
                        &Request::Read {
                            handle,
                            offset: 0,
                            length: 3,
                        },
                        16,
                    )
                    .unwrap(),
                },
                deadline(),
            )
            .unwrap();
        client.cancel(2, deadline()).unwrap();
        std::thread::sleep(Duration::from_millis(150));
        assert_eq!(
            client
                .receive(Instant::now() + Duration::from_millis(20))
                .unwrap_err(),
            TransportError::Timeout
        );
        client
            .send(
                &Frame {
                    kind: MessageType::Close,
                    request_id: 0,
                    features: 0,
                    payload: vec![],
                },
                deadline(),
            )
            .unwrap();
        thread.join().unwrap().unwrap();
    }

    #[test]

    fn dup_and_fork_share_offset_and_close_exactly_once() {
        let mock = Arc::new(Mock::new());
        let mut table = Descriptors::new(16, 1024);
        let fd = table
            .open(mock.clone(), ServiceId(1), true, true, false, deadline())
            .unwrap();
        let duplicate = table.dup(fd, false).unwrap();
        assert_eq!(table.read(fd, 2, None, deadline()).unwrap(), b"ab");
        assert_eq!(table.read(duplicate, 2, None, deadline()).unwrap(), b"cd");
        let mut child = table.fork();
        assert_eq!(child.read(fd, 2, None, deadline()).unwrap(), b"ef");
        table.close(fd).unwrap();
        table.close(duplicate).unwrap();
        assert_eq!(mock.closes.load(Ordering::Relaxed), 0);
        child.close(fd).unwrap();
        child.close(duplicate).unwrap();
        assert_eq!(mock.closes.load(Ordering::Relaxed), 1);
    }

    #[test]

    fn positioned_io_stat_poll_bounds_cloexec_and_timeout_are_typed() {
        let mock = Arc::new(Mock::new());
        let mut table = Descriptors::new(4, 4);
        let fd = table
            .open(mock.clone(), ServiceId(2), true, true, true, deadline())
            .unwrap();
        assert_eq!(table.write(fd, b"XY", Some(1), deadline()).unwrap(), 2);
        assert_eq!(table.read(fd, 3, Some(0), deadline()).unwrap(), b"aXY");
        assert_eq!(table.stat(fd, deadline()).unwrap().size, 6);
        assert!(table
            .poll(
                fd,
                Interest {
                    readable: true,
                    writable: true,
                    priority: false
                },
                deadline()
            )
            .unwrap()
            .states
            .contains(&ReadyState::Readable));
        assert!(matches!(
            table.write(fd, b"oversized", None, deadline()),
            Err(ServiceFailure::Linux(LinuxError { errno: 22, .. }))
        ));
        *mock.timeout.lock().unwrap() = true;
        assert_eq!(
            table.read(fd, 1, None, deadline()).unwrap_err(),
            ServiceFailure::Transport(TransportError::Timeout)
        );
        assert_eq!(mock.cancels.load(Ordering::Relaxed), 1);
        *mock.timeout.lock().unwrap() = false;
        table.exec();
        assert!(matches!(
            table.read(fd, 1, None, deadline()),
            Err(ServiceFailure::Linux(LinuxError { errno: 9, .. }))
        ));
        assert_eq!(mock.closes.load(Ordering::Relaxed), 1);
    }
}
