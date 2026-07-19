use crate::{ResourceError, ResourceId};
use hl_engine_api::{
    extension::{Inheritance, Protections, ProviderId, Sharing},
    Version,
};

pub trait Memory: Send + Sync {
    /// Allocates a provider resource matching the declared bounds.
    ///
    /// # Errors
    /// Returns a typed resource error for invalid, unsupported, exhausted, or failed allocations.
    fn allocate(&self, request: AllocationRequest) -> Result<HostResource, ResourceError>;
    /// Imports a validated opaque provider descriptor.
    ///
    /// # Errors
    /// Returns a typed resource error when the descriptor is incompatible or cannot be imported.
    fn import(&self, descriptor: &ResourceDescriptor) -> Result<HostResource, ResourceError>;

    /// Releases one resource previously returned by this provider.
    ///
    /// The default supports providers whose returned resource is independently owned. Providers
    /// retaining state behind a resource id override this callback.
    fn release(&self, _resource: ResourceId) {}
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AllocationRequest {
    pub size: u64,
    pub alignment: u64,
    pub protections: Protections,
    pub sharing: Sharing,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourceDescriptor {
    pub provider: ProviderId,
    pub version: Version,
    pub bytes: Vec<u8>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HostResource {
    pub id: ResourceId,
    pub regions: Vec<Region>,
    pub handles: Vec<TransferHandle>,
    pub coherency: Coherency,
    pub inheritance: Inheritance,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Region {
    pub offset: u64,
    pub size: u64,
    pub protections: Protections,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferHandle(pub u64);
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Coherency {
    Coherent,
    Explicit,
}
