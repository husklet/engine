use super::*;
use crate::{
    extension::{Interest, LinuxError, Readiness, ReadyState, ServiceId},
    protocol::{Reply, Request, SeekWhence, ServiceFailure, ServiceStat, TransportError},
    service::ServiceTransport,
};
use std::{
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};

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
                    .map_err(|_| ServiceFailure::linux(22, "offset exceeds host range"))?;
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
                    .map_err(|_| ServiceFailure::linux(22, "offset exceeds host range"))?;
                if bytes.len() < start + input.len() {
                    bytes.resize(start + input.len(), 0);
                }
                bytes[start..start + input.len()].copy_from_slice(&input);
                Reply::Written(
                    u32::try_from(input.len())
                        .map_err(|_| ServiceFailure::linux(22, "write exceeds protocol range"))?,
                )
            }
            Request::Seek { offset, whence, .. } => {
                let base = match whence {
                    SeekWhence::Start | SeekWhence::Current => 0,
                    SeekWhence::End => i64::try_from(bytes.len())
                        .map_err(|_| ServiceFailure::linux(75, "size exceeds seek range"))?,
                };
                Reply::Offset(
                    u64::try_from(base + offset)
                        .map_err(|_| ServiceFailure::linux(22, "negative seek"))?,
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
            Request::Ioctl { argument, .. } => Reply::Ioctl {
                value: 0,
                argument,
                writes: Vec::new(),
            },
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
