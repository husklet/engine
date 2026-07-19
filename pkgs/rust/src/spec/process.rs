use std::ffi::OsString;

use crate::{Domain, Size};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ProcessSpec {
    pub executable: OsString,
    pub argv: Vec<OsString>,
    pub env: Vec<(OsString, OsString)>,
    pub cwd: OsString,
    pub umask: u32,
    pub terminal: Option<Size>,
    /// Existing process domain joined by an exec-like launch.
    pub domain: Option<Domain>,
}

impl ProcessSpec {
    #[must_use]
    pub fn new(executable: impl Into<OsString>) -> Self {
        let executable = executable.into();
        Self {
            argv: vec![executable.clone()],
            executable,
            env: Vec::new(),
            cwd: OsString::from("/"),
            umask: 0o022,
            terminal: None,
            domain: None,
        }
    }
}
