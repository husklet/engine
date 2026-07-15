use crate::{Mount, Sandbox};
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;

/// Configuration for one Linux launch and its initial process.
#[derive(Clone, Debug, Default)]
pub struct Config {
    pub(crate) rootfs: Option<PathBuf>,
    pub(crate) working_directory: Option<OsString>,
    pub(crate) hostname: Option<OsString>,
    pub(crate) environment: Vec<(OsString, OsString)>,
    pub(crate) memory_limit: u64,
    pub(crate) pid_limit: u32,
    pub(crate) cpu_limit: u32,
    pub(crate) uid: Option<i32>,
    pub(crate) gid: Option<i32>,
    pub(crate) rootfs_read_only: bool,
    pub(crate) network_isolated: bool,
    pub(crate) sandbox: Sandbox,
    pub(crate) translation_cache: Option<PathBuf>,
    pub(crate) mounts: Vec<Mount>,
}

impl Config {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }
    #[must_use]
    pub fn root(mut self, path: impl Into<PathBuf>) -> Self {
        self.rootfs = Some(path.into());
        self
    }
    #[must_use]
    pub fn working_dir(mut self, path: impl Into<OsString>) -> Self {
        self.working_directory = Some(path.into());
        self
    }
    #[must_use]
    pub fn hostname(mut self, value: impl Into<OsString>) -> Self {
        self.hostname = Some(value.into());
        self
    }
    #[must_use]
    pub fn env(mut self, name: impl Into<OsString>, value: impl Into<OsString>) -> Self {
        self.set_environment(name.into(), value.into());
        self
    }
    pub(crate) fn set_environment(&mut self, name: OsString, value: OsString) {
        self.environment.retain(|(current, _)| current != &name);
        self.environment.push((name, value));
    }
    pub(crate) fn environment_value(&self, name: &OsStr) -> Option<&OsStr> {
        self.environment
            .iter()
            .find(|(current, _)| current == name)
            .map(|(_, value)| value.as_os_str())
    }
    #[must_use]
    pub const fn memory_limit(mut self, value: u64) -> Self {
        self.memory_limit = value;
        self
    }
    #[must_use]
    pub const fn process_limit(mut self, value: u32) -> Self {
        self.pid_limit = value;
        self
    }
    #[must_use]
    pub const fn cpu_limit(mut self, value: u32) -> Self {
        self.cpu_limit = value;
        self
    }
    #[must_use]
    pub const fn uid(mut self, value: i32) -> Self {
        self.uid = Some(value);
        self
    }
    #[must_use]
    pub const fn gid(mut self, value: i32) -> Self {
        self.gid = Some(value);
        self
    }
    #[must_use]
    pub const fn read_only_root(mut self, value: bool) -> Self {
        self.rootfs_read_only = value;
        self
    }
    #[must_use]
    pub const fn network(mut self, value: bool) -> Self {
        self.network_isolated = value;
        self
    }
    #[must_use]
    pub const fn sandbox(mut self, value: Sandbox) -> Self {
        self.sandbox = value;
        self
    }
    #[must_use]
    pub fn translation_cache(mut self, path: impl Into<PathBuf>) -> Self {
        self.translation_cache = Some(path.into());
        self
    }
}
