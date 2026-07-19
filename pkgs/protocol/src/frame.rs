const MAGIC: u32 = 0x484c_5052;
const VERSION: u16 = 1;
pub const HEADER_BYTES: usize = 32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum MessageType {
    Hello = 1,
    Ready = 2,
    Request = 3,
    Reply = 4,
    Cancel = 5,
    Close = 6,
    NamespaceInstall = 7,
    NamespaceReady = 8,
    Subscribe = 9,
    Unsubscribe = 10,
    ReadinessEvent = 11,
}

impl TryFrom<u16> for MessageType {
    type Error = TransportError;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::Hello),
            2 => Ok(Self::Ready),
            3 => Ok(Self::Request),
            4 => Ok(Self::Reply),
            5 => Ok(Self::Cancel),
            6 => Ok(Self::Close),
            7 => Ok(Self::NamespaceInstall),
            8 => Ok(Self::NamespaceReady),
            9 => Ok(Self::Subscribe),
            10 => Ok(Self::Unsubscribe),
            11 => Ok(Self::ReadinessEvent),
            _ => Err(TransportError::Malformed),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    pub kind: MessageType,
    pub request_id: u64,
    pub features: u64,
    pub payload: Vec<u8>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransportError {
    Malformed,
    Version,
    Oversized,
    Timeout,
    PeerClosed,
    Io,
    DuplicateProvider,
    Quota,
}

/// Encodes the frozen envelope header.
///
/// # Errors
/// Returns [`TransportError::Oversized`] when the payload length cannot be represented.
pub fn encode_header(frame: &Frame) -> Result<[u8; HEADER_BYTES], TransportError> {
    let length = u32::try_from(frame.payload.len()).map_err(|_| TransportError::Oversized)?;
    let mut bytes = [0_u8; HEADER_BYTES];
    bytes[0..4].copy_from_slice(&MAGIC.to_le_bytes());
    bytes[4..6].copy_from_slice(&VERSION.to_le_bytes());
    bytes[6..8].copy_from_slice(&(frame.kind as u16).to_le_bytes());
    bytes[8..12].copy_from_slice(&length.to_le_bytes());
    bytes[12..20].copy_from_slice(&frame.request_id.to_le_bytes());
    bytes[20..28].copy_from_slice(&frame.features.to_le_bytes());
    Ok(bytes)
}

/// Decodes and validates the frozen envelope header.
///
/// # Errors
/// Returns a malformed or version error when the header violates the wire contract.
pub fn decode_header(
    bytes: &[u8; HEADER_BYTES],
) -> Result<(MessageType, u64, u64, u32), TransportError> {
    if u32::from_le_bytes(
        bytes[0..4]
            .try_into()
            .map_err(|_| TransportError::Malformed)?,
    ) != MAGIC
    {
        return Err(TransportError::Malformed);
    }
    if u16::from_le_bytes(
        bytes[4..6]
            .try_into()
            .map_err(|_| TransportError::Malformed)?,
    ) != VERSION
    {
        return Err(TransportError::Version);
    }
    let kind = MessageType::try_from(u16::from_le_bytes(
        bytes[6..8]
            .try_into()
            .map_err(|_| TransportError::Malformed)?,
    ))?;
    let length = u32::from_le_bytes(
        bytes[8..12]
            .try_into()
            .map_err(|_| TransportError::Malformed)?,
    );
    let request_id = u64::from_le_bytes(
        bytes[12..20]
            .try_into()
            .map_err(|_| TransportError::Malformed)?,
    );
    let features = u64::from_le_bytes(
        bytes[20..28]
            .try_into()
            .map_err(|_| TransportError::Malformed)?,
    );
    if bytes[28..32] != [0; 4] {
        return Err(TransportError::Malformed);
    }
    Ok((kind, request_id, features, length))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frozen_header_round_trips_and_rejects_malformed_versions() {
        let frame = Frame {
            kind: MessageType::Request,
            request_id: 42,
            features: 7,
            payload: b"copied".to_vec(),
        };
        let header = encode_header(&frame).unwrap();
        assert_eq!(
            decode_header(&header).unwrap(),
            (MessageType::Request, 42, 7, 6)
        );
        let mut malformed = [0; HEADER_BYTES];
        assert_eq!(decode_header(&malformed), Err(TransportError::Malformed));
        malformed[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        malformed[4..6].copy_from_slice(&(VERSION + 1).to_le_bytes());
        assert_eq!(decode_header(&malformed), Err(TransportError::Version));
    }
}
