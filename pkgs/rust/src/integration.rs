use crate::{BoxConfig, Error, Guest};
use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;

/// Stable attribution key for an external integration.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub struct IntegrationId(pub &'static str);

/// Read-only context supplied while composing one launch.
#[derive(Clone, Copy, Debug)]
pub struct IntegrationContext {
    guest: Guest,
}
impl IntegrationContext {
    /// Selected Linux guest ISA.
    #[must_use]
    pub const fn guest(&self) -> Guest {
        self.guest
    }
    pub(crate) const fn new(guest: Guest) -> Self {
        Self { guest }
    }
}

/// External launch extension. Implementations contain all device-specific policy.
pub trait Integration: Send + Sync {
    /// Stable identifier used for ordering diagnostics and conflict attribution.
    fn id(&self) -> IntegrationId;
    /// Applies typed operations to the pending launch.
    ///
    /// # Errors
    ///
    /// Returns an attributed policy error or a typed [`LaunchPlan`] conflict.
    fn apply(&self, context: &IntegrationContext, launch: &mut LaunchPlan) -> Result<(), Error>;
}

/// Typed path exposure visible to the Linux guest.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Mount {
    /// Existing host path.
    pub host: PathBuf,
    /// Absolute path presented to the guest.
    pub guest: PathBuf,
    /// Whether guest writes are forbidden.
    pub read_only: bool,
}

/// Validated, deterministic set of external launch mutations.
#[derive(Debug, Default)]
pub struct LaunchPlan {
    mounts: BTreeMap<PathBuf, (IntegrationId, Mount)>,
    environment: BTreeMap<OsString, (IntegrationId, OsString)>,
    path_prefixes: BTreeMap<OsString, Vec<(IntegrationId, PathBuf)>>,
    active: Option<IntegrationId>,
}

impl LaunchPlan {
    pub(crate) fn begin(&mut self, id: IntegrationId) -> Result<(), Error> {
        if id.0.is_empty() {
            return Err(Error::Integration {
                id,
                message: "integration id must not be empty".into(),
            });
        }
        self.active = Some(id);
        Ok(())
    }
    pub(crate) fn finish(&mut self) {
        self.active = None;
    }
    fn id(&self) -> IntegrationId {
        self.active
            .expect("launch mutations occur only while applying an integration")
    }

    /// Exposes one host path at an absolute guest path.
    ///
    /// # Errors
    ///
    /// Rejects relative guest paths and conflicting claims.
    pub fn mount(&mut self, mount: Mount) -> Result<(), Error> {
        let id = self.id();
        if !mount.guest.is_absolute() {
            return Err(Error::Integration {
                id,
                message: "guest mount path must be absolute".into(),
            });
        }
        if let Some((owner, existing)) = self.mounts.get(&mount.guest) {
            if existing == &mount {
                return Ok(());
            }
            return Err(Error::Conflict {
                resource: mount.guest.display().to_string(),
                first: *owner,
                second: id,
            });
        }
        self.mounts.insert(mount.guest.clone(), (id, mount));
        Ok(())
    }

    /// Sets one guest environment variable.
    ///
    /// # Errors
    ///
    /// Rejects invalid names and a different value already claimed by another integration.
    pub fn set_environment(
        &mut self,
        name: impl Into<OsString>,
        value: impl Into<OsString>,
    ) -> Result<(), Error> {
        let id = self.id();
        let name = name.into();
        let value = value.into();
        if name.is_empty()
            || name.as_encoded_bytes().contains(&b'=')
            || name.as_encoded_bytes().contains(&b'\n')
        {
            return Err(Error::Integration {
                id,
                message: "invalid environment name".into(),
            });
        }
        if let Some((owner, existing)) = self.environment.get(&name) {
            if existing == &value {
                return Ok(());
            }
            return Err(Error::Conflict {
                resource: name.to_string_lossy().into_owned(),
                first: *owner,
                second: id,
            });
        }
        self.environment.insert(name, (id, value));
        Ok(())
    }

    /// Prepends a path component, preserving integration order and removing duplicates.
    ///
    /// # Errors
    ///
    /// Rejects empty variable names or path components containing `:`.
    pub fn prepend_path(
        &mut self,
        name: impl Into<OsString>,
        path: impl Into<PathBuf>,
    ) -> Result<(), Error> {
        let id = self.id();
        let name = name.into();
        let path = path.into();
        if name.is_empty() || path.as_os_str().as_encoded_bytes().contains(&b':') {
            return Err(Error::Integration {
                id,
                message: "invalid path environment edit".into(),
            });
        }
        let values = self.path_prefixes.entry(name).or_default();
        if !values.iter().any(|(_, existing)| existing == &path) {
            values.push((id, path));
        }
        Ok(())
    }

    pub(crate) fn apply_to(self, config: &mut BoxConfig) {
        for (_, (_, mount)) in self.mounts {
            config.mounts.push(mount);
        }
        for (name, (_, value)) in self.environment {
            config.set_environment(name, value);
        }
        for (name, prefixes) in self.path_prefixes {
            let mut value = OsString::new();
            for (_, prefix) in prefixes {
                if !value.is_empty() {
                    value.push(OsStr::new(":"));
                }
                value.push(prefix);
            }
            if let Some(existing) = config.environment_value(&name) {
                if !value.is_empty() {
                    value.push(OsStr::new(":"));
                }
                value.push(existing);
            }
            config.set_environment(name, value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    struct Custom(IntegrationId);
    impl Integration for Custom {
        fn id(&self) -> IntegrationId {
            self.0
        }
        fn apply(&self, _: &IntegrationContext, plan: &mut LaunchPlan) -> Result<(), Error> {
            plan.prepend_path("PATH", Path::new("/custom"))
        }
    }

    fn apply(plan: &mut LaunchPlan, integration: &dyn Integration) -> Result<(), Error> {
        plan.begin(integration.id())?;
        let result = integration.apply(&IntegrationContext::new(Guest::Aarch64), plan);
        plan.finish();
        result
    }

    #[test]
    fn ordering_path_merge_and_custom_integration() {
        let mut plan = LaunchPlan::default();
        apply(&mut plan, &Custom(IntegrationId("first"))).unwrap();
        apply(&mut plan, &Custom(IntegrationId("second"))).unwrap();
        let mut config = BoxConfig::new().environment("PATH", "/bin");
        plan.apply_to(&mut config);
        assert_eq!(
            config.environment_value(OsStr::new("PATH")),
            Some(OsStr::new("/custom:/bin"))
        );
    }

    #[test]
    fn conflicts_are_attributed() {
        let mut plan = LaunchPlan::default();
        plan.begin(IntegrationId("one")).unwrap();
        plan.set_environment("MODE", "a").unwrap();
        plan.finish();
        plan.begin(IntegrationId("two")).unwrap();
        assert!(matches!(
            plan.set_environment("MODE", "b"),
            Err(Error::Conflict {
                first: IntegrationId("one"),
                second: IntegrationId("two"),
                ..
            })
        ));
    }

    #[test]
    fn device_mount_conflicts_are_rejected_before_launch() {
        let mut plan = LaunchPlan::default();
        plan.begin(IntegrationId("one")).unwrap();
        plan.mount(Mount {
            host: "/dev/one".into(),
            guest: "/dev/shared".into(),
            read_only: false,
        })
        .unwrap();
        plan.finish();
        plan.begin(IntegrationId("two")).unwrap();
        let conflict = plan.mount(Mount {
            host: "/dev/two".into(),
            guest: "/dev/shared".into(),
            read_only: false,
        });
        assert!(matches!(
            conflict,
            Err(Error::Conflict {
                first: IntegrationId("one"),
                second: IntegrationId("two"),
                ..
            })
        ));
    }
}
