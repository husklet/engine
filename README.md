# hl-engine

Standalone, pure-C execution engine for Linux guests. macOS is the current production host. A typed host-service ABI
defines the portable boundary, and Linux has a native backend whose implemented service groups are tested
independently. The production target roots do not yet use that boundary exclusively, so Linux-host execution is not
end-to-end production-ready.

## Build and test

```text
make all
make test
make compat-build
make format-check
```

Make orchestrates the C compiler and archiver. Maintained tooling and tests are C, and the normal build and test path
does not require Python or shell-script helpers. CMake metadata is also provided for installation and Rust `build.rs`
consumers.

Tagged logging calls compile to no-ops in release builds. Build with `make DEBUG=1` to compile them in, then select
tags with `HL_LOG=log:fs,log:jit` or the launch wire's `debug_log_offset`. Available tags are `fs`, `jit`, `syscall`,
`process`, `network`, `signal`, and `translate`; `log:all` enables every tag. Filtering is common portable-core
behavior, while each host backend only supplies the final byte sink.

## Domains

- `include/hl` — stable C/C++-compatible lifecycle, launch-wire and host-service ABI, using the husklet `hl_` namespace.
- `src/core` — engine lifecycle and host-service validation.
- `src/translator/ir` — private validated, host-OS-free translation IR.
- `src/translator/guest` — Linux guest ISA frontends.
- `src/translator/host` — host CPU lowering backends.
- `src/linux_abi` — Linux-visible syscall/process/fd/OFD/proc/sys models; never a host passthrough.
- `src/host` — fake, macOS, and Linux implementations of the universal host-service ABI; the Windows directory
  documents the same backend boundary but does not yet contain an implementation.
- `src/runner` — generic C command-line entry for launching a Linux guest.
- `src/core/target` — AArch64 and x86-64 Linux guest target roots. Each is currently a unity translation unit that
  includes the remaining target-specific translator and Linux ABI implementation.
- `tests` — C unit and Linux compatibility tests.

The intended dependency direction is `runner -> core -> translator + linux_abi -> host_services <- host backend`.
Independently compiled portable sources follow this rule and are checked by a C domain tool. The current production
unity roots still include platform-dependent implementation files, as described below.

The build emits separate `libhl-engine`, `libhl-translator`, `libhl-linux-abi`, and host-backend archives. Code already
listed as a standalone archive source is compiled independently. The two target roots remain unity objects and link
against those archives; their textual includes are the remaining boundary to remove.

## Current status

The libraries provide ABI validation, IR construction and validation, Linux fd/OFD semantics, host services, tagged
debug logging, and the public lifecycle. The macOS executables run AArch64 and x86-64 Linux guests through the typed
`hl_launch_config` wire. Linux is the sole guest personality; native macOS programs are not a supported guest. The
Linux backend is currently validated at the service and unit-test boundaries, not by a production guest run.

See `docs/ARCHITECTURE.md` for the dependency and ownership rules.
