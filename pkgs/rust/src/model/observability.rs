//! Stable observability schemas.

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
        let mut writer = Writer::new();
        writer.u32(MAGIC);
        writer.u16(VERSION);
        writer.u16(self.kind as u16);
        writer.u64(self.host_monotonic_ns);
        writer.u64(self.guest_time_ns);
        writer.extend(&self.correlation_id.to_le_bytes());
        writer.u64(self.machine);
        writer.optional(self.process);
        writer.optional(self.thread);
        writer.u64(self.lost_before);
        match self.registers {
            Some(ref r) => {
                writer.byte(1);
                writer.u64(r.guest_pc);
                writer.u64(r.stack_pointer);
                writer.u64(r.flags);
            }
            None => writer.byte(0),
        }
        writer.u16(u16::try_from(self.fields.len()).map_err(|_| Error::Limit)?);
        for f in &self.fields {
            if f.value.len() > l.field_bytes as usize {
                return Err(Error::Limit);
            }
            if f.privacy as u8 > l.maximum_privacy as u8 {
                return Err(Error::Privacy);
            }
            writer.bytes(f.name.as_bytes())?;
            writer.byte(f.privacy as u8);
            writer.bytes(&f.value)?;
        }
        if writer.len() > l.event_bytes as usize {
            return Err(Error::Limit);
        }
        Ok(writer.finish())
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

struct Writer(Vec<u8>);

impl Writer {
    fn new() -> Self {
        Self(Vec::new())
    }

    fn byte(&mut self, value: u8) {
        self.0.push(value);
    }

    fn u16(&mut self, value: u16) {
        self.extend(&value.to_le_bytes());
    }

    fn u32(&mut self, value: u32) {
        self.extend(&value.to_le_bytes());
    }

    fn u64(&mut self, value: u64) {
        self.extend(&value.to_le_bytes());
    }

    fn optional(&mut self, value: Option<u64>) {
        match value {
            Some(value) => {
                self.byte(1);
                self.u64(value);
            }
            None => self.byte(0),
        }
    }

    fn bytes(&mut self, value: &[u8]) -> Result<(), Error> {
        self.u32(u32::try_from(value.len()).map_err(|_| Error::Limit)?);
        self.extend(value);
        Ok(())
    }

    fn extend(&mut self, value: &[u8]) {
        self.0.extend_from_slice(value);
    }

    fn len(&self) -> usize {
        self.0.len()
    }

    fn finish(self) -> Vec<u8> {
        self.0
    }
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
