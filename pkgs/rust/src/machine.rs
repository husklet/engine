use std::{
    fs::{File, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::PathBuf,
    sync::Mutex,
    time::{Duration, Instant},
};

use crate::{
    control::{
        decrement, AttachRequest, Attachment, AttachmentKind, ControlError, EventStream,
        ExtensionHandle, NetworkUpdate, PauseGuard, ProcessInfo, ResourceUpdate, ShutdownPolicy,
        Signal, SignalTarget,
    },
    Child, Domain, Error, Exit, Terminal,
};

/// A live machine control handle.
#[derive(Debug)]
pub struct Machine {
    child: Child,
    pauses: Mutex<usize>,
    checkpoint_directory: Option<PathBuf>,
}

impl Machine {
    pub(crate) const fn new(child: Child, checkpoint_directory: Option<PathBuf>) -> Self {
        Self {
            child,
            pauses: Mutex::new(0),
            checkpoint_directory,
        }
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

    /// Persists the complete native process tree and waits for atomic manifest publication.
    ///
    /// A checkpoint-armed machine exits after capture. The destination must not already contain data;
    /// this prevents stale process records from being mistaken for members of the new checkpoint.
    ///
    /// # Errors
    /// Returns a typed control error if capture was not configured, the destination is unsafe, the native
    /// interrupt fails, the process exits without publishing a manifest, or the deadline expires.
    pub fn checkpoint(&self, timeout: Duration) -> Result<PathBuf, ControlError> {
        let directory = self
            .checkpoint_directory
            .as_ref()
            .ok_or_else(|| ControlError::unsupported("checkpoint"))?;
        if directory.exists() {
            let mut entries = std::fs::read_dir(directory)
                .map_err(|error| checkpoint_error("inspect checkpoint directory", &error))?;
            if entries.next().transpose().map_err(|error| {
                checkpoint_error("inspect checkpoint directory", &error)
            })?.is_some()
            {
                return Err(checkpoint_context(
                    "checkpoint destination already contains data",
                ));
            }
        } else {
            std::fs::create_dir(directory)
                .map_err(|error| checkpoint_error("create checkpoint directory", &error))?;
        }

        let trigger = PathBuf::from(format!("{}.trigger", directory.display()));
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&trigger)
            .map_err(|error| checkpoint_error("open checkpoint trigger", &error))?;
        let mut bytes = [0_u8; 4];
        let read = file
            .read(&mut bytes)
            .map_err(|error| checkpoint_error("read checkpoint trigger", &error))?;
        if read != 0 && read != bytes.len() {
            return Err(checkpoint_context("checkpoint trigger is corrupt"));
        }
        let generation = u32::from_le_bytes(bytes).wrapping_add(1).max(1);
        file.seek(SeekFrom::Start(0))
            .and_then(|_| file.write_all(&generation.to_le_bytes()))
            .and_then(|_| file.set_len(4))
            .and_then(|_| file.sync_data())
            .map_err(|error| checkpoint_error("publish checkpoint request", &error))?;

        crate::ffi::signal(self.id(), checkpoint_interrupt_signal())
            .map_err(|error| checkpoint_error("interrupt checkpoint target", &error))?;
        let manifest = directory.join("MANIFEST");
        let deadline = Instant::now() + timeout;
        loop {
            if manifest.is_file() {
                return Ok(directory.clone());
            }
            if self.child.completed() {
                return Err(checkpoint_context(
                    "engine exited without publishing a complete checkpoint manifest",
                ));
            }
            if Instant::now() >= deadline {
                return Err(checkpoint_context(
                    "checkpoint deadline expired before manifest publication",
                ));
            }
            std::thread::sleep(Duration::from_millis(2));
        }
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

    /// Live network mutation requires the forthcoming native control channel.
    ///
    /// # Errors
    /// Returns [`crate::ControlErrorCategory::Unsupported`] with the current backend.
    pub fn update_network(&self, _update: NetworkUpdate) -> Result<(), ControlError> {
        Err(ControlError::unsupported("update_network"))
    }

    /// Provider hotplug requires negotiated extension transport support.
    ///
    /// # Errors
    /// Returns [`crate::ControlErrorCategory::Unsupported`] with the current backend.
    pub fn hotplug(
        &self,
        _extension: crate::extension::ExtensionSpec,
    ) -> Result<ExtensionHandle, ControlError> {
        Err(ControlError::unsupported("hotplug"))
    }

    /// Structured live events require the forthcoming native control channel.
    ///
    /// # Errors
    /// Returns [`crate::ControlErrorCategory::Unsupported`] with the current backend.
    pub fn events(&self) -> Result<EventStream, ControlError> {
        Err(ControlError::unsupported("events"))
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

fn checkpoint_context(context: impl Into<String>) -> ControlError {
    ControlError {
        category: crate::ControlErrorCategory::Host,
        operation: "checkpoint",
        context: context.into(),
    }
}

fn checkpoint_error(context: &str, error: &std::io::Error) -> ControlError {
    checkpoint_context(format!("{context}: {error}"))
}

#[cfg(target_os = "linux")]
const fn checkpoint_interrupt_signal() -> i32 {
    23 // SIGURG: reserved engine interrupt on Linux.
}

#[cfg(target_os = "macos")]
const fn checkpoint_interrupt_signal() -> i32 {
    29 // SIGINFO: reserved engine interrupt on macOS.
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
