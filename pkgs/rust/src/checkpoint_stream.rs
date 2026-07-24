//! Checkpoint capture and restore through a caller-supplied store.
//!
//! A checkpoint is normally a directory tree written by the engine. That hard-wires *where* a checkpoint
//! lives. This module lets the embedder decide instead: implement [`CheckpointStore`], hand it to
//! [`crate::MachineSpec`], and every byte of the image is handed to you as named objects — an S3 bucket, an
//! encrypted blob, a database row, a pipe. Nothing touches the filesystem.
//!
//! # Why there is a server here
//!
//! The engine does not run in this process. Activation re-executes a separate engine executable and every
//! guest process is a further `fork()` of it, so at capture time the bytes are produced by N separate host
//! processes. A trait object cannot be handed across that boundary, so each engine process opens a private
//! channel to this crate and marshals the sink operations over it (`include/hl/checkpoint_stream.h`). The
//! server below accepts those channels, runs one thread per engine process, and replays the operations onto
//! your [`CheckpointStore`]. It is the only participant that sees all of them, so it also owns the parts of
//! the format that used to be properties of a directory:
//!
//! * **staging and atomicity** — an object is buffered until the engine finishes it, and the objects of one
//!   process image are buffered until that image is committed. Your store only ever sees complete objects,
//!   and only ever sees a process image all at once.
//! * **claim arbitration** — several engine processes can see the same pipe or socketpair and exactly one of
//!   them must record it. The filesystem elected that writer with `O_EXCL`; here it is a table in the server.
//! * **rendezvous** — the coordinator asks "has that peer finished?", which is answered from the set of
//!   committed images rather than by looking for a directory entry.
//! * **the image digest** — accumulated as objects are finished, never by re-reading the store.
//!
//! # Failure
//!
//! Returning an error from any [`CheckpointStore`] method fails the capture. The engine process that was
//! writing aborts its object and its whole process image, exits non-zero, and the coordinator refuses to
//! publish a manifest — so [`crate::Machine::checkpoint`] returns an error and **no commit ever happens**.
//! Nothing is rolled back: whatever partial writes your store accepted are yours to discard. That is the same
//! contract the directory format has always had.

use std::{
    collections::{BTreeMap, HashMap, HashSet},
    io::{Read, Write},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};

/// Selects the streaming sink/source instead of a workspace directory. Mirrors `HL_CKPT_STREAM_SENTINEL`.
pub(crate) const SENTINEL: &str = "@hl-checkpoint-stream";

const ABI: u32 = 1;
const MAGIC_REQUEST: u32 = 0x484b_4351;
const MAGIC_REPLY: u32 = 0x484b_4353;
const NAME_MAX: usize = 512;
const PAYLOAD_MAX: usize = 4 * 1024 * 1024;
const REQUEST_BYTES: usize = 48;
const REPLY_BYTES: usize = 32;

const STATUS_OK: i32 = 0;
const STATUS_ERROR: i32 = -1;
const STATUS_ALREADY: i32 = 1;

const OP_OBJECT_BEGIN: u32 = 1;
const OP_OBJECT_WRITE: u32 = 2;
const OP_OBJECT_WRITE_AT: u32 = 3;
const OP_OBJECT_TELL: u32 = 4;
const OP_OBJECT_FINISH: u32 = 5;
const OP_OBJECT_ABORT: u32 = 6;
const OP_GROUP_BEGIN: u32 = 7;
const OP_GROUP_COMMIT: u32 = 8;
const OP_GROUP_ABORT: u32 = 9;
const OP_CLAIM: u32 = 10;
const OP_UNCLAIM: u32 = 11;
const OP_COMMIT: u32 = 12;
const OP_GROUP_PRESENT: u32 = 13;
const OP_GROUP_COUNT: u32 = 14;
const OP_DIGEST: u32 = 15;
const OP_SOURCE_LIST: u32 = 16;
const OP_SOURCE_SIZE: u32 = 17;
const OP_SOURCE_READ: u32 = 18;

const HASH_BASIS: u64 = 14_695_981_039_346_656_037;
const HASH_PRIME: u64 = 1_099_511_628_211;

/// Why a store operation failed. The engine only distinguishes "worked" from "did not".
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StoreError {
    /// Human-readable cause, surfaced in the capture error.
    pub message: String,
}

impl StoreError {
    /// Builds a store error from any message.
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl std::fmt::Display for StoreError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for StoreError {}

/// Where checkpoint bytes live.
///
/// Objects are named with `/`-separated names: workspace-level records such as `pipe.<id>`, and per-process
/// images such as `proc.7/pages`. The names are opaque — treat them as keys.
///
/// Implementations are used from several threads at once (one per engine process), so they must be `Sync`.
pub trait CheckpointStore: Send + Sync {
    /// Stores one complete object. Called once per object, never partially.
    ///
    /// # Errors
    /// Any error fails the capture; see the module documentation.
    fn put(&self, name: &str, data: &[u8]) -> Result<(), StoreError>;

    /// Reads one object back, for restore.
    ///
    /// # Errors
    /// Returns an error when the object does not exist or cannot be read.
    fn get(&self, name: &str) -> Result<Vec<u8>, StoreError>;

    /// Every object name in the image, for restore.
    ///
    /// # Errors
    /// Returns an error when the store cannot be enumerated.
    fn list(&self) -> Result<Vec<String>, StoreError>;

    /// Completes the image. Called exactly once, last, after every object has been stored.
    ///
    /// The manifest is what makes the image restorable: until `commit` returns, the checkpoint is not a
    /// checkpoint. The default stores it as the object named `MANIFEST`.
    ///
    /// # Errors
    /// Any error fails the capture.
    fn commit(&self, manifest: &[u8]) -> Result<(), StoreError> {
        self.put("MANIFEST", manifest)
    }
}

/// A [`CheckpointStore`] that keeps the whole image in memory. Useful for tests and for callers that want to
/// hand the finished image to something else (encrypt it, upload it) in one piece.
#[derive(Debug, Default)]
pub struct MemoryStore {
    objects: Mutex<BTreeMap<String, Vec<u8>>>,
}

impl MemoryStore {
    /// An empty store.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The captured objects, by name.
    ///
    /// # Panics
    /// Panics if a previous store operation panicked while holding the lock.
    #[must_use]
    pub fn objects(&self) -> BTreeMap<String, Vec<u8>> {
        self.objects.lock().expect("memory store lock").clone()
    }

    /// Whether a complete image was committed.
    #[must_use]
    pub fn committed(&self) -> bool {
        self.objects
            .lock()
            .expect("memory store lock")
            .contains_key("MANIFEST")
    }

    /// Total captured bytes.
    #[must_use]
    pub fn bytes(&self) -> usize {
        self.objects
            .lock()
            .expect("memory store lock")
            .values()
            .map(Vec::len)
            .sum()
    }
}

impl CheckpointStore for MemoryStore {
    fn put(&self, name: &str, data: &[u8]) -> Result<(), StoreError> {
        self.objects
            .lock()
            .map_err(|_| StoreError::new("memory store lock is poisoned"))?
            .insert(name.to_owned(), data.to_vec());
        Ok(())
    }

    fn get(&self, name: &str) -> Result<Vec<u8>, StoreError> {
        self.objects
            .lock()
            .map_err(|_| StoreError::new("memory store lock is poisoned"))?
            .get(name)
            .cloned()
            .ok_or_else(|| StoreError::new(format!("no such object: {name}")))
    }

    fn list(&self) -> Result<Vec<String>, StoreError> {
        Ok(self
            .objects
            .lock()
            .map_err(|_| StoreError::new("memory store lock is poisoned"))?
            .keys()
            .cloned()
            .collect())
    }
}

// ---------------------------------------------------------------------------------------- digest

fn hash_bytes(mut hash: u64, data: &[u8]) -> u64 {
    for byte in data {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(HASH_PRIME);
    }
    hash
}

/// The per-object hash: name, then length, then contents. Identical to `ckpt_hash_object` in the engine.
fn object_hash(name: &str, data: &[u8]) -> u64 {
    let mut hash = hash_bytes(HASH_BASIS, name.as_bytes());
    hash = hash_bytes(hash, &[0]);
    hash = hash_bytes(hash, &(data.len() as u64).to_ne_bytes());
    hash_bytes(hash, data)
}

/// The image hash: the per-object hashes folded in ascending name order. Identical to the engine's
/// `ckpt_hash_combine` loop, and computable without ever re-reading a stored object.
fn image_digest(objects: &BTreeMap<String, (u64, u64)>) -> (u64, u64, u64) {
    let mut hash = HASH_BASIS;
    let mut bytes = 0_u64;
    for (name, (object, size)) in objects {
        hash = hash_bytes(hash, name.as_bytes());
        hash = hash_bytes(hash, &[0]);
        hash = hash_bytes(hash, &object.to_ne_bytes());
        bytes += *size;
    }
    (hash, objects.len() as u64, bytes)
}

/// Objects the digest never covers: the manifest authenticates the rest and cannot cover itself, and the
/// restore-side recovery journal is written after the image is already complete.
fn digested(name: &str) -> bool {
    name != "MANIFEST" && name != "RECOVERY.jsonl" && !name.starts_with(".RECOVERY.jsonl.tmp.")
}

// ---------------------------------------------------------------------------------------- server

#[derive(Debug)]
struct Object {
    name: String,
    bytes: Vec<u8>,
}

/// Everything the server knows that is not in the embedder's store. Guarded by one lock: capture is not a
/// throughput-critical path, and one lock makes the ordering guarantees obvious.
#[derive(Default)]
struct State {
    open: HashMap<(u64, u64), Object>,
    /// Objects finished inside a group, held until the group is committed.
    staged: HashMap<String, Vec<Object>>,
    committed_groups: HashSet<String>,
    claims: HashSet<String>,
    /// (name -> (object hash, size)) for every object handed to the store.
    digest: BTreeMap<String, (u64, u64)>,
    failure: Option<String>,
}

/// The demultiplexing server. One per launch; owns the broker and one thread per engine process.
pub(crate) struct SinkServer {
    store: Arc<dyn CheckpointStore>,
    state: Mutex<State>,
    committed: AtomicBool,
    running: AtomicBool,
}

impl std::fmt::Debug for SinkServer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("SinkServer")
    }
}

impl SinkServer {
    pub(crate) fn new(store: Arc<dyn CheckpointStore>) -> Self {
        Self {
            store,
            state: Mutex::new(State::default()),
            committed: AtomicBool::new(false),
            running: AtomicBool::new(true),
        }
    }

    pub(crate) fn committed(&self) -> bool {
        self.committed.load(Ordering::SeqCst)
    }

    pub(crate) fn stop(&self) {
        self.running.store(false, Ordering::SeqCst);
    }

    pub(crate) fn failure(&self) -> Option<String> {
        self.state
            .lock()
            .ok()
            .and_then(|state| state.failure.clone())
    }

    fn record_failure(&self, message: String) {
        if let Ok(mut state) = self.state.lock() {
            if state.failure.is_none() {
                state.failure = Some(message);
            }
        }
    }

    /// Hands one finished object to the embedder and folds it into the digest.
    fn publish(&self, object: &Object) -> Result<(), StoreError> {
        self.store.put(&object.name, &object.bytes)?;
        if digested(&object.name) {
            if let Ok(mut state) = self.state.lock() {
                state.digest.insert(
                    object.name.clone(),
                    (
                        object_hash(&object.name, &object.bytes),
                        object.bytes.len() as u64,
                    ),
                );
            }
        }
        Ok(())
    }

    /// The digest of an image being READ back. Restore recomputes it to authenticate the manifest, and the
    /// store is the only place the objects exist, so it is computed from the store rather than from the
    /// capture-time accumulator (which belongs to a different process's lifetime).
    fn stored_digest(&self) -> Result<(u64, u64, u64), StoreError> {
        let mut objects = BTreeMap::new();
        for name in self.store.list()? {
            if !digested(&name) {
                continue;
            }
            let bytes = self.store.get(&name)?;
            objects.insert(
                name.clone(),
                (object_hash(&name, &bytes), bytes.len() as u64),
            );
        }
        Ok(image_digest(&objects))
    }

    /// Serves one engine process until it closes its channel.
    fn serve(self: &Arc<Self>, channel: &mut std::os::unix::net::UnixStream, id: u64) {
        loop {
            let mut header = [0_u8; REQUEST_BYTES];
            match channel.read_exact(&mut header) {
                Ok(()) => {}
                Err(_) => return, // the engine process exited; that is the normal end of a channel
            }
            let request = match Request::decode(&header) {
                Some(request) => request,
                None => {
                    self.record_failure("checkpoint channel framing is invalid".into());
                    return;
                }
            };
            let mut name = vec![0_u8; request.name_size];
            if channel.read_exact(&mut name).is_err() {
                return;
            }
            let name = String::from_utf8_lossy(name.split_last().map_or(&[][..], |(_, rest)| rest))
                .into_owned();
            let mut payload = Vec::new();
            if request.carries_payload() {
                payload = vec![0_u8; request.length as usize];
                if channel.read_exact(&mut payload).is_err() {
                    return;
                }
            }
            let reply = self.dispatch(id, &request, &name, payload);
            if reply.write(channel).is_err() {
                return;
            }
        }
    }

    #[allow(clippy::too_many_lines)]
    fn dispatch(&self, id: u64, request: &Request, name: &str, payload: Vec<u8>) -> Reply {
        let key = (id, request.stream);
        match request.op {
            OP_OBJECT_BEGIN => {
                if name.len() > NAME_MAX {
                    return Reply::error();
                }
                let mut state = match self.state.lock() {
                    Ok(state) => state,
                    Err(_) => return Reply::error(),
                };
                state.open.insert(
                    key,
                    Object {
                        name: name.to_owned(),
                        bytes: Vec::new(),
                    },
                );
                Reply::ok()
            }
            OP_OBJECT_WRITE | OP_OBJECT_WRITE_AT => {
                let Ok(mut state) = self.state.lock() else {
                    return Reply::error();
                };
                let Some(object) = state.open.get_mut(&key) else {
                    return Reply::error();
                };
                if request.op == OP_OBJECT_WRITE {
                    object.bytes.extend_from_slice(&payload);
                } else {
                    let offset = request.offset as usize;
                    let end = offset + payload.len();
                    if object.bytes.len() < end {
                        object.bytes.resize(end, 0);
                    }
                    object.bytes[offset..end].copy_from_slice(&payload);
                }
                Reply::ok()
            }
            OP_OBJECT_TELL => {
                let Ok(state) = self.state.lock() else {
                    return Reply::error();
                };
                state.open.get(&key).map_or_else(Reply::error, |object| {
                    Reply::value(object.bytes.len() as u64)
                })
            }
            OP_OBJECT_FINISH => {
                let object = {
                    let Ok(mut state) = self.state.lock() else {
                        return Reply::error();
                    };
                    match state.open.remove(&key) {
                        Some(object) => object,
                        None => return Reply::error(),
                    }
                };
                // A group member is not visible until the group commits; a workspace-level object is
                // visible as soon as it is finished. Group membership is the object name's prefix.
                let group = object
                    .name
                    .split_once('/')
                    .map(|(group, _)| group.to_owned());
                if let Some(group) = group {
                    let Ok(mut state) = self.state.lock() else {
                        return Reply::error();
                    };
                    if state.staged.contains_key(&group) {
                        state.staged.entry(group).or_default().push(object);
                        return Reply::ok();
                    }
                }
                match self.publish(&object) {
                    Ok(()) => Reply::ok(),
                    Err(error) => {
                        self.record_failure(format!("store rejected {}: {error}", object.name));
                        Reply::error()
                    }
                }
            }
            OP_OBJECT_ABORT => {
                if let Ok(mut state) = self.state.lock() {
                    state.open.remove(&key);
                }
                Reply::ok()
            }
            OP_GROUP_BEGIN => {
                let Ok(mut state) = self.state.lock() else {
                    return Reply::error();
                };
                state.staged.insert(name.to_owned(), Vec::new());
                Reply::ok()
            }
            OP_GROUP_COMMIT => {
                let staged = {
                    let Ok(mut state) = self.state.lock() else {
                        return Reply::error();
                    };
                    state.staged.remove(name).unwrap_or_default()
                };
                for object in &staged {
                    if let Err(error) = self.publish(object) {
                        self.record_failure(format!("store rejected {}: {error}", object.name));
                        return Reply::error();
                    }
                }
                if let Ok(mut state) = self.state.lock() {
                    state.committed_groups.insert(name.to_owned());
                }
                Reply::ok()
            }
            OP_GROUP_ABORT => {
                if let Ok(mut state) = self.state.lock() {
                    state.staged.remove(name);
                }
                Reply::ok()
            }
            OP_CLAIM => {
                let Ok(mut state) = self.state.lock() else {
                    return Reply::error();
                };
                if state.claims.insert(name.to_owned()) {
                    Reply::ok()
                } else {
                    Reply::status(STATUS_ALREADY)
                }
            }
            OP_UNCLAIM => {
                if let Ok(mut state) = self.state.lock() {
                    state.claims.remove(name);
                }
                Reply::ok()
            }
            OP_GROUP_PRESENT => {
                let Ok(state) = self.state.lock() else {
                    return Reply::error();
                };
                Reply::value(u64::from(state.committed_groups.contains(name)))
            }
            OP_GROUP_COUNT => {
                let Ok(state) = self.state.lock() else {
                    return Reply::error();
                };
                Reply::value(
                    state
                        .committed_groups
                        .iter()
                        .filter(|group| group.starts_with(name))
                        .count() as u64,
                )
            }
            OP_DIGEST => {
                let digest = {
                    let Ok(state) = self.state.lock() else {
                        return Reply::error();
                    };
                    if state.digest.is_empty() {
                        None
                    } else {
                        Some(image_digest(&state.digest))
                    }
                };
                let digest = match digest {
                    Some(digest) => digest,
                    // Nothing was captured in this process's lifetime: this is a restore, so the digest is
                    // the one the stored image actually has.
                    None => match self.stored_digest() {
                        Ok(digest) => digest,
                        Err(_) => return Reply::error(),
                    },
                };
                let mut bytes = Vec::with_capacity(24);
                bytes.extend_from_slice(&digest.0.to_ne_bytes());
                bytes.extend_from_slice(&digest.1.to_ne_bytes());
                bytes.extend_from_slice(&digest.2.to_ne_bytes());
                Reply::payload(bytes)
            }
            OP_COMMIT => match self.store.commit(&payload) {
                Ok(()) => {
                    self.committed.store(true, Ordering::SeqCst);
                    Reply::ok()
                }
                Err(error) => {
                    self.record_failure(format!("store rejected the manifest: {error}"));
                    Reply::error()
                }
            },
            OP_SOURCE_LIST => {
                let Ok(names) = self.store.list() else {
                    return Reply::error();
                };
                // Restore enumerates by top-level name; group members are reported as their group, once.
                let mut seen = Vec::new();
                for full in names {
                    let entry = full.split_once('/').map_or(full.as_str(), |(head, _)| head);
                    if entry.starts_with(name) && !seen.iter().any(|held| held == entry) {
                        seen.push(entry.to_owned());
                    }
                }
                let mut bytes = Vec::new();
                for entry in &seen {
                    bytes.extend_from_slice(entry.as_bytes());
                    bytes.push(0);
                }
                let count = seen.len() as u64;
                Reply {
                    status: STATUS_OK,
                    value: count,
                    payload: bytes,
                }
            }
            OP_SOURCE_SIZE => match self.store.get(name) {
                Ok(bytes) => Reply::value(bytes.len() as u64),
                Err(_) => Reply::status(STATUS_ALREADY), // absent is not a failure
            },
            OP_SOURCE_READ => {
                let Ok(bytes) = self.store.get(name) else {
                    return Reply::error();
                };
                let offset = usize::try_from(request.offset).unwrap_or(usize::MAX);
                if offset >= bytes.len() {
                    return Reply::payload(Vec::new());
                }
                let length = usize::try_from(request.length)
                    .unwrap_or(0)
                    .min(PAYLOAD_MAX);
                let end = offset.saturating_add(length).min(bytes.len());
                Reply::payload(bytes[offset..end].to_vec())
            }
            _ => Reply::error(),
        }
    }
}

/// Runs the acceptor: every engine process that announces itself gets a thread.
pub(crate) fn serve(
    server: &Arc<SinkServer>,
    broker: std::os::unix::net::UnixDatagram,
) -> std::thread::JoinHandle<()> {
    let server = Arc::clone(server);
    std::thread::spawn(move || {
        let mut workers = Vec::new();
        while server.running.load(Ordering::SeqCst) {
            let Some(mut channel) =
                crate::ffi::broker_accept(&broker, std::time::Duration::from_millis(50))
            else {
                continue;
            };
            let worker = Arc::clone(&server);
            let id = workers.len() as u64 + 1;
            workers.push(std::thread::spawn(move || {
                worker.serve(&mut channel, id);
            }));
        }
        for worker in workers {
            let _ = worker.join();
        }
    })
}

// ---------------------------------------------------------------------------------------- codec

#[derive(Debug)]
struct Request {
    op: u32,
    stream: u64,
    offset: u64,
    length: u64,
    name_size: usize,
}

impl Request {
    fn decode(bytes: &[u8; REQUEST_BYTES]) -> Option<Self> {
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

    /// `length` is the number of bytes that follow, except for `SOURCE_READ`, where it is how many bytes the
    /// engine wants back.
    const fn carries_payload(&self) -> bool {
        self.length != 0 && self.op != OP_SOURCE_READ
    }
}

#[derive(Debug)]
struct Reply {
    status: i32,
    value: u64,
    payload: Vec<u8>,
}

impl Reply {
    const fn status(status: i32) -> Self {
        Self {
            status,
            value: 0,
            payload: Vec::new(),
        }
    }
    const fn ok() -> Self {
        Self::status(STATUS_OK)
    }
    const fn error() -> Self {
        Self::status(STATUS_ERROR)
    }
    const fn value(value: u64) -> Self {
        Self {
            status: STATUS_OK,
            value,
            payload: Vec::new(),
        }
    }
    const fn payload(payload: Vec<u8>) -> Self {
        Self {
            status: STATUS_OK,
            value: 0,
            payload,
        }
    }

    fn write(&self, channel: &mut std::os::unix::net::UnixStream) -> std::io::Result<()> {
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

#[cfg(test)]
mod tests {
    use super::{image_digest, object_hash, CheckpointStore, MemoryStore};
    use std::collections::BTreeMap;

    #[test]
    fn the_image_digest_is_order_independent_in_its_input() {
        let mut forward = BTreeMap::new();
        forward.insert("a".to_owned(), (object_hash("a", b"one"), 3));
        forward.insert("b".to_owned(), (object_hash("b", b"two"), 3));
        let mut backward = BTreeMap::new();
        backward.insert("b".to_owned(), (object_hash("b", b"two"), 3));
        backward.insert("a".to_owned(), (object_hash("a", b"one"), 3));
        assert_eq!(image_digest(&forward), image_digest(&backward));
        assert_eq!(image_digest(&forward).1, 2);
        assert_eq!(image_digest(&forward).2, 6);
    }

    #[test]
    fn a_memory_store_round_trips_objects() {
        let store = MemoryStore::new();
        store.put("proc.1/pages", b"payload").expect("put");
        assert_eq!(store.get("proc.1/pages").expect("get"), b"payload");
        assert_eq!(store.list().expect("list"), vec!["proc.1/pages".to_owned()]);
        assert!(!store.committed());
        store.commit(b"manifest").expect("commit");
        assert!(store.committed());
    }
}
