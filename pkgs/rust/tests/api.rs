use hl_engine::{extension, spec, transport};

fn accepts_api_provider(_: hl_engine::extension::ProviderId) {}
fn accepts_api_version(_: hl_engine::Version) {}
fn accepts_provider_process(_: hl_engine::extension::ProcessId) {}
fn accepts_protocol_frame(_: hl_engine::transport::Frame) {}
fn accepts_protocol_error(_: hl_engine::transport::TransportError) {}
fn accepts_api_capabilities(_: hl_engine::spec::EngineCapabilities) {}
fn accepts_api_filesystem(_: hl_engine::spec::FilesystemSpec) {}
fn accepts_api_resources(_: hl_engine::spec::ResourceSpec) {}
fn accepts_api_validation(_: hl_engine::spec::Validation) {}
fn accepts_api_attach(_: hl_engine::control::AttachRequest) {}

#[test]
fn legacy_module_paths_reexport_api_contracts() {
    let provider = extension::ProviderId::new("compatibility.test").expect("valid provider id");
    let version = spec::Version::new(1, 0);

    accepts_api_provider(provider);
    accepts_api_version(version);
    accepts_provider_process(extension::ProcessId(1));

    accepts_protocol_frame(transport::Frame {
        kind: transport::MessageType::Close,
        request_id: 0,
        features: 0,
        payload: Vec::new(),
    });
    accepts_protocol_error(transport::TransportError::Malformed);
    accepts_api_capabilities(hl_engine::Engine::new().capabilities());
    accepts_api_filesystem(spec::FilesystemSpec::default());
    accepts_api_resources(spec::ResourceSpec::default());
    accepts_api_validation(hl_engine::Validation {
        selected_extensions: Vec::new(),
        unavailable_optional_extensions: Vec::new(),
        degraded_features: Vec::new(),
        estimated_memory_bytes: 0,
        namespace_conflicts: Vec::new(),
        resources: spec::HostResourceEstimate::default(),
    });
    accepts_api_attach(hl_engine::AttachRequest::default());
}
