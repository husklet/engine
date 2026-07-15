# hl-engine

`hl-engine` is the safe Rust distribution of the standalone C engine. One crate build produces and embeds both
production Linux guest engines for the current AArch64 host:

- `aarch64-apple-darwin`
- `aarch64-unknown-linux-gnu`

The runtime is process-isolated because the two production translators intentionally own independent global JIT
state. `Guest::Aarch64` and `Guest::X86_64` select the embedded executable without asking applications to choose a
Cargo feature. There is no daemon, shell script, GUI, GPU, compositor, or Chrome integration.

```rust,no_run
use hl_engine::{BoxConfig, Engine, Guest, Sandbox};

let engine = Engine::new()?;
let box_config = BoxConfig::new()
    .rootfs("/var/lib/hl/alpine")
    .working_directory("/work")
    .environment("TERM", "xterm-256color")
    .memory_limit(512 * 1024 * 1024)
    .pid_limit(256)
    .sandbox(Sandbox::Enabled);
let exit = engine.run(Guest::X86_64, &box_config, "/bin/sh", ["-c", "echo hello"])?;
assert!(exit.success());
# Ok::<(), Box<dyn std::error::Error>>(())
```

`Engine::spawn` returns `RunningBox` for `try_wait`, `wait`, or `force_stop`. Dropping a live handle force-stops and
reaps its engine process. Launch settings are serialized through the C engine's versioned `hl_launch_config` wire
format; ambient `HL_*` variables are not used.

The published crate contains its exact C sources and headers. `build.rs` uses the platform C compiler and archiver,
links both engines, and ad-hoc signs macOS artifacts with the vendored JIT entitlement. No repository checkout or
files outside the crate are required. Unsupported targets fail at compile time with a direct diagnostic.
