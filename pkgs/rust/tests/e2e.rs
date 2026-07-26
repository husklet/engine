use hl_engine::{
    CheckpointStore, Engine, Exit, Guest, MachineSpec, MemoryStore, ProcessIo, Stdio,
    StoreDirection,
};

#[path = "support/checkpoint_env.rs"]
mod checkpoint_env;

use std::{
    fs,
    path::{Path, PathBuf},
    sync::Arc,
    thread,
    time::{Duration, Instant},
};

/// One in-memory image, shared by the capture launch and the restore launch that reads it back.
fn store() -> Arc<MemoryStore> {
    Arc::new(MemoryStore::new())
}

fn as_store(store: &Arc<MemoryStore>) -> Arc<dyn CheckpointStore> {
    Arc::clone(store) as Arc<dyn CheckpointStore>
}

fn fixture(status: i32, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/exit{status}-{isa}"))
}

#[test]
fn both_guest_isas_report_typed_exit_42() {
    let engine = Engine::new();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let exit = engine.command(guest, fixture(42, guest)).status().unwrap();
        assert_eq!(exit, Exit::Code(42));
    }
}

#[test]
fn guest_exit_70_remains_distinct_from_engine_failure() {
    let engine = Engine::new();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let exit = engine.command(guest, fixture(70, guest)).status().unwrap();
        assert_eq!(exit, Exit::Code(70));
    }
}

#[test]
fn initial_executable_authority_is_not_reused_by_exec() {
    let engine = Engine::new();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let executable = fixture_named("exec-denied", guest);
        assert_eq!(
            engine
                .command(guest, &executable)
                .arg(&executable)
                .status()
                .unwrap(),
            Exit::Code(0)
        );
    }
}

#[test]
fn rust_api_checkpoints_and_restores_a_three_process_tree() {
    // Both arches: the x86_64 fd-restore bug (a dup2'd fd came back pointing at the
    // launcher's stdio) is fixed -- x86 dup2 (33) now maps to canonical dup3 (24) so
    // checkpoint captures dup2'd descriptors. See tests/policy.rs.
    if checkpoint_env::skip_if_unavailable("rust_api_checkpoints_and_restores_a_three_process_tree")
    {
        return;
    }
    checkpoints_and_restores_a_three_process_tree(Guest::Aarch64);
    checkpoints_and_restores_a_three_process_tree(Guest::X86_64);
}

fn checkpoints_and_restores_a_three_process_tree(guest: Guest) {
    let root = scratch_root("checkpoint-e2e", guest);
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-tree", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    let image = store();
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new()
        .spawn_with_store(capture, io, as_store(&image), StoreDirection::Capture)
        .unwrap();
    let deadline = Instant::now() + READY_DEADLINE;
    while machine.processes().unwrap().len() != 3 {
        assert!(
            Instant::now() < deadline,
            "guest process tree did not become ready"
        );
        thread::sleep(Duration::from_millis(2));
    }
    machine.checkpoint_into_store(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    assert_eq!(
        image
            .objects()
            .keys()
            .filter(|name| name.starts_with("proc.") && name.ends_with("/meta"))
            .count(),
        3
    );

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    assert_eq!(
        Engine::new()
            .spawn_with_store(restore, io, as_store(&image), StoreDirection::Restore)
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
    // The exit status alone cannot tell a resumed tree from a relaunched one:
    // only the restored process writes this line, through its restored fds.
    assert!(fs::read_to_string(output)
        .unwrap()
        .contains("TREE-RESTORED"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn rust_api_restores_buffered_cross_process_pipe_state() {
    // Both arches: x86_64 fd restore is fixed (x86 dup2 now maps to canonical dup3
    // so checkpoint captures dup2'd descriptors). See tests/policy.rs.
    if checkpoint_env::skip_if_unavailable("rust_api_restores_buffered_cross_process_pipe_state") {
        return;
    }
    restores_buffered_cross_process_pipe_state(Guest::Aarch64);
    restores_buffered_cross_process_pipe_state(Guest::X86_64);
}

fn restores_buffered_cross_process_pipe_state(guest: Guest) {
    let root = scratch_root("checkpoint-pipe-e2e", guest);
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-pipe", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    let image = store();
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new()
        .spawn_with_store(capture, io, as_store(&image), StoreDirection::Capture)
        .unwrap();
    let deadline = Instant::now() + READY_DEADLINE;
    loop {
        let ready = fs::read_to_string(&output).unwrap_or_default();
        if machine.processes().unwrap().len() == 2
            && ready.contains("READY 1")
            && ready.contains("READY 2")
        {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "pipe process tree did not become ready"
        );
        thread::sleep(Duration::from_millis(2));
    }
    machine.checkpoint_into_store(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    assert_eq!(
        Engine::new()
            .spawn_with_store(restore, io, as_store(&image), StoreDirection::Restore)
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
    assert!(fs::read_to_string(output)
        .unwrap()
        .contains("PIPE-RESTORED"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn rust_api_restores_unlinked_regular_file_content_and_offset() {
    // Both arches: x86_64 fd restore is fixed (x86 dup2 now maps to canonical dup3
    // so checkpoint captures dup2'd descriptors). See tests/policy.rs.
    if checkpoint_env::skip_if_unavailable(
        "rust_api_restores_unlinked_regular_file_content_and_offset",
    ) {
        return;
    }
    restores_unlinked_regular_file_content_and_offset(Guest::Aarch64);
    restores_unlinked_regular_file_content_and_offset(Guest::X86_64);
}

fn restores_unlinked_regular_file_content_and_offset(guest: Guest) {
    let root = scratch_root("checkpoint-deleted-e2e", guest);
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-deleted", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    let image = store();
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new()
        .spawn_with_store(capture, io, as_store(&image), StoreDirection::Capture)
        .unwrap();
    let deadline = Instant::now() + READY_DEADLINE;
    while !fs::read_to_string(&output)
        .unwrap_or_default()
        .contains("READY 1")
    {
        assert!(
            Instant::now() < deadline,
            "deleted-file guest did not become ready"
        );
        thread::sleep(Duration::from_millis(2));
    }
    machine.checkpoint_into_store(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    assert_eq!(
        Engine::new()
            .spawn_with_store(restore, io, as_store(&image), StoreDirection::Restore)
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
    assert!(fs::read_to_string(output)
        .unwrap()
        .contains("DELETED-RESTORED"));
    fs::remove_dir_all(root).unwrap();
}

/// Deadlines for the checkpoint tests.
///
/// These bound a WAIT, not a performance claim: the assertion is that capture
/// succeeds and the tree becomes ready, never that either is fast. They were 10s
/// and 5s, which is ample on an idle machine and not on a contended CI runner --
/// the three checkpoint tests capture multi-process trees CONCURRENTLY under
/// `cargo test`, and all three failed in CI with "checkpoint deadline expired
/// before manifest publication" while passing locally. A genuinely hung capture
/// still fails, just later; a slow one no longer reports a false failure.
const CHECKPOINT_DEADLINE: Duration = Duration::from_secs(120);
const READY_DEADLINE: Duration = Duration::from_secs(60);

/// Restore an image while arming the next capture in the same spec, twice, and
/// prove the guest survived both hops instead of being relaunched.
#[test]
fn rust_api_restores_while_arming_the_next_capture() {
    // Both arches: x86_64 fd restore is fixed (x86 dup2 now maps to canonical dup3
    // so checkpoint captures dup2'd descriptors). See tests/policy.rs.
    if checkpoint_env::skip_if_unavailable("rust_api_restores_while_arming_the_next_capture") {
        return;
    }
    restores_while_arming_the_next_capture(Guest::Aarch64);
    restores_while_arming_the_next_capture(Guest::X86_64);
}

fn restores_while_arming_the_next_capture(guest: Guest) {
    let root = scratch_root("checkpoint-rearm-e2e", guest);
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-cycle", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    let first = store();
    let second = store();
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new()
        .spawn_with_store(capture, io, as_store(&first), StoreDirection::Capture)
        .unwrap();
    await_output(
        &output,
        "STAGE 1",
        "cycle guest did not reach its first stage",
    );
    machine.checkpoint_into_store(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    // Restore the first image and arm the second capture in one launch: the shape
    // a caller uses to checkpoint the same guest repeatedly. One channel carries
    // both directions, so the store it reads is the store it writes.
    fs::write(root.join("release.go1"), []).unwrap();
    let mut rearm = MachineSpec::new(guest, &executable);
    rearm.checkpoint.enabled = true;
    let machine = Engine::new()
        .spawn_with_store(
            rearm,
            io,
            Arc::new(RestoreInto {
                source: Arc::clone(&first),
                destination: Arc::clone(&second),
            }) as Arc<dyn CheckpointStore>,
            StoreDirection::Both,
        )
        .unwrap();
    await_output(
        &output,
        "STAGE 2",
        "restored guest did not reach its second stage",
    );
    machine.checkpoint_into_store(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(root.join("release.go2"), []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    assert_eq!(
        Engine::new()
            .spawn_with_store(restore, io, as_store(&second), StoreDirection::Restore)
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );

    // A guest that was silently relaunched would boot again and reset its
    // counter; continuity demands one boot and a strictly advancing counter.
    let transcript = fs::read_to_string(&output).unwrap();
    assert_eq!(
        transcript
            .lines()
            .filter(|line| line.starts_with("BOOT "))
            .count(),
        1,
        "guest booted more than once across the restore hops: {transcript}"
    );
    assert!(
        transcript.contains("CYCLE-RESTORED"),
        "guest did not finish the cycle: {transcript}"
    );
    let stages: Vec<u64> = transcript
        .lines()
        .filter_map(|line| line.strip_prefix("STAGE "))
        .filter_map(|line| line.split_whitespace().nth(1))
        .map(|value| value.parse().unwrap())
        .collect();
    assert_eq!(stages.len(), 3, "expected three stage marks: {transcript}");
    assert!(
        stages[0] < stages[1] && stages[1] < stages[2],
        "counter did not advance across both hops: {stages:?}"
    );
    fs::remove_dir_all(root).unwrap();
}

fn await_output(output: &Path, needle: &str, message: &str) {
    let deadline = Instant::now() + READY_DEADLINE;
    while !fs::read_to_string(output)
        .unwrap_or_default()
        .contains(needle)
    {
        assert!(Instant::now() < deadline, "{message}");
        thread::sleep(Duration::from_millis(2));
    }
}

fn scratch_root(name: &str, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("{name}-{isa}-{}", std::process::id()))
}

fn fixture_named(name: &str, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/{name}-{isa}"))
}

/// Reads one image and writes the next: the re-arm launch restores from `source` while capturing into
/// `destination`, which a single store cannot express without the new image overwriting the old one.
#[derive(Debug)]
struct RestoreInto {
    source: Arc<MemoryStore>,
    destination: Arc<MemoryStore>,
}

impl CheckpointStore for RestoreInto {
    fn put(&self, name: &str, data: &[u8]) -> Result<(), hl_engine::StoreError> {
        self.destination.put(name, data)
    }
    fn get(&self, name: &str) -> Result<Vec<u8>, hl_engine::StoreError> {
        self.source.get(name)
    }
    fn list(&self) -> Result<Vec<String>, hl_engine::StoreError> {
        self.source.list()
    }
    fn commit(&self, manifest: &[u8]) -> Result<(), hl_engine::StoreError> {
        self.destination.commit(manifest)
    }
}
