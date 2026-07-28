use crate::Error;

/// Completed guest status.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Exit {
    Code(i32),
    Signal(i32),
    Fault { status: i32, detail: u64 },
}
impl Exit {
    pub(crate) fn from_native(value: crate::ffi::EngineExit) -> Result<Self, Error> {
        if value.abi != 5 || value.size != 24 {
            return Err(Error::ResultProtocol("invalid native exit layout".into()));
        }
        match value.kind {
            1 => Ok(Self::Code(value.guest_status)),
            2 => Ok(Self::Signal(value.guest_status)),
            3 => Ok(Self::Fault {
                status: value.guest_status,
                detail: value.detail,
            }),
            4 => Err(Error::Engine {
                status: value.guest_status,
                detail: value.detail,
            }),
            kind => Err(Error::ResultProtocol(format!(
                "invalid native exit kind {kind}"
            ))),
        }
    }

    #[must_use]
    pub const fn code(self) -> Option<i32> {
        if let Self::Code(value) = self {
            Some(value)
        } else {
            None
        }
    }
    #[must_use]
    pub const fn signal(self) -> Option<i32> {
        if let Self::Signal(value) = self {
            Some(value)
        } else {
            None
        }
    }
    #[must_use]
    pub const fn success(self) -> bool {
        matches!(self, Self::Code(0))
    }
}
