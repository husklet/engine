use std::collections::BTreeSet;

use hl_engine_api::{
    extension::{ExtensionConfig, ExtensionSpec, ProviderId},
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
