use hl_engine::{
    spec::{
        AccountingSpec, CachePolicy, CheckpointMode, DebugAuthorization, DebugOperation,
        EntropySpec, IncompatibleResourcePolicy, TimeRate, TracePrivacy, TraceSpec,
    },
    Engine, Guest, MachineSpec,
};

fn spec() -> MachineSpec {
    MachineSpec::new(Guest::Aarch64, "/bin/true")
}

#[test]
fn discovery_never_implies_unimplemented_control_planes() {
    let capabilities = Engine::new().capabilities();
    assert!(!capabilities.observability.structured_events);
    assert!(!capabilities.observability.metrics);
    assert!(capabilities.debugging.operations.is_empty());
    assert!(!capabilities.time.virtual_time);
    assert!(!capabilities.time.deterministic_entropy);
    assert!(capabilities.resources.live_updates.is_empty());
    assert_eq!(
        capabilities.control.operations,
        std::collections::BTreeSet::from([
            hl_engine::spec::ControlOperation::ProcessInventory,
            hl_engine::spec::ControlOperation::Signal,
            hl_engine::spec::ControlOperation::Pause,
            hl_engine::spec::ControlOperation::Attach,
        ])
    );
    assert_eq!(capabilities.limits.processes, 65_536);
}

#[test]
fn resource_shapes_fail_before_backend_support_checks() {
    let mut value = spec();
    value.resources.memory_reservation_bytes = Some(0);
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().field,
        "resources"
    );

    let mut value = spec();
    value.resources.memory_bytes = Some(1024);
    value.resources.memory_reservation_bytes = Some(2048);
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().field,
        "resources.memory_reservation_bytes"
    );

    let mut value = spec();
    value.resources.accounting = AccountingSpec::Machine;
    let error = Engine::new().validate(&value).unwrap_err();
    assert_eq!(error.field, "resources");
    assert_eq!(
        error.category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );
}

#[test]
fn time_and_entropy_distinguish_invalid_from_unsupported() {
    let mut value = spec();
    value.time.rate = TimeRate {
        numerator: 1,
        denominator: 0,
    };
    assert_eq!(Engine::new().validate(&value).unwrap_err().field, "time");

    let mut value = spec();
    value.time.rate = TimeRate {
        numerator: 2,
        denominator: 1,
    };
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );

    let mut value = spec();
    value.entropy = EntropySpec::Deterministic(Vec::new());
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Invalid
    );
    value.entropy = EntropySpec::Deterministic(vec![1, 2, 3]);
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );
}

#[test]
fn cache_policy_is_explicit_and_read_write_host_store_is_supported() {
    let mut value = spec();
    value.cache.directory = Some("relative".into());
    value.cache.policy = CachePolicy::ReadWrite;
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().field,
        "cache.directory"
    );

    value.cache.directory = Some("/tmp/hl-cache".into());
    Engine::new().validate(&value).unwrap();

    value.cache.policy = CachePolicy::ReadOnly;
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );
}

#[test]
fn checkpoint_observability_and_debug_require_valid_bounded_policy() {
    let mut value = spec();
    value.checkpoint.mode = CheckpointMode::Incremental;
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Conflict
    );
    value.checkpoint.enabled = true;
    value.checkpoint.maximum_pause_ms = Some(0);
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().field,
        "checkpoint.maximum_pause_ms"
    );
    value.checkpoint.maximum_pause_ms = Some(100);
    value.checkpoint.incompatible_resources = IncompatibleResourcePolicy::Refuse;
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );

    let mut value = spec();
    value.observability.trace = Some(TraceSpec {
        queue_capacity: 1,
        include_guest_time: false,
        include_host_time: false,
        privacy: TracePrivacy::MetadataOnly,
    });
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Invalid
    );
    value
        .observability
        .trace
        .as_mut()
        .unwrap()
        .include_host_time = true;
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );

    let mut value = spec();
    value.debug.operations.insert(DebugOperation::Memory);
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().field,
        "debug.authorization"
    );
    value.debug.authorization = Some(DebugAuthorization {
        capability: vec![1],
    });
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Unsupported
    );
}
