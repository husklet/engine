//! Typed discovery and launch-plane facade.

mod error;
mod machine;
mod network;
mod process;

pub use error::SpawnError;
pub use crate::api::spec::*;
pub use machine::MachineSpec;
pub use network::NetworkSpec;
pub use process::ProcessSpec;
