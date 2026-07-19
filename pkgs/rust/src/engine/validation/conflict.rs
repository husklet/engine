use super::{
    resource_error, spec_error, BTreeMap, BTreeSet, EngineCapabilities, MachineSpec, SpecError,
    SpecErrorCategory,
};

#[derive(Clone)]
struct Projection {
    owner: crate::spec::NamespaceOwner,
    active: bool,
    directory: bool,
}

pub(super) fn namespace_conflicts(
    spec: &MachineSpec,
    active: &BTreeSet<&crate::extension::ProviderId>,
) -> Result<Vec<crate::spec::NamespaceConflict>, SpecError> {
    use crate::spec::NamespaceOwner;

    let mut projections = BTreeMap::<std::path::PathBuf, Projection>::new();
    let mut conflicts = Vec::new();
    for mount in &spec.filesystem.mounts {
        projections.insert(
            mount.path.clone(),
            Projection {
                owner: NamespaceOwner::Filesystem,
                active: true,
                directory: false,
            },
        );
    }
    for extension in &spec.extensions {
        let projection_active = active.contains(&extension.provider);
        for entry in &extension.namespace {
            let projection = Projection {
                owner: NamespaceOwner::Extension(extension.provider.clone()),
                active: projection_active,
                directory: matches!(entry, crate::extension::NamespaceEntry::Directory(_)),
            };
            if let Some(first) = projections.insert(entry.path().to_owned(), projection.clone()) {
                record_namespace_conflict(entry.path(), &first, &projection, &mut conflicts)?;
            }
        }
    }
    for (path, projection) in &projections {
        for ancestor in path
            .ancestors()
            .skip(1)
            .take_while(|path| *path != std::path::Path::new("/"))
        {
            if let Some(parent) = projections.get(ancestor).filter(|parent| !parent.directory) {
                record_namespace_conflict(ancestor, parent, projection, &mut conflicts)?;
            }
        }
    }
    conflicts.sort_by(|left, right| left.path.cmp(&right.path));
    Ok(conflicts)
}

fn record_namespace_conflict(
    path: &std::path::Path,
    first: &Projection,
    second: &Projection,
    conflicts: &mut Vec<crate::spec::NamespaceConflict>,
) -> Result<(), SpecError> {
    if first.active && second.active {
        return Err(resource_error(
            SpecErrorCategory::Conflict,
            "namespaces",
            crate::spec::SpecResource::Path(path.to_owned()),
            "active namespace projections conflict",
        ));
    }
    conflicts.push(crate::spec::NamespaceConflict {
        path: path.to_owned(),
        first: first.owner.clone(),
        second: second.owner.clone(),
        disposition: crate::spec::ConflictDisposition::InactiveOptional,
    });
    Ok(())
}

pub(super) fn estimate_resources(
    spec: &MachineSpec,
    active: &BTreeSet<&crate::extension::ProviderId>,
    capabilities: &EngineCapabilities,
) -> Result<crate::spec::HostResourceEstimate, SpecError> {
    let selected = spec
        .extensions
        .iter()
        .filter(|extension| active.contains(&extension.provider));
    let (mut extension_memory_bytes, mut namespace_entries, mut services, mut mappings) =
        (0_u64, spec.filesystem.mounts.len(), 0_usize, 0_usize);
    for extension in selected {
        for memory in &extension.memory {
            extension_memory_bytes =
                extension_memory_bytes
                    .checked_add(memory.size)
                    .ok_or_else(|| {
                        spec_error(
                            SpecErrorCategory::Limit,
                            "resources",
                            "resource estimate overflow",
                        )
                    })?;
        }
        namespace_entries = namespace_entries
            .checked_add(extension.namespace.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "resources",
                    "resource estimate overflow",
                )
            })?;
        services = services
            .checked_add(extension.services.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "resources",
                    "resource estimate overflow",
                )
            })?;
        mappings = mappings
            .checked_add(extension.memory.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "resources",
                    "resource estimate overflow",
                )
            })?;
    }
    let size = |value| {
        u64::try_from(value).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources",
                "resource estimate exceeds u64",
            )
        })
    };
    validate_estimate_bounds(
        size(namespace_entries)?,
        size(services)?,
        size(mappings)?,
        extension_memory_bytes,
        capabilities,
    )?;
    Ok(crate::spec::HostResourceEstimate {
        memory_bytes: spec.resources.memory_bytes.unwrap_or(0),
        extension_memory_bytes,
        processes: spec.resources.process_limit.unwrap_or(0),
        cpus: spec.resources.cpu_limit.or(spec.cpu.count).unwrap_or(0),
        namespace_entries: u32::try_from(namespace_entries).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.namespace_entries",
                "resource estimate exceeds u32",
            )
        })?,
        services: u32::try_from(services).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.services",
                "resource estimate exceeds u32",
            )
        })?,
        mappings: u32::try_from(mappings).map_err(|_| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.mappings",
                "resource estimate exceeds u32",
            )
        })?,
        event_queue: event_queue(spec),
        cache_bytes: spec.cache.budget_bytes.unwrap_or(0),
    })
}

fn event_queue(spec: &MachineSpec) -> u32 {
    spec.observability
        .queue_capacity
        .or_else(|| {
            spec.observability
                .trace
                .as_ref()
                .map(|trace| trace.queue_capacity)
        })
        .unwrap_or(0)
}

fn validate_estimate_bounds(
    namespace_entries: u64,
    services: u64,
    mappings: u64,
    extension_memory_bytes: u64,
    capabilities: &EngineCapabilities,
) -> Result<(), SpecError> {
    let bounded = [
        (
            namespace_entries,
            u64::from(capabilities.limits.namespace_entries),
            "resources.namespace_entries",
        ),
        (
            services,
            u64::from(capabilities.limits.handles),
            "resources.services",
        ),
        (
            mappings,
            u64::from(capabilities.limits.mappings),
            "resources.mappings",
        ),
        (
            extension_memory_bytes,
            capabilities.limits.mapped_bytes,
            "resources.extension_memory_bytes",
        ),
    ];
    if let Some((actual, maximum, field)) = bounded
        .into_iter()
        .find(|(actual, maximum, _)| actual > maximum)
    {
        return Err(resource_error(
            SpecErrorCategory::Limit,
            field,
            crate::spec::SpecResource::Limit { actual, maximum },
            "selected extensions exceed a host resource limit",
        ));
    }
    Ok(())
}
