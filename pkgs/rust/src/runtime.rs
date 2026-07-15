use crate::{result, Error, Exit};
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    process::ExitStatus,
    sync::atomic::{AtomicU64, Ordering},
};

static UNIQUE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug)]
pub(crate) struct RuntimeDir {
    root: PathBuf,
}
impl RuntimeDir {
    pub(crate) fn create() -> std::io::Result<Self> {
        use std::os::unix::fs::DirBuilderExt;
        let root = std::env::temp_dir().join(format!(
            "hl-engine-rs-{}-{}",
            std::process::id(),
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::DirBuilder::new().mode(0o700).create(&root)?;
        Ok(Self { root })
    }
    pub(crate) fn install(&self, name: &str, bytes: &[u8]) -> std::io::Result<PathBuf> {
        let path = self.root.join(name);
        write(&path, bytes, true)?;
        Ok(path)
    }
    pub(crate) fn files(&self) -> std::io::Result<LaunchFiles> {
        let id = UNIQUE.fetch_add(1, Ordering::Relaxed);
        let files = LaunchFiles {
            config: self.root.join(format!("launch-{id}.bin")),
            result: self.root.join(format!("result-{id}.bin")),
        };
        write(&files.result, &[], false)?;
        Ok(files)
    }
}

#[derive(Debug)]
pub(crate) struct LaunchFiles {
    config: PathBuf,
    result: PathBuf,
}
impl LaunchFiles {
    pub(crate) fn result_path(&self) -> &Path {
        &self.result
    }
    pub(crate) fn config_path(&self) -> &Path {
        &self.config
    }
    pub(crate) fn write_config(&self, bytes: &[u8]) -> std::io::Result<()> {
        write(&self.config, bytes, false)
    }
    pub(crate) fn finish(&self, status: ExitStatus) -> Result<Exit, Error> {
        let _ = fs::remove_file(&self.config);
        result::read(&self.result, status)
    }
}
impl Drop for LaunchFiles {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.config);
        let _ = fs::remove_file(&self.result);
    }
}

fn write(path: &Path, bytes: &[u8], executable: bool) -> std::io::Result<()> {
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
