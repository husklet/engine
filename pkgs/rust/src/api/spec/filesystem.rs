//! Filesystem tree, coherence, and initial ownership models.

use std::path::PathBuf;

use crate::api::extension::{HostBindEntry, ProviderId};

use super::{SpecError, SpecErrorCategory, SpecResource};

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct FilesystemSpec {
    pub root: Option<TreeSource>,
    pub read_only: bool,
    pub mounts: Vec<HostBindEntry>,
    pub coherence: Option<CoherenceHandle>,
    pub ownership: Vec<InitialOwnership>,
}
/// Opaque channel used to notify the engine about externally changed filesystem identities.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct CoherenceHandle(PathBuf);

impl CoherenceHandle {
    /// Grants the engine access to a host-backed coherence notification channel.
    ///
    /// The current backend implements the channel using a generation file. Callers treat the
    /// returned value as an opaque capability rather than deriving guest-visible path policy from it.
    ///
    /// # Errors
    /// Returns a specification error when the granted host path is not absolute.
    pub fn from_host_file(path: impl Into<PathBuf>) -> Result<Self, SpecError> {
        let path = path.into();
        if !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0) {
            return Err(SpecError {
                category: SpecErrorCategory::Invalid,
                field: "filesystem.coherence".into(),
                resource: Some(SpecResource::Path(path)),
                context: "coherence host paths must be absolute".into(),
            });
        }
        Ok(Self(path))
    }

    #[doc(hidden)]
    #[must_use]
    pub fn host_path(&self) -> &std::path::Path {
        &self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct InitialOwnership {
    pub path: PathBuf,
    pub uid: u32,
    pub gid: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TreeSource {
    HostDirectory(PathBuf),
    ImageLayer(ImageLayerHandle),
    Overlay {
        lower: Vec<TreeSource>,
        upper: PathBuf,
        work: PathBuf,
    },
    Provider(ProviderId),
}
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ImageLayerHandle(pub u64);
