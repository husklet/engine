use hl_engine::{Container, Engine, Guest, Mount};

fn standard_paths(container: &mut Container) {
    container.prepend_path("PATH", "/opt/tools");
    container.append_path("PATH", "/usr/local/bin");
}

#[test]
fn container_accepts_closures_and_reusable_functions() {
    let engine = Engine::new();
    let command = engine
        .command(Guest::Aarch64, "/missing")
        .apply(standard_paths)
        .apply(|container| {
            container.env("MODE", "test");
        })
        .mount(Mount::read_only("/tmp", "/host-tmp"));
    assert!(command.status().is_err());
}
