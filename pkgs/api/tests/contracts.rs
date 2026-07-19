use std::collections::BTreeSet;

use hl_engine_api::{
    extension::{ExtensionConfig, ExtensionSpec, ProviderId},
    spec::{
        FilesystemSpec, GuestPlatform, HostResourceEstimate, NamespaceSpec, ResourceSpec,
        SecuritySpec, TimeSpec, Validation,
    },
    Guest, Version,
};

#[test]
fn launch_contracts_are_backend_independent() {
    let provider = ProviderId::new("example.storage").expect("portable provider id");
    let specification = ExtensionSpec {
        provider: provider.clone(),
        version: Version::new(1, 2),
        required: true,
        required_features: BTreeSet::new(),
        optional_features: BTreeSet::new(),
        config: ExtensionConfig::empty("example.storage/v1"),
        namespace: Vec::new(),
        services: Vec::new(),
        memory: Vec::new(),
        environment: Vec::new(),
    };

    assert_eq!(specification.provider, provider);
    assert_eq!(specification.version, Version::new(1, 2));
    assert_eq!(Guest::Aarch64, Guest::Aarch64);
}

#[test]
fn launch_policy_models_construct_without_a_backend() {
    let platform = GuestPlatform::linux(Guest::Aarch64);
    let filesystem = FilesystemSpec::default();
    let namespaces = NamespaceSpec::default();
    let resources = ResourceSpec::default();
    let security = SecuritySpec::default();
    let time = TimeSpec::default();
    let validation = Validation {
        selected_extensions: Vec::new(),
        unavailable_optional_extensions: Vec::new(),
        degraded_features: Vec::new(),
        estimated_memory_bytes: 0,
        namespace_conflicts: Vec::new(),
        resources: HostResourceEstimate::default(),
    };

    assert_eq!(platform.architecture, Guest::Aarch64);
    assert_eq!(filesystem, FilesystemSpec::default());
    assert_eq!(namespaces, NamespaceSpec::default());
    assert_eq!(resources, ResourceSpec::default());
    assert_eq!(security, SecuritySpec::default());
    assert_eq!(time, TimeSpec::default());
    assert!(validation.selected_extensions.is_empty());
}
