# HL Engine

HL Engine runs AArch64 and x86-64 Linux programs on AArch64 macOS and Linux hosts. It provides a standalone C execution engine and a safe, process-isolated Rust API for configuring Linux containers, lifecycle, isolation, filesystems, and external device integrations.

| Host | Status | Linux guests |
| --- | --- | --- |
| macOS AArch64 | Supported | AArch64, x86-64 |
| Linux AArch64 | Supported | AArch64, x86-64 |
| Linux x86-64 | In progress — see below | AArch64, x86-64 (interpreted) |

"Supported" means the exact-golden compatibility, lifecycle and production matrices pass on that host for **both**
guest ISAs. Linux x86-64 is not there yet, and the table says only what is proven on it:

- The engine, both guest fixture corpora and `ctest -L unit` (115/115) build and pass, and the published Rust crate
  accepts the host.
- **Both** guest ISAs run to completion through the production engine: `production.smoke-aarch64`,
  `production.smoke-x86_64`, `production.matrix`, both `production.full-*.core-abi` manifests,
  `production.full-aarch64.signals`, `compat.isa-aarch64` and all ten `lifecycle` cases pass, and
  `checkpoint` is 77/78.
- Measured across the whole compatibility corpus — 24 manifests, 1580 active cases, 3013 (case, guest-ISA)
  runs — **87.4% pass** (aarch64 guest 85.7%, x86-64 guest 89.0%), with zero cross-ISA output divergence
  among cases passing on both. Most of the remainder is unimplemented named CPU extensions.
  **None of that is gated by CI yet**: `cmake/CiLanes.cmake` still excludes this host from the compat
  shards, so `.github/workflows/linux-x86_64.yml` runs only `ctest -L unit`.
- The engine hosts **itself**: an amd64 host interpreting the AArch64 build of hl-engine, which in turn
  JIT-compiles an x86-64 guest, runs to completion.
- Guest execution there is **interpreted**, not JIT-compiled, so it is roughly 10-50× slower than on an AArch64
  host and the `perf-linux` lane is record-only rather than threshold-enforcing. Neither guest ISA has a code
  generator for an x86-64 host: both production frontends emit ARM64 directly.

[`docs/amd64-host.md`](docs/amd64-host.md) explains the seam, the (host CPU × guest ISA) matrix and the staging;
[`docs/amd64-host-findings.md`](docs/amd64-host-findings.md) records the defects that work turned up and what a
reviewer needs to know. Windows is a reserved boundary with no code.

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
