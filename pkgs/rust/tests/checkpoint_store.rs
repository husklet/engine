//! Capture and restore a real multi-process guest through a caller-supplied store, with no checkpoint
//! directory anywhere. This is the test that decides whether "checkpoint/restore streams through the Rust
//! API" is true: the image exists only as bytes this test held in memory.

use hl_engine::{
    CheckpointStore, Engine, Exit, Guest, MachineSpec, MemoryStore, ProcessIo, Size, Stdio,
    StoreDirection, StoreError,
};
#[path = "support/checkpoint_env.rs"]
mod checkpoint_env;

use std::{
    collections::BTreeMap,
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};

fn fixture(name: &str, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/{name}-{isa}"))
}

/// The fixture announces each of its three processes on its own output file. Waiting for the host process
/// registry instead would race the guest's own `fork()`: the tree must exist before it can be captured.
fn wait_until_ready(output: &std::path::Path) {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let text = fs::read_to_string(output).unwrap_or_default();
        if ["READY 1", "READY 2", "READY 3"]
            .iter()
            .all(|marker| text.contains(marker))
        {
            return;
        }
        assert!(Instant::now() < deadline, "guest tree did not become ready");
        thread::sleep(Duration::from_millis(5));
    }
}

fn io() -> ProcessIo {
    ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    }
}

/// The whole point: a three-process guest is captured into memory and resumed from memory. The guest proves
/// its own state survived -- it exits 0 only if every process came back and observed what it had before.
#[test]
fn a_three_process_tree_is_captured_into_memory_and_restored_from_it() {
    if checkpoint_env::skip_if_unavailable(
        "a_three_process_tree_is_captured_into_memory_and_restored_from_it",
    ) {
        return;
    }
    // Both arches: the x86_64 fd-restore bug is fixed (x86 dup2 now maps to
    // canonical dup3 so checkpoint captures dup2'd descriptors). See tests/policy.rs.
    captured_into_memory_and_restored(Guest::Aarch64);
    captured_into_memory_and_restored(Guest::X86_64);
}

fn captured_into_memory_and_restored(guest: Guest) {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-store-{}-{guest:?}", std::process::id()));
    let release = root.join("release");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).expect("scratch directory");

    let executable = fixture("checkpoint-tree", guest);
    let store = Arc::new(MemoryStore::new());

    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    let machine = Engine::new()
        .spawn_with_store(
            capture,
            io(),
            Arc::clone(&store) as Arc<dyn CheckpointStore>,
            StoreDirection::Capture,
        )
        .expect("capture launch");

    wait_until_ready(&root.join("release.output"));

    machine
        .checkpoint_into_store(Duration::from_secs(20))
        .expect("capture into the store");
    assert_eq!(machine.wait().expect("capture exit"), Exit::Code(0));

    // Nothing was written anywhere but this process's heap.
    assert!(store.committed(), "the image was never committed");
    let objects = store.objects();
    assert_eq!(
        objects
            .keys()
            .filter(|name| name.starts_with("proc.") && name.ends_with("/meta"))
            .count(),
        3,
        "expected three process images, got {:?}",
        objects.keys().collect::<Vec<_>>()
    );
    assert!(store.bytes() > 0);

    // Restore FROM those bytes. The guest's release file is its own resume gate, not part of the image.
    fs::write(&release, []).expect("release the restored tree");
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    let restored = Engine::new()
        .spawn_with_store(
            restore,
            io(),
            Arc::clone(&store) as Arc<dyn CheckpointStore>,
            StoreDirection::Restore,
        )
        .expect("restore launch");
    assert_eq!(restored.wait().expect("restore exit"), Exit::Code(0));

    fs::remove_dir_all(root).expect("scratch cleanup");
}

/// The regression this whole change exists for: a streaming checkpoint store MUST compose with a PTY.
/// Before the fix the Rust launch path chose terminal XOR checkpoint, hardcoding `size = null` and
/// dropping the broker; the guest then aborted with "streaming checkpoint requested without a broker
/// descriptor". Here the same three-process tree is captured into memory WHILE a terminal is attached, so
/// the capture only succeeds if terminal + checkpoint + trigger were all forwarded together.
#[test]
fn a_store_captures_while_a_terminal_is_attached() {
    use std::io::Read;

    if checkpoint_env::skip_if_unavailable("a_store_captures_while_a_terminal_is_attached") {
        return;
    }
    let guest = Guest::Aarch64;
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-store-terminal-{}", std::process::id()));
    let release = root.join("release");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).expect("scratch directory");

    let executable = fixture("checkpoint-tree", guest);
    let store = Arc::new(MemoryStore::new());

    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    // The composition under test: a PTY requested alongside the streaming store.
    capture.process.terminal = Some(Size::new(24, 80).expect("terminal size"));

    let mut machine = Engine::new()
        .spawn_with_store(
            capture,
            io(),
            Arc::clone(&store) as Arc<dyn CheckpointStore>,
            StoreDirection::Capture,
        )
        .expect("capture launch with a terminal attached");

    // Drain the pty so the guest never blocks on a full master buffer while we wait for readiness.
    let terminal = machine.take_terminal().expect("a terminal was requested");
    let mut reader = terminal.try_clone().expect("clone the pty master");
    let drain = thread::spawn(move || {
        let mut sink = Vec::new();
        let _ = reader.read_to_end(&mut sink);
    });

    wait_until_ready(&root.join("release.output"));

    machine
        .checkpoint_into_store(Duration::from_secs(20))
        .expect("capture into the store must succeed with a terminal active");
    assert_eq!(machine.wait().expect("capture exit"), Exit::Code(0));

    assert!(
        store.committed(),
        "the image was never committed: terminal + checkpoint did not compose"
    );
    assert_eq!(
        store
            .objects()
            .keys()
            .filter(|name| name.starts_with("proc.") && name.ends_with("/meta"))
            .count(),
        3,
        "expected three process images while a terminal was attached"
    );

    drop(terminal);
    let _ = drain.join();
    let _ = fs::remove_dir_all(root);
}

/// A store that fails after N successful objects, so the failure lands in the middle of a process image.
#[derive(Debug)]
struct FailsAfter {
    allowed: usize,
    written: AtomicUsize,
    objects: Mutex<BTreeMap<String, Vec<u8>>>,
}

impl CheckpointStore for FailsAfter {
    fn put(&self, name: &str, data: &[u8]) -> Result<(), StoreError> {
        if self.written.fetch_add(1, Ordering::SeqCst) >= self.allowed {
            return Err(StoreError::new("the store is out of space"));
        }
        self.objects
            .lock()
            .expect("store lock")
            .insert(name.to_owned(), data.to_vec());
        Ok(())
    }
    fn get(&self, name: &str) -> Result<Vec<u8>, StoreError> {
        self.objects
            .lock()
            .expect("store lock")
            .get(name)
            .cloned()
            .ok_or_else(|| StoreError::new("absent"))
    }
    fn list(&self) -> Result<Vec<String>, StoreError> {
        Ok(self
            .objects
            .lock()
            .expect("store lock")
            .keys()
            .cloned()
            .collect())
    }
}

/// The documented failure contract: an error from the store fails the capture and NOTHING is committed.
/// Whatever the store already accepted is debris the caller may discard; the engine never calls back to
/// undo it, and never presents it as a checkpoint.
#[test]
fn a_store_error_mid_capture_fails_the_capture_without_committing() {
    if checkpoint_env::skip_if_unavailable(
        "a_store_error_mid_capture_fails_the_capture_without_committing",
    ) {
        return;
    }
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-store-failure-{}", std::process::id()));
    let release = root.join("release");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).expect("scratch directory");

    let store = Arc::new(FailsAfter {
        allowed: 2,
        written: AtomicUsize::new(0),
        objects: Mutex::new(BTreeMap::new()),
    });

    let executable = fixture("checkpoint-tree", Guest::Aarch64);
    let mut capture = MachineSpec::new(Guest::Aarch64, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    let machine = Engine::new()
        .spawn_with_store(
            capture,
            io(),
            Arc::clone(&store) as Arc<dyn CheckpointStore>,
            StoreDirection::Capture,
        )
        .expect("capture launch");

    wait_until_ready(&root.join("release.output"));

    let error = machine
        .checkpoint_into_store(Duration::from_secs(20))
        .expect_err("a failing store must fail the capture");
    assert!(
        format!("{error}").contains("store rejected"),
        "the capture must fail with the store's own rejection, not a timeout: {error}"
    );
    assert!(
        !store
            .objects
            .lock()
            .expect("store lock")
            .contains_key("MANIFEST"),
        "a refused capture must never be committed"
    );

    let _ = fs::remove_dir_all(root);
}
