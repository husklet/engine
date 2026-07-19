use hl_engine::{Engine, Exit, Guest};
use std::path::PathBuf;

fn fixture(status: i32, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/exit{status}-{isa}"))
}

#[test]
fn both_guest_isas_report_typed_exit_42() {
    let engine = Engine::new();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let exit = engine.command(guest, fixture(42, guest)).status().unwrap();
        assert_eq!(exit, Exit::Code(42));
    }
}

#[test]
fn guest_exit_70_remains_distinct_from_engine_failure() {
    let engine = Engine::new();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let exit = engine.command(guest, fixture(70, guest)).status().unwrap();
        assert_eq!(exit, Exit::Code(70));
    }
}

#[test]
fn initial_executable_authority_is_not_reused_by_exec() {
    let engine = Engine::new();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let executable = fixture_named("exec-denied", guest);
        assert_eq!(
            engine
                .command(guest, &executable)
                .arg(&executable)
                .status()
                .unwrap(),
            Exit::Code(0)
        );
    }
}

fn fixture_named(name: &str, guest: Guest) -> PathBuf {
    let isa = match guest {
        Guest::Aarch64 => "aarch64",
        Guest::X86_64 => "x86_64",
    };
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("testdata/{name}-{isa}"))
}
