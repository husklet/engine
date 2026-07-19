use crate::{Config, Error, Mount};
use std::{
    collections::BTreeMap,
    ffi::{OsStr, OsString},
    path::PathBuf,
};

/// Scoped container customization for a command.
#[derive(Clone, Debug, Default)]
pub struct Container {
    mounts: Vec<Mount>,
    environment: Vec<(OsString, OsString)>,
    prepend: Vec<(OsString, PathBuf)>,
    append: Vec<(OsString, PathBuf)>,
}
impl Container {
    pub fn mount(&mut self, value: Mount) -> &mut Self {
        self.mounts.push(value);
        self
    }
    pub fn env(&mut self, name: impl Into<OsString>, value: impl Into<OsString>) -> &mut Self {
        self.environment.push((name.into(), value.into()));
        self
    }
    pub fn prepend_path(
        &mut self,
        name: impl Into<OsString>,
        path: impl Into<PathBuf>,
    ) -> &mut Self {
        self.prepend.push((name.into(), path.into()));
        self
    }
    pub fn append_path(
        &mut self,
        name: impl Into<OsString>,
        path: impl Into<PathBuf>,
    ) -> &mut Self {
        self.append.push((name.into(), path.into()));
        self
    }
    pub(crate) fn resolve(self, config: &mut Config) -> Result<(), Error> {
        let mut mounts = BTreeMap::new();
        for mount in self.mounts {
            if !mount.guest.is_absolute() {
                return Err(Error::InvalidConfig("guest mount path must be absolute"));
            }
            if mounts.insert(mount.guest.clone(), mount).is_some() {
                return Err(Error::InvalidConfig("duplicate guest mount path"));
            }
        }
        config.mounts.extend(mounts.into_values());
        for (name, value) in self.environment {
            validate_name(&name)?;
            config.set_environment(name, value);
        }
        let mut edits: BTreeMap<OsString, (Vec<PathBuf>, Vec<PathBuf>)> = BTreeMap::new();
        for (name, path) in self.prepend {
            validate_path(&name, &path)?;
            edits.entry(name).or_default().0.push(path);
        }
        for (name, path) in self.append {
            validate_path(&name, &path)?;
            edits.entry(name).or_default().1.push(path);
        }
        for (name, (before, after)) in edits {
            let mut value = OsString::new();
            for path in before {
                push_path(&mut value, path.as_os_str());
            }
            if let Some(current) = config.environment_value(&name) {
                push_path(&mut value, current);
            }
            for path in after {
                push_path(&mut value, path.as_os_str());
            }
            config.set_environment(name, value);
        }
        Ok(())
    }
}
fn validate_name(name: &OsStr) -> Result<(), Error> {
    if name.is_empty()
        || name.as_encoded_bytes().contains(&b'=')
        || name.as_encoded_bytes().contains(&b'\n')
    {
        Err(Error::InvalidConfig("invalid environment name"))
    } else {
        Ok(())
    }
}
fn validate_path(name: &OsStr, path: &std::path::Path) -> Result<(), Error> {
    validate_name(name)?;
    if path.as_os_str().as_encoded_bytes().contains(&b':') {
        Err(Error::InvalidConfig("path component contains ':'"))
    } else {
        Ok(())
    }
}
fn push_path(value: &mut OsString, part: &OsStr) {
    if !value.is_empty() {
        value.push(":");
    }
    value.push(part);
}
