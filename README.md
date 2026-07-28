# HL Engine

HL Engine runs AArch64 and x86-64 Linux programs on AArch64 macOS and Linux hosts. It provides a standalone C execution engine and a safe, process-isolated Rust API for configuring Linux containers, lifecycle, isolation, filesystems, and external device integrations.

| Host | Status | Linux guests |
| --- | --- | --- |
| macOS AArch64 | Supported | AArch64, x86-64 |
| Linux AArch64 | Supported | AArch64, x86-64 |
| Linux x86-64 | In progress | AArch64, x86-64 |

```sh
cargo add hl-engine
```

```rust,no_run
use hl_engine::{Config, Container, Engine, Guest, Mount};

fn accelerator(container: &mut Container) {
    container.mount(Mount::read_write("/dev/acme0", "/dev/acme0"));
    container.prepend_path("LD_LIBRARY_PATH", "/opt/acme/lib");
}

let output = Engine::new()
    .command(Guest::X86_64, "/bin/sh")
    .config(
        Config::new()
            .root("/var/lib/hl/alpine")
            .working_dir("/work")
            .env("TERM", "xterm-256color"),
    )
    .args(["-c", "printf 'hello from Linux'"])
    .apply(accelerator)
    .output()?;
assert!(output.exit.success());
# Ok::<(), Box<dyn std::error::Error>>(())
```

Applied container closures contribute only mounts and environment edits; the engine remains device-neutral.

## Build and test

Nix is the contributor interface. Build the engine, run all checks, or enter a
development shell:

```sh
nix build
nix flake check
nix develop
```

The flake owns supported toolchains and contributor entry points; CMake owns the
internal build graph. Inside `nix develop`, `ctest --print-labels` lists test
lanes. Always use `--no-tests=error` when selecting a label.

## Repository map

| Path | Owner |
| --- | --- |
| `include/hl/` | Public C interfaces |
| `src/` | Engine, translators, Linux ABI, and host backends |
| `pkgs/rust/` | Process-isolated Rust API |
| `linter/` | Static-analysis driver and policy |
| `tests/` | Unit, integration, compatibility, and compliance tests |
| `tools/` | Maintainer utilities and benchmarks |
| `cmake/`, `flake.nix` | Build graph and reproducible contributor environment |

Component-specific contracts live beside their code, for example in
[`linter/README.md`](linter/README.md) and
[`pkgs/rust/README.md`](pkgs/rust/README.md).
