use hl_engine::{extension, spec, transport};

fn accepts_api_provider(_: hl_engine_api::extension::ProviderId) {}
fn accepts_api_version(_: hl_engine_api::Version) {}
fn accepts_provider_process(_: hl_engine_provider::ProcessId) {}
fn accepts_protocol_frame(_: hl_engine_protocol::Frame) {}
fn accepts_protocol_error(_: hl_engine_protocol::TransportError) {}

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
}
