use std::{
    fs::File,
    sync::Mutex,
    time::{Duration, Instant},
};

use crate::{
    control::{
        decrement, AttachRequest, Attachment, AttachmentKind, ControlError, PauseGuard,
        ProcessInfo, ResourceUpdate, ShutdownPolicy, Signal, SignalTarget,
    },
    Child, Domain, Error, Exit, Terminal,
};

/// A live machine control handle.
#[derive(Debug)]
pub struct Machine {
    child: Child,
    pauses: Mutex<usize>,
    /// Set when this launch was armed with a caller-supplied checkpoint store.
    store: Option<StoreChannel>,
}

/// The live transport to a caller-supplied checkpoint store, owned for the machine's lifetime because every
/// engine process in the tree keeps talking to it until it exits.
#[derive(Debug)]
struct StoreChannel {
    server: std::sync::Arc<crate::checkpoint_stream::SinkServer>,
    trigger: crate::ffi::Trigger,
    acceptor: Option<std::thread::JoinHandle<()>>,
}

impl Drop for StoreChannel {
    fn drop(&mut self) {
        self.server.stop();
        if let Some(acceptor) = self.acceptor.take() {
            let _ = acceptor.join();
        }
    }
}

impl Machine {
    pub(crate) const fn new(child: Child) -> Self {
        Self {
            child,
            pauses: Mutex::new(0),
            store: None,
        }
    }

    pub(crate) fn with_store(
        child: Child,
        server: std::sync::Arc<crate::checkpoint_stream::SinkServer>,
        trigger: crate::ffi::Trigger,
        acceptor: std::thread::JoinHandle<()>,
    ) -> Self {
        Self {
            child,
            pauses: Mutex::new(0),
            store: Some(StoreChannel {
                server,
                trigger,
                acceptor: Some(acceptor),
            }),
        }
    }

    /// Requests a capture into the caller-supplied store and waits for the image to be committed.
    ///
    /// Completion is not "a manifest file appeared" -- there is no file. It is the explicit
    /// [`crate::CheckpointStore::commit`] call, which the engine makes exactly once, last.
    ///
    /// # Errors
    /// Returns a control error when this machine has no store, when the store rejected part of the image,
    /// when the engine exited without committing, or when the deadline expires.
    pub fn checkpoint_into_store(&self, timeout: Duration) -> Result<(), ControlError> {
        let channel = self
            .store
            .as_ref()
            .ok_or_else(|| ControlError::unsupported("checkpoint"))?;
        channel.trigger.bump();
        let mut kicked = self.kick_participants();
        if kicked.is_empty() {
            return Err(checkpoint_context(
                "no engine process could be interrupted to start a checkpoint",
            ));
        }
        let deadline = Instant::now() + timeout;
        let mut next_kick = Instant::now() + KICK_INTERVAL;
        loop {
            if channel.server.committed() {
                return Ok(());
            }
            if let Some(failure) = channel.server.failure() {
                return Err(checkpoint_context(failure));
            }
            let progress = channel.server.progress();
            if self.child.completed() {
                return Err(checkpoint_context(format!(
                    "engine exited without committing a complete checkpoint image ({progress})"
                )));
            }
            if Instant::now() >= deadline {
                return Err(checkpoint_context(capture_stall(&progress, &kicked)));
            }
            if Instant::now() >= next_kick {
                // Re-kick: a process that entered a blocking host syscall after the first interrupt (or
                // registered late) would otherwise sit there until the deadline, and the whole tree waits
                // on it. Kicking again is free -- the signal is engine-owned and its handler is empty.
                kicked = self.kick_participants();
                next_kick = Instant::now() + KICK_INTERVAL;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    /// Bounces every process in this machine's domain out of any blocking host syscall so it reaches the
    /// dispatcher safepoint where the checkpoint trigger is read. Returns the host pids actually signalled.
    ///
    /// Signalling only the launch process is not enough: the guest tree runs as further host processes, and
    /// the coordinator among them is the *container init*, not the process this handle names.
    fn kick_participants(&self) -> Vec<u64> {
        let signal = crate::ffi::interrupt_signal();
        let mut kicked = Vec::new();
        for process in self.processes().unwrap_or_default() {
            // Never the launch process itself: it runs no guest and therefore never installs the engine's
            // handler for this signal, so delivering it there would KILL the launch instead of nudging it.
            if process.host_id != self.id() && crate::ffi::signal(process.host_id, signal).is_ok() {
                kicked.push(process.host_id);
            }
        }
        kicked
    }

    #[must_use]
    pub fn id(&self) -> u64 {
        self.child.id()
    }

    /// Returns the durable identity shared by all processes descended from this launch.
    #[must_use]
    pub const fn domain(&self) -> Domain {
        self.child.domain()
    }

    pub fn take_stdin(&mut self) -> Option<File> {
        self.child.take_stdin()
    }

    pub fn take_stdout(&mut self) -> Option<File> {
        self.child.take_stdout()
    }

    pub fn take_stderr(&mut self) -> Option<File> {
        self.child.take_stderr()
    }

    pub fn take_terminal(&mut self) -> Option<Terminal> {
        self.child.take_terminal()
    }

    /// Polls for initial-process completion without consuming the machine.
    ///
    /// # Errors
    /// Returns lifecycle or result-protocol failures.
    pub fn try_wait(&mut self) -> Result<Option<Exit>, Error> {
        self.child.try_wait()
    }

    /// Returns the verified live initial guest process.
    ///
    /// # Errors
    /// Returns a typed finished error once the initial guest process has left
    /// the process-domain inventory, or an engine error if inventory fails.
    pub fn initial_process(&self) -> Result<ProcessInfo, ControlError> {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        loop {
            if let Some(process) = self
                .processes()?
                .into_iter()
                .find(|process| process.initial)
            {
                return Ok(process);
            }
            if self.child.completed() || std::time::Instant::now() >= deadline {
                return Err(ControlError::finished("initial_process"));
            }
            std::thread::yield_now();
        }
    }

    /// Delivers a typed signal to the selected process.
    ///
    /// # Errors
    /// Returns a control error when the target is gone or the host rejects delivery.
    pub fn signal(&self, target: SignalTarget, signal: Signal) -> Result<(), ControlError> {
        if self.child.completed() {
            return Err(ControlError::finished("signal"));
        }
        match target {
            SignalTarget::InitialProcess => self
                .child
                .signal(signal.host_number())
                .map_err(|error| ControlError::engine("signal", &error)),
        }
    }

    /// Acquires one reference-counted pause of the engine process.
    ///
    /// # Errors
    /// Returns a control error if the process cannot be stopped.
    pub fn pause(&self) -> Result<PauseGuard<'_>, ControlError> {
        let mut pauses = self.pauses.lock().map_err(|_| ControlError {
            category: crate::ControlErrorCategory::Host,
            operation: "pause",
            context: "pause state lock is poisoned".into(),
        })?;
        if *pauses == 0 {
            self.child
                .signal(stop_signal())
                .map_err(|error| ControlError::engine("pause", &error))?;
        }
        *pauses = pauses.checked_add(1).ok_or_else(|| ControlError {
            category: crate::ControlErrorCategory::Host,
            operation: "pause",
            context: "pause reference count is exhausted".into(),
        })?;
        Ok(PauseGuard {
            machine: self,
            active: true,
        })
    }

    pub(crate) fn release_pause(&self, report: bool) -> Result<(), ControlError> {
        let pauses = self.pauses.lock().map_err(|_| ControlError {
            category: crate::ControlErrorCategory::Host,
            operation: "resume",
            context: "pause state lock is poisoned".into(),
        })?;
        if !decrement(pauses) {
            return Ok(());
        }
        self.child.signal(continue_signal()).map_err(|error| {
            let error = ControlError::engine("resume", &error);
            if report {
                error
            } else {
                ControlError {
                    context: "automatic resume failed".into(),
                    ..error
                }
            }
        })
    }

    /// Requests graceful or forced shutdown without waiting for completion.
    ///
    /// # Errors
    /// Returns a control error when the shutdown signal cannot be delivered.
    pub fn shutdown(&mut self, policy: ShutdownPolicy) -> Result<(), ControlError> {
        match policy {
            ShutdownPolicy::Signal(signal) => self.signal(SignalTarget::InitialProcess, signal),
            ShutdownPolicy::Force => self
                .force_stop()
                .map_err(|error| ControlError::engine("shutdown", &error)),
        }
    }

    /// Returns a bounded snapshot of verified live process-domain members.
    ///
    /// # Errors
    /// Returns a typed engine error if the native process registry cannot be read.
    pub fn processes(&self) -> Result<Vec<ProcessInfo>, ControlError> {
        crate::ffi::domain_processes(self.domain().identity(), self.id(), 65_536)
            .map(|processes| {
                processes
                    .into_iter()
                    .map(|process| ProcessInfo {
                        host_id: process.host_id,
                        initial: process.initial != 0,
                    })
                    .collect()
            })
            .map_err(|status| {
                ControlError::engine("processes", &Error::Engine { status, detail: 0 })
            })
    }

    /// Transfers selected initial-process streams into one attachment.
    ///
    /// # Errors
    /// Returns an invalid-control error when any requested stream was absent or already attached.
    pub fn attach(&mut self, request: AttachRequest) -> Result<Attachment, ControlError> {
        let AttachRequest { streams } = request;
        let wants = |kind| streams.contains(&kind);
        let missing = (wants(AttachmentKind::Stdin) && self.child.stdin.is_none())
            || (wants(AttachmentKind::Stdout) && self.child.stdout.is_none())
            || (wants(AttachmentKind::Stderr) && self.child.stderr.is_none())
            || (wants(AttachmentKind::Terminal) && self.child.terminal.is_none());
        if missing {
            return Err(ControlError {
                category: crate::ControlErrorCategory::Invalid,
                operation: "attach",
                context: "a requested stream is absent or already attached".into(),
            });
        }
        let attachment = Attachment {
            stdin: wants(AttachmentKind::Stdin)
                .then(|| self.take_stdin())
                .flatten(),
            stdout: wants(AttachmentKind::Stdout)
                .then(|| self.take_stdout())
                .flatten(),
            stderr: wants(AttachmentKind::Stderr)
                .then(|| self.take_stderr())
                .flatten(),
            terminal: wants(AttachmentKind::Terminal)
                .then(|| self.take_terminal())
                .flatten(),
        };
        Ok(attachment)
    }

    /// Live resource mutation requires the forthcoming native control channel.
    ///
    /// # Errors
    /// Returns [`crate::ControlErrorCategory::Unsupported`] with the current backend.
    pub fn update_resources(&self, _update: ResourceUpdate) -> Result<(), ControlError> {
        Err(ControlError::unsupported("update_resources"))
    }

    /// Force-stops the initial process and its engine-owned process domain.
    ///
    /// # Errors
    /// Returns a process-control failure when the machine can no longer be stopped.
    pub fn force_stop(&mut self) -> Result<(), Error> {
        self.child.force_stop()
    }

    /// Waits for the machine's initial process to finish.
    ///
    /// # Errors
    /// Returns lifecycle or result-protocol failures.
    pub fn wait(self) -> Result<Exit, Error> {
        self.child.wait()
    }
}

/// How often a capture in flight re-interrupts its participants.
const KICK_INTERVAL: Duration = Duration::from_millis(250);

/// Explains a capture that ran out of time, naming what was outstanding rather than only the deadline.
fn capture_stall(progress: &crate::checkpoint_stream::Progress, kicked: &[u64]) -> String {
    if !progress.aborted.is_empty() {
        return format!(
            "checkpoint abandoned: {} was aborted by the engine (an unsupported resource is refused during \
             capture; the engine's [ckpt] log names it) -- {progress}",
            progress.aborted.join(", ")
        );
    }
    if !progress.incomplete.is_empty() {
        return format!(
            "checkpoint deadline expired: {} entered capture but never committed -- {progress}",
            progress.incomplete.join(", ")
        );
    }
    if progress.any_image_started() {
        return format!("checkpoint deadline expired before the image was committed -- {progress}");
    }
    format!(
        "checkpoint deadline expired before any process entered capture: no engine process opened a process \
         image after {} interrupt(s) to host pid(s) {kicked:?} -- {progress}",
        kicked.len()
    )
}

fn checkpoint_context(context: impl Into<String>) -> ControlError {
    ControlError {
        category: crate::ControlErrorCategory::Host,
        operation: "checkpoint",
        context: context.into(),
    }
}

#[cfg(target_os = "linux")]
const fn stop_signal() -> i32 {
    19
}
#[cfg(target_os = "macos")]
const fn stop_signal() -> i32 {
    17
}
#[cfg(target_os = "linux")]
const fn continue_signal() -> i32 {
    18
}
#[cfg(target_os = "macos")]
const fn continue_signal() -> i32 {
    19
}
