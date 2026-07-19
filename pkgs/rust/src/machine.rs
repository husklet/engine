use std::{fs::File, sync::Mutex};

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
}

impl Machine {
    pub(crate) const fn new(child: Child) -> Self {
        Self {
            child,
            pauses: Mutex::new(0),
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

    /// Returns information for the initial process owned by this control handle.
    #[must_use]
    pub fn initial_process(&self) -> ProcessInfo {
        ProcessInfo {
            host_id: self.id(),
            initial: true,
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

    /// Full process enumeration requires the forthcoming native control channel.
    ///
    /// # Errors
    /// Returns [`crate::ControlErrorCategory::Unsupported`] with the current backend.
    pub fn processes(&self) -> Result<Vec<ProcessInfo>, ControlError> {
        Err(ControlError::unsupported("processes"))
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
