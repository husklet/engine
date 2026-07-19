use crate::{Handles, Memory};
use hl_engine_api::extension::{ContractError, ProviderId};
use std::{collections::BTreeMap, sync::Arc, time::Duration};

/// Launch-scoped authority for provider-backed open services.
///
/// Authorities are deliberately separate from [`ExtensionSpec`]: a declarative
/// machine specification remains cloneable and inspectable, while host authority
/// is granted explicitly at spawn and cannot leak into another launch.
#[derive(Default)]
pub struct HandlesAuthority {
    providers: BTreeMap<ProviderId, Arc<dyn Handles>>,
}

/// The narrow live ports granted to one provider for one launch.
#[derive(Default)]
pub struct ProviderAuthority {
    pub handles: Option<Arc<dyn Handles>>,
    pub memory: Option<Arc<dyn Memory>>,
}

/// Launch-scoped provider authority, kept separate from serializable specifications.
#[derive(Default)]
pub struct Authorities {
    providers: BTreeMap<ProviderId, ProviderAuthority>,
}

impl Authorities {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Grants one provider's selected live ports.
    ///
    /// # Errors
    /// Returns [`ContractError::DuplicateProvider`] for a duplicate provider grant.
    pub fn grant(
        &mut self,
        provider: ProviderId,
        authority: ProviderAuthority,
    ) -> Result<(), ContractError> {
        if self.providers.insert(provider, authority).is_some() {
            return Err(ContractError::DuplicateProvider);
        }
        Ok(())
    }

    #[must_use]
    pub fn provider(&self, provider: &ProviderId) -> Option<&ProviderAuthority> {
        self.providers.get(provider)
    }

    pub fn iter(&self) -> impl Iterator<Item = (&ProviderId, &ProviderAuthority)> {
        self.providers.iter()
    }
}

impl From<HandlesAuthority> for Authorities {
    fn from(authority: HandlesAuthority) -> Self {
        Self {
            providers: authority
                .providers
                .into_iter()
                .map(|(provider, handles)| {
                    (
                        provider,
                        ProviderAuthority {
                            handles: Some(handles),
                            memory: None,
                        },
                    )
                })
                .collect(),
        }
    }
}

impl HandlesAuthority {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Grants one provider's handle authority to this launch.
    ///
    /// # Errors
    /// Returns [`ContractError::DuplicateProvider`] when the provider was
    /// already granted.
    pub fn grant(
        &mut self,
        provider: ProviderId,
        handles: Arc<dyn Handles>,
    ) -> Result<(), ContractError> {
        if self.providers.insert(provider, handles).is_some() {
            return Err(ContractError::DuplicateProvider);
        }
        Ok(())
    }

    /// Returns the authority granted for a provider, when present.
    #[must_use]
    pub fn handles(&self, provider: &ProviderId) -> Option<&Arc<dyn Handles>> {
        self.providers.get(provider)
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ProcessId(pub u64);
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ResourceId(pub u64);
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LinuxError {
    pub errno: i32,
    pub context: String,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourceError {
    pub category: ResourceErrorCategory,
    pub context: String,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResourceErrorCategory {
    Unsupported,
    Invalid,
    Quota,
    Host,
    Provider,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionError {
    pub category: ExtensionErrorCategory,
    pub context: String,
    pub retry_after: Option<Duration>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExtensionErrorCategory {
    Unsupported,
    Invalid,
    Protocol,
    Timeout,
    Cancelled,
    Quota,
    Provider,
}
