#![cfg(target_os = "linux")]
//! Capture and restore an INTERACTIVE shell that owns a PTY.
//!
//! Linux hosts only: the guest is the host's own `/bin/bash`, a real aarch64 Linux ELF there and a Mach-O on
//! darwin. None of the committed rootfs assets carries a bash, so the darwin half of this case waits for one;
//! the fix it covers is host-independent and its Linux and macOS halves share one code path.
//!
//! This is the shape a terminal embedder actually runs -- `bash -il` on a pty, job control on, parked in
//! `read(2)` on its terminal at the prompt -- and it is exactly the shape that used to be uncapturable.
//! A guest parked in a blocking host syscall only reaches the dispatcher safepoint where the checkpoint
//! trigger is read when something interrupts that syscall, and the interrupt used to be sent with a
//! guest-reachable signal whose default action is *ignore*. Nothing was interrupted, no process ever
//! entered capture, and the caller saw only "deadline expired". Every assertion here is about the guest
//! coming BACK, so a capture that silently captures nothing cannot pass it.

use hl_engine::{
    CheckpointStore, Engine, Guest, MachineSpec, MemoryStore, ProcessIo, Size, Stdio,
    StoreDirection, Terminal,
};
use std::{
    io::{Read, Write},
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

#[path = "support/checkpoint_env.rs"]
mod checkpoint_env;

const ROWS: u16 = 31;
const COLUMNS: u16 = 117;
const DEADLINE: Duration = Duration::from_secs(30);
/// Bound on a shell becoming interactive, and on it answering a request. Generous on purpose: the hosted
/// four-core runner is an order of magnitude slower than a development box, and every wait here ends on a
/// marker the guest produced, so a long bound costs nothing when things work.
const READY_DEADLINE: Duration = Duration::from_secs(90);
const ANSWER_DEADLINE: Duration = Duration::from_secs(60);
/// How many times readiness is re-asked before the wait becomes purely passive.
const SYNC_ATTEMPTS: usize = 12;

/// An interactive shell that exists on every host this crate builds for. The alpine fixture rootfs has no
/// bash, and the defect is about a job-control shell on a pty, which is what `bash -il` is.
const SHELL: &str = "/bin/bash";

fn io() -> ProcessIo {
    ProcessIo {
        stdin: Stdio::Null,
        stdout: Stdio::Null,
        stderr: Stdio::Null,
    }
}

fn spec() -> MachineSpec {
    let mut spec = MachineSpec::new(Guest::Aarch64, SHELL);
    spec.process.argv.push("-il".into());
    spec.process.env.push(("TERM".into(), "xterm".into()));
    spec.process.env.push(("PS1".into(), "hlprompt> ".into()));
    spec.process
        .env
        .push(("HL_TERMINAL_PROBE".into(), "carried-across-restore".into()));
    spec.process.terminal = Some(Size::new(ROWS, COLUMNS).expect("terminal size"));
    spec.checkpoint.enabled = true;
    spec
}

/// A pty master plus everything the guest has written to it so far.
struct Session {
    terminal: Terminal,
    transcript: Arc<Mutex<String>>,
}

impl Session {
    fn attach(terminal: Terminal) -> Self {
        let transcript = Arc::new(Mutex::new(String::new()));
        let mut reader = terminal.try_clone().expect("clone the pty master");
        let sink = Arc::clone(&transcript);
        thread::spawn(move || {
            let mut buffer = [0_u8; 4096];
            while let Ok(count) = reader.read(&mut buffer) {
                if count == 0 {
                    return;
                }
                sink.lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .push_str(&String::from_utf8_lossy(&buffer[..count]));
            }
        });
        Self {
            terminal,
            transcript,
        }
    }

    fn text(&self) -> String {
        self.transcript
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }

    /// Types `bytes` at the terminal in small chunks, the way a person does.
    fn send(&self, bytes: &[u8]) {
        let mut master = self.terminal.try_clone().expect("clone the pty master");
        for chunk in bytes.chunks(16) {
            master.write_all(chunk).expect("write to the pty");
            master.flush().expect("flush the pty");
            thread::sleep(Duration::from_millis(2));
        }
    }

    /// Runs `command` in the guest shell and waits for `marker` to come back out of the pty.
    fn ask(&self, command: &str, marker: &str, tag: &str) -> String {
        self.send(format!("{command}\n").as_bytes());
        self.expect(marker, tag)
    }

    fn expect(&self, marker: &str, tag: &str) -> String {
        let deadline = Instant::now() + ANSWER_DEADLINE;
        loop {
            let text = self.text();
            // Skip the shell's own echo of the line we just typed: the answer is what follows it. Every
            // marker this test waits for is computed by the shell (`$((20+3))`), so it can never appear in
            // the echo of the request -- the transcript is one interleaved stream of echo and output, and
            // matching the echo would prove nothing about the guest.
            if let Some(offset) = text.rfind(marker) {
                if text[offset..].contains('\n') {
                    return text;
                }
            }
            assert!(
                Instant::now() < deadline,
                "[{tag}] the guest never produced {marker:?}; transcript so far:\n{text}"
            );
            thread::sleep(Duration::from_millis(20));
        }
    }

    /// Blocks until the guest has written anything at all to its terminal.
    fn wait_for_first_output(&self, tag: &str) {
        let deadline = Instant::now() + READY_DEADLINE;
        while self.text().is_empty() {
            assert!(
                Instant::now() < deadline,
                "[{tag}] the guest never wrote anything to its terminal"
            );
            thread::sleep(Duration::from_millis(10));
        }
    }
}

/// Waits until the shell is genuinely interactive, and proves it with an answer the shell had to compute.
///
/// Readiness is never inferred from a delay. It also cannot be established with a single write: a shell
/// that has not finished setting up its line discipline discards type-ahead, and the discarded bytes were
/// still echoed by the tty, so the transcript shows the request while the shell never saw it. On a busy
/// four-core runner that window is wide enough to swallow the whole probe. The request is therefore
/// idempotent (`echo`) and repeated until the shell answers it, under one generous overall deadline.
fn synchronise(session: &Session, tag: &str) {
    let marker = format!("SYNC{tag}-42");
    let request = format!("echo SYNC{tag}-$((6*7))\n");
    let deadline = Instant::now() + READY_DEADLINE;
    // Bounded resends, then a pure wait. Repeating forever would eventually fill the terminal's input queue
    // against a shell that is not reading, and the write -- not the marker -- would be what blocked.
    for _ in 0..SYNC_ATTEMPTS {
        session.send(request.as_bytes());
        let retry = Instant::now() + Duration::from_millis(750);
        while Instant::now() < retry {
            if session.text().contains(&marker) {
                return;
            }
            thread::sleep(Duration::from_millis(10));
        }
    }
    while Instant::now() < deadline {
        if session.text().contains(&marker) {
            return;
        }
        thread::sleep(Duration::from_millis(20));
    }
    panic!(
        "[{tag}] the shell never became interactive; transcript so far:\n{}",
        session.text()
    );
}

/// Every probe the acceptance criteria ask for, in ONE line typed at the terminal, ending in a sentinel.
///
/// One line because the whole point is a shell that is alive on its terminal: if the guest answers all of
/// it, it read from the pty, ran builtins, and wrote back.
const PROBE: &str = "echo SIZE=[$LINES $COLUMNS]; \
     t=; for d in 0 1 2; do if [ -t $d ]; then t=$t$d; fi; done; echo TTYFDS=[$t]; \
     read -r _p _c _s _pp pgrp _rest < /proc/self/stat; echo PGRP=[$pgrp]; \
     echo ENV=[$HL_TERMINAL_PROBE]; echo CWD=[$PWD]; echo PROBE-$((20+3))";

fn between<'a>(text: &'a str, marker: &str) -> &'a str {
    text.rsplit(marker)
        .next()
        .and_then(|rest| rest.split(']').next())
        .unwrap_or_default()
}

/// What the shell reports about itself and its terminal.
struct Observed {
    process_group: String,
    directory: String,
}

/// Drives the shell through every acceptance probe and returns the state that must survive a round trip.
fn probe_terminal_state(session: &Session, tag: &str) -> Observed {
    let text = session.ask(PROBE, "PROBE-23", tag);
    let expected = format!("SIZE=[{ROWS} {COLUMNS}]");
    assert!(
        text.contains(&expected),
        "[{tag}] the terminal size is not {ROWS}x{COLUMNS}:\n{text}"
    );
    assert!(
        text.contains("TTYFDS=[012]"),
        "[{tag}] stdin/stdout/stderr are not all open terminal descriptors:\n{text}"
    );
    assert!(
        text.contains("ENV=[carried-across-restore]"),
        "[{tag}] the environment is not intact:\n{text}"
    );
    let group = between(&text, "PGRP=[").to_owned();
    let directory = between(&text, "CWD=[").to_owned();
    assert!(
        !group.is_empty() && directory.starts_with('/'),
        "[{tag}] no process group / working directory:\n{text}"
    );
    // Foreground ownership is asserted behaviourally rather than from a number: the probe was typed at the
    // pty and answered. A shell that had lost the terminal's foreground group would have been stopped by
    // SIGTTIN on its read instead of replying.
    Observed {
        process_group: group,
        directory,
    }
}

/// Captures an interactive `bash -il` from a pty, then restores it onto a fresh pty.
///
/// `foreground` runs one command in the shell's foreground before the capture, so the restored tree has a
/// real job to come back to; `interrupt` is sent after the restore to end it.
fn round_trip(foreground: Option<&str>, interrupt: bool) -> bool {
    if checkpoint_env::skip_if_unavailable("an_interactive_shell_on_a_pty_round_trips") {
        return true;
    }
    let store = Arc::new(MemoryStore::new());

    let mut machine = Engine::new()
        .spawn_with_store(
            spec(),
            io(),
            Arc::clone(&store) as Arc<dyn CheckpointStore>,
            StoreDirection::Capture,
        )
        .expect("launch an interactive shell with a store attached");
    let session = Session::attach(machine.take_terminal().expect("a pty was requested"));
    session.wait_for_first_output("before capture");
    synchronise(&session, "A");
    let before = probe_terminal_state(&session, "before capture");
    if let Some(command) = foreground {
        session.send(format!("{command}\n").as_bytes());
        // Give the shell time to fork the job and park in wait(2) -- the state the capture must handle.
        thread::sleep(Duration::from_millis(750));
    }

    machine
        .checkpoint_into_store(DEADLINE)
        .expect("an interactive shell on a pty must be capturable");
    assert!(
        store.committed(),
        "capture returned success without committing an image"
    );
    assert_eq!(
        machine.wait().expect("capture exit"),
        hl_engine::Exit::Code(0)
    );
    drop(session);

    let restored = Engine::new()
        .spawn_with_store(
            spec(),
            io(),
            Arc::clone(&store) as Arc<dyn CheckpointStore>,
            StoreDirection::Restore,
        )
        .expect("restore the captured shell");
    let mut restored = restored;
    let session = Session::attach(restored.take_terminal().expect("a pty was requested"));
    if interrupt {
        // The restored shell is still waiting on its foreground job; Ctrl-C must end the job and give the
        // prompt back, exactly as it would have before the capture.
        //
        // Retyped rather than typed once: `spawn_with_store` returns as soon as the restore has been asked
        // for, and a ^C that lands before the job's process group has been re-created cannot reach a group
        // that does not exist yet. A person hits it again; so does this. (It must never kill the machine
        // either -- the restore driver ignores terminal signals until it has replayed the guest's own
        // dispositions -- which is why the early ones are simply discarded instead of fatal.)
        for _ in 0..SYNC_ATTEMPTS {
            thread::sleep(Duration::from_millis(500));
            session.send(b"\x03");
        }
    }
    // The restored shell must serve MORE than one line. Establishing readiness the same way the pre-capture
    // half does costs one command, so a shell that accepts a single line and then reports EOF fails here
    // instead of spending its one working line on the probe and looking healthy.
    synchronise(&session, "B");
    let after = probe_terminal_state(&session, "after restore");
    assert_eq!(
        before.directory, after.directory,
        "the shell's working directory changed across the round trip"
    );

    let _ = restored.force_stop();
    before.process_group == after.process_group
}

#[test]
fn an_idle_interactive_shell_on_a_pty_is_captured_and_restored() {
    round_trip(None, false);
}

/// Ctrl-C at the restored prompt must end the foreground job and hand the terminal back, after which the
/// shell keeps serving commands. This is the case that proved the restored init never re-created its own
/// process group: the shell's terminal handoff named a group it was not in, its next read was a background
/// read, and it saw EOF and logged out.
#[test]
fn an_interactive_shell_with_a_foreground_job_is_captured_and_restored() {
    round_trip(Some("sleep 1000"), true);
}

/// The restored shell is still the container init's own process group, so `/proc/self/stat` reports guest
/// pgid 1 -- the same value it reported before the capture -- and not a raw host pgid.
#[test]
fn the_restored_shell_keeps_its_guest_process_group() {
    assert!(
        round_trip(None, false),
        "the restored shell's process group differs from the captured one"
    );
}
