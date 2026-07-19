//! Frozen, backend-independent provider wire protocol.
#![deny(unsafe_code)]

mod frame;
mod service;

pub use frame::{decode_header, encode_header, Frame, MessageType, TransportError, HEADER_BYTES};
pub use service::{
    decode_namespace_install, decode_reply, decode_request, encode_namespace_install, encode_reply,
    encode_request, ProjectionKind, Reply, Request, SeekWhence, ServiceFailure, ServiceProjection,
    ServiceStat,
};
