use crate::{network, Domain, Mount, Sandbox};
use std::ffi::{OsStr, OsString};
use std::net::Ipv4Addr;
use std::path::PathBuf;

/// Configuration for one Linux launch and its initial process.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) enum NetworkTransport {
    #[default]
    Virtual,
    Isolated,
    Host,
}

#[derive(Clone, Debug, Default)]
pub struct Config {
    pub(crate) rootfs: Option<PathBuf>,
    pub(crate) lower_layers: Vec<PathBuf>,
    pub(crate) overlay_work: Option<PathBuf>,
    pub(crate) working_directory: Option<OsString>,
    pub(crate) hostname: Option<OsString>,
    pub(crate) environment: Vec<(OsString, OsString)>,
    pub(crate) memory_limit: u64,
    pub(crate) pid_limit: u32,
    pub(crate) cpu_limit: u32,
    pub(crate) uid: Option<i32>,
    pub(crate) gid: Option<i32>,
    pub(crate) rootfs_read_only: bool,
    pub(crate) network_transport: NetworkTransport,
    pub(crate) network_namespace: Option<network::Namespace>,
    pub(crate) network_bridge: Option<network::Bridge>,
    pub(crate) network_ipv4: Option<Ipv4Addr>,
    pub(crate) network_interfaces: Vec<network::Interface>,
    pub(crate) publish: Vec<network::Rule>,
    pub(crate) publish_external: bool,
    pub(crate) sandbox: Sandbox,
    pub(crate) translation_cache: Option<PathBuf>,
    pub(crate) filesystem_generation: Option<PathBuf>,
    pub(crate) checkpoint_directory: Option<PathBuf>,
    pub(crate) restore_directory: Option<PathBuf>,
    pub(crate) mounts: Vec<Mount>,
    pub(crate) file_owners: Vec<(PathBuf, u32, u32)>,
    pub(crate) process_domain: Option<Domain>,
    pub(crate) executable_host: Option<PathBuf>,
}

impl Config {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }
    #[must_use]
    pub fn root(mut self, path: impl Into<PathBuf>) -> Self {
        self.rootfs = Some(path.into());
        self
    }
    #[must_use]
    pub fn overlay(
        mut self,
        lower: Vec<PathBuf>,
        upper: impl Into<PathBuf>,
        work: impl Into<PathBuf>,
    ) -> Self {
        self.rootfs = Some(upper.into());
        self.lower_layers = lower;
        self.overlay_work = Some(work.into());
        self
    }
    #[must_use]
    pub fn working_dir(mut self, path: impl Into<OsString>) -> Self {
        self.working_directory = Some(path.into());
        self
    }
    #[must_use]
    pub fn hostname(mut self, value: impl Into<OsString>) -> Self {
        self.hostname = Some(value.into());
        self
    }
    #[must_use]
    pub fn env(mut self, name: impl Into<OsString>, value: impl Into<OsString>) -> Self {
        self.set_environment(name.into(), value.into());
        self
    }
    pub(crate) fn set_environment(&mut self, name: OsString, value: OsString) {
        self.environment.retain(|(current, _)| current != &name);
        self.environment.push((name, value));
    }
    pub(crate) fn environment_value(&self, name: &OsStr) -> Option<&OsStr> {
        self.environment
            .iter()
            .find(|(current, _)| current == name)
            .map(|(_, value)| value.as_os_str())
    }
    #[must_use]
    pub const fn memory_limit(mut self, value: u64) -> Self {
        self.memory_limit = value;
        self
    }
    #[must_use]
    pub const fn process_limit(mut self, value: u32) -> Self {
        self.pid_limit = value;
        self
    }
    #[must_use]
    pub const fn cpu_limit(mut self, value: u32) -> Self {
        self.cpu_limit = value;
        self
    }
    #[must_use]
    pub const fn uid(mut self, value: i32) -> Self {
        self.uid = Some(value);
        self
    }
    #[must_use]
    pub const fn gid(mut self, value: i32) -> Self {
        self.gid = Some(value);
        self
    }
    #[must_use]
    pub const fn read_only_root(mut self, value: bool) -> Self {
        self.rootfs_read_only = value;
        self
    }
    #[must_use]
    pub const fn network(mut self, value: bool) -> Self {
        self.network_transport = if value {
            NetworkTransport::Isolated
        } else {
            NetworkTransport::Virtual
        };
        self
    }
    /// Use the host network stack, including host loopback, without a virtual namespace.
    #[must_use]
    pub const fn host_network(mut self, value: bool) -> Self {
        self.network_transport = if value {
            NetworkTransport::Host
        } else {
            NetworkTransport::Virtual
        };
        self
    }
    /// Select the virtual network namespace shared by related engine instances.
    #[must_use]
    pub fn network_namespace(mut self, value: network::Namespace) -> Self {
        self.network_namespace = Some(value);
        self
    }
    /// Attach the guest to one engine virtual bridge.
    #[must_use]
    pub fn network_bridge(mut self, value: network::Bridge) -> Self {
        self.network_bridge = Some(value);
        self
    }
    /// Assign the guest's IPv4 identity on its selected virtual bridge.
    #[must_use]
    pub const fn network_ipv4(mut self, value: Ipv4Addr) -> Self {
        self.network_ipv4 = Some(value);
        self
    }
    /// Attach one independently routed virtual IPv4 interface.
    #[must_use]
    pub fn interface(mut self, value: network::Interface) -> Self {
        self.network_interfaces.push(value);
        self
    }
    /// Add one host-to-guest publication rule.
    #[must_use]
    pub fn publish(mut self, value: network::Rule) -> Self {
        self.publish.push(value);
        self
    }
    /// Delegate external host listeners to the embedding daemon.
    ///
    /// This requires at least one publication rule.
    #[must_use]
    pub const fn publish_external(mut self, value: bool) -> Self {
        self.publish_external = value;
        self
    }
    #[must_use]
    pub const fn sandbox(mut self, value: Sandbox) -> Self {
        self.sandbox = value;
        self
    }
    #[must_use]
    pub fn translation_cache(mut self, path: impl Into<PathBuf>) -> Self {
        self.translation_cache = Some(path.into());
        self
    }
    /// Select the shared filesystem-generation file used to invalidate path caches.
    #[must_use]
    pub fn filesystem_generation(mut self, path: impl Into<PathBuf>) -> Self {
        self.filesystem_generation = Some(path.into());
        self
    }

    /// Arm native whole-process-tree checkpoint capture for this launch.
    #[must_use]
    pub fn checkpoint_directory(mut self, path: impl Into<PathBuf>) -> Self {
        self.checkpoint_directory = Some(path.into());
        self
    }

    /// Restore a native whole-process-tree checkpoint for this launch.
    #[must_use]
    pub fn restore_directory(mut self, path: impl Into<PathBuf>) -> Self {
        self.restore_directory = Some(path.into());
        self
    }

    /// Join this launch to an existing container process domain.
    #[must_use]
    pub const fn domain(mut self, domain: Domain) -> Self {
        self.process_domain = Some(domain);
        self
    }

    /// Set the initial Linux-visible owner for one rootfs-relative path.
    #[must_use]
    pub fn owner(mut self, path: impl Into<PathBuf>, uid: u32, gid: u32) -> Self {
        let path = path.into();
        self.file_owners.retain(|(current, _, _)| current != &path);
        self.file_owners.push((path, uid, gid));
        self
    }
}
