//! Safe process-isolated lifecycle API for the HL Linux guest engine.
#![deny(unsafe_code)]

#[cfg(not(any(
    all(target_arch = "aarch64", target_os = "macos"),
    all(target_arch = "aarch64", target_os = "linux")
)))]
compile_error!("hl-engine supports only aarch64-apple-darwin and aarch64-unknown-linux-gnu hosts");

pub mod checkpoint;
mod child;
mod command;
mod config;
mod container;
pub mod control;
mod domain;
mod engine;
mod error;
pub mod extension;
mod ffi;
mod machine;
pub mod network;
pub mod observability;
mod projection;
mod result;
mod runtime;
#[allow(dead_code)]
mod service;
pub mod spec;
mod terminal;
pub mod transport;
mod types;
mod wire;

pub use child::{Child, Output};
pub use command::Command;
pub use config::Config;
pub use container::Container;
pub use control::{
    AttachRequest, Attachment, AttachmentKind, ControlError, ControlErrorCategory, EventStream,
    ExtensionHandle, NetworkUpdate, PauseGuard, ProcessInfo, ResourceUpdate, ShutdownPolicy,
    Signal, SignalTarget,
};
pub use domain::Domain;
pub use engine::Engine;
pub use error::Error;
pub use extension::HandlesAuthority;
pub use machine::Machine;
pub use result::Exit;
pub use spec::{
    EngineCapabilities, MachineSpec, ProcessIo, SpawnError, SpecError, TreeSource, Validation,
};
pub use terminal::{Size, Terminal};
pub use types::{Access, Guest, Mount, Sandbox, Stdio};
