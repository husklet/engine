use crate::runtime::RuntimeDir;
use std::{io, path::PathBuf};

#[cfg(target_os = "macos")]
const AARCH64: &[u8] = include_bytes!("../assets/bin/aarch64-apple-darwin/hl-engine-linux-aarch64");
#[cfg(target_os = "macos")]
const X86_64: &[u8] = include_bytes!("../assets/bin/aarch64-apple-darwin/hl-engine-linux-x86_64");
#[cfg(target_os = "linux")]
const AARCH64: &[u8] =
    include_bytes!("../assets/bin/aarch64-unknown-linux-gnu/hl-engine-linux-aarch64");
#[cfg(target_os = "linux")]
const X86_64: &[u8] =
    include_bytes!("../assets/bin/aarch64-unknown-linux-gnu/hl-engine-linux-x86_64");

/// Temporary internal distribution fallback pending unified native archive linkage.
#[derive(Debug)]
pub(crate) struct Artifacts {
    pub(crate) runtime: RuntimeDir,
    pub(crate) aarch64: PathBuf,
    pub(crate) x86_64: PathBuf,
}
impl Artifacts {
    pub(crate) fn install() -> io::Result<Self> {
        let runtime = RuntimeDir::create()?;
        let aarch64 = runtime.install("hl-engine-linux-aarch64", AARCH64)?;
        let x86_64 = runtime.install("hl-engine-linux-x86_64", X86_64)?;
        Ok(Self {
            runtime,
            aarch64,
            x86_64,
        })
    }
}
