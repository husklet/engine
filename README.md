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

## Documentation

- [`DOCS.md`](DOCS.md) — normative design: layering, public contracts, build and test model, roadmap. Start here.
  It indexes the rest of `docs/`.
- [`docs/arch.md`](docs/arch.md) — where each layer lives in the source, at symbol level.
- [`pkgs/rust/README.md`](pkgs/rust/README.md) — the Rust crate's own API surface.

Build and test, from a `nix develop` shell:

```sh
cmake -G Ninja -B build-cmake
ninja -C build-cmake
ctest --test-dir build-cmake -L unit
```

`ctest --print-labels` lists every lane. A ~2900-line `Makefile` still exists and is what CI runs for the
compatibility shards and the macOS lane; see [`docs/makefile-retirement.md`](docs/makefile-retirement.md).
