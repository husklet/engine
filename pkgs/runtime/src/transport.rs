//! Frozen provider transport foundation. This is not a discovered runtime capability yet.

use hl_engine_api::extension::ProviderId;
use hl_engine_provider::Handles;
use std::{
    collections::BTreeMap,
    io::{Read, Write},
    os::unix::net::UnixStream,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};

use hl_engine_protocol::{decode_header, encode_header, HEADER_BYTES};
pub use hl_engine_protocol::{Frame, MessageType, TransportError};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransportLimits {
    pub payload_bytes: u32,
    pub providers: u32,
}

impl Default for TransportLimits {
    fn default() -> Self {
        Self {
            payload_bytes: 1024 * 1024,
            providers: 64,
        }
    }
}

/// One bounded full-duplex provider channel. The owner decides how its socket is inherited.
#[derive(Debug)]
pub struct Channel {
    reader: Mutex<UnixStream>,
    writer: Mutex<UnixStream>,
    limits: TransportLimits,
    next_request: AtomicU64,
}

impl Channel {
    /// Creates the two endpoints before process creation so neither side discovers ambient fds.
    ///
    /// # Errors
    /// Returns [`TransportError::Io`] when the host cannot create the channel.
    pub fn pair(limits: TransportLimits) -> Result<(Self, Self), TransportError> {
        let (left, right) = UnixStream::pair().map_err(|_| TransportError::Io)?;
        Ok((
            Self::from_stream(left, limits)?,
            Self::from_stream(right, limits)?,
        ))
    }

    /// Takes ownership of a socket explicitly inherited by an activation transport.
    ///
    /// # Errors
    /// Returns [`TransportError::Io`] when a separate writer endpoint cannot be duplicated.
    pub fn from_stream(
        stream: UnixStream,
        limits: TransportLimits,
    ) -> Result<Self, TransportError> {
        let writer = stream.try_clone().map_err(|_| TransportError::Io)?;
        Ok(Self {
            reader: Mutex::new(stream),
            writer: Mutex::new(writer),
            limits,
            next_request: AtomicU64::new(1),
        })
    }

    /// Sends one request with an engine-owned id and waits for its correlated reply.
    ///
    /// # Errors
    /// Returns a protocol, bound, deadline, peer-close, or host I/O error.
    pub fn request(
        &self,
        features: u64,
        payload: Vec<u8>,
        deadline: Instant,
    ) -> Result<Frame, TransportError> {
        let request_id = self.next_request.fetch_add(1, Ordering::Relaxed);
        if request_id == 0 {
            return Err(TransportError::Quota);
        }
        self.send(
            &Frame {
                kind: MessageType::Request,
                request_id,
                features,
                payload,
            },
            deadline,
        )?;
        let reply = self.receive(deadline)?;
        if reply.kind != MessageType::Reply || reply.request_id != request_id {
            return Err(TransportError::Malformed);
        }
        Ok(reply)
    }

    /// Sends one copied and bounded frame.
    ///
    /// # Errors
    /// Returns a bound, deadline, peer-close, or host I/O error.
    pub fn send(&self, frame: &Frame, deadline: Instant) -> Result<(), TransportError> {
        if frame.payload.len() > self.limits.payload_bytes as usize {
            return Err(TransportError::Oversized);
        }
        let mut stream = self.writer.lock().map_err(|_| TransportError::Io)?;
        stream
            .set_write_timeout(Some(remaining(deadline)?))
            .map_err(io_error)?;
        let header = encode_header(frame)?;
        write_all(&mut stream, &header)?;
        write_all(&mut stream, &frame.payload)
    }

    /// Receives and validates one complete frame before exposing its payload.
    ///
    /// # Errors
    /// Returns a protocol, bound, deadline, peer-close, or host I/O error.
    pub fn receive(&self, deadline: Instant) -> Result<Frame, TransportError> {
        let mut stream = self.reader.lock().map_err(|_| TransportError::Io)?;
        stream
            .set_read_timeout(Some(remaining(deadline)?))
            .map_err(io_error)?;
        let mut header = [0_u8; HEADER_BYTES];
        read_exact(&mut stream, &mut header)?;
        let (kind, request_id, features, length) = decode_header(&header)?;
        if length > self.limits.payload_bytes {
            return Err(TransportError::Oversized);
        }
        let mut payload = vec![0; length as usize];
        read_exact(&mut stream, &mut payload)?;
        Ok(Frame {
            kind,
            request_id,
            features,
            payload,
        })
    }

    /// Sends cancellation for one outstanding request.
    ///
    /// # Errors
    /// Returns a bound, deadline, peer-close, or host I/O error.
    pub fn cancel(&self, request_id: u64, deadline: Instant) -> Result<(), TransportError> {
        self.send(
            &Frame {
                kind: MessageType::Cancel,
                request_id,
                features: 0,
                payload: Vec::new(),
            },
            deadline,
        )
    }

    /// Performs the hello/ready lifecycle handshake and returns selected feature bits.
    ///
    /// # Errors
    /// Returns a protocol, deadline, peer-close, or host I/O error.
    pub fn handshake(&self, features: u64, deadline: Instant) -> Result<u64, TransportError> {
        self.send(
            &Frame {
                kind: MessageType::Hello,
                request_id: 0,
                features,
                payload: Vec::new(),
            },
            deadline,
        )?;
        let reply = self.receive(deadline)?;
        if reply.kind != MessageType::Ready || reply.request_id != 0 || !reply.payload.is_empty() {
            return Err(TransportError::Malformed);
        }
        Ok(reply.features)
    }

    /// Accepts a child-initiated activation handshake and selects supported feature bits.
    ///
    /// # Errors
    /// Returns a protocol, deadline, peer-close, or host I/O error.
    pub fn accept_handshake(
        &self,
        supported: u64,
        deadline: Instant,
    ) -> Result<u64, TransportError> {
        let hello = self.receive(deadline)?;
        if hello.kind != MessageType::Hello || hello.request_id != 0 || !hello.payload.is_empty() {
            return Err(TransportError::Malformed);
        }
        let selected = hello.features & supported;
        self.send(
            &Frame {
                kind: MessageType::Ready,
                request_id: 0,
                features: selected,
                payload: Vec::new(),
            },
            deadline,
        )?;
        Ok(selected)
    }

    /// Installs one validated launch namespace transaction before guest execution.
    ///
    /// # Errors
    /// Returns a protocol, bound, deadline, peer-close, or host I/O error.
    pub fn install_namespace(
        &self,
        payload: Vec<u8>,
        deadline: Instant,
    ) -> Result<(), TransportError> {
        self.send(
            &Frame {
                kind: MessageType::NamespaceInstall,
                request_id: 0,
                features: 0,
                payload,
            },
            deadline,
        )?;
        let reply = self.receive(deadline)?;
        if reply.kind != MessageType::NamespaceReady
            || reply.request_id != 0
            || reply.features != 0
            || !reply.payload.is_empty()
        {
            return Err(TransportError::Malformed);
        }
        Ok(())
    }
}

/// Launch-scoped mapping from opaque provider identity to handle authority.
#[derive(Default)]

pub struct ProviderRegistry {
    providers: BTreeMap<ProviderId, Arc<dyn Handles>>,
    maximum: u32,
}

impl ProviderRegistry {
    #[must_use]
    pub fn new(maximum: u32) -> Self {
        Self {
            providers: BTreeMap::new(),
            maximum,
        }
    }

    /// Installs one provider authority for this launch.
    ///
    /// # Errors
    /// Returns duplicate-provider or launch-quota errors.
    pub fn register(
        &mut self,
        id: ProviderId,
        handles: Arc<dyn Handles>,
    ) -> Result<(), TransportError> {
        if self.providers.contains_key(&id) {
            return Err(TransportError::DuplicateProvider);
        }
        if self.providers.len() >= self.maximum as usize {
            return Err(TransportError::Quota);
        }
        self.providers.insert(id, handles);
        Ok(())
    }

    #[must_use]
    pub fn get(&self, id: &ProviderId) -> Option<&Arc<dyn Handles>> {
        self.providers.get(id)
    }
}

fn remaining(deadline: Instant) -> Result<Duration, TransportError> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|value| !value.is_zero())
        .ok_or(TransportError::Timeout)
}

fn write_all(stream: &mut UnixStream, bytes: &[u8]) -> Result<(), TransportError> {
    stream.write_all(bytes).map_err(io_error)
}

fn read_exact(stream: &mut UnixStream, bytes: &mut [u8]) -> Result<(), TransportError> {
    stream.read_exact(bytes).map_err(io_error)
}

fn io_error(error: std::io::Error) -> TransportError {
    #[cfg(target_os = "macos")]
    if matches!(error.raw_os_error(), Some(22 | 32 | 54 | 57)) {
        return TransportError::PeerClosed;
    }
    let kind = error.kind();
    drop(error);
    match kind {
        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock => TransportError::Timeout,
        std::io::ErrorKind::UnexpectedEof
        | std::io::ErrorKind::BrokenPipe
        | std::io::ErrorKind::ConnectionReset
        | std::io::ErrorKind::ConnectionAborted
        | std::io::ErrorKind::NotConnected => TransportError::PeerClosed,
        _ => TransportError::Io,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use hl_engine_provider::{LinuxError, OpenRequest};

    fn deadline() -> Instant {
        Instant::now() + Duration::from_secs(1)
    }

    #[test]
    fn frame_round_trip_and_cancellation_preserve_ids_and_features() {
        let (left, right) = Channel::pair(TransportLimits::default()).unwrap();
        left.send(
            &Frame {
                kind: MessageType::Request,
                request_id: 42,
                features: 7,
                payload: b"copied".to_vec(),
            },
            deadline(),
        )
        .unwrap();
        assert_eq!(
            right.receive(deadline()).unwrap(),
            Frame {
                kind: MessageType::Request,
                request_id: 42,
                features: 7,
                payload: b"copied".to_vec(),
            }
        );
        right.cancel(42, deadline()).unwrap();
        assert_eq!(left.receive(deadline()).unwrap().kind, MessageType::Cancel);
    }

    #[test]
    fn timeout_and_peer_close_are_observable() {
        let (left, right) = Channel::pair(TransportLimits::default()).unwrap();
        assert_eq!(
            left.receive(Instant::now() + Duration::from_millis(10))
                .unwrap_err(),
            TransportError::Timeout
        );
        drop((left, right));
        let (left, right) = Channel::pair(TransportLimits::default()).unwrap();
        drop(right);
        assert_eq!(
            left.receive(deadline()).unwrap_err(),
            TransportError::PeerClosed
        );
    }

    #[test]
    fn lifecycle_handshake_negotiates_features() {
        let (left, right) = Channel::pair(TransportLimits::default()).unwrap();
        let server = std::thread::spawn(move || {
            let hello = right.receive(deadline()).unwrap();
            assert_eq!(hello.kind, MessageType::Hello);
            right
                .send(
                    &Frame {
                        kind: MessageType::Ready,
                        request_id: 0,
                        features: hello.features & 0b101,
                        payload: Vec::new(),
                    },
                    deadline(),
                )
                .unwrap();
        });
        assert_eq!(left.handshake(0b111, deadline()).unwrap(), 0b101);
        server.join().unwrap();
    }

    #[test]
    fn provider_accepts_child_initiated_handshake() {
        let (child, provider) = Channel::pair(TransportLimits::default()).unwrap();
        let server =
            std::thread::spawn(move || provider.accept_handshake(0b101, deadline()).unwrap());
        assert_eq!(child.handshake(0b111, deadline()).unwrap(), 0b101);
        assert_eq!(server.join().unwrap(), 0b101);
    }

    #[test]
    fn namespace_install_is_acknowledged_before_requests() {
        let (parent, child) = Channel::pair(TransportLimits::default()).unwrap();
        let server = std::thread::spawn(move || {
            let install = child.receive(deadline()).unwrap();
            assert_eq!(install.kind, MessageType::NamespaceInstall);
            assert_eq!(install.request_id, 0);
            assert_eq!(install.payload, b"transaction");
            child
                .send(
                    &Frame {
                        kind: MessageType::NamespaceReady,
                        request_id: 0,
                        features: 0,
                        payload: vec![],
                    },
                    deadline(),
                )
                .unwrap();
        });
        parent
            .install_namespace(b"transaction".to_vec(), deadline())
            .unwrap();
        server.join().unwrap();
    }

    #[test]
    fn requests_receive_monotonic_engine_owned_ids() {
        let (left, right) = Channel::pair(TransportLimits::default()).unwrap();
        let server = std::thread::spawn(move || {
            for expected in 1..=2 {
                let request = right.receive(deadline()).unwrap();
                assert_eq!(request.request_id, expected);
                right
                    .send(
                        &Frame {
                            kind: MessageType::Reply,
                            request_id: request.request_id,
                            features: 0,
                            payload: request.payload,
                        },
                        deadline(),
                    )
                    .unwrap();
            }
        });
        assert_eq!(
            left.request(0, b"one".to_vec(), deadline())
                .unwrap()
                .payload,
            b"one"
        );
        assert_eq!(
            left.request(0, b"two".to_vec(), deadline())
                .unwrap()
                .payload,
            b"two"
        );
        server.join().unwrap();
    }

    struct HandlesStub;
    impl Handles for HandlesStub {
        fn open(
            &self,
            _request: OpenRequest,
        ) -> Result<Box<dyn hl_engine_provider::OpenHandle>, LinuxError> {
            Err(LinuxError {
                errno: 95,
                context: "not wired".into(),
            })
        }
    }

    #[test]
    fn provider_registry_enforces_identity_and_launch_quota() {
        let first = ProviderId::new("first").unwrap();
        let second = ProviderId::new("second").unwrap();
        let handles: Arc<dyn Handles> = Arc::new(HandlesStub);
        let mut registry = ProviderRegistry::new(1);
        registry.register(first.clone(), handles.clone()).unwrap();
        assert!(registry.get(&first).is_some());
        assert_eq!(
            registry.register(first, handles.clone()).unwrap_err(),
            TransportError::DuplicateProvider
        );
        assert_eq!(
            registry.register(second, handles).unwrap_err(),
            TransportError::Quota
        );
    }
}
