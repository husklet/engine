use super::{
    handles_provider, namespace_provider, resource_error, BTreeSet, BindAccess, MachineSpec,
    ProviderId, SpecError, SpecErrorCategory,
};

#[allow(clippy::too_many_lines)]
pub(super) fn validate_selected_runtime(
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
                crate::extension::HandleOperation::Metadata,
                crate::extension::HandleOperation::Poll,
                crate::extension::HandleOperation::Ioctl,
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
                    "handles contract v1 advertises only read, write, metadata, poll, ioctl, and engine-owned OFD lifecycle",
                ));
            }
            for entry in &extension.namespace {
                if matches!(entry, NamespaceEntry::Device(value) if value.service.is_none()) {
                    return Err(resource_error(
                        SpecErrorCategory::Unsupported,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(entry.path().to_owned()),
                        "projected devices require an open-handle service",
                    ));
                }
                if !matches!(
                    entry,
                    NamespaceEntry::Service(_) | NamespaceEntry::Device(_)
                ) {
                    return Err(resource_error(
                        SpecErrorCategory::Unsupported,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(entry.path().to_owned()),
                        "handles contract projects only services and service-backed devices",
                    ));
                }
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
                    | NamespaceEntry::Socket(_)
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
            if let NamespaceEntry::Socket(socket) = entry {
                let metadata = std::fs::symlink_metadata(&socket.host).map_err(|_| {
                    resource_error(
                        SpecErrorCategory::Invalid,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(socket.host.clone()),
                        "projected socket must name an existing host socket",
                    )
                })?;
                if !crate::sys::is_socket(&metadata) {
                    return Err(resource_error(
                        SpecErrorCategory::Invalid,
                        "extensions.namespace",
                        crate::spec::SpecResource::Path(socket.host.clone()),
                        "projected socket must name a Unix socket",
                    ));
                }
            }
        }
    }
    Ok(())
}

pub(in crate::engine) fn validate_authorities(
    spec: &MachineSpec,
    authorities: &crate::extension::Authorities,
) -> Result<(), SpecError> {
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
