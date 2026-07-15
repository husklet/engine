use hl_engine::{Engine, Guest};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args().skip(1);
    let guest = if arguments.next().as_deref() == Some("x86_64") {
        Guest::X86_64
    } else {
        Guest::Aarch64
    };
    let program = arguments.next().unwrap_or_else(|| "/bin/true".to_owned());
    let engine = Engine::new();
    let status = engine.command(guest, program).status()?;
    println!("guest exit: {:?}", status.code());
    Ok(())
}
