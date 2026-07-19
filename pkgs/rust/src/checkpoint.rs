//! Versioned checkpoint manifest foundation. Native capture/restore is not implemented.

const MAGIC: u32 = 0x484c_434b;
const VERSION: u16 = 1;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Manifest {
    pub engine_build: String,
    pub platform: PlatformIdentity,
    pub processes: Vec<ProcessState>,
    pub mappings: Vec<MappingState>,
    pub fds: Vec<FdDescription>,
    pub mounts: Vec<String>,
    pub sockets: u32,
    pub timers: u32,
    pub pending_signals: Vec<u32>,
    pub extensions: Vec<ExtensionState>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PlatformIdentity {
    pub architecture: String,
    pub cpu_model: String,
    pub page_size: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ProcessState {
    pub id: u64,
    pub parent: u64,
    pub threads: Vec<u64>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MappingState {
    pub address: u64,
    pub length: u64,
    pub protection: u8,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FdDescription {
    pub id: u64,
    pub offset: u64,
    pub flags: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum ResourcePolicy {
    Checkpointable = 1,
    Reconnectable = 2,
    Discardable = 3,
    Blocking = 4,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExtensionState {
    pub provider: String,
    pub version: u16,
    pub policy: ResourcePolicy,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Limits {
    pub bytes: u32,
    pub records: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Error {
    Corrupt,
    Version,
    Limit,
    IncompatibleBuild,
    BlockingResource,
}

impl Manifest {
    /// Encodes a bounded checkpoint manifest.
    ///
    /// # Errors
    /// Returns an error for unsupported resources or values exceeding the limits.
    pub fn encode(&self, limits: Limits) -> Result<Vec<u8>, Error> {
        if self
            .extensions
            .iter()
            .any(|value| value.policy == ResourcePolicy::Blocking)
        {
            return Err(Error::BlockingResource);
        }
        let mut out = Vec::new();
        put_u32(&mut out, MAGIC);
        put_u16(&mut out, VERSION);
        put_u16(&mut out, 0);
        put_string(&mut out, &self.engine_build)?;
        put_string(&mut out, &self.platform.architecture)?;
        put_string(&mut out, &self.platform.cpu_model)?;
        put_u32(&mut out, self.platform.page_size);
        put_vec(&mut out, &self.processes, |o, v| {
            put_u64(o, v.id);
            put_u64(o, v.parent);
            put_vec(o, &v.threads, |o, t| {
                put_u64(o, *t);
                Ok(())
            })
        })?;
        put_vec(&mut out, &self.mappings, |o, v| {
            put_u64(o, v.address);
            put_u64(o, v.length);
            o.push(v.protection);
            Ok(())
        })?;
        put_vec(&mut out, &self.fds, |o, v| {
            put_u64(o, v.id);
            put_u64(o, v.offset);
            put_u32(o, v.flags);
            Ok(())
        })?;
        put_vec(&mut out, &self.mounts, |o, v| put_string(o, v))?;
        put_u32(&mut out, self.sockets);
        put_u32(&mut out, self.timers);
        put_vec(&mut out, &self.pending_signals, |o, v| {
            put_u32(o, *v);
            Ok(())
        })?;
        put_vec(&mut out, &self.extensions, |o, v| {
            put_string(o, &v.provider)?;
            put_u16(o, v.version);
            o.push(v.policy as u8);
            put_bytes(o, &v.bytes)
        })?;
        if out.len() > limits.bytes as usize || records(self) > limits.records {
            return Err(Error::Limit);
        }
        Ok(out)
    }

    /// Decodes and validates a bounded checkpoint manifest.
    ///
    /// # Errors
    /// Returns an error for corrupt, incompatible, or over-limit input.
    pub fn decode(bytes: &[u8], limits: Limits, build: &str) -> Result<Self, Error> {
        if bytes.len() > limits.bytes as usize {
            return Err(Error::Limit);
        }
        let mut r = Reader { b: bytes, p: 0 };
        if r.u32()? != MAGIC {
            return Err(Error::Corrupt);
        }
        if r.u16()? != VERSION {
            return Err(Error::Version);
        }
        if r.u16()? != 0 {
            return Err(Error::Corrupt);
        }
        let engine_build = r.string()?;
        if engine_build != build {
            return Err(Error::IncompatibleBuild);
        }
        let platform = PlatformIdentity {
            architecture: r.string()?,
            cpu_model: r.string()?,
            page_size: r.u32()?,
        };
        let processes = r.vec(|r| {
            Ok(ProcessState {
                id: r.u64()?,
                parent: r.u64()?,
                threads: r.vec(Reader::u64)?,
            })
        })?;
        let mappings = r.vec(|r| {
            Ok(MappingState {
                address: r.u64()?,
                length: r.u64()?,
                protection: r.byte()?,
            })
        })?;
        let fds = r.vec(|r| {
            Ok(FdDescription {
                id: r.u64()?,
                offset: r.u64()?,
                flags: r.u32()?,
            })
        })?;
        let mounts = r.vec(Reader::string)?;
        let sockets = r.u32()?;
        let timers = r.u32()?;
        let pending_signals = r.vec(Reader::u32)?;
        let extensions = r.vec(|r| {
            let provider = r.string()?;
            let version = r.u16()?;
            let policy = match r.byte()? {
                1 => ResourcePolicy::Checkpointable,
                2 => ResourcePolicy::Reconnectable,
                3 => ResourcePolicy::Discardable,
                4 => ResourcePolicy::Blocking,
                _ => return Err(Error::Corrupt),
            };
            Ok(ExtensionState {
                provider,
                version,
                policy,
                bytes: r.bytes()?,
            })
        })?;
        let value = Self {
            engine_build,
            platform,
            processes,
            mappings,
            fds,
            mounts,
            sockets,
            timers,
            pending_signals,
            extensions,
        };
        if r.p != bytes.len() {
            return Err(Error::Corrupt);
        }
        if records(&value) > limits.records {
            return Err(Error::Limit);
        }
        Ok(value)
    }
}
fn records(v: &Manifest) -> u32 {
    u32::try_from(
        v.processes.len()
            + v.mappings.len()
            + v.fds.len()
            + v.mounts.len()
            + v.pending_signals.len()
            + v.extensions.len(),
    )
    .unwrap_or(u32::MAX)
}
fn put_u16(o: &mut Vec<u8>, v: u16) {
    o.extend(v.to_le_bytes());
}
fn put_u32(o: &mut Vec<u8>, v: u32) {
    o.extend(v.to_le_bytes());
}
fn put_u64(o: &mut Vec<u8>, v: u64) {
    o.extend(v.to_le_bytes());
}
fn put_bytes(o: &mut Vec<u8>, v: &[u8]) -> Result<(), Error> {
    put_u32(o, u32::try_from(v.len()).map_err(|_| Error::Limit)?);
    o.extend(v);
    Ok(())
}
fn put_string(o: &mut Vec<u8>, v: &str) -> Result<(), Error> {
    put_bytes(o, v.as_bytes())
}
fn put_vec<T, F: FnMut(&mut Vec<u8>, &T) -> Result<(), Error>>(
    o: &mut Vec<u8>,
    v: &[T],
    mut f: F,
) -> Result<(), Error> {
    put_u32(o, u32::try_from(v.len()).map_err(|_| Error::Limit)?);
    for x in v {
        f(o, x)?;
    }
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
    fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(
            self.take(2)?.try_into().map_err(|_| Error::Corrupt)?,
        ))
    }
    fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(
            self.take(4)?.try_into().map_err(|_| Error::Corrupt)?,
        ))
    }
    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(
            self.take(8)?.try_into().map_err(|_| Error::Corrupt)?,
        ))
    }
    fn bytes(&mut self) -> Result<Vec<u8>, Error> {
        let n = self.u32()? as usize;
        Ok(self.take(n)?.to_vec())
    }
    fn string(&mut self) -> Result<String, Error> {
        String::from_utf8(self.bytes()?).map_err(|_| Error::Corrupt)
    }
    fn vec<T, F: FnMut(&mut Self) -> Result<T, Error>>(
        &mut self,
        mut f: F,
    ) -> Result<Vec<T>, Error> {
        let n = self.u32()? as usize;
        if n > 1_000_000 {
            return Err(Error::Limit);
        }
        (0..n).map(|_| f(self)).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn value() -> Manifest {
        Manifest {
            engine_build: "build".into(),
            platform: PlatformIdentity {
                architecture: "aarch64".into(),
                cpu_model: "generic".into(),
                page_size: 4096,
            },
            processes: vec![ProcessState {
                id: 1,
                parent: 0,
                threads: vec![1, 2],
            }],
            mappings: vec![MappingState {
                address: 4096,
                length: 4096,
                protection: 3,
            }],
            fds: vec![FdDescription {
                id: 3,
                offset: 7,
                flags: 0,
            }],
            mounts: vec!["/".into()],
            sockets: 1,
            timers: 2,
            pending_signals: vec![15],
            extensions: vec![ExtensionState {
                provider: "mock".into(),
                version: 1,
                policy: ResourcePolicy::Checkpointable,
                bytes: vec![1, 2],
            }],
        }
    }
    #[test]
    fn deterministic_roundtrip_and_compatibility() {
        let l = Limits {
            bytes: 65536,
            records: 64,
        };
        let v = value();
        let a = v.encode(l).unwrap();
        let b = v.encode(l).unwrap();
        assert_eq!(a, b);
        assert_eq!(Manifest::decode(&a, l, "build").unwrap(), v);
        assert_eq!(
            Manifest::decode(&a, l, "other").unwrap_err(),
            Error::IncompatibleBuild
        );
    }
    #[test]
    fn corruption_version_limits_and_policy_are_typed() {
        let l = Limits {
            bytes: 65536,
            records: 64,
        };
        let mut b = value().encode(l).unwrap();
        b[0] ^= 1;
        assert_eq!(
            Manifest::decode(&b, l, "build").unwrap_err(),
            Error::Corrupt
        );
        let mut b = value().encode(l).unwrap();
        b[4] = 2;
        assert_eq!(
            Manifest::decode(&b, l, "build").unwrap_err(),
            Error::Version
        );
        assert_eq!(
            value()
                .encode(Limits {
                    bytes: 1,
                    records: 64
                })
                .unwrap_err(),
            Error::Limit
        );
        let mut v = value();
        v.extensions[0].policy = ResourcePolicy::Blocking;
        assert_eq!(v.encode(l).unwrap_err(), Error::BlockingResource);
    }
}
