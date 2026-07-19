//! Frozen, backend-independent provider wire protocol.
//!
//! The encode/decode codec is kept deliberately complete and symmetric even
//! though the host only drives one direction of some request/reply pairs, so
//! the counterpart halves (and their unit tests) can round-trip the frozen wire
//! format. Those intact-but-host-unused entry points (and their re-exports) are
//! allowed to be dead within the host build.
#![allow(dead_code, unused_imports)]

mod frame;
mod service;

pub use frame::{decode_header, encode_header, Frame, MessageType, TransportError, HEADER_BYTES};
pub use service::{
    decode_namespace_install, decode_reply, decode_request, encode_namespace_install, encode_reply,
    encode_request, ProjectionKind, Reply, Request, SeekWhence, ServiceFailure, ServiceProjection,
    ServiceStat,
};
