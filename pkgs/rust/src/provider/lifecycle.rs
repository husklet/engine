use crate::provider::{ExtensionError, ProcessId, ResourceId};

pub trait Lifecycle: Send + Sync {
    /// Observes creation before the first guest instruction.
    ///
    /// # Errors
    /// Returns a typed extension error to abort process creation.
    fn process_created(&self, process: ProcessId) -> Result<(), ExtensionError>;
    /// Prepares resources before an emulated fork.
    ///
    /// # Errors
    /// Returns a typed extension error to veto the fork.
    fn fork_prepare(&self, parent: ProcessId, child: ProcessId) -> Result<(), ExtensionError>;
    /// Completes an ordered fork transition.
    ///
    /// # Errors
    /// Returns a typed extension error when resource inheritance cannot complete.
    fn fork_complete(&self, process: ProcessId, child: bool) -> Result<(), ExtensionError>;
    /// Observes exec after close-on-exec cleanup.
    ///
    /// # Errors
    /// Returns a typed extension error when surviving resources cannot transition.
    fn exec(&self, process: ProcessId, surviving: &[ResourceId]) -> Result<(), ExtensionError>;
    fn exit(&self, process: ProcessId);
}
