//! Checkpoint capture and restore through caller-owned storage.

use std::{
    collections::BTreeMap,
    io::Read,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};

mod digest;
mod progress;
mod protocol;
mod state;
mod store;

use digest::CheckpointDigest;
use progress::Participant;
pub(crate) use progress::Progress;
use protocol::{Operation, Reply, Request};
use state::{Object, State};
pub use store::{CheckpointStore, MemoryStore, StoreError};

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

    /// Records that an engine process announced itself on the broker.
    pub(crate) fn attach(&self, id: u64, host_pid: u64) {
        if let Ok(mut state) = self.state.lock() {
            let participant = state.participants.entry(id).or_default();
            participant.host_pid = host_pid;
            participant.last_op = "HELLO";
        }
    }

    /// A snapshot of who is participating and how far each one got.
    pub(crate) fn progress(&self) -> Progress {
        let Ok(state) = self.state.lock() else {
            return Progress::default();
        };
        let mut progress = Progress {
            participants: state.participants.len(),
            ..Progress::default()
        };
        for participant in state.participants.values() {
            for group in &participant.open_groups {
                progress.incomplete.push(group.clone());
            }
            for group in &participant.aborted_groups {
                progress.aborted.push(group.clone());
            }
            for group in &participant.committed_groups {
                progress.committed.push(group.clone());
            }
            progress.detail.push(format!(
                "pid {} last op {}",
                participant.host_pid, participant.last_op
            ));
        }
        progress
    }

    /// Folds one protocol operation into the participant record for `id`.
    fn observe(state: &mut State, id: u64, op: &'static str, group: Option<&str>) {
        let participant = state.participants.entry(id).or_default();
        participant.last_op = op;
        let Some(group) = group else { return };
        match op {
            "GROUP_BEGIN" => {
                participant.open_groups.insert(group.to_owned());
            }
            "GROUP_COMMIT" => {
                participant.open_groups.remove(group);
                participant.committed_groups.insert(group.to_owned());
            }
            "GROUP_ABORT" => {
                participant.open_groups.remove(group);
                participant.aborted_groups.insert(group.to_owned());
            }
            _ => {}
        }
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
        if CheckpointDigest::includes(&object.name) {
            if let Ok(mut state) = self.state.lock() {
                state.digest.insert(
                    object.name.clone(),
                    (
                        CheckpointDigest::object(&object.name, &object.bytes),
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
            if !CheckpointDigest::includes(&name) {
                continue;
            }
            let bytes = self.store.get(&name)?;
            objects.insert(
                name.clone(),
                (CheckpointDigest::object(&name, &bytes), bytes.len() as u64),
            );
        }
        Ok(CheckpointDigest::image(&objects))
    }

    /// Serves one engine process until it closes its channel.
    fn serve(self: &Arc<Self>, channel: &mut crate::sys::Stream, id: u64) {
        loop {
            let mut header = [0_u8; REQUEST_BYTES];
            match channel.read_exact(&mut header) {
                Ok(()) => {}
                Err(_) => return, // the engine process exited; that is the normal end of a channel
            }
            let Some(request) = Request::decode(&header) else {
                self.record_failure("checkpoint channel framing is invalid".into());
                return;
            };
            let mut name = vec![0_u8; request.name_size];
            if channel.read_exact(&mut name).is_err() {
                return;
            }
            let name = String::from_utf8_lossy(name.split_last().map_or(&[][..], |(_, rest)| rest))
                .into_owned();
            let mut payload = Vec::new();
            if request.carries_payload() {
                // `length` is validated against `PAYLOAD_MAX` in `Request::decode`, so this never fails.
                payload = vec![0_u8; usize::try_from(request.length).unwrap_or(0)];
                if channel.read_exact(&mut payload).is_err() {
                    return;
                }
            }
            let reply = self.dispatch(id, &request, &name, &payload);
            if reply.write(channel).is_err() {
                return;
            }
        }
    }

    #[allow(clippy::too_many_lines)]
    fn dispatch(&self, id: u64, request: &Request, name: &str, payload: &[u8]) -> Reply {
        let key = (id, request.stream);
        if let Ok(mut state) = self.state.lock() {
            let group = matches!(
                request.op,
                OP_GROUP_BEGIN | OP_GROUP_COMMIT | OP_GROUP_ABORT
            )
            .then_some(name);
            Self::observe(&mut state, id, Operation::name(request.op), group);
        }
        match request.op {
            OP_OBJECT_BEGIN => {
                if name.len() > NAME_MAX {
                    return Reply::error();
                }
                let Ok(mut state) = self.state.lock() else {
                    return Reply::error();
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
                    object.bytes.extend_from_slice(payload);
                } else {
                    let offset = usize::try_from(request.offset).unwrap_or(usize::MAX);
                    let end = offset + payload.len();
                    if object.bytes.len() < end {
                        object.bytes.resize(end, 0);
                    }
                    object.bytes[offset..end].copy_from_slice(payload);
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
                        Some(CheckpointDigest::image(&state.digest))
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
            OP_COMMIT => match self.store.commit(payload) {
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

impl SinkServer {
    pub(crate) fn start(
        server: &Arc<Self>,
        broker: crate::ffi::Broker,
    ) -> std::thread::JoinHandle<()> {
        let server = Arc::clone(server);
        std::thread::spawn(move || {
            let mut workers = Vec::new();
            while server.running.load(Ordering::SeqCst) {
                let Some((mut channel, host_pid)) =
                    broker.accept(std::time::Duration::from_millis(50))
                else {
                    continue;
                };
                let worker = Arc::clone(&server);
                let id = workers.len() as u64 + 1;
                worker.attach(id, host_pid);
                workers.push(std::thread::spawn(move || {
                    worker.serve(&mut channel, id);
                }));
            }
            for worker in workers {
                let _ = worker.join();
            }
        })
    }
}

#[cfg(test)]
mod tests;
