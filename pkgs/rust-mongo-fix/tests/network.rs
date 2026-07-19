use std::net::Ipv4Addr;
use std::path::PathBuf;

use hl_engine::network::{Bridge, Namespace, Rule};
use hl_engine::{Config, Engine, Error, Exit, Guest};

fn fixture() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("testdata/exit42-aarch64")
}

#[test]
fn typed_network_configuration_reaches_the_native_launcher() {
    let config = Config::new()
        .network_namespace(Namespace::new("rust-net-integration").unwrap())
        .network_bridge(Bridge::new("rust-bridge-integration").unwrap())
        .network_ipv4(Ipv4Addr::new(172, 30, 0, 2))
        .publish(Rule::new(65_530, 8_080).unwrap())
        .publish_external(true);
    let exit = Engine::new()
        .command(Guest::Aarch64, fixture())
        .config(config)
        .status()
        .unwrap();
    assert_eq!(exit, Exit::Code(42));
}

#[test]
fn incompatible_network_policy_is_rejected_before_process_start() {
    let config = Config::new()
        .network(true)
        .network_bridge(Bridge::new("bridge").unwrap());
    let error = Engine::new()
        .command(Guest::Aarch64, fixture())
        .config(config)
        .status()
        .unwrap_err();
    assert!(matches!(error, Error::InvalidConfig(_)));
}

#[test]
fn host_network_rejects_virtual_network_configuration() {
    let config = Config::new()
        .host_network(true)
        .network_namespace(Namespace::new("virtual").unwrap());
    let error = Engine::new()
        .command(Guest::Aarch64, fixture())
        .config(config)
        .status()
        .unwrap_err();
    assert!(matches!(error, Error::InvalidConfig(_)));
}
