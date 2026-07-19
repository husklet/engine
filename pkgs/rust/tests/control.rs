use std::{
    collections::BTreeSet,
    fs,
    io::Read,
    path::PathBuf,
    process::Command,
    sync::OnceLock,
    time::{Duration, Instant},
};

use hl_engine::{
    spec::TreeSource, AttachRequest, AttachmentKind, ControlErrorCategory, Engine, Exit, Guest,
    MachineSpec, ProcessIo, ResourceUpdate, ShutdownPolicy, Signal, SignalTarget, Stdio,
};

fn rootfs() -> &'static PathBuf {
    static ROOTFS: OnceLock<PathBuf> = OnceLock::new();
    ROOTFS.get_or_init(|| {
        let path = std::env::temp_dir().join(format!("hl-control-{}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir(&path).unwrap();
        let fixture = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("assets/alpine/alpine-minirootfs-3.24.1-aarch64.tar.gz");
        assert!(Command::new("tar")
            .args(["-xzf"])
            .arg(fixture)
            .arg("-C")
            .arg(&path)
            .status()
            .unwrap()
            .success());
        path
    })
}

fn launch_spec(program: &str) -> MachineSpec {
    let mut spec = MachineSpec::new(Guest::Aarch64, program);
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec
}

#[test]
fn typed_signal_and_shutdown_control_the_initial_process() {
    let mut spec = launch_spec("/bin/sleep");
    spec.process.argv.push("30".into());
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    machine
        .signal(SignalTarget::InitialProcess, Signal::Terminate)
        .unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Signal(15));

    let mut spec = launch_spec("/bin/sleep");
    spec.process.argv.push("30".into());
    let mut machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    machine.shutdown(ShutdownPolicy::Force).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Signal(9));
}

#[test]
fn pause_guards_are_reference_counted() {
    let mut spec = launch_spec("/bin/sleep");
    spec.process.argv.push("30".into());
    let mut machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    let first = machine.pause().unwrap();
    let second = machine.pause().unwrap();
    assert!(wait_for_state(machine.id(), 'T'));
    drop(first);
    assert!(wait_for_state(machine.id(), 'T'));
    second.resume().unwrap();
    assert!(wait_for_not_state(machine.id(), 'T'));
    machine.shutdown(ShutdownPolicy::Force).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Signal(9));
}

#[test]
fn attachment_transfers_only_requested_streams() {
    let mut spec = launch_spec("/bin/echo");
    spec.process.argv.push("typed-attachment".into());
    let io = ProcessIo {
        stdout: Stdio::Piped,
        ..ProcessIo::default()
    };
    let mut machine = Engine::new().spawn(spec, io).unwrap();
    let mut attachment = machine
        .attach(AttachRequest {
            streams: BTreeSet::from([AttachmentKind::Stdout]),
        })
        .unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    let mut output = String::new();
    attachment
        .stdout
        .take()
        .unwrap()
        .read_to_string(&mut output)
        .unwrap();
    assert_eq!(output, "typed-attachment\n");
}

#[test]
fn unavailable_resource_mutations_fail_with_typed_unsupported_errors() {
    let mut spec = launch_spec("/bin/sleep");
    spec.process.argv.push("30".into());
    let mut machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    let error = machine
        .update_resources(ResourceUpdate::default())
        .unwrap_err();
    assert_eq!(error.category, ControlErrorCategory::Unsupported);
    machine.shutdown(ShutdownPolicy::Force).unwrap();
    let _ = machine.wait().unwrap();
}

#[test]
fn process_inventory_tracks_initial_descendants_and_finished_lifecycle() {
    let mut spec = launch_spec("/bin/sh");
    spec.process
        .argv
        .extend(["-c".into(), "sleep 30 & wait".into()]);
    let mut machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    let processes = wait_for_process_count(&machine, 2);
    assert_eq!(
        processes.iter().filter(|process| process.initial).count(),
        1
    );
    assert_eq!(
        machine.initial_process().unwrap(),
        *processes.iter().find(|process| process.initial).unwrap()
    );
    assert!(processes
        .windows(2)
        .all(|pair| pair[0].host_id < pair[1].host_id));
    machine.shutdown(ShutdownPolicy::Force).unwrap();
    let _ = machine.wait().unwrap();

    let mut machine = Engine::new()
        .spawn(launch_spec("/bin/true"), ProcessIo::default())
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    while machine.try_wait().unwrap().is_none() {
        assert!(Instant::now() < deadline, "initial process did not finish");
        std::thread::yield_now();
    }
    while !machine.processes().unwrap().is_empty() {
        assert!(
            Instant::now() < deadline,
            "finished process remained in live inventory"
        );
        std::thread::yield_now();
    }
}

fn wait_for_process_count(
    machine: &hl_engine::Machine,
    minimum: usize,
) -> Vec<hl_engine::ProcessInfo> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let processes = machine.processes().unwrap();
        if processes.len() >= minimum {
            return processes;
        }
        assert!(
            Instant::now() < deadline,
            "process inventory did not converge"
        );
        std::thread::yield_now();
    }
}

fn wait_for_state(process: u64, wanted: char) -> bool {
    wait_for(process, |state| state == wanted)
}

fn wait_for_not_state(process: u64, unwanted: char) -> bool {
    wait_for(process, |state| state != unwanted)
}

fn wait_for(process: u64, predicate: impl Fn(char) -> bool) -> bool {
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline {
        let output = Command::new("ps")
            .args(["-o", "state=", "-p", &process.to_string()])
            .output()
            .unwrap();
        if output.status.success()
            && String::from_utf8_lossy(&output.stdout)
                .trim()
                .chars()
                .next()
                .is_some_and(&predicate)
        {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    false
}
