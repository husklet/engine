use hl_engine::{Engine, Exit, Guest, MachineSpec, ProcessIo, Stdio};
use std::{
    fs,
    path::PathBuf,
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
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-e2e-{}", std::process::id()));
    let checkpoint = root.join("image");
    let release = root.join("release");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-tree", Guest::Aarch64);
    let mut capture = MachineSpec::new(Guest::Aarch64, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(checkpoint.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    while machine.processes().unwrap().len() != 3 {
        assert!(
            Instant::now() < deadline,
            "guest process tree did not become ready"
        );
        thread::sleep(Duration::from_millis(2));
    }
    assert_eq!(
        machine.checkpoint(Duration::from_secs(10)).unwrap(),
        checkpoint
    );
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
    let mut restore = MachineSpec::new(Guest::Aarch64, &executable);
    restore.checkpoint.enabled = true;
    restore.checkpoint.restore_directory = Some(checkpoint);
    assert_eq!(
        Engine::new().spawn(restore, io).unwrap().wait().unwrap(),
        Exit::Code(0)
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn rust_api_restores_buffered_cross_process_pipe_state() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-pipe-e2e-{}", std::process::id()));
    let checkpoint = root.join("image");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-pipe", Guest::Aarch64);
    let mut capture = MachineSpec::new(Guest::Aarch64, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(checkpoint.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
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
    machine.checkpoint(Duration::from_secs(10)).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(Guest::Aarch64, &executable);
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
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-deleted-e2e-{}", std::process::id()));
    let checkpoint = root.join("image");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();

    let executable = fixture_named("checkpoint-deleted", Guest::Aarch64);
    let mut capture = MachineSpec::new(Guest::Aarch64, &executable);
    capture.process.argv.push(release.clone().into_os_string());
    capture.checkpoint.enabled = true;
    capture.checkpoint.capture_directory = Some(checkpoint.clone());
    let io = ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    };
    let machine = Engine::new().spawn(capture, io).unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
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
    machine.checkpoint(Duration::from_secs(10)).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));

    fs::write(&release, []).unwrap();
    let mut restore = MachineSpec::new(Guest::Aarch64, &executable);
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

fn fixture_named(name: &str, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/{name}-{isa}"))
}
