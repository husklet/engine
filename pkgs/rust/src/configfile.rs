use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};
static UNIQUE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug)]
pub(crate) struct ConfigFile {
    path: PathBuf,
}
impl ConfigFile {
    pub(crate) fn create(bytes: &[u8]) -> std::io::Result<Self> {
        let path = std::env::temp_dir().join(format!(
            "hl-engine-config-{}-{}",
            std::process::id(),
            UNIQUE.fetch_add(1, Ordering::Relaxed)
        ));
        // Private, not merely new: this file is the complete launch configuration and it lives in a
        // directory every local account can write to.
        let mut file = crate::sys::create_private_file(&path)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        Ok(Self { path })
    }
    pub(crate) fn path(&self) -> &Path {
        &self.path
    }
}
impl Drop for ConfigFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}
