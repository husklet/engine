use hl_engine::{extension, spec};

fn accepts_api_provider(_: hl_engine_api::extension::ProviderId) {}
fn accepts_api_version(_: hl_engine_api::Version) {}

#[test]
fn legacy_module_paths_reexport_api_contracts() {
    let provider = extension::ProviderId::new("compatibility.test").expect("valid provider id");
    let version = spec::Version::new(1, 0);

    accepts_api_provider(provider);
    accepts_api_version(version);
}
