use hl_engine::{Engine, Exit, Guest, MachineSpec, ProcessIo, Stdio};

#[path = "support/checkpoint_env.rs"]
mod checkpoint_env;

use std::{
    fs,
    path::{Path, PathBuf},
    thread,
    time::{Duration, Instant},
};

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
    // aarch64 only: x86_64 checkpoint is refused by validation because RESTORE is
    // broken there -- the process returns with its file descriptors pointing at the
    // launcher's stdio instead of the captured ones, while registers and memory
    // restore correctly. Capture itself works. See the refusal assertion in
    // tests/policy.rs; re-add Guest::X86_64 once the fd-restore path is fixed.
    if checkpoint_env::skip_if_unavailable("rust_api_checkpoints_and_restores_a_three_process_tree")
    {
        return;
    }
    checkpoints_and_restores_a_three_process_tree(Guest::Aarch64);
}

fn checkpoints_and_restores_a_three_process_tree(guest: Guest) {
    let root = scratch_root("checkpoint-e2e", guest);
    let checkpoint = root.join("image");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-tree", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(checkpoint.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
    let deadline = Instant::now() + READY_DEADLINE;
    while machine.processes().unwrap().len() != 3 {
        assert!(
            Instant::now() < deadline,
            "guest process tree did not become ready"
        );
        thread::sleep(Duration::from_millis(2));
    }
    assert_eq!(machine.checkpoint(CHECKPOINT_DEADLINE).unwrap(), checkpoint);
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    assert_eq!(
        fs::read_dir(&checkpoint)
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| entry.file_name().to_string_lossy().starts_with("proc."))
            .count(),
        3
    );

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    restore.checkpoint.restore_directory = Some(checkpoint);
    assert_eq!(
        Engine::new().spawn(restore, io).unwrap().wait().unwrap(),
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
    // aarch64 only: x86_64 checkpoint is refused by validation because RESTORE is
    // broken there -- the process returns with its file descriptors pointing at the
    // launcher's stdio instead of the captured ones, while registers and memory
    // restore correctly. Capture itself works. See the refusal assertion in
    // tests/policy.rs; re-add Guest::X86_64 once the fd-restore path is fixed.
    if checkpoint_env::skip_if_unavailable("rust_api_restores_buffered_cross_process_pipe_state") {
        return;
    }
    restores_buffered_cross_process_pipe_state(Guest::Aarch64);
}

fn restores_buffered_cross_process_pipe_state(guest: Guest) {
    let root = scratch_root("checkpoint-pipe-e2e", guest);
    let checkpoint = root.join("image");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-pipe", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(checkpoint.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
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
    machine.checkpoint(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    restore.checkpoint.restore_directory = Some(checkpoint);
    assert_eq!(
        Engine::new().spawn(restore, io).unwrap().wait().unwrap(),
        Exit::Code(0)
    );
    assert!(fs::read_to_string(output)
        .unwrap()
        .contains("PIPE-RESTORED"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn rust_api_restores_unlinked_regular_file_content_and_offset() {
    // aarch64 only: x86_64 checkpoint is refused by validation because RESTORE is
    // broken there -- the process returns with its file descriptors pointing at the
    // launcher's stdio instead of the captured ones, while registers and memory
    // restore correctly. Capture itself works. See the refusal assertion in
    // tests/policy.rs; re-add Guest::X86_64 once the fd-restore path is fixed.
    if checkpoint_env::skip_if_unavailable(
        "rust_api_restores_unlinked_regular_file_content_and_offset",
    ) {
        return;
    }
    restores_unlinked_regular_file_content_and_offset(Guest::Aarch64);
}

fn restores_unlinked_regular_file_content_and_offset(guest: Guest) {
    let root = scratch_root("checkpoint-deleted-e2e", guest);
    let checkpoint = root.join("image");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-deleted", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(checkpoint.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
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
    machine.checkpoint(CHECKPOINT_DEADLINE).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    restore.checkpoint.restore_directory = Some(checkpoint);
    assert_eq!(
        Engine::new().spawn(restore, io).unwrap().wait().unwrap(),
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
    // aarch64 only: x86_64 checkpoint is refused by validation because RESTORE is
    // broken there -- the process comes back with its file descriptors pointing at
    // the launcher's stdio instead of the captured ones, while registers and memory
    // restore correctly. Capture itself works. See the refusal assertion in
    // tests/policy.rs; re-add Guest::X86_64 here once the fd-restore path is fixed.
    if checkpoint_env::skip_if_unavailable("rust_api_restores_while_arming_the_next_capture") {
        return;
    }
    restores_while_arming_the_next_capture(Guest::Aarch64);
}

fn restores_while_arming_the_next_capture(guest: Guest) {
    let root = scratch_root("checkpoint-rearm-e2e", guest);
    let first = root.join("image.a");
    let second = root.join("image.b");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-cycle", guest);
    let mut capture = MachineSpec::new(guest, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(first.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
    await_output(
        &output,
        "STAGE 1",
        "cycle guest did not reach its first stage",
    );
    assert_eq!(machine.checkpoint(CHECKPOINT_DEADLINE).unwrap(), first);
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    // Restore the first image and arm the second capture in one spec: the shape
    // a caller uses to checkpoint the same guest repeatedly.
    fs::write(root.join("release.go1"), []).unwrap();
    let mut rearm = MachineSpec::new(guest, &executable);
    rearm.checkpoint.enabled = true;
    rearm.checkpoint.restore_directory = Some(first);
    rearm.checkpoint.capture_directory = Some(second.clone());
    let machine = Engine::new().spawn(rearm, io).unwrap();
    await_output(
        &output,
        "STAGE 2",
        "restored guest did not reach its second stage",
    );
    assert_eq!(machine.checkpoint(CHECKPOINT_DEADLINE).unwrap(), second);
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(root.join("release.go2"), []).unwrap();
    let mut restore = MachineSpec::new(guest, &executable);
    restore.checkpoint.enabled = true;
    restore.checkpoint.restore_directory = Some(second);
    assert_eq!(
        Engine::new().spawn(restore, io).unwrap().wait().unwrap(),
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
