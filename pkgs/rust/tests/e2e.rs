use hl_engine::{BoxConfig, Engine, Exit, Guest};
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
    let engine = Engine::new().unwrap();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let exit = engine
            .run(
                guest,
                &BoxConfig::new(),
                fixture(42, guest),
                std::iter::empty::<&str>(),
            )
            .unwrap();
        assert_eq!(exit, Exit::Code(42));
    }
}

#[test]
fn guest_exit_70_remains_distinct_from_engine_failure() {
    let engine = Engine::new().unwrap();
    for guest in [Guest::Aarch64, Guest::X86_64] {
        let exit = engine
            .run(
                guest,
                &BoxConfig::new(),
                fixture(70, guest),
                std::iter::empty::<&str>(),
            )
            .unwrap();
        assert_eq!(exit, Exit::Code(70));
    }
}
