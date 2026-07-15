# hl-engine

`hl-engine` is the safe Rust distribution of the standalone C engine. One crate build produces and embeds both
production Linux guest engines for the current AArch64 host:

- `aarch64-apple-darwin`
- `aarch64-unknown-linux-gnu`

The runtime is process-isolated because the two production translators intentionally own independent global JIT
state. `Guest::Aarch64` and `Guest::X86_64` select the embedded executable without asking applications to choose a
Cargo feature. There is no daemon, shell script, GUI, GPU, compositor, or Chrome integration.

```rust,no_run
use hl_engine::{Config, Engine, Guest, Sandbox};

let engine = Engine::new();
let config = Config::new()
    .root("/var/lib/hl/alpine")
    .working_dir("/work")
    .env("TERM", "xterm-256color")
    .memory_limit(512 * 1024 * 1024)
    .process_limit(256)
    .sandbox(Sandbox::Enabled);
let exit = engine
    .command(Guest::X86_64, "/bin/sh")
    .config(config)
    .args(["-c", "echo hello"])
    .status()?;
assert!(exit.success());
# Ok::<(), Box<dyn std::error::Error>>(())
```

`Command::spawn` returns `Child` for `try_wait`, `wait`, `output`, or `force_stop`. Dropping a live handle force-stops and
reaps its engine process. Command settings are serialized through the C engine's versioned `hl_launch_config` wire
format; ambient `HL_*` variables are not used.

The published crate contains four pinned production executables: both guest ISAs for each supported host target.
Cargo selects and embeds the matching pair without compiling C or downloading anything. macOS artifacts are
stripped, ad-hoc signed with the JIT entitlement, and signature-verified before publication. Exact source commit,
checksums, and fixture provenance are recorded in `assets/PROVENANCE.md`. Unsupported targets fail at compile time.
