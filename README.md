# HL Engine

HL Engine runs AArch64 and x86-64 Linux programs on AArch64 macOS and Linux hosts. It provides a standalone C execution engine and a safe, process-isolated Rust API for configuring Linux containers, lifecycle, isolation, filesystems, and external device integrations.

| Host | Status | Linux guests |
| --- | --- | --- |
| macOS AArch64 | Supported | AArch64, x86-64 |
| Linux AArch64 | Supported | AArch64, x86-64 |
| Linux x86-64 | In progress — see below | AArch64, x86-64 (interpreted) |

"Supported" means the exact-golden compatibility, lifecycle and production matrices pass on that host for **both**
guest ISAs. Linux x86-64 is not there yet, and the table says only what is proven on it:

- The engine, both guest fixture corpora, `ctest -L unit` and `ctest -L package` build and pass, and the
  published Rust crate accepts the host.
- **Both** guest ISAs run to completion through the production engine: `production.smoke-aarch64`,
  `production.smoke-x86_64`, `production.matrix`, both `production.full-*.core-abi` manifests,
  `production.full-aarch64.signals`, `compat.isa-aarch64` and all ten `lifecycle` cases pass;
  `checkpoint` is **82/82**, and `ckpt-cross` (an image captured by one backend and restored by the other,
  both directions) is 11/11.
- Measured across the whole compatibility corpus — 24 manifests, both guest ISAs, 3036 (case, guest-ISA)
  runs, per case, nothing sampled, binaries pinned, in an isolated worktree — **99.84% pass** (aarch64 guest
  1500/1503, x86-64 guest 1531/1533), with zero cross-ISA output divergence. The five residual legs are
  four environmental (a `nice ≤ 5` precondition the harness does not enforce) and one real defect, named
  with a reproducer in [`docs/amd64-host-findings.md`](docs/amd64-host-findings.md) §3.12.
- **The compat corpus is not gated by CI here yet**: `cmake/CiLanes.cmake` still excludes this host from the
  compat shards. `.github/workflows/linux-x86_64.yml` does gate `unit`, `nested-engine`,
  `emulated-aarch64-gated` and the package check.
- The engine hosts **itself**, and it is a gate rather than a habit — `nested.*`, five cells. An amd64 host
  interpreting the AArch64 build of hl-engine, which in turn JIT-compiles an x86-64 guest, runs to
  completion; so does a three-engine chain. The lane skips loudly, never silently, when the aarch64 cross
  build is absent.
- Guest execution there is **interpreted** by default. Measured overhead is not one number: **3.4× on
  kernel-bound work, 94–605× on guest-execution-bound work** (median 25.3×), so the `perf-linux` lane is
  record-only rather than threshold-enforcing — see
  [`docs/amd64-host-performance.md`](docs/amd64-host-performance.md), where the single largest cost turned
  out to be a host syscall per guest basic block rather than the interpretation itself.
- An **x86-64 same-ISA transliterator** exists behind `HL_X86_TRANSLIT=1` (`-DHL_TRANSLIT=ON` moves the
  default), giving **15× on compute** — 351 ns → 23.4 ns per guest block — and 9.7×–23× on the fixtures it
  used to refuse. It is off by default and falls back to the interpreter **per block**, so anything it does
  not handle costs speed rather than correctness. It declines guest `%fs`, SSE/AVX and x87 today; see
  [`docs/amd64-host-translit.md`](docs/amd64-host-translit.md). The **aarch64** guest is still interpreted
  and always will be on this host without a new backend — that quadrant is cross-ISA.
- The AArch64 host arm can be **executed** on an x86-64 box under `qemu-aarch64`, which is how several
  defects in the shipped AArch64 JIT were confirmed rather than merely inferred.
  [`docs/emulated-aarch64.md`](docs/emulated-aarch64.md) states precisely what emulation vouches for and
  what it does not — notably not weak memory ordering.

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
