use crate::api::extension::NamespaceEntry;
use crate::provider::LinuxError;

/// Transactional namespace installation port.
pub trait Namespace: Send + Sync {
    /// Validates and atomically installs all entries.
    ///
    /// # Errors
    /// Returns a Linux error without changing the namespace when validation or installation fails.
    fn install(&self, entries: Vec<NamespaceEntry>) -> Result<NamespaceGeneration, LinuxError>;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NamespaceGeneration(pub u64);
