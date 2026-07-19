use std::{collections::BTreeSet, sync::Arc, time::SystemTime};

use hl_engine_api::{
    extension::{
        ExtensionConfig, ExtensionLimits, Feature, FileEntry, FileSource, Inheritance,
        MemoryRequirement, Metadata, NamespaceEntry, Protections, ProviderId, Sharing,
    },
    Version,
};
use hl_engine_provider::{
    ExtensionError, ExtensionErrorCategory, ExtensionManifest, ExtensionProvider, PrepareContext,
    PreparedExtension, ProcessId,
};

struct Files;

impl ExtensionProvider for Files {
    fn manifest(&self) -> ExtensionManifest {
        manifest("test.files", "test.files/v1", "immutable")
    }

    fn prepare(
        &self,
        _context: &PrepareContext,
        config: &ExtensionConfig,
    ) -> Result<PreparedExtension, ExtensionError> {
        validate_schema(config, "test.files/v1")?;
        Ok(PreparedExtension {
            namespace: vec![NamespaceEntry::File(FileEntry {
                path: "/provider/version".into(),
                metadata: Metadata {
                    mode: 0o444,
                    uid: 0,
                    gid: 0,
                },
                source: FileSource::Immutable(Arc::from(*b"one")),
            })],
            services: Vec::new(),
            memory: Vec::new(),
            lifecycle: None,
        })
    }
}

struct MemoryRegions;

impl ExtensionProvider for MemoryRegions {
    fn manifest(&self) -> ExtensionManifest {
        manifest("test.memory", "test.memory/v1", "shared")
    }

    fn prepare(
        &self,
        _context: &PrepareContext,
        config: &ExtensionConfig,
    ) -> Result<PreparedExtension, ExtensionError> {
        validate_schema(config, "test.memory/v1")?;
        Ok(PreparedExtension {
            namespace: Vec::new(),
            services: Vec::new(),
            memory: vec![MemoryRequirement {
                size: 4096,
                alignment: 4096,
                protections: Protections {
                    read: true,
                    write: true,
                    execute: false,
                },
                sharing: Sharing::Shared,
                inheritance: Inheritance::Retain,
            }],
            lifecycle: None,
        })
    }
}

fn manifest(provider: &str, schema: &str, feature: &str) -> ExtensionManifest {
    ExtensionManifest {
        provider: ProviderId::new(provider).expect("test provider id is valid"),
        versions: vec![Version::new(1, 0)],
        schema: schema.into(),
        features: BTreeSet::from([Feature::new(feature).expect("test feature is valid")]),
        limits: ExtensionLimits {
            namespace_entries: 1,
            services: 0,
            mappings: 1,
            queued_events: 0,
            request_bytes: 64,
        },
    }
}

fn validate_schema(config: &ExtensionConfig, expected: &str) -> Result<(), ExtensionError> {
    if config.schema == expected {
        return Ok(());
    }
    Err(ExtensionError {
        category: ExtensionErrorCategory::Invalid,
        context: "configuration schema does not match provider manifest".into(),
        retry_after: None,
    })
}

fn prepare(
    provider: &dyn ExtensionProvider,
    schema: &str,
) -> Result<PreparedExtension, ExtensionError> {
    provider.prepare(
        &PrepareContext {
            process: ProcessId(7),
            deadline: SystemTime::now(),
        },
        &ExtensionConfig::empty(schema),
    )
}

#[test]
fn unrelated_providers_implement_the_same_ports_without_engine_dependencies() {
    let files = prepare(&Files, "test.files/v1").expect("file provider prepares");
    let memory = prepare(&MemoryRegions, "test.memory/v1").expect("memory provider prepares");

    assert_eq!(files.namespace.len(), 1);
    assert!(files.memory.is_empty());
    assert!(memory.namespace.is_empty());
    assert_eq!(memory.memory.len(), 1);
}

#[test]
fn each_provider_validates_its_own_versioned_configuration() {
    let Err(error) = prepare(&Files, "test.memory/v1") else {
        panic!("mismatched provider schema must fail");
    };

    assert_eq!(error.category, ExtensionErrorCategory::Invalid);
}
