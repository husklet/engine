use super::{
    handles_provider, namespace_provider, spec_error, BindAccess, Config, Guest, MachineSpec,
    Mount, NetworkMode, ProviderId, Size, SpecError, SpecErrorCategory, TreeSource,
};

pub(super) struct Launch {
    pub(super) guest: Guest,
    pub(super) config: Config,
    pub(super) program: std::ffi::OsString,
    pub(super) arguments: Vec<std::ffi::OsString>,
    pub(super) terminal: Option<Size>,
    pub(super) projections: Vec<crate::projection::Projection>,
    pub(super) services: Option<ServiceLaunch>,
}

pub(crate) struct ServiceLaunch {
    pub(super) provider: ProviderId,
    pub(super) registrations: Vec<crate::extension::ServiceRegistration>,
    pub(super) projections: Vec<crate::service::ServiceProjection>,
    pub(super) credentials: crate::extension::Credentials,
}

impl Launch {
    #[allow(clippy::too_many_lines)]
    pub(super) fn from_spec(spec: MachineSpec) -> Result<Self, SpecError> {
        let services = lower_services(&spec);
        let mut config = Config::new()
            .working_dir(spec.process.cwd)
            .read_only_root(spec.filesystem.read_only)
            .sandbox(spec.security.sandbox);
        config = match spec.network.mode {
            NetworkMode::Host => config.host_network(true),
            NetworkMode::None => config.network(true),
            NetworkMode::Virtual => config,
        };
        match spec.filesystem.root {
            Some(TreeSource::HostDirectory(root)) => config = config.root(root),
            Some(TreeSource::Overlay { lower, upper, work }) => {
                let lower = lower
                    .into_iter()
                    .map(|layer| match layer {
                        TreeSource::HostDirectory(path) => path,
                        _ => unreachable!("validated overlay lower"),
                    })
                    .collect();
                config = config.overlay(lower, upper, work);
            }
            _ => {}
        }
        if let Some(uid) = spec.identity.uid {
            config = config.uid(i32::try_from(uid).map_err(|_| {
                spec_error(
                    SpecErrorCategory::Invalid,
                    "identity.uid",
                    "uid exceeds the backend range",
                )
            })?);
        }
        if let Some(gid) = spec.identity.gid {
            config = config.gid(i32::try_from(gid).map_err(|_| {
                spec_error(
                    SpecErrorCategory::Invalid,
                    "identity.gid",
                    "gid exceeds the backend range",
                )
            })?);
        }
        if let Some(hostname) = spec.identity.hostname {
            config = config.hostname(hostname);
        }
        if let Some(memory) = spec.resources.memory_bytes {
            config = config.memory_limit(memory);
        }
        if let Some(processes) = spec.resources.process_limit {
            config = config.process_limit(processes);
        }
        if let Some(cpus) = spec.resources.cpu_limit.or(spec.cpu.count) {
            config = config.cpu_limit(cpus);
        }
        if let Some(cache) = spec.cache.directory {
            config = config.translation_cache(cache);
        }
        if let Some(coherence) = spec.filesystem.coherence {
            config = config.filesystem_generation(coherence.host_path());
        }
        for (name, value) in spec.process.env {
            config = config.env(name, value);
        }
        for mount in spec.filesystem.mounts {
            config.mounts.push(match mount.access {
                BindAccess::ReadOnly => Mount::read_only(mount.host, mount.path),
                BindAccess::ReadWrite => Mount::read_write(mount.host, mount.path),
            });
        }
        let (mut config, projections) = lower_projections(config, &spec.extensions)?;
        for ownership in spec.filesystem.ownership {
            let relative = ownership.path.strip_prefix("/").map_err(|_| {
                spec_error(
                    SpecErrorCategory::Invalid,
                    "filesystem.ownership",
                    "ownership paths must be absolute guest paths",
                )
            })?;
            config = config.owner(relative, ownership.uid, ownership.gid);
        }
        if let Some(domain) = spec.process.domain {
            config = config.domain(domain);
        }
        if let Some(namespace) = spec.network.namespace {
            config = config.network_namespace(namespace);
        }
        for interface in spec.network.interfaces {
            config = config.interface(interface);
        }
        for rule in spec.network.port_forwards {
            config = config.publish(rule);
        }
        config = config.publish_external(spec.network.external_listeners);
        let program = spec.process.executable;
        let arguments = spec.process.argv.into_iter().skip(1).collect();
        Ok(Self {
            guest: spec.guest.architecture,
            config,
            program,
            arguments,
            terminal: spec.process.terminal,
            projections,
            services,
        })
    }
}

fn lower_services(spec: &MachineSpec) -> Option<ServiceLaunch> {
    let extension = spec
        .extensions
        .iter()
        .find(|extension| extension.provider == handles_provider())?;
    let projections = extension
        .namespace
        .iter()
        .filter_map(|entry| match entry {
            crate::extension::NamespaceEntry::Service(value) => {
                Some(crate::service::ServiceProjection {
                    path: value.path.clone(),
                    service: value.service,
                    mode: value.metadata.mode,
                    uid: value.metadata.uid,
                    gid: value.metadata.gid,
                })
            }
            _ => None,
        })
        .collect();
    Some(ServiceLaunch {
        provider: extension.provider.clone(),
        registrations: extension.services.clone(),
        projections,
        credentials: crate::extension::Credentials {
            uid: spec.identity.uid.unwrap_or(0),
            gid: spec.identity.gid.unwrap_or(0),
            groups: spec.identity.supplementary_groups.clone(),
        },
    })
}

fn lower_projections(
    mut config: Config,
    extensions: &[crate::extension::ExtensionSpec],
) -> Result<(Config, Vec<crate::projection::Projection>), SpecError> {
    let entries = extensions
        .iter()
        .filter(|extension| extension.provider == namespace_provider())
        .flat_map(|extension| extension.namespace.iter())
        .collect::<Vec<_>>();
    if entries.is_empty() {
        return Ok((config, Vec::new()));
    }
    let materialized = entries
        .iter()
        .copied()
        .filter(|entry| !matches!(entry, crate::extension::NamespaceEntry::HostBind(_)))
        .collect::<Vec<_>>();
    let mut projections = Vec::new();
    if !materialized.is_empty() {
        let (projection, mounts) = crate::projection::Projection::materialize(&materialized)?;
        config.mounts.extend(mounts);
        projections.push(projection);
    }
    for entry in &entries {
        if let crate::extension::NamespaceEntry::HostBind(bind) = entry {
            config.mounts.push(Mount::read_only(&bind.host, &bind.path));
        }
    }
    for entry in entries {
        let (uid, gid) = match entry {
            crate::extension::NamespaceEntry::Directory(value) => {
                (value.metadata.uid, value.metadata.gid)
            }
            crate::extension::NamespaceEntry::File(value) => {
                (value.metadata.uid, value.metadata.gid)
            }
            crate::extension::NamespaceEntry::Symlink(value) => (value.uid, value.gid),
            _ => continue,
        };
        let relative = entry.path().strip_prefix("/").map_err(|_| {
            spec_error(
                SpecErrorCategory::Invalid,
                "extensions.namespace",
                "projection path must be absolute",
            )
        })?;
        config = config.owner(relative, uid, gid);
    }
    Ok((config, projections))
}
