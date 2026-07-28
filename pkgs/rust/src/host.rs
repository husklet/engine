use std::path::PathBuf;

pub(crate) struct HostPaths;

impl HostPaths {
    pub(crate) fn temporary(name: &str) -> PathBuf {
        std::env::temp_dir().join(name)
    }
}
