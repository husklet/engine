use std::{fmt, io};

/// Distribution, configuration, or process error.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    InvalidConfig(&'static str),
    InvalidState,
    Distribution(String),
    Unsupported(&'static str),
    Engine { status: i32, detail: u64 },
    ResultProtocol(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(e) => e.fmt(f),
            Self::InvalidConfig(m) | Self::Unsupported(m) => f.write_str(m),
            Self::InvalidState => f.write_str("engine process has already been consumed"),
            Self::Distribution(m) => write!(f, "cannot install engine distribution: {m}"),
            Self::Engine { status, detail } => {
                write!(f, "engine failure status={status} detail={detail}")
            }
            Self::ResultProtocol(m) => write!(f, "invalid engine result: {m}"),
        }
    }
}
impl std::error::Error for Error {}
impl From<io::Error> for Error {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}
