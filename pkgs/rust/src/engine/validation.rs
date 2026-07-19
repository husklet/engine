mod conflict;
mod extension;
mod policy;

use self::{
    conflict::{estimate_resources, namespace_conflicts},
    extension::{namespace_defaults, validate_extension_shapes},
    policy::{
        validate_cache, validate_checkpoint, validate_debug, validate_guest_path, validate_network,
        validate_observability, validate_resources, validate_time_entropy,
    },
};
use super::{
    handles_provider, namespace_provider, resource_error, spec_error, BTreeMap, BTreeSet,
    BindAccess, EngineCapabilities, MachineSpec, NetworkMode, ProviderId, SpecError,
    SpecErrorCategory, TreeSource, Validation,
};

// Keeping the complete preflight in one linear pass makes `validate` and `spawn` structurally
// incapable of drifting to different policy. Each rejected field is still named explicitly.
#[allow(clippy::too_many_lines)]
pub(super) fn validate(
    capabilities: &EngineCapabilities,
    spec: &MachineSpec,
) -> Result<Validation, SpecError> {
    if !capabilities.guests.contains(&spec.guest) {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "guest",
            "guest platform is not supported",
        ));
    }
    if spec.process.executable.as_encoded_bytes().is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process.executable",
            "executable must not be empty",
        ));
    }
    if spec.process.executable.as_encoded_bytes().contains(&0)
        || spec
            .process
            .argv
            .iter()
            .any(|value| value.as_encoded_bytes().contains(&0))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process",
            "executable and arguments must not contain NUL",
        ));
    }
    if spec.process.argv.is_empty() || spec.process.argv[0].as_encoded_bytes().is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process.argv",
            "argv and argv[0] must not be empty",
        ));
    }
    if spec.process.argv[0] != spec.process.executable {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "process.argv[0]",
            "this backend cannot select an argv[0] different from the executable",
        ));
    }
    let cwd = std::path::Path::new(&spec.process.cwd);
    if !cwd.is_absolute()
        || cwd.as_os_str().as_encoded_bytes().contains(&0)
        || cwd.as_os_str().as_encoded_bytes().len() > capabilities.limits.path_bytes as usize
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "process.cwd",
            "working directory must be absolute",
        ));
    }
    if spec.process.umask != 0o022 {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "process.umask",
            "custom umask is not implemented by this backend",
        ));
    }
    let mut environment = BTreeSet::new();
    let mut environment_bytes = 0usize;
    for (name, value) in &spec.process.env {
        let name = name.as_encoded_bytes();
        let value = value.as_encoded_bytes();
        if name.is_empty()
            || name.contains(&b'=')
            || name.contains(&b'\n')
            || name.contains(&0)
            || value.contains(&b'\n')
            || value.contains(&0)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "process.env",
                "environment contains an invalid record",
            ));
        }
        if !environment.insert(name.to_vec()) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "process.env",
                "environment keys must be unique",
            ));
        }
        environment_bytes = environment_bytes.saturating_add(name.len() + value.len() + 2);
    }
    if environment_bytes > capabilities.limits.environment_bytes as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "process.env",
            "environment exceeds the engine limit",
        ));
    }
    if spec.process.argv.len() > capabilities.limits.arguments as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "process.argv",
            "argument count exceeds the engine limit",
        ));
    }
    if spec.cpu.count == Some(0) || spec.resources.cpu_limit == Some(0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "cpu.count",
            "CPU limits must be greater than zero",
        ));
    }
    if !spec.cpu.features.is_empty() || spec.cpu.model.is_some() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "cpu",
            "CPU feature and model selection is not implemented by this backend",
        ));
    }
    match &spec.filesystem.root {
        None => {}
        Some(TreeSource::HostDirectory(path))
            if path.is_absolute()
                && !path.as_os_str().as_encoded_bytes().contains(&0)
                && path.is_dir() => {}
        Some(TreeSource::HostDirectory(_)) => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "filesystem.root",
                "host-directory roots must be absolute existing directories",
            ))
        }
        Some(TreeSource::Overlay { lower, upper, work }) => {
            if lower.is_empty() || lower.len() > 8 {
                return Err(spec_error(
                    SpecErrorCategory::Limit,
                    "filesystem.root.lower",
                    "overlay roots require between one and eight lower layers",
                ));
            }
            let mut granted = Vec::with_capacity(lower.len() + 2);
            granted.push(("filesystem.root.upper", upper));
            granted.push(("filesystem.root.work", work));
            for (index, layer) in lower.iter().enumerate() {
                let TreeSource::HostDirectory(path) = layer else {
                    return Err(spec_error(
                        SpecErrorCategory::Unsupported,
                        format!("filesystem.root.lower[{index}]"),
                        "this backend accepts host-directory overlay lowers",
                    ));
                };
                granted.push(("filesystem.root.lower", path));
            }
            for (field, path) in &granted {
                if !path.is_absolute()
                    || path.as_os_str().as_encoded_bytes().contains(&0)
                    || !path.is_dir()
                {
                    return Err(spec_error(
                        SpecErrorCategory::Invalid,
                        *field,
                        "overlay paths must be absolute existing directories",
                    ));
                }
            }
            for left in 0..granted.len() {
                for right in left + 1..granted.len() {
                    if granted[left].1.starts_with(granted[right].1)
                        || granted[right].1.starts_with(granted[left].1)
                    {
                        return Err(spec_error(
                            SpecErrorCategory::Conflict,
                            "filesystem.root",
                            "overlay upper, work, and lower authorities must not overlap",
                        ));
                    }
                }
            }
        }
        Some(_) => {
            return Err(spec_error(
                SpecErrorCategory::Unsupported,
                "filesystem.root",
                "this backend accepts host-directory and host-backed overlay roots",
            ))
        }
    }
    let mut paths = BTreeMap::new();
    for mount in &spec.filesystem.mounts {
        validate_guest_path(
            &mount.path,
            capabilities.limits.path_bytes,
            "filesystem.mounts",
        )?;
        if paths.insert(&mount.path, "filesystem.mounts").is_some() {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "filesystem.mounts",
                "multiple entries project the same guest path",
            ));
        }
        if !mount.host.is_absolute() || !mount.host.exists() {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "filesystem.mounts",
                "bound host paths must be absolute and exist",
            ));
        }
        if [mount.host.as_os_str(), mount.path.as_os_str()]
            .iter()
            .any(|path| {
                path.as_encoded_bytes()
                    .iter()
                    .any(|byte| matches!(byte, b':' | b',' | 0))
            })
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "filesystem.mounts",
                crate::spec::SpecResource::Path(mount.path.clone()),
                "current host-bind transport cannot encode ':', ',', or NUL in paths",
            ));
        }
    }
    let mut ownership_paths = BTreeSet::new();
    for ownership in &spec.filesystem.ownership {
        validate_guest_path(
            &ownership.path,
            capabilities.limits.path_bytes,
            "filesystem.ownership",
        )?;
        if ownership.path == std::path::Path::new("/")
            || !ownership_paths.insert(&ownership.path)
            || ownership
                .path
                .as_os_str()
                .as_encoded_bytes()
                .iter()
                .any(|byte| matches!(byte, b'\n' | b'\t' | 0))
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "filesystem.ownership",
                crate::spec::SpecResource::Path(ownership.path.clone()),
                "ownership paths must be unique, non-root, and wire-safe",
            ));
        }
    }
    if spec
        .identity
        .uid
        .is_some_and(|value| value > i32::MAX as u32)
        || spec
            .identity
            .gid
            .is_some_and(|value| value > i32::MAX as u32)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "identity",
            "uid and gid exceed the current backend range",
        ));
    }
    if spec.identity.hostname.as_ref().is_some_and(|value| {
        value.as_encoded_bytes().contains(&0) || value.as_encoded_bytes().contains(&b'\n')
    }) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "identity.hostname",
            "hostname contains an unsupported byte",
        ));
    }
    if !spec.identity.supplementary_groups.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "identity.supplementary_groups",
            "supplementary groups are not implemented by this backend",
        ));
    }
    if spec.identity.domain_name.is_some() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "identity.domain_name",
            "domain name selection is not implemented by this backend",
        ));
    }
    validate_resources(spec, capabilities.limits.handles)?;
    validate_network(spec)?;
    if !namespace_defaults(&spec.namespaces) {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "namespaces",
            "custom namespace sharing modes are not implemented by this backend",
        ));
    }
    validate_checkpoint(spec)?;
    validate_time_entropy(spec)?;
    validate_cache(spec)?;
    validate_observability(spec, capabilities)?;
    validate_debug(spec)?;
    if spec.security.trusted_guest || !spec.security.executable_memory {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "security",
            "the selected trust or executable-memory policy is not implemented by this backend",
        ));
    }
    if spec.extensions.len() > capabilities.limits.extension_specs as usize {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "extensions",
            "extension count exceeds the engine limit",
        ));
    }
    validate_extension_shapes(capabilities, spec)?;
    let available = capabilities
        .extensions
        .iter()
        .map(|item| (&item.provider, item))
        .collect::<BTreeMap<_, _>>();
    let mut unavailable = Vec::new();
    let mut selected_extensions = Vec::new();
    let mut degraded_features = Vec::new();
    for extension in &spec.extensions {
        if !spec.security.allowed_providers.is_empty()
            && !spec
                .security
                .allowed_providers
                .contains(&extension.provider)
        {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "security.allowed_providers",
                "an extension provider is outside the launch allowlist",
            ));
        }
        let Some(capability) = available.get(&extension.provider) else {
            if extension.required {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "a required extension provider is unavailable",
                ));
            }
            unavailable.push(extension.provider.clone());
            continue;
        };
        if !capability.versions.contains(&extension.version) {
            if extension.required {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.version",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "a required extension contract version is unavailable",
                ));
            }
            unavailable.push(extension.provider.clone());
            continue;
        }
        let missing_required = extension
            .required_features
            .difference(&capability.features)
            .next();
        if missing_required.is_some() {
            if extension.required {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.required_features",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "a required extension feature is unavailable",
                ));
            }
            unavailable.push(extension.provider.clone());
            continue;
        }
        let selected = extension
            .required_features
            .union(&extension.optional_features)
            .filter(|feature| capability.features.contains(*feature))
            .cloned()
            .collect();
        degraded_features.extend(
            extension
                .optional_features
                .difference(&capability.features)
                .cloned()
                .map(|feature| crate::spec::DegradedFeature {
                    provider: extension.provider.clone(),
                    feature,
                }),
        );
        selected_extensions.push(crate::spec::SelectedExtension {
            provider: extension.provider.clone(),
            version: extension.version,
            features: selected,
        });
    }
    let active = selected_extensions
        .iter()
        .map(|extension| &extension.provider)
        .collect::<BTreeSet<_>>();
    validate_selected_runtime(spec, &active)?;
    let namespace_conflicts = namespace_conflicts(spec, &active)?;
    let resources = estimate_resources(spec, &active, capabilities)?;
    let estimated_memory_bytes = resources
        .memory_bytes
        .checked_add(resources.extension_memory_bytes)
        .ok_or_else(|| {
            spec_error(
                SpecErrorCategory::Limit,
                "resources.memory_bytes",
                "combined host memory estimate overflows u64",
            )
        })?;
    Ok(Validation {
        selected_extensions,
        unavailable_optional_extensions: unavailable,
        degraded_features,
        estimated_memory_bytes,
        namespace_conflicts,
        resources,
    })
}

fn validate_selected_runtime(
    spec: &MachineSpec,
    active: &BTreeSet<&ProviderId>,
) -> Result<(), SpecError> {
    use crate::extension::{FileSource, NamespaceEntry};
    for extension in &spec.extensions {
        if !active.contains(&extension.provider) {
            continue;
        }
        if extension.provider == handles_provider() {
            let supported = BTreeSet::from([
                crate::extension::HandleOperation::Read,
                crate::extension::HandleOperation::Write,
                crate::extension::HandleOperation::Poll,
            ]);
            if extension
                .services
                .iter()
                .any(|service| !service.operations.is_subset(&supported))
            {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.services.operations",
                    crate::spec::SpecResource::Provider(extension.provider.clone()),
                    "handles contract v1 advertises only read, write, poll, and engine-owned OFD lifecycle",
                ));
            }
            continue;
        }
        if extension.provider != namespace_provider() {
            continue;
        }
        if !extension.services.is_empty() || !extension.memory.is_empty() {
            return Err(resource_error(
                SpecErrorCategory::Unsupported,
                "extensions",
                crate::spec::SpecResource::Provider(extension.provider.clone()),
                "open services and provider memory have no runtime implementation",
            ));
        }
        for entry in &extension.namespace {
            if !matches!(
                entry,
                NamespaceEntry::Directory(_)
                    | NamespaceEntry::Symlink(_)
                    | NamespaceEntry::File(crate::extension::FileEntry {
                        source: FileSource::Immutable(_) | FileSource::Mutable(_),
                        ..
                    })
                    | NamespaceEntry::HostBind(crate::extension::HostBindEntry {
                        access: BindAccess::ReadOnly,
                        ..
                    })
            ) {
                return Err(resource_error(
                    SpecErrorCategory::Unsupported,
                    "extensions.namespace",
                    crate::spec::SpecResource::Path(entry.path().to_owned()),
                    "this projected namespace node kind has no runtime implementation",
                ));
            }
            if let NamespaceEntry::HostBind(bind) = entry {
                let metadata = std::fs::symlink_metadata(&bind.host).map_err(|_| {
                    resource_error(
                        SpecErrorCategory::Invalid,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(bind.host.clone()),
                        "projected host binds must name an existing host file or directory",
                    )
                })?;
                if metadata.file_type().is_symlink()
                    || !(metadata.file_type().is_file() || metadata.file_type().is_dir())
                {
                    return Err(resource_error(
                        SpecErrorCategory::Unsupported,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(bind.host.clone()),
                        "projected host binds support only regular files and directories",
                    ));
                }
            }
        }
    }
    Ok(())
}

pub(super) fn validate_authorities(
    spec: &MachineSpec,
    authorities: &crate::extension::Authorities,
) -> Result<(), SpecError> {
    if spec.process.terminal.is_some()
        && spec
            .extensions
            .iter()
            .any(|extension| {
                extension.provider == handles_provider() && !extension.services.is_empty()
            })
    {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "process.terminal",
            "provider service activation with a controlling terminal is not implemented",
        ));
    }
    for extension in &spec.extensions {
        if extension.provider != handles_provider() {
            continue;
        }
        let authority = authorities.provider(&extension.provider);
        if !extension.services.is_empty()
            && authority.and_then(|value| value.handles.as_ref()).is_none()
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "extensions.authority",
                crate::spec::SpecResource::Provider(extension.provider.clone()),
                "selected handle services require launch-scoped Handles authority",
            ));
        }
        if !extension.memory.is_empty()
            && authority.and_then(|value| value.memory.as_ref()).is_none()
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "extensions.authority",
                crate::spec::SpecResource::Provider(extension.provider.clone()),
                "selected memory requirements need launch-scoped Memory authority",
            ));
        }
    }
    for (provider, authority) in authorities.iter() {
        let extension = spec
            .extensions
            .iter()
            .find(|value| &value.provider == provider);
        let handles_needed = extension.is_some_and(|value| !value.services.is_empty());
        let memory_needed = extension.is_some_and(|value| !value.memory.is_empty());
        if authority.handles.is_some() != handles_needed
            || authority.memory.is_some() != memory_needed
        {
            return Err(resource_error(
                SpecErrorCategory::Invalid,
                "extensions.authority",
                crate::spec::SpecResource::Provider(provider.clone()),
                "provider grant contains missing or excess live ports",
            ));
        }
    }
    Ok(())
}
