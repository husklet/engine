use crate::{runtime::LaunchFiles, Error, Exit};
use std::process::{Child as Process, ChildStderr, ChildStdin, ChildStdout};

/// Owned running engine process.
#[derive(Debug)]
pub struct Child {
    pub(crate) process: Option<Process>,
    pub(crate) files: LaunchFiles,
}
/// Captured output and typed guest status.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Output {
    pub status: Exit,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}
impl Child {
    #[must_use]
    pub fn id(&self) -> u32 {
        self.process.as_ref().map_or(0, Process::id)
    }
    pub fn take_stdin(&mut self) -> Option<ChildStdin> {
        self.process.as_mut()?.stdin.take()
    }
    pub fn take_stdout(&mut self) -> Option<ChildStdout> {
        self.process.as_mut()?.stdout.take()
    }
    pub fn take_stderr(&mut self) -> Option<ChildStderr> {
        self.process.as_mut()?.stderr.take()
    }
    /// Polls for completion.
    ///
    /// # Errors
    /// Returns process or result-protocol failures.
    pub fn try_wait(&mut self) -> Result<Option<Exit>, Error> {
        let Some(process) = self.process.as_mut() else {
            return Ok(None);
        };
        let Some(status) = process.try_wait()? else {
            return Ok(None);
        };
        self.process = None;
        Ok(Some(self.files.finish(status)?))
    }
    /// Stops the engine process.
    ///
    /// # Errors
    /// Returns process-control failures.
    pub fn force_stop(&mut self) -> Result<(), Error> {
        if let Some(process) = self.process.as_mut() {
            process.kill()?;
        }
        Ok(())
    }
    /// Waits for completion.
    ///
    /// # Errors
    /// Returns process or result-protocol failures.
    pub fn wait(mut self) -> Result<Exit, Error> {
        let status = self.process.as_mut().ok_or(Error::InvalidState)?.wait()?;
        self.process = None;
        self.files.finish(status)
    }
    /// Waits and captures piped standard output and error.
    ///
    /// # Errors
    /// Returns process or result-protocol failures.
    pub fn output(mut self) -> Result<Output, Error> {
        let process = self.process.take().ok_or(Error::InvalidState)?;
        let output = process.wait_with_output()?;
        let status = self.files.finish(output.status)?;
        Ok(Output {
            status,
            stdout: output.stdout,
            stderr: output.stderr,
        })
    }
}
impl Drop for Child {
    fn drop(&mut self) {
        if let Some(process) = self.process.as_mut() {
            let _ = process.kill();
            let _ = process.wait();
        }
    }
}
