//! Safe process-isolated lifecycle API for the HL Linux guest engine.
#![deny(unsafe_code)]

#[cfg(not(any(
    all(target_arch = "aarch64", target_os = "macos"),
    all(target_arch = "aarch64", target_os = "linux")
)))]
compile_error!("hl-engine supports only aarch64-apple-darwin and aarch64-unknown-linux-gnu hosts");

mod child;
mod command;
mod config;
mod container;
mod engine;
mod error;
mod ffi;
mod result;
mod runtime;
mod types;
mod wire;

pub use child::{Child, Output};
pub use command::Command;
pub use config::Config;
pub use container::Container;
pub use engine::Engine;
pub use error::Error;
pub use result::Exit;
pub use types::{Access, Guest, Mount, Sandbox, Stdio};
