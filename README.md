# hl-engine

Portable, pure-C execution engine for Linux guests. This repository is the Phase 5 extraction workspace: it
contains a preserved snapshot of the current production runtime and a new layered implementation designed for
macOS, Linux and Windows host backends.

## Build and test

```text
make all
make test
make compat-build
make format-check
```

Make orchestrates the C compiler and archiver. Project tooling and tests are C; there are no Python or shell-script
build/test helpers. CMake metadata is also provided for installation and Rust `build.rs` consumers.

## Domains

- `include/hl` — stable C/C++-compatible lifecycle, launch-wire and host-service ABI, using the husklet `hl_` namespace.
- `src/core` — engine lifecycle and host-service validation.
- `src/translator/ir` — private validated, host-OS-free translation IR.
- `src/translator/guest` — Linux guest ISA frontends.
- `src/translator/host` — host CPU lowering backends.
- `src/linux_abi` — Linux-visible syscall/process/fd/OFD/proc/sys models; never a host passthrough.
- `src/host` — fake, macOS, Linux and Windows implementations of the universal host-service ABI.
- `src/runner` — isolated C runner. Rust bindings should supervise this runner until the engine is safe to embed.
- `src/production` — complete transferred production runtime at the commit recorded in `SOURCE_SNAPSHOT`; it is the
  runnable oracle while its unity domains are replaced.
- `tests` — C unit tests and imported Linux compatibility fixtures.

The dependency direction is `runner -> core -> translator + linux_abi -> host_services <- host backend`.
Platform headers are forbidden in portable source and checked by a C domain tool.

The build emits separate `libhl-engine`, `libhl-translator`, and `libhl-linux-abi` archives so ownership is
enforced at link time rather than represented only by directories.

## Current status

The new libraries build and prove ABI validation, IR construction/validation, Linux fd/OFD lifetime, fake-host
failure injection and the public lifecycle. The transferred Linux production engines execute AArch64 and x86-64
guests through both the new `hl_launch_config` wire and its legacy input adapter. The layered engine execution entry
still returns `HL_STATUS_NOT_SUPPORTED`; migrate one proven domain at a time from `src/production`, keeping that
end-to-end lane as the oracle. Darwin/macOS is a host backend only; there is no Darwin guest personality.

See `docs/ARCHITECTURE.md` and `docs/MIGRATION.md` before moving code.
