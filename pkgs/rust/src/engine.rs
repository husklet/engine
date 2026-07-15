use crate::{
    artifacts::Artifacts, wire, Child, Command as GuestCommand, Config, Error, Guest, Stdio,
};
use std::{ffi::OsStr, process::Command, sync::OnceLock};

static ARTIFACTS: OnceLock<Result<Artifacts, String>> = OnceLock::new();

/// Installed engine distribution for one host.
#[derive(Clone, Debug)]
pub struct Engine;
impl Engine {
    #[must_use]
    pub const fn new() -> Self {
        Self
    }
    fn artifacts() -> Result<&'static Artifacts, Error> {
        match ARTIFACTS.get_or_init(|| Artifacts::install().map_err(|e| e.to_string())) {
            Ok(artifacts) => Ok(artifacts),
            Err(message) => Err(Error::Distribution(message.clone())),
        }
    }
    #[must_use]
    pub fn command(&self, guest: Guest, program: impl Into<std::ffi::OsString>) -> GuestCommand {
        GuestCommand::new(guest, program.into())
    }
    pub(crate) fn start<I, S>(
        guest: Guest,
        config: &Config,
        program: impl AsRef<OsStr>,
        arguments: I,
        streams: (Stdio, Stdio, Stdio),
    ) -> Result<Child, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        use std::os::unix::ffi::OsStrExt;
        let mut argv = vec![program.as_ref().to_owned()];
        argv.extend(arguments.into_iter().map(|v| v.as_ref().to_owned()));
        if argv[0].as_bytes().is_empty() {
            return Err(Error::InvalidConfig("program must not be empty"));
        }
        let artifacts = Self::artifacts()?;
        let files = artifacts.runtime.files()?;
        files.write_config(&wire::encode(config, &argv, Some(files.result_path()))?)?;
        let executable = match guest {
            Guest::Aarch64 => &artifacts.aarch64,
            Guest::X86_64 => &artifacts.x86_64,
        };
        let mut command = Command::new(executable);
        command.arg("--configfile").arg(files.config_path());
        command
            .stdin(stream(streams.0))
            .stdout(stream(streams.1))
            .stderr(stream(streams.2));
        Ok(Child {
            process: Some(command.spawn()?),
            files,
        })
    }
}
impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}
fn stream(value: Stdio) -> std::process::Stdio {
    match value {
        Stdio::Inherit => std::process::Stdio::inherit(),
        Stdio::Null => std::process::Stdio::null(),
        Stdio::Piped => std::process::Stdio::piped(),
    }
}
