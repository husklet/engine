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
fn checkpoint_validates_capture_and_restore_directories_independently() {
    let mut value = spec();
    value.checkpoint.enabled = true;
    value.checkpoint.capture_directory = Some("/tmp/checkpoint".into());
    value.checkpoint.restore_directory = Some("relative/restore".into());
    let error = Engine::new().validate(&value).unwrap_err();
    assert_eq!(error.category, hl_engine::spec::SpecErrorCategory::Invalid);
    assert_eq!(error.field, "checkpoint.restore_directory");

    value.checkpoint.capture_directory = Some("relative/checkpoint".into());
    value.checkpoint.restore_directory = Some("/tmp/restore".into());
    let error = Engine::new().validate(&value).unwrap_err();
    assert_eq!(error.category, hl_engine::spec::SpecErrorCategory::Invalid);
    assert_eq!(error.field, "checkpoint.capture_directory");

    value.checkpoint.capture_directory = Some("/tmp/checkpoint".into());
    value.checkpoint.restore_directory = Some("/".into());
    let error = Engine::new().validate(&value).unwrap_err();
    assert_eq!(error.category, hl_engine::spec::SpecErrorCategory::Invalid);
    assert_eq!(error.field, "checkpoint.restore_directory");

    value.checkpoint.restore_directory = Some("/tmp/restore".into());
    Engine::new().validate(&value).unwrap();

    let mut value = spec();
    value.checkpoint.enabled = true;
    value.checkpoint.restore_directory = Some("relative/restore".into());
    let error = Engine::new().validate(&value).unwrap_err();
    assert_eq!(error.category, hl_engine::spec::SpecErrorCategory::Invalid);
    assert_eq!(error.field, "checkpoint.restore_directory");
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
    value.checkpoint.capture_directory = Some("/tmp/checkpoint".into());
    value.checkpoint.maximum_pause_ms = Some(0);
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().field,
        "checkpoint.maximum_pause_ms"
    );
    value.checkpoint.maximum_pause_ms = None;
    value.checkpoint.mode = CheckpointMode::Full;
    value.checkpoint.incompatible_resources = IncompatibleResourcePolicy::Refuse;
    Engine::new().validate(&value).unwrap();

    value.checkpoint.restore_directory = Some("/tmp/restore".into());
    Engine::new().validate(&value).unwrap();

    let mut value = spec();
    value.checkpoint.enabled = true;
    value.checkpoint.capture_directory = Some("relative/checkpoint".into());
    assert_eq!(
        Engine::new().validate(&value).unwrap_err().category,
        hl_engine::spec::SpecErrorCategory::Invalid
    );

    // x86_64 checkpoint is now ACCEPTED. The x86_64 fd-restore bug (a guest that
    // dup2'd fd 1 onto a file wrote to the launcher's stdio after restore instead
    // of the file) was fixed in the C backend: `x86_number()` now maps x86 dup2
    // (33) onto canonical dup3 (24) so the shared fd-publish path registers a
    // dup2'd descriptor and checkpoint captures it. Validation reads the
    // capability set, which now advertises x86_64, so the launch is accepted.
    let mut value = MachineSpec::new(hl_engine::Guest::X86_64, "/bin/true");
    value.checkpoint.enabled = true;
    value.checkpoint.restore_directory = Some("/tmp/restore".into());
    Engine::new().validate(&value).unwrap();

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

/// Every guest the capability set reports as checkpoint-capable must be accepted
/// by preflight, and every guest it omits must be refused. Discovery and the
/// validator read one set, so this asserts they cannot drift apart again.
#[test]
fn checkpoint_capability_and_validation_agree_for_every_guest() {
    let engine = Engine::new();
    let checkpoint = engine.capabilities().checkpoint;
    assert!(
        checkpoint.is_available(),
        "a format is advertised, so at least one guest must be checkpointable"
    );
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let mut value = MachineSpec::new(guest, "/bin/true");
        value.checkpoint.enabled = true;
        value.checkpoint.capture_directory = Some("/tmp/hl-checkpoint-capability".into());
        let outcome = engine.validate(&value);
        if checkpoint.supports(guest) {
            assert!(
                outcome.is_ok(),
                "{guest:?} is reported as checkpoint-capable but preflight rejected it"
            );
        } else {
            let error = outcome.expect_err("unreported guest must be rejected");
            assert_eq!(error.field, "checkpoint");
            assert_eq!(
                error.category,
                hl_engine::spec::SpecErrorCategory::Unsupported
            );
        }
    }
}

/// The advertised CPU ceiling is a promise about what a launch may ask for, so
/// preflight must accept exactly up to it and refuse the first value beyond it.
#[test]
fn reported_cpu_maximum_bounds_the_accepted_topology() {
    let engine = Engine::new();
    let maximum = engine.capabilities().cpu.maximum_cpus;
    assert!(maximum > 0 && maximum < u32::MAX);

    let mut value = spec();
    value.cpu.count = Some(maximum);
    engine.validate(&value).unwrap();

    value.cpu.count = Some(maximum + 1);
    let error = engine.validate(&value).unwrap_err();
    assert_eq!(error.field, "cpu.count");
    assert_eq!(error.category, hl_engine::spec::SpecErrorCategory::Limit);
}

/// The reported network maxima are the same numbers preflight enforces.
#[test]
fn reported_network_maxima_bound_accepted_port_forwards() {
    let engine = Engine::new();
    let maximum = engine.capabilities().networking.maximum_port_forwards;

    let mut value = spec();
    value.network.mode = hl_engine::spec::NetworkMode::Virtual;
    value.network.namespace = Some(hl_engine::network::Namespace::new("caps").unwrap());
    value.network.port_forwards = (0..=maximum)
        .map(|index| {
            hl_engine::network::Rule::new(
                u16::try_from(20_000 + index).unwrap(),
                u16::try_from(30_000 + index).unwrap(),
            )
            .unwrap()
        })
        .collect();
    let error = engine.validate(&value).unwrap_err();
    assert_eq!(error.field, "network.port_forwards");
    assert_eq!(error.category, hl_engine::spec::SpecErrorCategory::Limit);

    value.network.port_forwards.pop();
    engine.validate(&value).unwrap();
}
