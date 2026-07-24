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

mod descriptor;
mod provider;
mod server;

#[cfg(test)]
use crate::protocol::{decode_reply, encode_request};
pub(crate) use crate::protocol::{
    decode_request, encode_namespace_install, encode_reply, ProjectionKind, Reply, Request,
    SeekWhence, ServiceFailure, ServiceProjection, ServiceStat,
};
#[cfg(test)]
use descriptor::Descriptors;
pub(crate) use provider::*;
pub(crate) use server::*;

pub(crate) trait ServiceTransport: Send + Sync {
    fn request(
        &self,
        id: u64,
        request: Request,
        deadline: Instant,
    ) -> Result<Reply, ServiceFailure>;
    fn cancel(&self, id: u64);
}

fn system_deadline(deadline: Instant) -> SystemTime {
    SystemTime::now() + deadline.saturating_duration_since(Instant::now())
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
