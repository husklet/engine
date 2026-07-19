use super::{
    spec_error, validate_guest_path, BTreeMap, BTreeSet, EngineCapabilities, MachineSpec,
    SpecError, SpecErrorCategory,
};

pub(super) fn validate_extension_shapes(
    capabilities: &EngineCapabilities,
    spec: &MachineSpec,
) -> Result<(), SpecError> {
    let mut providers = BTreeSet::new();
    let mut environment = spec
        .process
        .env
        .iter()
        .map(|(name, _)| name.as_encoded_bytes().to_vec())
        .collect::<BTreeSet<_>>();
    let mut environment_bytes = spec
        .process
        .env
        .iter()
        .map(|(name, value)| name.as_encoded_bytes().len() + value.as_encoded_bytes().len() + 2)
        .sum::<usize>();
    for (index, extension) in spec.extensions.iter().enumerate() {
        if !providers.insert(&extension.provider) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].provider"),
                "a provider may appear only once per launch",
            ));
        }
        if extension.config.schema.is_empty()
            || extension.config.schema.len() > 256
            || extension.config.bytes.len() > capabilities.limits.request_bytes as usize
        {
            return Err(spec_error(
                SpecErrorCategory::Limit,
                format!("extensions[{index}].config"),
                "extension configuration schema or bytes exceed engine bounds",
            ));
        }
        if extension
            .required_features
            .intersection(&extension.optional_features)
            .next()
            .is_some()
        {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].features"),
                "the same feature cannot be both required and optional",
            ));
        }
        validate_extension_namespace(capabilities, extension, index)?;
        validate_extension_services(capabilities, extension, index)?;
        validate_extension_memory(capabilities, extension, index)?;
        validate_extension_environment(extension, index, &mut environment, &mut environment_bytes)?;
    }
    if environment_bytes > capabilities.limits.environment_bytes as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "extensions.environment",
            "combined guest environment exceeds the engine limit",
        ));
    }
    Ok(())
}

fn validate_extension_environment(
    extension: &crate::extension::ExtensionSpec,
    index: usize,
    environment: &mut BTreeSet<Vec<u8>>,
    environment_bytes: &mut usize,
) -> Result<(), SpecError> {
    for (name, value) in &extension.environment {
        let name_bytes = name.as_encoded_bytes();
        let value_bytes = value.as_encoded_bytes();
        if name_bytes.is_empty()
            || name_bytes.contains(&b'=')
            || name_bytes.contains(&b'\n')
            || name_bytes.contains(&0)
            || value_bytes.contains(&b'\n')
            || value_bytes.contains(&0)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                format!("extensions[{index}].environment"),
                "extension environment contains an invalid record",
            ));
        }
        if !environment.insert(name_bytes.to_vec()) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].environment"),
                "guest environment keys must be unique across the complete launch",
            ));
        }
        *environment_bytes = environment_bytes
            .checked_add(name_bytes.len() + value_bytes.len() + 2)
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "extensions.environment",
                    "guest environment size overflow",
                )
            })?;
    }
    Ok(())
}

fn validate_extension_namespace(
    capabilities: &EngineCapabilities,
    extension: &crate::extension::ExtensionSpec,
    index: usize,
) -> Result<(), SpecError> {
    use crate::extension::{FileSource, NamespaceEntry};

    if extension.namespace.len() > capabilities.limits.namespace_entries as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].namespace"),
            "namespace entry count exceeds the engine limit",
        ));
    }
    let mut paths = BTreeMap::new();
    let mut projected_bytes = 0_u64;
    for entry in &extension.namespace {
        let field = format!("extensions[{index}].namespace");
        validate_guest_path(
            entry.path(),
            capabilities.limits.path_bytes,
            "extensions.namespace",
        )
        .map_err(|mut error| {
            error.field.clone_from(&field);
            error
        })?;
        if entry.path() == std::path::Path::new("/") {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                field,
                "extensions cannot replace the guest namespace root",
            ));
        }
        if paths.insert(entry.path(), entry).is_some() {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                field,
                "namespace transaction contains duplicate guest paths",
            ));
        }
        validate_namespace_entry(capabilities, entry, index)?;
        if let NamespaceEntry::File(file) = entry {
            if let FileSource::Immutable(bytes) | FileSource::Mutable(bytes) = &file.source {
                projected_bytes =
                    projected_bytes
                        .checked_add(bytes.len() as u64)
                        .ok_or_else(|| {
                            spec_error(
                                SpecErrorCategory::Limit,
                                format!("extensions[{index}].namespace"),
                                "projected file bytes overflow the engine limit",
                            )
                        })?;
                if projected_bytes > capabilities.limits.projected_file_bytes {
                    return Err(spec_error(
                        SpecErrorCategory::Limit,
                        format!("extensions[{index}].namespace"),
                        "combined projected file bytes exceed the engine limit",
                    ));
                }
            }
        }
    }
    for path in paths.keys() {
        for ancestor in path
            .ancestors()
            .skip(1)
            .take_while(|path| *path != std::path::Path::new("/"))
        {
            if let Some(entry) = paths.get(ancestor) {
                if !matches!(entry, NamespaceEntry::Directory(_)) {
                    return Err(spec_error(
                        SpecErrorCategory::Conflict,
                        format!("extensions[{index}].namespace"),
                        "a non-directory projected entry cannot contain descendants",
                    ));
                }
            }
        }
    }
    Ok(())
}

fn validate_namespace_entry(
    capabilities: &EngineCapabilities,
    entry: &crate::extension::NamespaceEntry,
    index: usize,
) -> Result<(), SpecError> {
    use crate::extension::{FileSource, NamespaceEntry};

    let field = format!("extensions[{index}].namespace");
    let metadata = match entry {
        NamespaceEntry::Directory(value) => Some(value.metadata),
        NamespaceEntry::File(value) => Some(value.metadata),
        NamespaceEntry::Device(value) => Some(value.metadata),
        NamespaceEntry::Service(value) => Some(value.metadata),
        NamespaceEntry::Symlink(value) => {
            let target = value.target.as_os_str().as_encoded_bytes();
            if target.is_empty()
                || target.contains(&0)
                || target.len() > capabilities.limits.path_bytes as usize
            {
                return Err(spec_error(
                    SpecErrorCategory::Invalid,
                    field,
                    "symlink target is empty or exceeds path bounds",
                ));
            }
            None
        }
        NamespaceEntry::HostBind(value) => {
            validate_host_path(&value.host, &field)?;
            None
        }
        NamespaceEntry::Socket(value) => {
            validate_host_path(&value.host, &field)?;
            None
        }
    };
    if metadata.is_some_and(|metadata| metadata.mode & !0o7777 != 0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "namespace modes contain unsupported file-type bits",
        ));
    }
    match entry {
        NamespaceEntry::File(value) => match &value.source {
            FileSource::Host(path) => validate_host_path(path, &field)?,
            FileSource::Shared(id) if id.0 == 0 => {
                return Err(spec_error(
                    SpecErrorCategory::Invalid,
                    field,
                    "shared-byte identity must be nonzero",
                ))
            }
            FileSource::Generated(id) if id.0 == 0 => {
                return Err(spec_error(
                    SpecErrorCategory::Invalid,
                    field,
                    "generator identity must be nonzero",
                ))
            }
            _ => {}
        },
        NamespaceEntry::Device(value) if value.major >= 4096 || value.minor >= (1 << 20) => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                field,
                "device major or minor exceeds Linux dev_t bounds",
            ))
        }
        NamespaceEntry::Service(value) if value.service.0 == 0 => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                field,
                "service identity must be nonzero",
            ))
        }
        _ => {}
    }
    Ok(())
}

fn validate_extension_services(
    capabilities: &EngineCapabilities,
    extension: &crate::extension::ExtensionSpec,
    index: usize,
) -> Result<(), SpecError> {
    use crate::extension::NamespaceEntry;

    let contract_limit = capabilities
        .extensions
        .iter()
        .find(|capability| capability.provider == extension.provider)
        .map_or(capabilities.limits.handles, |capability| {
            capability.limits.services
        });
    if extension.services.len() > contract_limit.min(capabilities.limits.handles) as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].services"),
            "service count exceeds the engine limit",
        ));
    }
    let mut services = BTreeSet::new();
    for service in &extension.services {
        if service.id.0 == 0 || !services.insert(service.id) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                format!("extensions[{index}].services"),
                "service identities must be nonzero and unique",
            ));
        }
        if service.operations.is_empty()
            || service.max_request_bytes == 0
            || service.max_request_bytes > capabilities.limits.request_bytes
            || capabilities
                .extensions
                .iter()
                .find(|capability| capability.provider == extension.provider)
                .is_some_and(|capability| {
                    service.max_request_bytes > capability.limits.request_bytes
                })
        {
            return Err(spec_error(
                SpecErrorCategory::Limit,
                format!("extensions[{index}].services"),
                "service operations or request bound are invalid",
            ));
        }
    }
    for entry in &extension.namespace {
        let service = match entry {
            NamespaceEntry::Service(value) => Some(value.service),
            NamespaceEntry::Device(value) => value.service,
            _ => None,
        };
        if service.is_some_and(|service| !services.contains(&service)) {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                format!("extensions[{index}].namespace"),
                "namespace entry references an unregistered service",
            ));
        }
    }
    Ok(())
}

fn validate_extension_memory(
    capabilities: &EngineCapabilities,
    extension: &crate::extension::ExtensionSpec,
    index: usize,
) -> Result<(), SpecError> {
    if extension.memory.len() > capabilities.limits.mappings as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].memory"),
            "mapping count exceeds the engine limit",
        ));
    }
    let mut total = 0_u64;
    for memory in &extension.memory {
        if memory.size == 0
            || memory.alignment == 0
            || !memory.alignment.is_power_of_two()
            || memory.alignment > (1 << 30)
            || !(memory.protections.read || memory.protections.write || memory.protections.execute)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                format!("extensions[{index}].memory"),
                "memory size, alignment, or protections are invalid",
            ));
        }
        total = total.checked_add(memory.size).ok_or_else(|| {
            spec_error(
                SpecErrorCategory::Limit,
                format!("extensions[{index}].memory"),
                "memory requirements overflow",
            )
        })?;
    }
    if total > capabilities.limits.mapped_bytes {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            format!("extensions[{index}].memory"),
            "combined memory requirements exceed the engine limit",
        ));
    }
    Ok(())
}

fn validate_host_path(path: &std::path::Path, field: &str) -> Result<(), SpecError> {
    if !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0) {
        Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "host capability paths must be absolute and contain no NUL",
        ))
    } else {
        Ok(())
    }
}

pub(super) fn namespace_defaults(namespaces: &crate::spec::NamespaceSpec) -> bool {
    use crate::spec::IsolationMode;
    matches!(namespaces.mount, IsolationMode::Private)
        && matches!(namespaces.pid, IsolationMode::Private)
        && matches!(namespaces.uts, IsolationMode::Private)
        && matches!(namespaces.ipc, IsolationMode::Private)
        && matches!(namespaces.network, IsolationMode::Private)
        && matches!(namespaces.user, IsolationMode::Private)
        && matches!(namespaces.cgroup, IsolationMode::Private)
}
