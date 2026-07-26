# HL Engine

HL Engine runs AArch64 and x86-64 Linux programs on AArch64 macOS and Linux hosts. It provides a standalone C execution engine and a safe, process-isolated Rust API for configuring Linux containers, lifecycle, isolation, filesystems, and external device integrations.

| Host | Status | Linux guests |
| --- | --- | --- |
| macOS AArch64 | Supported | AArch64, x86-64 |
| Linux AArch64 | Supported | AArch64, x86-64 |
| Linux x86-64 | Builds and passes the host unit lane; **runs no guests yet** | none yet |

"Supported" means the exact-golden compatibility, lifecycle and production matrices pass on that host for both guest
ISAs. Linux x86-64 is not there: the engine, both guest fixture corpora and the `unit` lane build and pass on it, and
the published Rust crate accepts the host, but neither guest ISA has a code generator for an x86-64 host yet — both
production frontends emit ARM64 directly. `production.smoke-x86_64` is the milestone to watch.
[`docs/amd64-host.md`](docs/amd64-host.md) explains the seam and what is gated on what. Windows is a reserved
boundary with no code.

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
- [`docs/amd64-host.md`](docs/amd64-host.md) — the x86-64 Linux host: the host-CPU seam and what still gates guest
  execution there.
- [`pkgs/rust/README.md`](pkgs/rust/README.md) — the Rust crate's own API surface.

Build and test, from a `nix develop` shell:

```sh
cmake -G Ninja -B build-cmake
ninja -C build-cmake
ctest --test-dir build-cmake -L unit --no-tests=error
```

CMake is the only build system. `ctest --print-labels` lists every lane; always pass
`--no-tests=error`, because `ctest -L` on an unknown label exits 0. `cmake/CiLanes.cmake` declares
which lanes CI runs and `gate.ci-lane-parity` fails the build if one of them selects no tests.
