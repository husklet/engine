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
const ABI: u32 = 6;
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
const PUBLISH_EXTERNAL_OFFSET: usize = 52;
const ROOTFS_OFFSET: usize = 56;
const HOSTNAME_OFFSET: usize = 64;
const NETWORK_NAMESPACE_OFFSET: usize = 68;
const PUBLISH_OFFSET: usize = 72;
const VOLUMES_OFFSET: usize = 76;
const WORKDIR_OFFSET: usize = 84;
const ENVIRONMENT_OFFSET: usize = 88;
const CACHE_OFFSET: usize = 92;
const NETWORK_BRIDGE_OFFSET: usize = 96;
const IP_OFFSET: usize = 100;
const FILESYSTEM_GENERATION_OFFSET: usize = 104;
const ARGUMENTS_OFFSET: usize = 108;
const RESULT_OFFSET: usize = 132;
const PUBLISH_COUNT_OFFSET: usize = 136;
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

    fn publish(&mut self, rules: &[crate::network::Rule]) -> Result<u32, Error> {
        if rules.is_empty() {
            return Ok(0);
        }
        while self.0.len() % 4 != 0 {
            self.0.push(0);
        }
        let offset = u32::try_from(self.0.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        for rule in rules {
            self.0.extend_from_slice(&rule.host_address().octets());
            self.0.extend_from_slice(&rule.host().to_le_bytes());
            self.0.extend_from_slice(&rule.guest().to_le_bytes());
        }
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

fn validate_publish(config: &Config) -> Result<(), Error> {
    if config.publish.len() > 32 {
        return Err(Error::InvalidConfig(
            "at most 32 port publication rules are supported",
        ));
    }
    Ok(())
}

fn validate_network(config: &Config) -> Result<(), Error> {
    let configured = config.network_bridge.is_some()
        || config.network_ipv4.is_some()
        || !config.publish.is_empty()
        || config.publish_external;
    if config.network_isolated && configured {
        return Err(Error::InvalidConfig(
            "isolated networking cannot use bridge, IPv4, or publication settings",
        ));
    }
    if config.network_ipv4.is_some() && config.network_bridge.is_none() {
        return Err(Error::InvalidConfig(
            "a network IPv4 address requires a virtual bridge",
        ));
    }
    if config.publish_external && config.publish.is_empty() {
        return Err(Error::InvalidConfig(
            "external publication requires at least one port rule",
        ));
    }
    Ok(())
}

pub(crate) fn encode(
    config: &Config,
    arguments: &[OsString],
    result: Option<&Path>,
) -> Result<Vec<u8>, Error> {
    validate_network(config)?;
    validate_publish(config)?;
    let mut pool = Pool::new();
    let rootfs = pool.path(config.rootfs.as_deref())?;
    let hostname = pool.string(config.hostname.as_deref())?;
    let workdir = pool.string(config.working_directory.as_deref())?;
    let environment = environment(config)?;
    let environment = pool.string(environment.as_deref())?;
    let cache = pool.path(config.translation_cache.as_deref())?;
    let namespace = pool.string(
        config
            .network_namespace
            .as_ref()
            .map(|value| OsStr::new(value.as_str())),
    )?;
    let bridge = pool.string(
        config
            .network_bridge
            .as_ref()
            .map(|value| OsStr::new(value.as_str())),
    )?;
    let ipv4 = config
        .network_ipv4
        .map(|value| OsString::from(value.to_string()));
    let ipv4 = pool.string(ipv4.as_deref())?;
    let filesystem_generation = pool.path(config.filesystem_generation.as_deref())?;
    let publish = pool.publish(&config.publish)?;
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
    header.u32(PUBLISH_EXTERNAL_OFFSET, u32::from(config.publish_external));
    header.u32(ROOTFS_OFFSET, rootfs);
    header.u32(HOSTNAME_OFFSET, hostname);
    header.u32(NETWORK_NAMESPACE_OFFSET, namespace);
    header.u32(PUBLISH_OFFSET, publish);
    header.u32(VOLUMES_OFFSET, volumes);
    header.u32(WORKDIR_OFFSET, workdir);
    header.u32(ENVIRONMENT_OFFSET, environment);
    header.u32(CACHE_OFFSET, cache);
    header.u32(NETWORK_BRIDGE_OFFSET, bridge);
    header.u32(IP_OFFSET, ipv4);
    header.u32(FILESYSTEM_GENERATION_OFFSET, filesystem_generation);
    header.u32(ARGUMENTS_OFFSET, arguments);
    header.u32(RESULT_OFFSET, result);
    header.u32(PUBLISH_COUNT_OFFSET, config.publish.len() as u32);
    header.u32(TAIL_PADDING_OFFSET, 0);

    let mut wire = Vec::with_capacity(HEADER_SIZE + pool.0.len());
    wire.extend_from_slice(&header.0);
    wire.extend_from_slice(&pool.0);
    debug_assert_eq!(wire.len(), HEADER_SIZE + pool_size as usize);
    Ok(wire)
}

#[cfg(test)]
mod tests {
    use std::ffi::OsString;
    use std::net::Ipv4Addr;

    use crate::network::{Bridge, Namespace, Rule};

    use super::*;

    fn word(wire: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(wire[offset..offset + 4].try_into().unwrap())
    }

    fn string(wire: &[u8], field: usize) -> Option<&str> {
        let offset = word(wire, field) as usize;
        if offset == 0 {
            return None;
        }
        let pool = &wire[HEADER_SIZE..];
        let end = pool[offset..]
            .iter()
            .position(|byte| *byte == 0)
            .map(|length| offset + length)
            .unwrap();
        Some(std::str::from_utf8(&pool[offset..end]).unwrap())
    }

    #[test]
    fn network_fields_use_the_exact_launch_abi_five_offsets() {
        let config = Config::new()
            .network_namespace(Namespace::new("container-alpha").unwrap())
            .network_bridge(Bridge::new("bridge-alpha").unwrap())
            .network_ipv4(Ipv4Addr::new(172, 18, 0, 2))
            .publish(Rule::new(8_080, 80).unwrap().address(Ipv4Addr::LOCALHOST))
            .publish(Rule::new(8_443, 443).unwrap())
            .publish_external(true);
        let wire = encode(&config, &[OsString::from("/bin/true")], None).unwrap();

        assert_eq!(word(&wire, MAGIC_OFFSET), MAGIC);
        assert_eq!(word(&wire, HEADER_SIZE_OFFSET), HEADER_SIZE_U32);
        assert_eq!(word(&wire, ABI_OFFSET), ABI);
        assert_eq!(word(&wire, NETWORK_ISOLATED_OFFSET), 0);
        assert_eq!(word(&wire, PUBLISH_EXTERNAL_OFFSET), 1);
        assert_eq!(
            string(&wire, NETWORK_NAMESPACE_OFFSET),
            Some("container-alpha")
        );
        let offset = word(&wire, PUBLISH_OFFSET) as usize;
        let pool = &wire[HEADER_SIZE..];
        assert_eq!(
            &pool[offset..offset + 8],
            &[127, 0, 0, 1, 0x90, 0x1f, 80, 0]
        );
        assert_eq!(
            &pool[offset + 8..offset + 16],
            &[0, 0, 0, 0, 0xfb, 0x20, 0xbb, 1]
        );
        assert_eq!(word(&wire, PUBLISH_COUNT_OFFSET), 2);
        assert_eq!(string(&wire, NETWORK_BRIDGE_OFFSET), Some("bridge-alpha"));
        assert_eq!(string(&wire, IP_OFFSET), Some("172.18.0.2"));
        assert_eq!(word(&wire, TAIL_PADDING_OFFSET), 0);
    }

    #[test]
    fn existing_network_isolation_encoding_is_preserved() {
        let wire = encode(
            &Config::new()
                .network(true)
                .network_namespace(Namespace::new("isolated-namespace").unwrap()),
            &[OsString::from("/bin/true")],
            None,
        )
        .unwrap();
        assert_eq!(word(&wire, NETWORK_ISOLATED_OFFSET), 1);
        assert_eq!(word(&wire, PUBLISH_EXTERNAL_OFFSET), 0);
        assert_eq!(
            string(&wire, NETWORK_NAMESPACE_OFFSET),
            Some("isolated-namespace")
        );
        assert_eq!(string(&wire, PUBLISH_OFFSET), None);
        assert_eq!(word(&wire, PUBLISH_COUNT_OFFSET), 0);
        assert_eq!(string(&wire, NETWORK_BRIDGE_OFFSET), None);
        assert_eq!(string(&wire, IP_OFFSET), None);
    }

    #[test]
    fn filesystem_generation_uses_the_c_abi_offset_and_rejects_nul() {
        let wire = encode(
            &Config::new().filesystem_generation("/run/hl/filesystem-generation"),
            &[OsString::from("/bin/true")],
            None,
        )
        .unwrap();
        assert_eq!(
            string(&wire, FILESYSTEM_GENERATION_OFFSET),
            Some("/run/hl/filesystem-generation")
        );

        use std::os::unix::ffi::OsStringExt;
        let invalid = std::path::PathBuf::from(OsString::from_vec(b"/run/bad\0path".to_vec()));
        assert!(encode(
            &Config::new().filesystem_generation(invalid),
            &[OsString::from("/bin/true")],
            None,
        )
        .is_err());
    }

    #[test]
    fn invalid_network_combinations_fail_before_launch() {
        let arguments = [OsString::from("/bin/true")];
        assert!(encode(
            &Config::new()
                .network(true)
                .network_bridge(Bridge::new("isolated").unwrap()),
            &arguments,
            None,
        )
        .is_err());
        assert!(encode(
            &Config::new().network_ipv4(Ipv4Addr::LOCALHOST),
            &arguments,
            None,
        )
        .is_err());
        assert!(encode(&Config::new().publish_external(true), &arguments, None,).is_err());

        let mut config = Config::new();
        for port in 1..=33 {
            config = config.publish(Rule::new(port, port).unwrap());
        }
        assert!(encode(&config, &arguments, None).is_err());
    }
}
