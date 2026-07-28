//! Safe process-isolated lifecycle API for the HL Linux guest engine.
//!
#![deny(unsafe_code)]

#[cfg(not(any(
    all(target_arch = "aarch64", target_os = "macos"),
    all(target_arch = "aarch64", target_os = "linux")
)))]
compile_error!("hl-engine supports only aarch64-apple-darwin and aarch64-unknown-linux-gnu hosts");

pub(crate) mod api;
pub(crate) mod protocol;
pub(crate) mod provider;
pub(crate) mod runtime;

// Host-side implementation modules.
mod checkpoint_stream;
mod child;
mod command;
mod config;
mod configfile;
mod container;
pub mod control;
mod domain;
mod engine;
mod error;
pub mod extension;
mod ffi;
mod host;
mod machine;
pub mod network;
mod projection;
mod result;
#[allow(dead_code)]
mod service;
pub mod spec;
mod terminal;
pub mod transport;
mod wire;

pub use crate::wire::launch_abi;

pub use crate::api::{checkpoint, observability};
pub use crate::api::{Access, Guest, Mount, Sandbox, Stdio, Version};
pub use checkpoint_stream::{CheckpointStore, MemoryStore, StoreError};

pub use child::{Child, Output};
pub use command::Command;
pub use config::Config;
pub use container::Container;
pub use domain::Domain;
pub use engine::{Engine, StoreDirection};
pub use error::Error;
pub use machine::Machine;
pub use result::Exit;
pub use terminal::{Size, Terminal};

// --- Live control plane ---
pub use control::{
    AttachRequest, Attachment, AttachmentKind, ControlError, ControlErrorCategory, PauseGuard,
    ProcessInfo, ResourceUpdate, ShutdownPolicy, Signal, SignalTarget,
};

// --- Extension provider authorities ---
pub use extension::{Authorities, HandlesAuthority, ProviderAuthority};

// --- Launch specification ---
pub use spec::{
    EngineCapabilities, MachineSpec, ProcessIo, SpawnError, SpecError, TreeSource, Validation,
};
