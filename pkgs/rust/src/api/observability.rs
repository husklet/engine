//! Stable observability schemas. Native emission/control is not implemented.

use std::collections::VecDeque;
const MAGIC: u32 = 0x484c_4f42;
const VERSION: u16 = 1;
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum Kind {
    MachineLifecycle = 1,
    ProcessLifecycle = 2,
    ThreadLifecycle = 3,
    Exec = 4,
    Signal = 5,
    GuestFault = 6,
    SyscallSample = 7,
    TranslationStats = 8,
    CacheStats = 9,
    FdUsage = 10,
    FilesystemUsage = 11,
    NetworkUsage = 12,
    ProviderUsage = 13,
    ExtensionFailure = 14,
    CheckpointProgress = 15,
}
impl TryFrom<u16> for Kind {
    type Error = Error;
    fn try_from(v: u16) -> Result<Self, Error> {
        match v {
            1 => Ok(Self::MachineLifecycle),
            2 => Ok(Self::ProcessLifecycle),
            3 => Ok(Self::ThreadLifecycle),
            4 => Ok(Self::Exec),
            5 => Ok(Self::Signal),
            6 => Ok(Self::GuestFault),
            7 => Ok(Self::SyscallSample),
            8 => Ok(Self::TranslationStats),
            9 => Ok(Self::CacheStats),
            10 => Ok(Self::FdUsage),
            11 => Ok(Self::FilesystemUsage),
            12 => Ok(Self::NetworkUsage),
            13 => Ok(Self::ProviderUsage),
            14 => Ok(Self::ExtensionFailure),
            15 => Ok(Self::CheckpointProgress),
            _ => Err(Error::Version),
        }
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Privacy {
    Public = 1,
    Sensitive = 2,
    Secret = 3,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Field {
    pub name: String,
    pub value: Vec<u8>,
    pub privacy: Privacy,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RegisterSummary {
    pub guest_pc: u64,
    pub stack_pointer: u64,
    pub flags: u64,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Event {
    pub kind: Kind,
    pub host_monotonic_ns: u64,
    pub guest_time_ns: u64,
    pub correlation_id: u128,
    pub machine: u64,
    pub process: Option<u64>,
    pub thread: Option<u64>,
    pub lost_before: u64,
    pub registers: Option<RegisterSummary>,
    pub fields: Vec<Field>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Limits {
    pub event_bytes: u32,
    pub fields: u16,
    pub field_bytes: u32,
    pub queue: u32,
    pub maximum_privacy: Privacy,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Error {
    Corrupt,
    Version,
    Limit,
    Privacy,
    Backpressure,
}
impl Event {
    /// Encodes this event within the supplied bounds.
    ///
    /// # Errors
    /// Returns an error when a field violates limits or privacy policy.
    pub fn encode(&self, l: Limits) -> Result<Vec<u8>, Error> {
        if self.fields.len() > l.fields as usize {
            return Err(Error::Limit);
        }
        let mut o = Vec::new();
        u32p(&mut o, MAGIC);
        u16p(&mut o, VERSION);
        u16p(&mut o, self.kind as u16);
        u64p(&mut o, self.host_monotonic_ns);
        u64p(&mut o, self.guest_time_ns);
        o.extend(self.correlation_id.to_le_bytes());
        u64p(&mut o, self.machine);
        optional(&mut o, self.process);
        optional(&mut o, self.thread);
        u64p(&mut o, self.lost_before);
        match self.registers {
            Some(ref r) => {
                o.push(1);
                u64p(&mut o, r.guest_pc);
                u64p(&mut o, r.stack_pointer);
                u64p(&mut o, r.flags);
            }
            None => o.push(0),
        }
        u16p(
            &mut o,
            u16::try_from(self.fields.len()).map_err(|_| Error::Limit)?,
        );
        for f in &self.fields {
            if f.value.len() > l.field_bytes as usize {
                return Err(Error::Limit);
            }
            if f.privacy as u8 > l.maximum_privacy as u8 {
                return Err(Error::Privacy);
            }
            bytes(&mut o, f.name.as_bytes())?;
            o.push(f.privacy as u8);
            bytes(&mut o, &f.value)?;
        }
        if o.len() > l.event_bytes as usize {
            return Err(Error::Limit);
        }
        Ok(o)
    }
    /// Decodes one bounded event.
    ///
    /// # Errors
    /// Returns an error for malformed, unsupported, over-limit, or disallowed data.
    pub fn decode(b: &[u8], l: Limits) -> Result<Self, Error> {
        if b.len() > l.event_bytes as usize {
            return Err(Error::Limit);
        }
        let mut r = Reader { b, p: 0 };
        if r.u32() != Some(MAGIC) {
            return Err(Error::Corrupt);
        }
        if r.u16() != Some(VERSION) {
            return Err(Error::Version);
        }
        let kind = Kind::try_from(r.u16().ok_or(Error::Corrupt)?)?;
        let host_monotonic_ns = r.u64().ok_or(Error::Corrupt)?;
        let guest_time_ns = r.u64().ok_or(Error::Corrupt)?;
        let correlation_id =
            u128::from_le_bytes(r.take(16)?.try_into().map_err(|_| Error::Corrupt)?);
        let machine = r.u64().ok_or(Error::Corrupt)?;
        let process = r.optional()?;
        let thread = r.optional()?;
        let lost_before = r.u64().ok_or(Error::Corrupt)?;
        let registers = match r.byte()? {
            0 => None,
            1 => Some(RegisterSummary {
                guest_pc: r.u64().ok_or(Error::Corrupt)?,
                stack_pointer: r.u64().ok_or(Error::Corrupt)?,
                flags: r.u64().ok_or(Error::Corrupt)?,
            }),
            _ => return Err(Error::Corrupt),
        };
        let n = r.u16().ok_or(Error::Corrupt)?;
        if n > l.fields {
            return Err(Error::Limit);
        }
        let mut fields = Vec::new();
        for _ in 0..n {
            let name = String::from_utf8(r.bytes(l.field_bytes)?).map_err(|_| Error::Corrupt)?;
            let privacy = match r.byte()? {
                1 => Privacy::Public,
                2 => Privacy::Sensitive,
                3 => Privacy::Secret,
                _ => return Err(Error::Corrupt),
            };
            if privacy as u8 > l.maximum_privacy as u8 {
                return Err(Error::Privacy);
            }
            let value = r.bytes(l.field_bytes)?;
            fields.push(Field {
                name,
                value,
                privacy,
            });
        }
        if r.p != b.len() {
            return Err(Error::Corrupt);
        }
        Ok(Self {
            kind,
            host_monotonic_ns,
            guest_time_ns,
            correlation_id,
            machine,
            process,
            thread,
            lost_before,
            registers,
            fields,
        })
    }
}
pub struct EventQueue {
    values: VecDeque<Event>,
    maximum: usize,
    lost: u64,
}
impl EventQueue {
    #[must_use]
    pub fn new(maximum: u32) -> Self {
        Self {
            values: VecDeque::new(),
            maximum: maximum as usize,
            lost: 0,
        }
    }
    /// Adds an event, accounting for any evicted event.
    ///
    /// # Errors
    /// Returns [`Error::Backpressure`] when the queue has zero capacity.
    pub fn push(&mut self, mut event: Event) -> Result<(), Error> {
        if self.maximum == 0 {
            return Err(Error::Backpressure);
        }
        if self.values.len() == self.maximum {
            self.values.pop_front();
            self.lost = self.lost.saturating_add(1);
        }
        event.lost_before = self.lost;
        self.values.push_back(event);
        Ok(())
    }
    pub fn pop(&mut self) -> Option<Event> {
        self.values.pop_front()
    }
    #[must_use]
    pub const fn lost(&self) -> u64 {
        self.lost
    }
}
fn u16p(o: &mut Vec<u8>, v: u16) {
    o.extend(v.to_le_bytes());
}
fn u32p(o: &mut Vec<u8>, v: u32) {
    o.extend(v.to_le_bytes());
}
fn u64p(o: &mut Vec<u8>, v: u64) {
    o.extend(v.to_le_bytes());
}
fn optional(o: &mut Vec<u8>, v: Option<u64>) {
    match v {
        Some(v) => {
            o.push(1);
            u64p(o, v);
        }
        None => o.push(0),
    }
}
fn bytes(o: &mut Vec<u8>, v: &[u8]) -> Result<(), Error> {
    u32p(o, u32::try_from(v.len()).map_err(|_| Error::Limit)?);
    o.extend(v);
    Ok(())
}
struct Reader<'a> {
    b: &'a [u8],
    p: usize,
}
impl Reader<'_> {
    fn take(&mut self, n: usize) -> Result<&[u8], Error> {
        let e = self.p.checked_add(n).ok_or(Error::Corrupt)?;
        let v = self.b.get(self.p..e).ok_or(Error::Corrupt)?;
        self.p = e;
        Ok(v)
    }
    fn byte(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Option<u16> {
        Some(u16::from_le_bytes(self.take(2).ok()?.try_into().ok()?))
    }
    fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.take(4).ok()?.try_into().ok()?))
    }
    fn u64(&mut self) -> Option<u64> {
        Some(u64::from_le_bytes(self.take(8).ok()?.try_into().ok()?))
    }
    fn optional(&mut self) -> Result<Option<u64>, Error> {
        match self.byte()? {
            0 => Ok(None),
            1 => Ok(Some(self.u64().ok_or(Error::Corrupt)?)),
            _ => Err(Error::Corrupt),
        }
    }
    fn bytes(&mut self, max: u32) -> Result<Vec<u8>, Error> {
        let n = self.u32().ok_or(Error::Corrupt)?;
        if n > max {
            return Err(Error::Limit);
        }
        Ok(self.take(n as usize)?.to_vec())
    }
}
