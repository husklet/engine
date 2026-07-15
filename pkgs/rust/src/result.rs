use crate::Error;
use std::{fs, path::Path, process::ExitStatus};

const MAGIC: u32 = 0x484c_5253;
const ABI: u32 = 1;
const SIZE: usize = 32;

/// Completed guest status from the typed launcher-result channel.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Exit {
    Code(i32),
    Signal(i32),
    Fault { status: i32, detail: u64 },
}
impl Exit {
    #[must_use]
    pub const fn code(self) -> Option<i32> {
        if let Self::Code(v) = self {
            Some(v)
        } else {
            None
        }
    }
    #[must_use]
    pub const fn signal(self) -> Option<i32> {
        if let Self::Signal(v) = self {
            Some(v)
        } else {
            None
        }
    }
    #[must_use]
    pub const fn success(self) -> bool {
        matches!(self, Self::Code(0))
    }
}

pub(crate) fn read(path: &Path, transport: ExitStatus) -> Result<Exit, Error> {
    let bytes = fs::read(path)?;
    let _ = fs::remove_file(path);
    if !transport.success() {
        use std::os::unix::process::ExitStatusExt;
        return Err(Error::ResultProtocol(format!(
            "launcher transport exited code={:?} signal={:?}",
            transport.code(),
            transport.signal()
        )));
    }
    if bytes.len() != SIZE {
        return Err(Error::ResultProtocol(format!(
            "expected {SIZE} bytes, got {}",
            bytes.len()
        )));
    }
    let u32_at = |o| u32::from_le_bytes(bytes[o..o + 4].try_into().expect("fixed result field"));
    let i32_at = |o| i32::from_le_bytes(bytes[o..o + 4].try_into().expect("fixed result field"));
    let detail = u64::from_le_bytes(bytes[24..32].try_into().expect("fixed result detail"));
    if u32_at(0) != MAGIC || u32_at(4) != ABI || u32_at(20) != 0 {
        return Err(Error::ResultProtocol(
            "bad magic, ABI, or reserved field".into(),
        ));
    }
    let guest = i32_at(12);
    let engine = i32_at(16);
    match u32_at(8) {
        1 if engine == 0 => Ok(Exit::Code(guest)),
        2 if engine == 0 => Ok(Exit::Signal(guest)),
        3 if engine == 0 => Ok(Exit::Fault {
            status: guest,
            detail,
        }),
        4 => Err(Error::Engine {
            status: engine,
            detail,
        }),
        kind => Err(Error::ResultProtocol(format!(
            "invalid result kind/status {kind}/{engine}"
        ))),
    }
}
