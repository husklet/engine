//! Safe process-isolated lifecycle API for the HL Linux guest engine.
//!
//! This crate is organised in two layers:
//!
//! * The **public host API** — [`Engine`], [`Machine`], [`Container`],
//!   [`Command`] and their supporting types — is what callers use to launch and
//!   drive guest machines. It is surfaced by the re-exports at the bottom of
//!   this module and by the public [`control`], [`extension`], [`network`],
//!   [`spec`] and [`transport`] facades.
//! * The internal [`api`], [`provider`], [`protocol`] and [`runtime`] modules
//!   hold the backend-independent contracts, provider ports, frozen wire
//!   protocol and host-side runtime mechanisms. They were once sibling crates;
//!   they are now private modules and are exposed only through the curated
//!   re-exports below, so the crate's public surface is unchanged.
#![deny(unsafe_code)]

#[cfg(not(any(
    all(target_arch = "aarch64", target_os = "macos"),
    all(target_arch = "aarch64", target_os = "linux")
)))]
compile_error!("hl-engine supports only aarch64-apple-darwin and aarch64-unknown-linux-gnu hosts");

// Internal contract layers (formerly the api/provider/protocol/runtime crates).
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

// --- Backend-independent contracts (re-exported from `api`) ---
pub use crate::api::{checkpoint, observability};
pub use crate::api::{Access, Guest, Mount, Sandbox, Stdio, Version};
pub use checkpoint_stream::{CheckpointStore, MemoryStore, StoreError};

// --- Engine entry points and lifecycle handles ---
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
    AttachRequest, Attachment, AttachmentKind, ControlError, ControlErrorCategory, EventStream,
    ExtensionHandle, NetworkUpdate, PauseGuard, ProcessInfo, ResourceUpdate, ShutdownPolicy,
    Signal, SignalTarget,
};

// --- Extension provider authorities ---
pub use extension::{Authorities, HandlesAuthority, ProviderAuthority};

// --- Launch specification ---
pub use spec::{
    EngineCapabilities, MachineSpec, ProcessIo, SpawnError, SpecError, TreeSource, Validation,
};
