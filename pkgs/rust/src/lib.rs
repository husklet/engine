//! Safe process-isolated lifecycle API for the standalone HL engine.
//!
//! Both Linux guest engines are built from the vendored C implementation and
//! embedded in this crate. [`Engine`] selects the correct executable through
//! [`Guest`], writes the typed ABI-5 launch record, and owns its lifecycle.

#![forbid(unsafe_code)]

mod integration;
pub use integration::{Integration, IntegrationContext, IntegrationId, LaunchPlan, Mount};

#[cfg(not(any(
    all(target_arch = "aarch64", target_os = "macos"),
    all(target_arch = "aarch64", target_os = "linux")
)))]
compile_error!("hl-engine supports only aarch64-apple-darwin and aarch64-unknown-linux-gnu hosts");

use std::ffi::{OsStr, OsString};
use std::fmt;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStderr, ChildStdin, ChildStdout, Command, ExitStatus};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::OnceLock;

const AARCH64_ENGINE: &[u8] = include_bytes!(concat!(
    env!("HL_ENGINE_NATIVE_DIR"),
    "/hl-engine-linux-aarch64"
));
const X86_64_ENGINE: &[u8] = include_bytes!(concat!(
    env!("HL_ENGINE_NATIVE_DIR"),
    "/hl-engine-linux-x86_64"
));
const CONFIG_MAGIC: u32 = 0x484c_4346;
const CONFIG_ABI: u32 = 5;
const CONFIG_HEADER_SIZE: u32 = 144;
const RESULT_MAGIC: u32 = 0x484c_5253;
const RESULT_ABI: u32 = 1;
const RESULT_SIZE: usize = 32;
static ARTIFACTS: OnceLock<Result<Artifacts, String>> = OnceLock::new();
static UNIQUE: AtomicU64 = AtomicU64::new(0);

/// Linux instruction set executed by the engine.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Guest {
    /// Linux `AArch64`.
    Aarch64,
    /// Linux x86-64 translated to the `AArch64` host CPU.
    X86_64,
}

/// Isolation level applied to the Linux box.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Sandbox {
    /// Normal host-backed execution.
    #[default]
    Disabled,
    /// Full sentry and worker isolation.
    Enabled,
    /// Route authority through the sentry without worker confinement.
    SentryOnly,
}

/// Ownership policy for a standard stream of the engine process.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Stream {
    /// Share the caller's stream.
    #[default]
    Inherit,
    /// Connect the stream to the null device.
    Null,
    /// Create an owned pipe available from [`RunningBox`].
    Piped,
}

/// Explicit standard-stream policy for a launch.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Stdio {
    /// Standard input ownership.
    pub stdin: Stream,
    /// Standard output ownership.
    pub stdout: Stream,
    /// Standard error ownership.
    pub stderr: Stream,
}

/// Configuration for one Linux box and its initial process.
#[derive(Clone, Debug, Default)]
pub struct BoxConfig {
    rootfs: Option<PathBuf>,
    working_directory: Option<OsString>,
    hostname: Option<OsString>,
    environment: Vec<(OsString, OsString)>,
    memory_limit: u64,
    pid_limit: u32,
    cpu_limit: u32,
    uid: Option<i32>,
    gid: Option<i32>,
    rootfs_read_only: bool,
    network_isolated: bool,
    sandbox: Sandbox,
    translation_cache: Option<PathBuf>,
    mounts: Vec<Mount>,
}

impl BoxConfig {
    /// Creates an empty configuration using engine defaults.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Selects the Linux root filesystem.
    #[must_use]
    pub fn rootfs(mut self, path: impl Into<PathBuf>) -> Self {
        self.rootfs = Some(path.into());
        self
    }

    /// Selects the initial guest working directory.
    #[must_use]
    pub fn working_directory(mut self, path: impl Into<OsString>) -> Self {
        self.working_directory = Some(path.into());
        self
    }

    /// Sets the guest hostname.
    #[must_use]
    pub fn hostname(mut self, hostname: impl Into<OsString>) -> Self {
        self.hostname = Some(hostname.into());
        self
    }

    /// Adds or replaces one guest environment variable.
    #[must_use]
    pub fn environment(mut self, name: impl Into<OsString>, value: impl Into<OsString>) -> Self {
        let name = name.into();
        self.environment.retain(|(current, _)| current != &name);
        self.environment.push((name, value.into()));
        self
    }

    fn set_environment(&mut self, name: OsString, value: OsString) {
        self.environment.retain(|(current, _)| current != &name);
        self.environment.push((name, value));
    }

    fn environment_value(&self, name: &OsStr) -> Option<&OsStr> {
        self.environment
            .iter()
            .find(|(current, _)| current == name)
            .map(|(_, value)| value.as_os_str())
    }

    /// Limits guest-addressable memory in bytes. Zero uses the engine default.
    #[must_use]
    pub const fn memory_limit(mut self, bytes: u64) -> Self {
        self.memory_limit = bytes;
        self
    }

    /// Limits Linux processes. Zero uses the engine default.
    #[must_use]
    pub const fn pid_limit(mut self, count: u32) -> Self {
        self.pid_limit = count;
        self
    }

    /// Limits visible CPUs. Zero uses the engine default.
    #[must_use]
    pub const fn cpu_limit(mut self, count: u32) -> Self {
        self.cpu_limit = count;
        self
    }

    /// Sets the guest UID and GID.
    #[must_use]
    pub const fn identity(mut self, uid: i32, gid: i32) -> Self {
        self.uid = Some(uid);
        self.gid = Some(gid);
        self
    }

    /// Makes the root filesystem read-only.
    #[must_use]
    pub const fn rootfs_read_only(mut self, enabled: bool) -> Self {
        self.rootfs_read_only = enabled;
        self
    }

    /// Isolates guest networking.
    #[must_use]
    pub const fn network_isolated(mut self, enabled: bool) -> Self {
        self.network_isolated = enabled;
        self
    }

    /// Selects the isolation policy.
    #[must_use]
    pub const fn sandbox(mut self, sandbox: Sandbox) -> Self {
        self.sandbox = sandbox;
        self
    }

    /// Uses a persistent translated-code cache directory.
    #[must_use]
    pub fn translation_cache(mut self, path: impl Into<PathBuf>) -> Self {
        self.translation_cache = Some(path.into());
        self
    }
}

/// Installed engine distribution for one host.
#[derive(Clone, Debug)]
pub struct Engine {
    artifacts: &'static Artifacts,
}

impl Engine {
    /// Installs the two embedded production engines into a private directory.
    ///
    /// # Errors
    ///
    /// Returns an I/O error when the private executable directory cannot be created.
    pub fn new() -> Result<Self, Error> {
        let result =
            ARTIFACTS.get_or_init(|| Artifacts::install().map_err(|error| error.to_string()));
        match result {
            Ok(artifacts) => Ok(Self { artifacts }),
            Err(message) => Err(Error::Distribution(message.clone())),
        }
    }

    /// Starts a composable typed launch for one guest ISA.
    #[must_use]
    pub fn box_builder(&self, guest: Guest) -> BoxBuilder<'_> {
        BoxBuilder {
            engine: self,
            guest,
            config: BoxConfig::new(),
            integrations: Vec::new(),
            devices: Vec::new(),
            stdio: Stdio::default(),
        }
    }

    /// Spawns a Linux process without invoking a shell.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid launch strings, distribution I/O, or process creation failure.
    pub fn spawn<I, S>(
        &self,
        guest: Guest,
        config: &BoxConfig,
        program: impl AsRef<OsStr>,
        arguments: I,
    ) -> Result<RunningBox, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        self.spawn_with_stdio(guest, config, program, arguments, Stdio::default())
    }

    /// Spawns a Linux process with an explicit standard-stream ownership policy.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid launch strings, distribution I/O, or process creation failure.
    pub fn spawn_with_stdio<I, S>(
        &self,
        guest: Guest,
        config: &BoxConfig,
        program: impl AsRef<OsStr>,
        arguments: I,
        stdio: Stdio,
    ) -> Result<RunningBox, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        use std::os::unix::ffi::OsStrExt;
        let mut argv = vec![program.as_ref().to_owned()];
        argv.extend(
            arguments
                .into_iter()
                .map(|argument| argument.as_ref().to_owned()),
        );
        if argv[0].as_bytes().is_empty() {
            return Err(Error::InvalidConfig("program must not be empty"));
        }
        let config_path = self.artifacts.directory.join(format!(
            "launch-{}-{}.bin",
            std::process::id(),
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        let result_path = self.artifacts.directory.join(format!(
            "result-{}-{}.bin",
            std::process::id(),
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        write_private(&result_path, &[], false)?;
        let wire = match encode_config(config, &argv, Some(&result_path)) {
            Ok(wire) => wire,
            Err(error) => {
                let _ = fs::remove_file(&result_path);
                return Err(error);
            }
        };
        if let Err(error) = write_private(&config_path, &wire, false) {
            let _ = fs::remove_file(&result_path);
            return Err(error.into());
        }
        let executable = match guest {
            Guest::Aarch64 => &self.artifacts.aarch64,
            Guest::X86_64 => &self.artifacts.x86_64,
        };
        let mut command = Command::new(executable);
        command.arg("--configfile").arg(&config_path);
        command
            .stdin(stream(stdio.stdin))
            .stdout(stream(stdio.stdout))
            .stderr(stream(stdio.stderr));
        let child = match command.spawn() {
            Ok(child) => child,
            Err(error) => {
                let _ = fs::remove_file(&config_path);
                let _ = fs::remove_file(&result_path);
                return Err(error.into());
            }
        };
        Ok(RunningBox {
            child: Some(child),
            config_path,
            result_path,
        })
    }

    /// Runs a Linux process to completion.
    ///
    /// # Errors
    ///
    /// Returns any launch or wait error.
    pub fn run<I, S>(
        &self,
        guest: Guest,
        config: &BoxConfig,
        program: impl AsRef<OsStr>,
        arguments: I,
    ) -> Result<Exit, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        self.spawn(guest, config, program, arguments)?.wait()
    }
}

/// Typed, deterministic Linux-box launch builder.
pub struct BoxBuilder<'a> {
    engine: &'a Engine,
    guest: Guest,
    config: BoxConfig,
    integrations: Vec<Box<dyn Integration>>,
    devices: Vec<Mount>,
    stdio: Stdio,
}

impl BoxBuilder<'_> {
    /// Replaces the base box configuration.
    #[must_use]
    pub fn config(mut self, config: BoxConfig) -> Self {
        self.config = config;
        self
    }

    /// Adds an external typed integration in deterministic insertion order.
    #[must_use]
    pub fn integration(mut self, integration: impl Integration + 'static) -> Self {
        self.integrations.push(Box::new(integration));
        self
    }

    /// Exposes a host device node or device directory at a guest path.
    #[must_use]
    pub fn device(
        mut self,
        host: impl Into<PathBuf>,
        guest: impl Into<PathBuf>,
        read_only: bool,
    ) -> Self {
        self.devices.push(Mount {
            host: host.into(),
            guest: guest.into(),
            read_only,
        });
        self
    }

    /// Selects explicit standard-stream ownership.
    #[must_use]
    pub const fn stdio(mut self, stdio: Stdio) -> Self {
        self.stdio = stdio;
        self
    }

    /// Validates, composes, and spawns the launch without invoking a shell.
    ///
    /// # Errors
    ///
    /// Returns attributed integration conflicts, invalid configuration, or process errors.
    pub fn spawn<I, S>(
        mut self,
        program: impl AsRef<OsStr>,
        arguments: I,
    ) -> Result<RunningBox, Error>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        let mut plan = LaunchPlan::default();
        plan.begin(IntegrationId("box.device"))?;
        for device in self.devices {
            plan.mount(device)?;
        }
        plan.finish();
        for integration in &self.integrations {
            let id = integration.id();
            plan.begin(id)?;
            let result = integration.apply(&IntegrationContext::new(self.guest), &mut plan);
            plan.finish();
            result?;
        }
        plan.apply_to(&mut self.config);
        self.engine
            .spawn_with_stdio(self.guest, &self.config, program, arguments, self.stdio)
    }
}

/// Owned running engine process.
#[derive(Debug)]
pub struct RunningBox {
    child: Option<Child>,
    config_path: PathBuf,
    result_path: PathBuf,
}

impl RunningBox {
    /// Host process identifier of the isolated engine.
    #[must_use]
    pub fn id(&self) -> u32 {
        self.child.as_ref().map_or(0, Child::id)
    }

    /// Takes ownership of piped standard input, when requested at launch.
    pub fn take_stdin(&mut self) -> Option<ChildStdin> {
        self.child.as_mut()?.stdin.take()
    }

    /// Takes ownership of piped standard output, when requested at launch.
    pub fn take_stdout(&mut self) -> Option<ChildStdout> {
        self.child.as_mut()?.stdout.take()
    }

    /// Takes ownership of piped standard error, when requested at launch.
    pub fn take_stderr(&mut self) -> Option<ChildStderr> {
        self.child.as_mut()?.stderr.take()
    }

    /// Returns the exit state without blocking.
    ///
    /// # Errors
    ///
    /// Returns an operating-system wait error.
    pub fn try_wait(&mut self) -> Result<Option<Exit>, Error> {
        let Some(child) = self.child.as_mut() else {
            return Ok(None);
        };
        let status = child.try_wait()?;
        if let Some(status) = status {
            self.child = None;
            let _ = fs::remove_file(&self.config_path);
            return Ok(Some(read_result(&self.result_path, status)?));
        }
        Ok(None)
    }

    /// Force-stops the engine and its active guest process.
    ///
    /// # Errors
    ///
    /// Returns an operating-system process-control error.
    pub fn force_stop(&mut self) -> Result<(), Error> {
        if let Some(child) = self.child.as_mut() {
            child.kill()?;
        }
        Ok(())
    }

    /// Waits for the guest to finish.
    ///
    /// # Errors
    ///
    /// Returns an operating-system wait error or [`Error::InvalidState`].
    pub fn wait(mut self) -> Result<Exit, Error> {
        let status = self.child.as_mut().ok_or(Error::InvalidState)?.wait()?;
        self.child = None;
        let _ = fs::remove_file(&self.config_path);
        read_result(&self.result_path, status)
    }
}

impl Drop for RunningBox {
    fn drop(&mut self) {
        if let Some(child) = self.child.as_mut() {
            let _ = child.kill();
            let _ = child.wait();
        }
        let _ = fs::remove_file(&self.config_path);
        let _ = fs::remove_file(&self.result_path);
    }
}

/// Completed guest status from the typed launcher-result channel.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Exit {
    /// Guest exited normally with this code.
    Code(i32),
    /// Guest was terminated by this Linux signal.
    Signal(i32),
    /// Guest fault carrying engine-defined detail.
    Fault { status: i32, detail: u64 },
}

impl Exit {
    /// Guest exit code, when it exited normally.
    #[must_use]
    pub const fn code(self) -> Option<i32> {
        if let Self::Code(code) = self {
            Some(code)
        } else {
            None
        }
    }
    /// Host signal that terminated the engine process.
    #[must_use]
    pub const fn signal(self) -> Option<i32> {
        if let Self::Signal(signal) = self {
            Some(signal)
        } else {
            None
        }
    }
    /// Whether the guest exited successfully.
    #[must_use]
    pub const fn success(self) -> bool {
        matches!(self, Self::Code(0))
    }
}

/// Distribution, configuration, or process error.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    InvalidConfig(&'static str),
    InvalidState,
    Distribution(String),
    /// An external integration rejected the launch.
    Integration {
        id: IntegrationId,
        message: String,
    },
    /// Two integrations claimed the same launch resource differently.
    Conflict {
        resource: String,
        first: IntegrationId,
        second: IntegrationId,
    },
    /// A typed operation is not supported by the current launch ABI.
    Unsupported(&'static str),
    /// Engine failed independently of the guest's exit code.
    Engine {
        status: i32,
        detail: u64,
    },
    /// Launcher-result transport was absent or malformed.
    ResultProtocol(String),
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => error.fmt(formatter),
            Self::InvalidConfig(message) | Self::Unsupported(message) => {
                formatter.write_str(message)
            }
            Self::InvalidState => formatter.write_str("engine process has already been consumed"),
            Self::Distribution(message) => {
                write!(formatter, "cannot install engine distribution: {message}")
            }
            Self::Integration { id, message } => {
                write!(formatter, "integration {}: {message}", id.0)
            }
            Self::Conflict {
                resource,
                first,
                second,
            } => write!(
                formatter,
                "launch conflict for {resource}: {} vs {}",
                first.0, second.0
            ),
            Self::Engine { status, detail } => {
                write!(formatter, "engine failure status={status} detail={detail}")
            }
            Self::ResultProtocol(message) => write!(formatter, "invalid engine result: {message}"),
        }
    }
}
impl std::error::Error for Error {}
impl Error {
    /// Creates a policy failure attributed to one integration.
    #[must_use]
    pub fn integration(id: IntegrationId, message: impl Into<String>) -> Self {
        Self::Integration {
            id,
            message: message.into(),
        }
    }
}
impl From<io::Error> for Error {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

#[derive(Debug)]
struct Artifacts {
    directory: PathBuf,
    aarch64: PathBuf,
    x86_64: PathBuf,
}
impl Artifacts {
    fn install() -> io::Result<Self> {
        let directory = std::env::temp_dir().join(format!(
            "hl-engine-rs-{}-{}",
            std::process::id(),
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&directory)?;
        let aarch64 = directory.join("hl-engine-linux-aarch64");
        let x86_64 = directory.join("hl-engine-linux-x86_64");
        write_private(&aarch64, AARCH64_ENGINE, true)?;
        write_private(&x86_64, X86_64_ENGINE, true)?;
        Ok(Self {
            directory,
            aarch64,
            x86_64,
        })
    }
}

fn write_private(path: &Path, bytes: &[u8], executable: bool) -> io::Result<()> {
    use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
    let mode = if executable { 0o700 } else { 0o600 };
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(mode)
        .open(path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::set_permissions(path, fs::Permissions::from_mode(mode))
}

fn checked_bytes(value: &OsStr) -> Result<&[u8], Error> {
    use std::os::unix::ffi::OsStrExt;
    if value.as_bytes().contains(&0) {
        Err(Error::InvalidConfig(
            "configuration strings must not contain NUL",
        ))
    } else {
        Ok(value.as_bytes())
    }
}

fn stream(stream: Stream) -> std::process::Stdio {
    match stream {
        Stream::Inherit => std::process::Stdio::inherit(),
        Stream::Null => std::process::Stdio::null(),
        Stream::Piped => std::process::Stdio::piped(),
    }
}

fn read_result(path: &Path, transport: ExitStatus) -> Result<Exit, Error> {
    let bytes = fs::read(path)?;
    let _ = fs::remove_file(path);
    if !transport.success() {
        use std::os::unix::process::ExitStatusExt;
        return Err(Error::ResultProtocol(format!(
            "launcher transport exited code={:?} signal={:?}",
            transport.code(),
            transport.signal()
        )));
    }
    if bytes.len() != RESULT_SIZE {
        return Err(Error::ResultProtocol(format!(
            "expected {RESULT_SIZE} bytes, got {}",
            bytes.len()
        )));
    }
    let u32_at = |offset| {
        u32::from_le_bytes(
            bytes[offset..offset + 4]
                .try_into()
                .expect("fixed result field"),
        )
    };
    let i32_at = |offset| {
        i32::from_le_bytes(
            bytes[offset..offset + 4]
                .try_into()
                .expect("fixed result field"),
        )
    };
    let detail = u64::from_le_bytes(bytes[24..32].try_into().expect("fixed result detail"));
    if u32_at(0) != RESULT_MAGIC || u32_at(4) != RESULT_ABI || u32_at(20) != 0 {
        return Err(Error::ResultProtocol(
            "bad magic, ABI, or reserved field".into(),
        ));
    }
    let guest_status = i32_at(12);
    let engine_status = i32_at(16);
    match u32_at(8) {
        1 if engine_status == 0 => Ok(Exit::Code(guest_status)),
        2 if engine_status == 0 => Ok(Exit::Signal(guest_status)),
        3 if engine_status == 0 => Ok(Exit::Fault {
            status: guest_status,
            detail,
        }),
        4 => Err(Error::Engine {
            status: engine_status,
            detail,
        }),
        kind => Err(Error::ResultProtocol(format!(
            "invalid result kind/status {kind}/{engine_status}"
        ))),
    }
}

#[allow(
    clippy::too_many_lines,
    clippy::items_after_statements,
    clippy::cast_sign_loss
)]
fn encode_config(
    config: &BoxConfig,
    arguments: &[OsString],
    result_path: Option<&Path>,
) -> Result<Vec<u8>, Error> {
    let mut pool = vec![0];
    fn string(pool: &mut Vec<u8>, value: Option<&OsStr>) -> Result<u32, Error> {
        let Some(value) = value else {
            return Ok(0);
        };
        let value = checked_bytes(value)?;
        let offset = u32::try_from(pool.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        pool.extend_from_slice(value);
        pool.push(0);
        Ok(offset)
    }
    fn path(pool: &mut Vec<u8>, value: Option<&Path>) -> Result<u32, Error> {
        match value {
            None => Ok(0),
            Some(value) => string(pool, Some(value.as_os_str())),
        }
    }
    let rootfs = path(&mut pool, config.rootfs.as_deref())?;
    let hostname = string(&mut pool, config.hostname.as_deref())?;
    let workdir = string(&mut pool, config.working_directory.as_deref())?;
    let environment_text = if config.environment.is_empty() {
        None
    } else {
        use std::os::unix::ffi::OsStringExt;
        let mut output = Vec::new();
        for (index, (name, value)) in config.environment.iter().enumerate() {
            let name = checked_bytes(name)?;
            let value = checked_bytes(value)?;
            if name.is_empty()
                || name.contains(&b'=')
                || name.contains(&b'\n')
                || value.contains(&b'\n')
            {
                return Err(Error::InvalidConfig("invalid environment record"));
            }
            if index != 0 {
                output.push(b'\n');
            }
            output.extend_from_slice(name);
            output.push(b'=');
            output.extend_from_slice(value);
        }
        Some(OsString::from_vec(output))
    };
    let environment = string(&mut pool, environment_text.as_deref())?;
    let translation_cache = path(&mut pool, config.translation_cache.as_deref())?;
    let volumes_text = if config.mounts.is_empty() {
        None
    } else {
        use std::os::unix::ffi::OsStringExt;
        let mut output = Vec::new();
        for (index, mount) in config.mounts.iter().enumerate() {
            let host = checked_bytes(mount.host.as_os_str())?;
            let guest = checked_bytes(mount.guest.as_os_str())?;
            if host.contains(&b',')
                || host.contains(&b':')
                || guest.contains(&b',')
                || guest.contains(&b':')
            {
                return Err(Error::InvalidConfig(
                    "mount paths must not contain ':' or ','",
                ));
            }
            if index != 0 {
                output.push(b',');
            }
            if mount.read_only {
                output.extend_from_slice(b"ro:");
            } else {
                output.extend_from_slice(b"rw:");
            }
            output.extend_from_slice(guest);
            output.push(b':');
            output.extend_from_slice(host);
        }
        Some(OsString::from_vec(output))
    };
    let volumes = string(&mut pool, volumes_text.as_deref())?;
    let result_path = path(&mut pool, result_path)?;
    let arguments_offset = u32::try_from(pool.len())
        .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
    for argument in arguments {
        let argument = checked_bytes(argument)?;
        pool.extend_from_slice(argument);
        pool.push(0);
    }
    pool.push(0);
    let pool_size = u32::try_from(pool.len())
        .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
    let mut wire = Vec::with_capacity(CONFIG_HEADER_SIZE as usize + pool.len());
    let mut u32s = Vec::with_capacity(32);
    u32s.extend([CONFIG_MAGIC, pool_size, CONFIG_HEADER_SIZE, CONFIG_ABI]);
    for value in u32s {
        wire.extend_from_slice(&value.to_le_bytes());
    }
    wire.extend_from_slice(&config.memory_limit.to_le_bytes());
    let sandbox = match config.sandbox {
        Sandbox::Disabled => 0,
        Sandbox::Enabled => 1,
        Sandbox::SentryOnly => 2,
    };
    let values = [
        config.pid_limit,
        config.cpu_limit,
        config.uid.unwrap_or(-1) as u32,
        config.gid.unwrap_or(-1) as u32,
        u32::from(config.rootfs_read_only),
        sandbox,
        u32::from(config.network_isolated),
        0,
        rootfs,
        0, // lower layers
        hostname,
        0, // network namespace
        0, // publish
        volumes,
        0, // limits
        workdir,
        environment,
        translation_cache,
        0, // network bridge
        0, // ip
        0, // filesystem generation
        arguments_offset,
        0, // translation cache disabled
        0, // egress proxy
        0, // debug log
        0, // checkpoint
        0, // restore
        result_path,
        0, // reserved
    ];
    for value in values {
        wire.extend_from_slice(&value.to_le_bytes());
    }
    wire.extend_from_slice(&0_u32.to_le_bytes()); // C struct tail padding (8-byte alignment).
    debug_assert_eq!(wire.len(), CONFIG_HEADER_SIZE as usize);
    wire.extend_from_slice(&pool);
    Ok(wire)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::process::ExitStatusExt;
    #[test]
    fn launch_wire_has_exact_header_and_pool() {
        let wire = encode_config(
            &BoxConfig::new().rootfs("/root").environment("A", "B"),
            &["/bin/true".into()],
            Some(Path::new("/result")),
        )
        .unwrap();
        assert_eq!(&wire[..4], &CONFIG_MAGIC.to_le_bytes());
        assert_eq!(
            u32::from_le_bytes(wire[8..12].try_into().unwrap()),
            CONFIG_HEADER_SIZE
        );
        assert_eq!(
            wire.len(),
            CONFIG_HEADER_SIZE as usize
                + u32::from_le_bytes(wire[4..8].try_into().unwrap()) as usize
        );
    }
    #[test]
    fn invalid_environment_is_rejected() {
        let error = encode_config(
            &BoxConfig::new().environment("A=B", "x"),
            &["true".into()],
            None,
        )
        .unwrap_err();
        assert!(matches!(error, Error::InvalidConfig(_)));
    }

    #[test]
    fn mount_wire_is_guest_then_host_with_ownership() {
        let mut config = BoxConfig::new();
        config.mounts.push(Mount {
            host: "/host/device".into(),
            guest: "/dev/device".into(),
            read_only: true,
        });
        let wire = encode_config(&config, &["true".into()], Some(Path::new("/result"))).unwrap();
        let offset = u32::from_le_bytes(wire[76..80].try_into().unwrap()) as usize;
        let pool = &wire[CONFIG_HEADER_SIZE as usize..];
        let end = pool[offset..].iter().position(|byte| *byte == 0).unwrap() + offset;
        assert_eq!(&pool[offset..end], b"ro:/dev/device:/host/device");
    }

    fn result_record(kind: u32, guest: i32, engine: i32, detail: u64) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&RESULT_MAGIC.to_le_bytes());
        bytes.extend_from_slice(&RESULT_ABI.to_le_bytes());
        bytes.extend_from_slice(&kind.to_le_bytes());
        bytes.extend_from_slice(&guest.to_le_bytes());
        bytes.extend_from_slice(&engine.to_le_bytes());
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        bytes.extend_from_slice(&detail.to_le_bytes());
        bytes
    }

    #[test]
    fn guest_exit_70_is_not_an_engine_error() {
        let path = std::env::temp_dir().join(format!(
            "hl-rust-result-{}",
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        write_private(&path, &result_record(1, 70, 0, 0), false).unwrap();
        assert_eq!(
            read_result(&path, ExitStatus::from_raw(0)).unwrap(),
            Exit::Code(70)
        );
    }

    #[test]
    fn engine_error_is_typed_and_distinct() {
        let path = std::env::temp_dir().join(format!(
            "hl-rust-result-{}",
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        write_private(&path, &result_record(4, 0, 12, 99), false).unwrap();
        assert!(matches!(
            read_result(&path, ExitStatus::from_raw(0)),
            Err(Error::Engine {
                status: 12,
                detail: 99
            })
        ));
    }
}
