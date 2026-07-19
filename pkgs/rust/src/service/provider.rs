use super::{
    decode_request, encode_reply, linux, protocol, require, system_deadline, Arc, AtomicU64,
    BTreeMap, Credentials, HandleOperation, Handles, Instant, LinuxError, Mutex, OpenAccess,
    OpenRequest, Ordering, ReadRequest, Reply, Request, SeekOrigin, SeekRequest, SeekWhence,
    ServiceFailure, ServiceId, ServiceRegistration, ServiceStat, TransportError, WriteRequest,
};
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
