# hl-engine

Portable, pure-C execution engine for Linux guests. Guest instruction translation and the Linux ABI are independent
of the host operating system; host behavior is supplied through a small service interface. macOS is the primary host
today, with Linux implemented as a host backend where its service groups are complete.

## Build and test

```text
make all
make test
make compat-build
make format-check
```

Make orchestrates the C compiler and archiver. Project tooling and tests are C; there are no Python or shell-script
build/test helpers. CMake metadata is also provided for installation and Rust `build.rs` consumers.

Tagged logging is absent from release call sites. Build with `make DEBUG=1` to compile it in, then select tags with
`HL_LOG=log:fs,log:jit` or the launch wire's `debug_log_offset`. Available tags are `fs`, `jit`, `syscall`, `process`,
`network`, `signal`, `gpu`, and `translate`; `log:all` enables every tag. Filtering is common portable-core behavior,
while each host backend only supplies the final byte sink.

## Domains

- `include/hl` — stable C/C++-compatible lifecycle, launch-wire and host-service ABI, using the husklet `hl_` namespace.
- `src/core` — engine lifecycle and host-service validation.
- `src/translator/ir` — private validated, host-OS-free translation IR.
- `src/translator/guest` — Linux guest ISA frontends.
- `src/translator/host` — host CPU lowering backends.
- `src/linux_abi` — Linux-visible syscall/process/fd/OFD/proc/sys models; never a host passthrough.
- `src/host` — fake, macOS, and Linux implementations of the universal host-service ABI; the Windows directory
  documents the same backend boundary but does not yet contain an implementation.
- `src/runner` — isolated C runner and command-line entry.
- `src/production` — the complete AArch64 and x86-64 Linux guest engines used by the macOS host build.
- `tests` — C unit and Linux compatibility tests.

The dependency direction is `runner -> core -> translator + linux_abi -> host_services <- host backend`.
Platform headers are forbidden in portable source and checked by a C domain tool.

The build emits separate `libhl-engine`, `libhl-translator`, and `libhl-linux-abi` archives so ownership is
enforced at link time rather than represented only by directories.

## Current status

The libraries provide ABI validation, IR construction and validation, concurrent Linux fd/OFD semantics, host
services, tagged debug logging, and the public lifecycle. The production engines execute AArch64 and x86-64 Linux
guests through the typed `hl_launch_config` wire. Darwin/macOS is a host backend only; there is no Darwin guest
personality.

See `docs/ARCHITECTURE.md` for the dependency and ownership rules.
