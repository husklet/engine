use super::{
    spec_error, BTreeSet, EngineCapabilities, MachineSpec, NetworkMode, SpecError,
    SpecErrorCategory,
};

pub(super) fn validate_resources(spec: &MachineSpec, guest_fd_limit: u32) -> Result<(), SpecError> {
    let resources = &spec.resources;
    let values = [
        resources.memory_reservation_bytes,
        resources.memory_bytes,
        resources.file_size_bytes,
        resources.locked_memory_bytes,
        resources.stack_bytes,
        resources.address_space_bytes,
        resources.io_read_bytes_per_second,
        resources.io_write_bytes_per_second,
        resources.cpu_quota_micros,
    ];
    if values.into_iter().flatten().any(|value| value == 0)
        || [
            resources.process_limit,
            resources.thread_limit,
            resources.cpu_limit,
            resources.open_files,
        ]
        .into_iter()
        .flatten()
        .any(|value| value == 0)
        || resources
            .extension_budgets
            .values()
            .any(|value| *value == 0)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources",
            "explicit resource declarations must be greater than zero",
        ));
    }
    if resources
        .memory_reservation_bytes
        .zip(resources.memory_bytes)
        .is_some_and(|(reservation, limit)| reservation > limit)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources.memory_reservation_bytes",
            "memory reservation must not exceed the memory limit",
        ));
    }
    validate_open_files(resources.open_files, guest_fd_limit)?;
    if resources
        .cpu_affinity
        .iter()
        .any(|cpu| spec.cpu.count.is_some_and(|count| *cpu >= count))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources.cpu_affinity",
            "CPU affinity references a CPU outside the visible topology",
        ));
    }
    let providers = spec
        .extensions
        .iter()
        .map(|extension| &extension.provider)
        .collect::<BTreeSet<_>>();
    if resources
        .extension_budgets
        .keys()
        .any(|provider| !providers.contains(provider))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "resources.extension_budgets",
            "extension budgets must name an extension requested by this launch",
        ));
    }
    let unsupported = resources.memory_reservation_bytes.is_some()
        || resources.thread_limit.is_some()
        || resources.cpu_quota_micros.is_some()
        || !resources.cpu_affinity.is_empty()
        || resources.open_files.is_some()
        || resources.file_size_bytes.is_some()
        || resources.locked_memory_bytes.is_some()
        || resources.stack_bytes.is_some()
        || resources.address_space_bytes.is_some()
        || resources.io_read_bytes_per_second.is_some()
        || resources.io_write_bytes_per_second.is_some()
        || !resources.extension_budgets.is_empty()
        || resources.accounting != crate::spec::AccountingSpec::Disabled;
    if unsupported {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "resources",
            "one or more selected resource controls are not implemented by this backend",
        ));
    }
    Ok(())
}

fn validate_open_files(open_files: Option<u32>, guest_fd_limit: u32) -> Result<(), SpecError> {
    if open_files.is_some_and(|value| value > guest_fd_limit) {
        Err(spec_error(
            SpecErrorCategory::Limit,
            "resources.open_files",
            "open-file limit exceeds the host-backed guest descriptor ceiling",
        ))
    } else {
        Ok(())
    }
}

pub(super) fn validate_checkpoint(spec: &MachineSpec) -> Result<(), SpecError> {
    let checkpoint = &spec.checkpoint;
    if checkpoint.maximum_pause_ms == Some(0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "checkpoint.maximum_pause_ms",
            "checkpoint pause bounds must be greater than zero",
        ));
    }
    if !checkpoint.enabled
        && (checkpoint.maximum_pause_ms.is_some()
            || checkpoint.mode != crate::spec::CheckpointMode::Full
            || checkpoint.incompatible_resources != crate::spec::IncompatibleResourcePolicy::Refuse)
    {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "checkpoint",
            "checkpoint options require checkpointing to be enabled",
        ));
    }
    if checkpoint.enabled {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "checkpoint.enabled",
            "checkpointing is not supported",
        ));
    }
    Ok(())
}

pub(super) fn validate_time_entropy(spec: &MachineSpec) -> Result<(), SpecError> {
    if spec.time.rate.numerator == 0
        || spec.time.rate.denominator == 0
        || spec.time.timer_resolution_ns == Some(0)
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "time",
            "time rate and timer resolution must be greater than zero",
        ));
    }
    if !matches!(spec.time.source, crate::spec::TimeSource::Host)
        || spec.time.monotonic != crate::spec::MonotonicOrigin::Host
        || spec.time.timer_resolution_ns.is_some()
        || spec.time.rate != crate::spec::TimeRate::default()
    {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "time",
            "virtual time selection is not implemented by this backend",
        ));
    }
    if matches!(&spec.entropy, crate::spec::EntropySpec::Deterministic(seed) if seed.is_empty()) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "entropy",
            "deterministic entropy requires a nonempty seed",
        ));
    }
    if !matches!(spec.entropy, crate::spec::EntropySpec::SecureHost) {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "entropy",
            "deterministic entropy is not implemented by this backend",
        ));
    }
    Ok(())
}

pub(super) fn validate_cache(spec: &MachineSpec) -> Result<(), SpecError> {
    let cache = &spec.cache;
    if cache.budget_bytes == Some(0)
        || cache
            .identity
            .iter()
            .any(|item| item.name.is_empty() || item.bytes.is_empty())
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "cache",
            "cache budgets and identity components must be nonempty",
        ));
    }
    let mut identities = BTreeSet::new();
    let identity_bytes = cache.identity.iter().try_fold(0_usize, |total, item| {
        if !identities.insert(&item.name) {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "cache.identity",
                "cache identity component names must be unique",
            ));
        }
        total
            .checked_add(item.name.len() + item.bytes.len())
            .ok_or_else(|| {
                spec_error(
                    SpecErrorCategory::Limit,
                    "cache.identity",
                    "cache identity components overflow engine bounds",
                )
            })
    })?;
    if identity_bytes > 1024 * 1024 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "cache.identity",
            "cache identity components exceed the engine limit",
        ));
    }
    if cache
        .directory
        .as_ref()
        .is_some_and(|path| !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "cache.directory",
            "translation-cache host paths must be absolute and contain no NUL",
        ));
    }
    match cache.policy {
        crate::spec::CachePolicy::Disabled
            if cache.directory.is_some()
                || cache.budget_bytes.is_some()
                || !cache.identity.is_empty() =>
        {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "cache.policy",
                "cache settings require an enabled cache policy",
            ))
        }
        crate::spec::CachePolicy::ReadWrite if cache.directory.is_none() => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "cache.directory",
                "read-write cache policy requires a cache directory",
            ))
        }
        crate::spec::CachePolicy::ReadOnly | crate::spec::CachePolicy::Refresh => {
            return Err(spec_error(
                SpecErrorCategory::Unsupported,
                "cache.policy",
                "the selected cache policy is not implemented by this backend",
            ))
        }
        _ => {}
    }
    if cache.budget_bytes.is_some() || !cache.identity.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "cache",
            "cache budgets and caller identity components are not implemented by this backend",
        ));
    }
    Ok(())
}

pub(super) fn validate_observability(
    spec: &MachineSpec,
    capabilities: &EngineCapabilities,
) -> Result<(), SpecError> {
    let value = &spec.observability;
    if value.syscall_sampling == Some(0)
        || value.syscall_sampling.is_some_and(|rate| rate > 1_000_000)
        || value.queue_capacity == Some(0)
        || value
            .queue_capacity
            .is_some_and(|size| size > capabilities.limits.queued_events)
        || value.trace.as_ref().is_some_and(|trace| {
            trace.queue_capacity == 0
                || trace.queue_capacity > capabilities.limits.queued_events
                || !(trace.include_guest_time || trace.include_host_time)
        })
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "observability",
            "sampling, queue, or trace bounds are invalid",
        ));
    }
    if !value.metrics && !value.metric_kinds.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "observability.metric_kinds",
            "metric kinds require metrics to be enabled",
        ));
    }
    if value != &crate::spec::ObservabilitySpec::default() {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "observability",
            "structured observability selection is not implemented by this backend",
        ));
    }
    Ok(())
}

pub(super) fn validate_debug(spec: &MachineSpec) -> Result<(), SpecError> {
    let value = &spec.debug;
    let selected = !value.operations.is_empty() || value.breakpoints != 0 || value.watchpoints != 0;
    if value.breakpoints > 65_536 || value.watchpoints > 65_536 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "debug",
            "debug point count exceeds the engine limit",
        ));
    }
    if selected && value.authorization.is_none() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "debug.authorization",
            "debug operations require explicit authorization",
        ));
    }
    if value
        .authorization
        .as_ref()
        .is_some_and(|value| value.capability.is_empty())
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "debug.authorization",
            "debug authorization capabilities must be nonempty",
        ));
    }
    if value.authorization.is_some() && !selected {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "debug",
            "debug authorization requires at least one selected operation",
        ));
    }
    if selected {
        return Err(spec_error(
            SpecErrorCategory::Unsupported,
            "debug",
            "debug control is not implemented by this backend",
        ));
    }
    Ok(())
}

pub(super) fn validate_network(spec: &MachineSpec) -> Result<(), SpecError> {
    let network = &spec.network;
    if network.mode == NetworkMode::Host && spec.security.sandbox != crate::Sandbox::Disabled {
        return Err(spec_error(
            SpecErrorCategory::Conflict,
            "network.mode",
            "host networking requires the host socket transport, which is unavailable in the selected sandbox",
        ));
    }
    let configured = network.namespace.is_some()
        || !network.interfaces.is_empty()
        || !network.port_forwards.is_empty()
        || network.external_listeners;
    match network.mode {
        NetworkMode::Host if configured => {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "network",
                "host networking cannot carry virtual network configuration",
            ))
        }
        NetworkMode::None
            if !network.interfaces.is_empty()
                || !network.port_forwards.is_empty()
                || network.external_listeners =>
        {
            return Err(spec_error(
                SpecErrorCategory::Conflict,
                "network",
                "disabled networking cannot carry interfaces, forwards, or listeners",
            ))
        }
        NetworkMode::Virtual if network.namespace.is_none() => {
            return Err(spec_error(
                SpecErrorCategory::Invalid,
                "network.namespace",
                "virtual networking requires an opaque namespace identity",
            ))
        }
        _ => {}
    }
    if network.interfaces.len() > 8 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "network.interfaces",
            "at most eight interfaces are supported",
        ));
    }
    if network.port_forwards.len() > 32 {
        return Err(spec_error(
            SpecErrorCategory::Limit,
            "network.port_forwards",
            "at most 32 port forwards are supported",
        ));
    }
    if network.external_listeners && network.port_forwards.is_empty() {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            "network.external_listeners",
            "external listeners require at least one port forward",
        ));
    }
    Ok(())
}

pub(super) fn validate_guest_path(
    path: &std::path::Path,
    maximum: u32,
    field: &'static str,
) -> Result<(), SpecError> {
    if !path.is_absolute() || path.as_os_str().as_encoded_bytes().len() > maximum as usize {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "guest paths must be absolute and within the engine path limit",
        ));
    }
    if path.as_os_str().as_encoded_bytes().contains(&0) {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "guest paths must not contain NUL",
        ));
    }
    if path
        .components()
        .any(|component| matches!(component, std::path::Component::ParentDir))
    {
        return Err(spec_error(
            SpecErrorCategory::Invalid,
            field,
            "guest paths must not escape through '..'",
        ));
    }
    Ok(())
}
