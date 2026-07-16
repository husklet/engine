use crate::{Child, Config, Container, Engine, Error, Exit, Guest, Mount, Output, Size, Stdio};
use std::ffi::OsString;

/// A configured guest process command.
pub struct Command {
    guest: Guest,
    program: OsString,
    args: Vec<OsString>,
    config: Config,
    container: Container,
    stdin: Stdio,
    stdout: Stdio,
    stderr: Stdio,
    terminal: Option<Size>,
}
impl Command {
    pub(crate) fn new(guest: Guest, program: OsString) -> Self {
        Self {
            guest,
            program,
            args: Vec::new(),
            config: Config::new(),
            container: Container::default(),
            stdin: Stdio::inherit(),
            stdout: Stdio::inherit(),
            stderr: Stdio::inherit(),
            terminal: None,
        }
    }
    #[must_use]
    pub fn config(mut self, value: Config) -> Self {
        self.config = value;
        self
    }
    #[must_use]
    pub fn arg(mut self, value: impl Into<OsString>) -> Self {
        self.args.push(value.into());
        self
    }
    #[must_use]
    pub fn args<I, S>(mut self, values: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<OsString>,
    {
        self.args.extend(values.into_iter().map(Into::into));
        self
    }
    #[must_use]
    pub fn env(mut self, name: impl Into<OsString>, value: impl Into<OsString>) -> Self {
        self.config = self.config.env(name, value);
        self
    }
    #[must_use]
    pub fn current_dir(mut self, value: impl Into<OsString>) -> Self {
        self.config = self.config.working_dir(value);
        self
    }
    #[must_use]
    pub const fn stdin(mut self, value: Stdio) -> Self {
        self.stdin = value;
        self
    }
    #[must_use]
    pub const fn stdout(mut self, value: Stdio) -> Self {
        self.stdout = value;
        self
    }
    #[must_use]
    pub const fn stderr(mut self, value: Stdio) -> Self {
        self.stderr = value;
        self
    }
    /// Gives the guest a controlling terminal with merged output.
    #[must_use]
    pub const fn terminal(mut self, size: Size) -> Self {
        self.terminal = Some(size);
        self
    }
    #[must_use]
    pub fn apply(mut self, edit: impl FnOnce(&mut Container)) -> Self {
        edit(&mut self.container);
        self
    }
    #[must_use]
    pub fn mount(mut self, value: Mount) -> Self {
        self.container.mount(value);
        self
    }
    fn resolved(mut self) -> Result<Self, Error> {
        self.container.clone().resolve(&mut self.config)?;
        Ok(self)
    }
    /// Starts the configured guest process.
    ///
    /// # Errors
    /// Returns validation, distribution, or process failures.
    pub fn spawn(self) -> Result<Child, Error> {
        let this = self.resolved()?;
        Engine::start(
            this.guest,
            &this.config,
            &this.program,
            this.args,
            (this.stdin, this.stdout, this.stderr),
            this.terminal,
        )
    }
    /// Runs the command and returns its typed status.
    ///
    /// # Errors
    /// Returns launch, wait, or result-protocol failures.
    pub fn status(self) -> Result<Exit, Error> {
        self.spawn()?.wait()
    }
    /// Runs the command and captures standard output and error.
    ///
    /// # Errors
    /// Returns launch, wait, I/O, or result-protocol failures.
    pub fn output(mut self) -> Result<Output, Error> {
        self.stdout = Stdio::piped();
        self.stderr = Stdio::piped();
        self.spawn()?.output()
    }
}
