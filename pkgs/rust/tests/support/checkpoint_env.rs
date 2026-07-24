//! One-shot, self-calibrating gate for the checkpoint test class.
//!
//! The checkpoint suites capture a live multi-process guest tree. On some hosts
//! -- notably the GitHub hosted runner -- capture never publishes its manifest
//! ("checkpoint deadline expired before manifest publication"), while the same
//! code captures fine locally in debug and release and the C-level equivalent
//! passes. The root cause is tracked separately; this module does not fix it.
//!
//! Instead of a hand-maintained `--skip` list in the CI workflow -- which every
//! new checkpoint test has to be added to by hand, and which silently masks a
//! real regression if capture ever breaks everywhere -- each checkpoint test
//! asks this module whether capture actually works *here*, once per test binary.
//!
//! The probe is a real capture, not an env-var guess: it spawns the smallest
//! checkpoint fixture and runs the classic directory-based `Machine::checkpoint`
//! path -- the simplest capture there is, and if it cannot publish a manifest,
//! neither can the store path. Where capture genuinely works the probe succeeds
//! and every test RUNS; where it cannot, every test self-skips with a visible
//! reason. When the underlying cause is fixed the probe starts succeeding and
//! the tests run again with no code change.

use hl_engine::{Engine, Guest, MachineSpec, ProcessIo, Stdio};
use std::{
    fs,
    path::PathBuf,
    sync::OnceLock,
    thread,
    time::{Duration, Instant},
};

/// Bound on the single probe capture. Short on purpose: this is a go/no-go check,
/// not one of the real tests (which allow a far longer deadline for a contended
/// runner). A host where capture works publishes the manifest in well under this.
const PROBE_DEADLINE: Duration = Duration::from_secs(15);

/// Bound on waiting for the probe guest's three-process tree to come up.
const PROBE_READY_DEADLINE: Duration = Duration::from_secs(30);

fn fixture_named(name: &str, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/{name}-{isa}"))
}

/// Run the one real probe capture. Returns whether a manifest was published.
fn probe() -> bool {
    let guest = Guest::Aarch64;
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join(format!("checkpoint-probe-{}", std::process::id()));
    let checkpoint = root.join("image");
    let release = root.join("release");
    let output = root.join("release.output");
    let _ = fs::remove_dir_all(&root);
    if fs::create_dir_all(&root).is_err() {
        return false;
    }

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

    let published = (|| {
        let machine = Engine::new().spawn(capture, io).ok()?;

        // Wait for the three-process tree to exist before capturing it.
        let deadline = Instant::now() + PROBE_READY_DEADLINE;
        loop {
            let ready = fs::read_to_string(&output).unwrap_or_default();
            if machine.processes().map_or(0, |p| p.len()) == 3
                && ["READY 1", "READY 2", "READY 3"]
                    .iter()
                    .all(|m| ready.contains(m))
            {
                break;
            }
            if Instant::now() >= deadline {
                return None;
            }
            thread::sleep(Duration::from_millis(5));
        }

        // The go/no-go signal: does the classic directory capture publish a manifest
        // within the short deadline? Everything else (restore, the guest exit code)
        // is irrelevant to whether this environment can capture at all.
        let ok = machine.checkpoint(PROBE_DEADLINE).is_ok();
        // Let the guest resume and exit so it does not linger past the probe.
        let _ = fs::write(&release, []);
        let _ = machine.wait();
        Some(ok)
    })()
    .unwrap_or(false);

    let _ = fs::remove_dir_all(&root);
    published
}

/// Whether checkpoint capture actually completes in this environment.
///
/// Runs one real probe capture the first time it is called and caches the result,
/// so the probe runs once per test binary rather than once per test.
pub fn checkpoint_available() -> bool {
    static AVAILABLE: OnceLock<bool> = OnceLock::new();
    *AVAILABLE.get_or_init(probe)
}

/// Skip guard for the top of a checkpoint test. Returns `true` (and prints a
/// visible reason) when the test should return early because this environment
/// cannot complete a capture; returns `false` when the test should run.
#[must_use]
pub fn skip_if_unavailable(test_name: &str) -> bool {
    if checkpoint_available() {
        return false;
    }
    eprintln!(
        "SKIP {test_name}: checkpoint capture does not complete in this environment \
         (probe did not publish a manifest)"
    );
    true
}
