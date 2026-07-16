use crate::{
    ffi, runtime::ConfigFile, wire, Child, Command as GuestCommand, Config, Error, Guest, Size,
    Stdio,
};
use std::{
    ffi::{CString, OsStr},
    fs::{File, OpenOptions},
    os::fd::AsRawFd,
    os::unix::ffi::OsStrExt,
    sync::OnceLock,
};

static EXECUTABLE: OnceLock<Result<CString, String>> = OnceLock::new();

/// Entry point for constructing guest commands.
#[derive(Clone, Copy, Debug, Default)]
pub struct Engine;
impl Engine {
    #[must_use]
    pub const fn new() -> Self {
        Self
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
        terminal: Option<Size>,
    ) -> Result<Child, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        let mut argv = vec![program.as_ref().to_owned()];
        argv.extend(arguments.into_iter().map(|value| value.as_ref().to_owned()));
        if argv[0].as_bytes().is_empty() {
            return Err(Error::InvalidConfig("program must not be empty"));
        }
        let encoded = wire::encode(config, &argv, None)?;
        let domain = crate::Domain::from_identity(wire::domain(&encoded));
        let config = ConfigFile::create(&encoded)?;
        let executable = EXECUTABLE
            .get_or_init(|| {
                let path = std::env::current_exe().map_err(|error| error.to_string())?;
                CString::new(path.as_os_str().as_bytes())
                    .map_err(|_| "current executable contains NUL".into())
            })
            .as_ref()
            .map_err(|message| Error::Distribution(message.clone()))?;
        let config_path = CString::new(config.path().as_os_str().as_bytes())
            .map_err(|_| Error::InvalidConfig("config path contains NUL"))?;
        let (process, stdin, stdout, stderr, terminal) = if let Some(size) = terminal {
            let (process, file) =
                ffi::start_terminal(executable, guest_number(guest), &config_path, size.native())
                    .map_err(native_error)?;
            (process, None, None, None, Some(crate::Terminal::new(file)))
        } else {
            let (input, stdin, input_child) = prepare(streams.0, true)?;
            let (output, stdout, output_child) = prepare(streams.1, false)?;
            let (error, stderr, error_child) = prepare(streams.2, false)?;
            let native = ffi::Streams {
                input,
                output,
                error,
            };
            let process = ffi::start(executable, guest_number(guest), &config_path, &native)
                .map_err(native_error)?;
            drop((input_child, output_child, error_child));
            (process, stdin, stdout, stderr, None)
        };
        Ok(Child {
            process: Some(process),
            _config: config,
            stdin,
            stdout,
            stderr,
            terminal,
            domain,
        })
    }
}

fn prepare(value: Stdio, input: bool) -> Result<(i32, Option<File>, Option<File>), Error> {
    match value {
        Stdio::Inherit => Ok((-1, None, None)),
        Stdio::Null => {
            let file = OpenOptions::new()
                .read(input)
                .write(!input)
                .open("/dev/null")?;
            Ok((file.as_raw_fd(), None, Some(file)))
        }
        Stdio::Piped => {
            let (read, write) = ffi::pipe_pair()?;
            let (parent, child) = if input { (write, read) } else { (read, write) };
            Ok((child.as_raw_fd(), Some(parent), Some(child)))
        }
    }
}
const fn guest_number(guest: Guest) -> u32 {
    match guest {
        Guest::Aarch64 => 1,
        Guest::X86_64 => 2,
    }
}
fn native_error(status: i32) -> Error {
    Error::Engine { status, detail: 0 }
}
