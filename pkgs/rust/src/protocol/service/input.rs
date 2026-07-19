use crate::protocol::TransportError;
use crate::provider::LinuxError;

use super::ServiceFailure;

pub(super) fn put_u16(out: &mut Vec<u8>, value: u16) {
    out.extend(value.to_le_bytes());
}

pub(super) fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend(value.to_le_bytes());
}

pub(super) fn put_i32(out: &mut Vec<u8>, value: i32) {
    out.extend(value.to_le_bytes());
}

pub(super) fn put_u64(out: &mut Vec<u8>, value: u64) {
    out.extend(value.to_le_bytes());
}

pub(super) fn put_i64(out: &mut Vec<u8>, value: i64) {
    out.extend(value.to_le_bytes());
}

pub(super) struct Input<'a> {
    bytes: &'a [u8],
    offset: usize,
}
impl<'a> Input<'a> {
    pub(super) const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }
    pub(super) fn bytes(&mut self, count: usize) -> Result<&'a [u8], ServiceFailure> {
        let end = self.offset.checked_add(count).ok_or_else(protocol)?;

        let value = self.bytes.get(self.offset..end).ok_or_else(protocol)?;
        self.offset = end;
        Ok(value)
    }

    pub(super) fn u8(&mut self) -> Result<u8, ServiceFailure> {
        Ok(self.bytes(1)?[0])
    }
    pub(super) fn u16(&mut self) -> Result<u16, ServiceFailure> {
        Ok(u16::from_le_bytes(
            self.bytes(2)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub(super) fn u32(&mut self) -> Result<u32, ServiceFailure> {
        Ok(u32::from_le_bytes(
            self.bytes(4)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub(super) fn i32(&mut self) -> Result<i32, ServiceFailure> {
        Ok(i32::from_le_bytes(
            self.bytes(4)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub(super) fn u64(&mut self) -> Result<u64, ServiceFailure> {
        Ok(u64::from_le_bytes(
            self.bytes(8)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub(super) fn i64(&mut self) -> Result<i64, ServiceFailure> {
        Ok(i64::from_le_bytes(
            self.bytes(8)?.try_into().map_err(|_| protocol())?,
        ))
    }
    pub(super) fn finish(self) -> Result<(), ServiceFailure> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err(protocol())
        }
    }
}

pub(super) fn linux(errno: i32, context: &str) -> ServiceFailure {
    ServiceFailure::Linux(LinuxError {
        errno,
        context: context.into(),
    })
}
pub(super) fn protocol() -> ServiceFailure {
    ServiceFailure::Transport(TransportError::Malformed)
}
