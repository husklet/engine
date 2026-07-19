//! CPU and process identity launch policy.

use std::{collections::BTreeSet, ffi::OsString};

use crate::Stdio;

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct CpuSpec {
    pub count: Option<u32>,
    pub features: BTreeSet<String>,
    pub model: Option<String>,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct IdentitySpec {
    pub uid: Option<u32>,
    pub gid: Option<u32>,
    pub supplementary_groups: Vec<u32>,
    pub hostname: Option<OsString>,
    pub domain_name: Option<OsString>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProcessIo {
    pub stdin: Stdio,
    pub stdout: Stdio,
    pub stderr: Stdio,
}

impl Default for ProcessIo {
    fn default() -> Self {
        Self {
            stdin: Stdio::Inherit,
            stdout: Stdio::Inherit,
            stderr: Stdio::Inherit,
        }
    }
}
