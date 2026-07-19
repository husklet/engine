use super::{
    linux, protocol, Arc, AtomicU64, BTreeMap, Duration, Instant, Interest, Mutex, Ordering,
    Readiness, Reply, Request, SeekWhence, ServiceFailure, ServiceId, ServiceStat,
    ServiceTransport, TransportError,
};
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
