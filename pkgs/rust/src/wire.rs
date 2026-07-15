use crate::{Access, Config, Error, Sandbox};
use std::ffi::{OsStr, OsString};
use std::path::Path;

fn checked_bytes(value: &OsStr) -> Result<&[u8], Error> {
    use std::os::unix::ffi::OsStrExt;
    if value.as_bytes().contains(&0) {
        Err(Error::InvalidConfig(
            "configuration strings must not contain NUL",
        ))
    } else {
        Ok(value.as_bytes())
    }
}

const MAGIC: u32 = 0x484c_4346;
const ABI: u32 = 5;
const HEADER_SIZE: usize = 144;
const HEADER_SIZE_U32: u32 = 144;
const MAGIC_OFFSET: usize = 0;
const POOL_SIZE_OFFSET: usize = 4;
const HEADER_SIZE_OFFSET: usize = 8;
const ABI_OFFSET: usize = 12;
const MEMORY_OFFSET: usize = 16;
const PID_OFFSET: usize = 24;
const CPU_OFFSET: usize = 28;
const UID_OFFSET: usize = 32;
const GID_OFFSET: usize = 36;
const ROOT_READ_ONLY_OFFSET: usize = 40;
const SANDBOX_OFFSET: usize = 44;
const NETWORK_ISOLATED_OFFSET: usize = 48;
const ROOTFS_OFFSET: usize = 56;
const HOSTNAME_OFFSET: usize = 64;
const VOLUMES_OFFSET: usize = 76;
const WORKDIR_OFFSET: usize = 84;
const ENVIRONMENT_OFFSET: usize = 88;
const CACHE_OFFSET: usize = 92;
const ARGUMENTS_OFFSET: usize = 108;
const RESULT_OFFSET: usize = 132;
const RESERVED_OFFSET: usize = 136;
const TAIL_PADDING_OFFSET: usize = 140;

const _: () = assert!(MEMORY_OFFSET % 8 == 0);
const _: () = assert!(RESULT_OFFSET == 132);
const _: () = assert!(TAIL_PADDING_OFFSET + 4 == HEADER_SIZE);

struct Pool(Vec<u8>);

impl Pool {
    fn new() -> Self {
        Self(vec![0])
    }

    fn string(&mut self, value: Option<&OsStr>) -> Result<u32, Error> {
        let Some(value) = value else { return Ok(0) };
        let value = checked_bytes(value)?;
        let offset = u32::try_from(self.0.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        self.0.extend_from_slice(value);
        self.0.push(0);
        Ok(offset)
    }

    fn path(&mut self, value: Option<&Path>) -> Result<u32, Error> {
        self.string(value.map(Path::as_os_str))
    }

    fn arguments(&mut self, values: &[OsString]) -> Result<u32, Error> {
        let offset = u32::try_from(self.0.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        for value in values {
            self.0.extend_from_slice(checked_bytes(value)?);
            self.0.push(0);
        }
        self.0.push(0);
        Ok(offset)
    }
}

struct Header([u8; HEADER_SIZE]);

impl Header {
    fn new() -> Self {
        Self([0; HEADER_SIZE])
    }
    fn u32(&mut self, offset: usize, value: u32) {
        self.0[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    fn i32(&mut self, offset: usize, value: i32) {
        self.u32(offset, u32::from_ne_bytes(value.to_ne_bytes()));
    }
    fn u64(&mut self, offset: usize, value: u64) {
        self.0[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    }
}

fn environment(config: &Config) -> Result<Option<OsString>, Error> {
    use std::os::unix::ffi::OsStringExt;
    if config.environment.is_empty() {
        return Ok(None);
    }
    let mut output = Vec::new();
    for (index, (name, value)) in config.environment.iter().enumerate() {
        let name = checked_bytes(name)?;
        let value = checked_bytes(value)?;
        if name.is_empty()
            || name.contains(&b'=')
            || name.contains(&b'\n')
            || value.contains(&b'\n')
        {
            return Err(Error::InvalidConfig("invalid environment record"));
        }
        if index != 0 {
            output.push(b'\n');
        }
        output.extend_from_slice(name);
        output.push(b'=');
        output.extend_from_slice(value);
    }
    Ok(Some(OsString::from_vec(output)))
}

fn volumes(config: &Config) -> Result<Option<OsString>, Error> {
    use std::os::unix::ffi::OsStringExt;
    if config.mounts.is_empty() {
        return Ok(None);
    }
    let mut output = Vec::new();
    for (index, mount) in config.mounts.iter().enumerate() {
        let host = checked_bytes(mount.host.as_os_str())?;
        let guest = checked_bytes(mount.guest.as_os_str())?;
        if host.contains(&b',')
            || host.contains(&b':')
            || guest.contains(&b',')
            || guest.contains(&b':')
        {
            return Err(Error::InvalidConfig(
                "mount paths must not contain ':' or ','",
            ));
        }
        if index != 0 {
            output.push(b',');
        }
        output.extend_from_slice(if mount.access == Access::ReadOnly {
            b"ro:"
        } else {
            b"rw:"
        });
        output.extend_from_slice(guest);
        output.push(b':');
        output.extend_from_slice(host);
    }
    Ok(Some(OsString::from_vec(output)))
}

pub(crate) fn encode(
    config: &Config,
    arguments: &[OsString],
    result: Option<&Path>,
) -> Result<Vec<u8>, Error> {
    let mut pool = Pool::new();
    let rootfs = pool.path(config.rootfs.as_deref())?;
    let hostname = pool.string(config.hostname.as_deref())?;
    let workdir = pool.string(config.working_directory.as_deref())?;
    let environment = environment(config)?;
    let environment = pool.string(environment.as_deref())?;
    let cache = pool.path(config.translation_cache.as_deref())?;
    let volumes = volumes(config)?;
    let volumes = pool.string(volumes.as_deref())?;
    let result = pool.path(result)?;
    let arguments = pool.arguments(arguments)?;
    let pool_size = u32::try_from(pool.0.len())
        .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;

    let mut header = Header::new();
    header.u32(MAGIC_OFFSET, MAGIC);
    header.u32(POOL_SIZE_OFFSET, pool_size);
    header.u32(HEADER_SIZE_OFFSET, HEADER_SIZE_U32);
    header.u32(ABI_OFFSET, ABI);
    header.u64(MEMORY_OFFSET, config.memory_limit);
    header.u32(PID_OFFSET, config.pid_limit);
    header.u32(CPU_OFFSET, config.cpu_limit);
    header.i32(UID_OFFSET, config.uid.unwrap_or(-1));
    header.i32(GID_OFFSET, config.gid.unwrap_or(-1));
    header.u32(ROOT_READ_ONLY_OFFSET, u32::from(config.rootfs_read_only));
    header.u32(
        SANDBOX_OFFSET,
        match config.sandbox {
            Sandbox::Disabled => 0,
            Sandbox::Enabled => 1,
            Sandbox::SentryOnly => 2,
        },
    );
    header.u32(NETWORK_ISOLATED_OFFSET, u32::from(config.network_isolated));
    header.u32(ROOTFS_OFFSET, rootfs);
    header.u32(HOSTNAME_OFFSET, hostname);
    header.u32(VOLUMES_OFFSET, volumes);
    header.u32(WORKDIR_OFFSET, workdir);
    header.u32(ENVIRONMENT_OFFSET, environment);
    header.u32(CACHE_OFFSET, cache);
    header.u32(ARGUMENTS_OFFSET, arguments);
    header.u32(RESULT_OFFSET, result);
    header.u32(RESERVED_OFFSET, 0);
    header.u32(TAIL_PADDING_OFFSET, 0);

    let mut wire = Vec::with_capacity(HEADER_SIZE + pool.0.len());
    wire.extend_from_slice(&header.0);
    wire.extend_from_slice(&pool.0);
    debug_assert_eq!(wire.len(), HEADER_SIZE + pool_size as usize);
    Ok(wire)
}
