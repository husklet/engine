/// Linux instruction set executed by the engine.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Guest {
    Aarch64,
    X86_64,
}

/// Isolation level applied to the Linux launch.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Sandbox {
    #[default]
    Disabled,
    Enabled,
    SentryOnly,
}

/// Ownership policy for a standard stream.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Stdio {
    #[default]
    Inherit,
    Null,
    Piped,
}

impl Stdio {
    #[must_use]
    pub const fn inherit() -> Self {
        Self::Inherit
    }
    #[must_use]
    pub const fn null() -> Self {
        Self::Null
    }
    #[must_use]
    pub const fn piped() -> Self {
        Self::Piped
    }
}

/// Guest access granted to a mounted host path.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Access {
    ReadOnly,
    ReadWrite,
}

/// Host path exposed inside the guest.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Mount {
    pub host: std::path::PathBuf,
    pub guest: std::path::PathBuf,
    pub access: Access,
}
impl Mount {
    #[must_use]
    pub fn read_only(
        host: impl Into<std::path::PathBuf>,
        guest: impl Into<std::path::PathBuf>,
    ) -> Self {
        Self {
            host: host.into(),
            guest: guest.into(),
            access: Access::ReadOnly,
        }
    }
    #[must_use]
    pub fn read_write(
        host: impl Into<std::path::PathBuf>,
        guest: impl Into<std::path::PathBuf>,
    ) -> Self {
        Self {
            host: host.into(),
            guest: guest.into(),
            access: Access::ReadWrite,
        }
    }
}
