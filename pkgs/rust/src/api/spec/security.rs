//! Guest security and provider authority policy.

use std::collections::BTreeSet;

use crate::api::{extension::ProviderId, Sandbox};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SecuritySpec {
    pub sandbox: Sandbox,
    pub trusted_guest: bool,
    pub executable_memory: bool,
    pub allowed_providers: BTreeSet<ProviderId>,
}
impl Default for SecuritySpec {
    fn default() -> Self {
        Self {
            sandbox: Sandbox::Disabled,
            trusted_guest: false,
            executable_memory: true,
            allowed_providers: BTreeSet::new(),
        }
    }
}
