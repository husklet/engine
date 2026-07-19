use super::{
    handles_provider, namespace_provider, resource_error, spec_error, BindAccess, Config, Guest,
    MachineSpec, Mount, NetworkMode, ProviderId, Size, SpecError, SpecErrorCategory, TreeSource,
};

pub(super) fn allocate_memory(
    spec: &MachineSpec,
    authorities: &crate::extension::Authorities,
) -> Result<Vec<AllocatedResource>, SpecError> {
    let mut resources = Vec::new();
    for extension in &spec.extensions {
        let Some(memory) = authorities
            .provider(&extension.provider)
            .and_then(|authority| authority.memory.as_ref())
        else {
            continue;
        };
        for requirement in &extension.memory {
            let resource = memory
                .allocate(crate::extension::AllocationRequest {
                    size: requirement.size,
                    alignment: requirement.alignment,
                    protections: requirement.protections,
                    sharing: requirement.sharing,
                })
                .map_err(|error| {
                    resource_error(
                        SpecErrorCategory::Invalid,
                        "extensions.memory",
                        crate::spec::SpecResource::Provider(extension.provider.clone()),
                        format!("provider memory allocation failed: {}", error.context),
                    )
                })?;
            if let Err(error) = validate_resource(extension, requirement, &resource) {
                memory.release(resource.id);
                return Err(error);
            }
            resources.push(AllocatedResource {
                id: resource.id,
                resource,
                provider: memory.clone(),
            });
        }
    }
    Ok(resources)
}

pub(crate) struct AllocatedResource {
    pub(crate) resource: crate::extension::HostResource,
    id: crate::extension::ResourceId,
    provider: std::sync::Arc<dyn crate::extension::Memory>,
}

impl std::fmt::Debug for AllocatedResource {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_tuple("AllocatedResource")
            .field(&self.resource)
            .finish()
    }
}

impl Drop for AllocatedResource {
    fn drop(&mut self) {
        self.provider.release(self.id);
    }
}

fn validate_resource(
    extension: &crate::extension::ExtensionSpec,
    requirement: &crate::extension::MemoryRequirement,
    resource: &crate::extension::HostResource,
) -> Result<(), SpecError> {
    let covered = resource.regions.iter().try_fold(0_u64, |total, region| {
        if region.size == 0
            || region.offset % requirement.alignment != 0
            || (region.protections.read && !requirement.protections.read)
            || (region.protections.write && !requirement.protections.write)
            || (region.protections.execute && !requirement.protections.execute)
        {
            return None;
        }
        total.checked_add(region.size)
    });
    if covered.map_or(true, |size| size < requirement.size)
        || resource.inheritance != requirement.inheritance
    {
        return Err(resource_error(
            SpecErrorCategory::Invalid,
            "extensions.memory",
            crate::spec::SpecResource::Provider(extension.provider.clone()),
            "provider returned memory outside the declared size, alignment, protection, or inheritance contract",
        ));
    }
    Ok(())
}

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
    let projections: Vec<_> = extension
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
    if extension.services.is_empty() && projections.is_empty() {
        return None;
    }
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
