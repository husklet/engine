use crate::{ffi, result, runtime::ConfigFile, Domain, Error, Exit, Terminal};
use std::{fs::File, io::Read};

/// Owned running engine process.
#[derive(Debug)]
pub struct Child {
    pub(crate) process: Option<ffi::Handle>,
    pub(crate) _config: ConfigFile,
    pub(crate) stdin: Option<File>,
    pub(crate) stdout: Option<File>,
    pub(crate) stderr: Option<File>,
    pub(crate) terminal: Option<Terminal>,
    pub(crate) domain: Domain,
    pub(crate) completed: bool,
    pub(crate) _projections: Vec<crate::projection::Projection>,
    pub(crate) _provider_resources: Vec<crate::engine::lowering::AllocatedResource>,
}
/// Captured output and typed guest status.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Output {
    pub exit: Exit,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}
impl Child {
    /// Returns a durable control handle for all processes descended from this launch.
    #[must_use]
    pub const fn domain(&self) -> Domain {
        self.domain
    }
    #[must_use]
    pub fn id(&self) -> u64 {
        self.process
            .as_ref()
            .and_then(|process| ffi::process_id(process).ok())
            .unwrap_or(0)
    }
    pub fn take_stdin(&mut self) -> Option<File> {
        self.stdin.take()
    }
    pub fn take_stdout(&mut self) -> Option<File> {
        self.stdout.take()
    }
    pub fn take_stderr(&mut self) -> Option<File> {
        self.stderr.take()
    }
    /// Takes ownership of the controlling terminal, when one was requested.
    pub fn take_terminal(&mut self) -> Option<Terminal> {
        self.terminal.take()
    }
    /// Polls for completion.
    ///
    /// # Errors
    /// Returns native lifecycle or result-protocol failures.
    pub fn try_wait(&mut self) -> Result<Option<Exit>, Error> {
        let exit = ffi::try_wait(self.process.as_ref().ok_or(Error::InvalidState)?)
            .map_err(native_error)?
            .map(result::native)
            .transpose()?;
        self.completed |= exit.is_some();
        Ok(exit)
    }
    /// Force-stops the child.
    ///
    /// # Errors
    /// Returns native process-control failures.
    pub fn force_stop(&mut self) -> Result<(), Error> {
        ffi::kill(self.process.as_ref().ok_or(Error::InvalidState)?).map_err(native_error)
    }
    pub(crate) fn signal(&self, signal: i32) -> Result<(), Error> {
        if self.completed {
            return Err(Error::InvalidState);
        }
        ffi::signal(self.id(), signal).map_err(Error::Io)
    }

    pub(crate) const fn completed(&self) -> bool {
        self.completed
    }
    /// Waits for the typed guest status.
    ///
    /// # Errors
    /// Returns native lifecycle or result-protocol failures.
    pub fn wait(mut self) -> Result<Exit, Error> {
        let exit = result::native(
            ffi::wait(self.process.as_ref().ok_or(Error::InvalidState)?).map_err(native_error)?,
        )?;
        ffi::destroy(self.process.take().ok_or(Error::InvalidState)?);
        Ok(exit)
    }
    /// Waits while concurrently capturing standard output and error.
    ///
    /// # Errors
    /// Returns I/O, native lifecycle, or result-protocol failures.
    pub fn output(mut self) -> Result<Output, Error> {
        drop(self.stdin.take());
        let stdout = self.stdout.take().map(|mut file| {
            std::thread::spawn(move || {
                let mut bytes = Vec::new();
                file.read_to_end(&mut bytes).map(|_| bytes)
            })
        });
        let stderr = self.stderr.take().map(|mut file| {
            std::thread::spawn(move || {
                let mut bytes = Vec::new();
                file.read_to_end(&mut bytes).map(|_| bytes)
            })
        });
        let status = self.wait()?;
        let stdout = join(stdout)?;
        let stderr = join(stderr)?;
        Ok(Output {
            exit: status,
            stdout,
            stderr,
        })
    }
}
impl Drop for Child {
    fn drop(&mut self) {
        if let Some(process) = self.process.take() {
            ffi::destroy(process);
        }
    }
}
fn native_error(status: i32) -> Error {
    Error::Engine { status, detail: 0 }
}
fn join(
    thread: Option<std::thread::JoinHandle<std::io::Result<Vec<u8>>>>,
) -> Result<Vec<u8>, Error> {
    match thread {
        None => Ok(Vec::new()),
        Some(thread) => thread
            .join()
            .map_err(|_| Error::InvalidState)?
            .map_err(Error::Io),
    }
}
