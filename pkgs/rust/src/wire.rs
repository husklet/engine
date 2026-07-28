use crate::{config::NetworkTransport, Access, Config, Error, Sandbox};
use std::ffi::{OsStr, OsString};
use std::path::Path;

pub(crate) struct LaunchWire;

impl LaunchWire {
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
}

const MAGIC: u32 = 0x484c_4346;
const ABI: u32 = 1;

#[must_use]
pub const fn launch_abi() -> u32 {
    ABI
}
const HEADER_SIZE: usize = 184;
const HEADER_SIZE_U32: u32 = 184;
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
const LOWER_LAYERS_OFFSET: usize = 60;
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
const CHECKPOINT_MODE_OFFSET: usize = 124;
const CHECKPOINT_POLICY_OFFSET: usize = 128;
const RESULT_OFFSET: usize = 132;
const PUBLISH_COUNT_OFFSET: usize = 136;
const INTERFACES_OFFSET: usize = 140;
const FILE_OWNERS_OFFSET: usize = 144;
const RESERVED_OFFSET: usize = 148;
const PROCESS_DOMAIN_OFFSET: usize = 152;
const EXECUTABLE_HOST_OFFSET: usize = 168;
const NETWORK_TRANSPORT_OFFSET: usize = 172;
const LOWER_COUNT_OFFSET: usize = 176;
const OVERLAY_WORK_OFFSET: usize = 180;

pub(crate) const CHECKPOINT_CAPTURE: u32 = 1;
pub(crate) const CHECKPOINT_RESTORE: u32 = 2;

const _: () = assert!(MEMORY_OFFSET % 8 == 0);
const _: () = assert!(RESULT_OFFSET == 132);
const _: () = assert!(RESERVED_OFFSET + 4 == PROCESS_DOMAIN_OFFSET);
const _: () = assert!(EXECUTABLE_HOST_OFFSET + 4 == NETWORK_TRANSPORT_OFFSET);
const _: () = assert!(OVERLAY_WORK_OFFSET + 4 == HEADER_SIZE);

struct Pool(Vec<u8>);

impl Pool {
    fn new() -> Self {
        Self(vec![0])
    }

    fn string(&mut self, value: Option<&OsStr>) -> Result<u32, Error> {
        let Some(value) = value else { return Ok(0) };
        let value = LaunchWire::checked_bytes(value)?;
        let offset = u32::try_from(self.0.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        self.0.extend_from_slice(value);
        self.0.push(0);
        Ok(offset)
    }

    fn path(&mut self, value: Option<&Path>) -> Result<u32, Error> {
        self.string(value.map(Path::as_os_str))
    }

    fn paths(&mut self, values: &[std::path::PathBuf]) -> Result<u32, Error> {
        if values.is_empty() {
            return Ok(0);
        }
        let offset = u32::try_from(self.0.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        for value in values {
            self.0
                .extend_from_slice(LaunchWire::checked_bytes(value.as_os_str())?);
            self.0.push(0);
        }
        Ok(offset)
    }

    fn arguments(&mut self, values: &[OsString]) -> Result<u32, Error> {
        let offset = u32::try_from(self.0.len())
            .map_err(|_| Error::InvalidConfig("launch configuration is too large"))?;
        for value in values {
            self.0.extend_from_slice(LaunchWire::checked_bytes(value)?);
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

impl LaunchWire {
    const fn network_transport(value: NetworkTransport) -> u32 {
        match value {
            NetworkTransport::Virtual => 0,
            NetworkTransport::Isolated => 1,
            NetworkTransport::Host => 2,
        }
    }
}

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

impl LaunchWire {
    fn environment(config: &Config) -> Result<Option<OsString>, Error> {
        use std::os::unix::ffi::OsStringExt;
        if config.environment.is_empty() {
            return Ok(None);
        }
        let mut output = Vec::new();
        for (index, (name, value)) in config.environment.iter().enumerate() {
            let name = Self::checked_bytes(name)?;
            let value = Self::checked_bytes(value)?;
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

    fn file_owners(config: &Config) -> Result<Option<OsString>, Error> {
        use std::os::unix::ffi::{OsStrExt as _, OsStringExt as _};
        if config.file_owners.is_empty() {
            return Ok(None);
        }
        let mut entries = config.file_owners.iter().collect::<Vec<_>>();
        entries.sort_by(|left, right| left.0.cmp(&right.0));
        let mut output = Vec::new();
        for (index, (path, uid, gid)) in entries.into_iter().enumerate() {
            let bytes = path.as_os_str().as_bytes();
            if path.is_absolute()
                || bytes.is_empty()
                || bytes.contains(&0)
                || bytes.contains(&b'\n')
                || bytes.contains(&b'\t')
                || path
                    .components()
                    .any(|part| !matches!(part, std::path::Component::Normal(_)))
            {
                return Err(Error::InvalidConfig(
                    "file owner paths must be normalized and relative",
                ));
            }
            if index != 0 {
                output.push(b'\n');
            }
            output.extend_from_slice(bytes);
            output.push(b'\t');
            output.extend_from_slice(uid.to_string().as_bytes());
            output.push(b'\t');
            output.extend_from_slice(gid.to_string().as_bytes());
        }
        Ok(Some(OsString::from_vec(output)))
    }

    fn volumes(config: &Config) -> Result<Option<OsString>, Error> {
        use std::os::unix::ffi::OsStringExt;
        if config.mounts.is_empty() && config.namespace_links.is_empty() {
            return Ok(None);
        }
        let mut output = Vec::new();
        let mut index = 0;
        for mount in &config.mounts {
            let host = Self::checked_bytes(mount.host.as_os_str())?;
            let guest = Self::checked_bytes(mount.guest.as_os_str())?;
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
            index += 1;
        }
        for (host, guest) in &config.namespace_links {
            let host = Self::checked_bytes(host.as_os_str())?;
            let guest = Self::checked_bytes(guest.as_os_str())?;
            if host.contains(&b',')
                || host.contains(&b':')
                || guest.contains(&b',')
                || guest.contains(&b':')
            {
                return Err(Error::InvalidConfig(
                    "namespace link paths must not contain ':' or ','",
                ));
            }
            if index != 0 {
                output.push(b',');
            }
            output.extend_from_slice(b"link:");
            output.extend_from_slice(guest);
            output.push(b':');
            output.extend_from_slice(host);
            index += 1;
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
            || config.publish_external
            || !config.network_interfaces.is_empty();
        if config.network_transport == NetworkTransport::Isolated && configured {
            return Err(Error::InvalidConfig(
                "isolated networking cannot use bridge, IPv4, or publication settings",
            ));
        }
        if config.network_transport == NetworkTransport::Host
            && (config.network_namespace.is_some() || configured)
        {
            return Err(Error::InvalidConfig(
                "host networking cannot be combined with isolation or virtual networking",
            ));
        }
        if config.network_ipv4.is_some() && config.network_bridge.is_none() {
            return Err(Error::InvalidConfig(
                "a network IPv4 address requires a virtual bridge",
            ));
        }
        if !config.network_interfaces.is_empty()
            && (config.network_bridge.is_some() || config.network_ipv4.is_some())
        {
            return Err(Error::InvalidConfig(
                "legacy bridge fields cannot be mixed with virtual interfaces",
            ));
        }
        if config.network_interfaces.len() > 8 {
            return Err(Error::InvalidConfig(
                "at most eight virtual network interfaces are supported",
            ));
        }
        if config.publish_external && config.publish.is_empty() {
            return Err(Error::InvalidConfig(
                "external publication requires at least one port rule",
            ));
        }
        Ok(())
    }

    fn interfaces(config: &Config) -> Option<OsString> {
        use std::os::unix::ffi::OsStringExt;
        if config.network_interfaces.is_empty() {
            return None;
        }
        let mut bytes = Vec::new();
        for (index, interface) in config.network_interfaces.iter().enumerate() {
            if index != 0 {
                bytes.push(b'\n');
            }
            bytes.extend_from_slice(interface.bridge().as_str().as_bytes());
            bytes.push(b'=');
            bytes.extend_from_slice(interface.address().to_string().as_bytes());
            bytes.push(b'/');
            bytes.extend_from_slice(interface.prefix().to_string().as_bytes());
        }
        Some(OsString::from_vec(bytes))
    }
}

#[allow(clippy::too_many_lines)]
impl LaunchWire {
    pub(crate) fn encode(
        config: &Config,
        arguments: &[OsString],
        result: Option<&Path>,
    ) -> Result<Vec<u8>, Error> {
        if config.lower_layers.len() > 8
            || (config.lower_layers.is_empty() != config.overlay_work.is_none())
        {
            return Err(Error::InvalidConfig(
                "overlay requires one to eight lower layers and a work directory",
            ));
        }
        Self::validate_network(config)?;
        Self::validate_publish(config)?;
        let mut pool = Pool::new();
        let rootfs = pool.path(config.rootfs.as_deref())?;
        let lower_layers = pool.paths(&config.lower_layers)?;
        let overlay_work = pool.path(config.overlay_work.as_deref())?;
        let executable_host = pool.path(config.executable_host.as_deref())?;
        let hostname = pool.string(config.hostname.as_deref())?;
        let workdir = pool.string(config.working_directory.as_deref())?;
        let environment = Self::environment(config)?;
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
        let interfaces = Self::interfaces(config);
        let interfaces = pool.string(interfaces.as_deref())?;
        let file_owners = Self::file_owners(config)?;
        let file_owners = pool.string(file_owners.as_deref())?;
        let filesystem_generation = pool.path(config.filesystem_generation.as_deref())?;
        let publish = pool.publish(&config.publish)?;
        let volumes = Self::volumes(config)?;
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
        header.u32(
            NETWORK_ISOLATED_OFFSET,
            u32::from(config.network_transport == NetworkTransport::Isolated),
        );
        header.u32(
            NETWORK_TRANSPORT_OFFSET,
            Self::network_transport(config.network_transport),
        );
        header.u32(PUBLISH_EXTERNAL_OFFSET, u32::from(config.publish_external));
        header.u32(ROOTFS_OFFSET, rootfs);
        header.u32(LOWER_LAYERS_OFFSET, lower_layers);
        header.u32(
            LOWER_COUNT_OFFSET,
            u32::try_from(config.lower_layers.len())
                .map_err(|_| Error::InvalidConfig("too many overlay lower layers"))?,
        );
        header.u32(OVERLAY_WORK_OFFSET, overlay_work);
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
        header.u32(CHECKPOINT_MODE_OFFSET, config.checkpoint_mode);
        header.u32(
            CHECKPOINT_POLICY_OFFSET,
            match config.checkpoint_policy {
                crate::spec::IncompatibleResourcePolicy::Unspecified => 0,
                crate::spec::IncompatibleResourcePolicy::Reconnect => 1,
                crate::spec::IncompatibleResourcePolicy::DiscardOptional => 2,
                crate::spec::IncompatibleResourcePolicy::Refuse => 3,
            },
        );
        header.u32(RESULT_OFFSET, result);
        header.u32(
            PUBLISH_COUNT_OFFSET,
            u32::try_from(config.publish.len())
                .map_err(|_| Error::InvalidConfig("too many port publication rules"))?,
        );
        header.u32(INTERFACES_OFFSET, interfaces);
        header.u32(FILE_OWNERS_OFFSET, file_owners);
        let process_domain = config
            .process_domain
            .unwrap_or(crate::Domain::create()?)
            .identity();
        header.u64(PROCESS_DOMAIN_OFFSET, process_domain[0]);
        header.u64(PROCESS_DOMAIN_OFFSET + 8, process_domain[1]);
        header.u32(EXECUTABLE_HOST_OFFSET, executable_host);

        let mut wire = Vec::with_capacity(HEADER_SIZE + pool.0.len());
        wire.extend_from_slice(&header.0);
        wire.extend_from_slice(&pool.0);
        debug_assert_eq!(wire.len(), HEADER_SIZE + pool_size as usize);
        Ok(wire)
    }

    pub(crate) fn domain(wire: &[u8]) -> [u64; 2] {
        let word = |offset| u64::from_le_bytes(wire[offset..offset + 8].try_into().unwrap());
        [word(PROCESS_DOMAIN_OFFSET), word(PROCESS_DOMAIN_OFFSET + 8)]
    }
}

#[cfg(test)]
mod tests;
