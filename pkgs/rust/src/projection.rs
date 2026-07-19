use crate::{
    extension::{FileSource, NamespaceEntry},
    spec::{SpecError, SpecErrorCategory},
    Mount,
};
use std::{
    fs,
    io::Write,
    os::unix::{fs::symlink, fs::PermissionsExt},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

static NEXT_PROJECTION: AtomicU64 = AtomicU64::new(1);

/// Host backing retained for the lifetime of a projected guest namespace.
#[derive(Debug)]
pub(crate) struct Projection {
    root: PathBuf,
}

impl Projection {
    pub(crate) fn materialize(
        entries: &[&NamespaceEntry],
    ) -> Result<(Self, Vec<Mount>), SpecError> {
        let root = create_root()?;
        let projection = Self { root };
        let result = projection.populate(entries);
        match result {
            Ok(mounts) => Ok((projection, mounts)),
            Err(error) => Err(error),
        }
    }

    fn populate(&self, entries: &[&NamespaceEntry]) -> Result<Vec<Mount>, SpecError> {
        let mut ordered = entries.to_vec();
        ordered.sort_by(|left, right| {
            left.path()
                .components()
                .count()
                .cmp(&right.path().components().count())
                .then_with(|| left.path().cmp(right.path()))
        });
        for entry in &ordered {
            self.create(entry)?;
        }
        let paths = ordered.iter().map(|entry| entry.path()).collect::<Vec<_>>();
        let mut mounts = Vec::new();
        for entry in &ordered {
            let path = entry.path();
            if paths
                .iter()
                .any(|candidate| *candidate != path && path.starts_with(candidate))
            {
                continue;
            }
            if matches!(entry, NamespaceEntry::File(value) if matches!(value.source, FileSource::Mutable(_)))
            {
                mounts.push(Mount::read_write(self.host_path(path)?, path));
            } else {
                mounts.push(Mount::read_only(self.host_path(path)?, path));
            }
        }
        for entry in entries {
            let has_ancestor = paths
                .iter()
                .any(|candidate| *candidate != entry.path() && entry.path().starts_with(candidate));
            if has_ancestor
                && matches!(entry, NamespaceEntry::File(value) if matches!(value.source, FileSource::Mutable(_)))
            {
                mounts.push(Mount::read_write(
                    self.host_path(entry.path())?,
                    entry.path(),
                ));
            }
        }
        Ok(mounts)
    }

    fn create(&self, entry: &NamespaceEntry) -> Result<(), SpecError> {
        let host = self.host_path(entry.path())?;
        if let Some(parent) = host.parent() {
            fs::create_dir_all(parent).map_err(projection_io)?;
        }
        match entry {
            NamespaceEntry::Directory(value) => {
                fs::create_dir(&host)
                    .or_else(|error| {
                        if error.kind() == std::io::ErrorKind::AlreadyExists {
                            Ok(())
                        } else {
                            Err(error)
                        }
                    })
                    .map_err(projection_io)?;
                fs::set_permissions(&host, fs::Permissions::from_mode(value.metadata.mode))
                    .map_err(projection_io)?;
            }
            NamespaceEntry::File(value) => {
                let (FileSource::Immutable(bytes) | FileSource::Mutable(bytes)) = &value.source
                else {
                    return Err(unsupported(
                        entry.path(),
                        "only immutable projected files are implemented",
                    ));
                };
                let mut file = fs::File::create(&host).map_err(projection_io)?;
                file.write_all(bytes).map_err(projection_io)?;
                file.sync_all().map_err(projection_io)?;
                fs::set_permissions(&host, fs::Permissions::from_mode(value.metadata.mode))
                    .map_err(projection_io)?;
            }
            NamespaceEntry::Symlink(value) => {
                symlink(&value.target, &host).map_err(projection_io)?;
            }
            _ => {
                return Err(unsupported(
                    entry.path(),
                    "projected node kind has no runtime implementation",
                ))
            }
        }
        Ok(())
    }

    fn host_path(&self, guest: &Path) -> Result<PathBuf, SpecError> {
        guest
            .strip_prefix("/")
            .map(|path| self.root.join(path))
            .map_err(|_| unsupported(guest, "projected paths must be absolute"))
    }
}

impl Drop for Projection {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn create_root() -> Result<PathBuf, SpecError> {
    for _ in 0..32 {
        let sequence = NEXT_PROJECTION.fetch_add(1, Ordering::Relaxed);
        let path =
            std::env::temp_dir().join(format!("hl-projection-{}-{sequence}", std::process::id()));
        match fs::create_dir(&path) {
            Ok(()) => return Ok(path),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
            Err(error) => return Err(projection_io(error)),
        }
    }
    Err(unsupported(
        Path::new("/"),
        "could not allocate projection backing storage",
    ))
}

fn projection_io(error: std::io::Error) -> SpecError {
    let message = error.to_string();
    drop(error);
    SpecError {
        category: SpecErrorCategory::Invalid,
        field: "extensions.namespace".into(),
        resource: None,
        context: format!("cannot materialize namespace projection: {message}"),
    }
}

fn unsupported(path: &Path, context: &str) -> SpecError {
    SpecError {
        category: SpecErrorCategory::Unsupported,
        field: "extensions.namespace".into(),
        resource: Some(crate::spec::SpecResource::Path(path.to_owned())),
        context: context.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::extension::{FileEntry, Metadata, SymlinkEntry};
    use std::sync::Arc;

    fn lock() -> std::sync::MutexGuard<'static, ()> {
        static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
        LOCK.lock().unwrap()
    }

    fn roots() -> usize {
        let prefix = format!("hl-projection-{}-", std::process::id());
        fs::read_dir(std::env::temp_dir())
            .into_iter()
            .flatten()
            .flatten()
            .filter(|entry| entry.file_name().to_string_lossy().starts_with(&prefix))
            .count()
    }

    #[test]
    fn failed_materialization_revokes_partial_backing() {
        let _lock = lock();
        let link = NamespaceEntry::Symlink(SymlinkEntry {
            path: "/parent".into(),
            target: "target".into(),
            uid: 0,
            gid: 0,
        });
        let child = NamespaceEntry::File(FileEntry {
            path: "/parent/child".into(),
            metadata: Metadata {
                mode: 0o444,
                uid: 0,
                gid: 0,
            },
            source: FileSource::Mutable(Arc::from(&b"bytes"[..])),
        });
        let before = roots();
        assert!(Projection::materialize(&[&link, &child]).is_err());
        assert_eq!(roots(), before);
    }

    #[test]
    fn mutable_backing_is_revoked_when_launch_storage_drops() {
        let _lock = lock();
        let file = NamespaceEntry::File(FileEntry {
            path: "/mutable".into(),
            metadata: Metadata {
                mode: 0o600,
                uid: 0,
                gid: 0,
            },
            source: FileSource::Mutable(Arc::from(&b"initial"[..])),
        });
        let before = roots();
        let (projection, _) = Projection::materialize(&[&file]).unwrap();
        assert_eq!(roots(), before + 1);
        drop(projection);
        assert_eq!(roots(), before);
    }
}
