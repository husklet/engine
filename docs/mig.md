# Migration plan: Linux/amd host validation + Windows host backend path

## 0) Objective and evidence scope

Current goal: explore required architecture shifts to reliably support two new production host targets in the near-term roadmap:

- `linux/amd64` (native host execution parity)
- `windows/amd64` (Windows host backend, theoretically runnable with the same runtime interface)

This document is descriptive and theoretical; it avoids proposing unverified behavior and highlights hard evidence from source.

## 1) Current state (ground truth)

- Guest ISA support already exists for both `AARCH64` and `X86_64`.
- Real host backends are present only for Linux and macOS.
- Host selection is currently hard-fenced:
  - `src/core/target/native.h` defines only `HL_NATIVE_HOST_NAME` for `__APPLE__` or `__linux__`.
  - `src/core/target/native.c` dispatches only to `hl_host_macos_create` or `hl_host_linux_create`.
  - `src/core/activation.c` repeats the same `#if defined(__APPLE__) / __linux__` host-type split.
- Windows directory is currently documentation-only: `src/host/windows/README.md`.
- Build pipeline also gates host platform to `linux|macos` (`Makefile`) and matrix/platform sets (`flake.nix`) omit Windows.

## 2) Key architectural impact by layer

### 2.1 Layer A: Host service provider
This is the first-order change set.

- New files are required because host services are selected by concrete structs in `include/hl/{linux.h,macos.h}` and instantiated in platform modules.
- `hl_native_host_create` is the core factory used by both `target/run.c` and activation.
- Without a native Windows provider, no production path can create a valid `hl_host_services` block for Windows.

### 2.2 Layer B: Activation/runtime bootstrap

- `src/core/activation.c` currently cannot compile for non-Apple/Linux hosts.
- Once Windows services exist, activation can remain flow-compatible:
  - register target backends
  - run `hl_embedded_runtime_init`
  - construct services and execute typed core engine.
- Process-adoption behavior and signal forwarding in activation are nontrivial and must be verified under Windows semantics.

### 2.3 Layer C: Native compat shim and signal/context layer

- `src/host/native_context.h` currently provides signal context extraction only for Apple/Linux.
- `src/host/native_compat.h` includes Linux fallback aliases (`epoll`-style implementation of BSD-style interfaces) and Linux/Apple concrete primitives.
- A Windows-native path will likely require dedicated implementations rather than compile-time aliases.

### 2.4 Layer D: Build system and artifact wiring

- `Makefile` and `flake.nix` currently hardcode host build domains.
- New host source groups are needed so that static library packaging and host-specific symbols are emitted cleanly.

---

## 3) Folder-structure migration proposal (theoretical)

### 3.1 Existing folders

- `src/host/linux/*` and `src/host/macos/*`:
  provide concrete implementations with `host.c`, `directory.c`, `process.c`, `system.c`, `range.c`, and host-specific `probe.h`.
- `src/host/*.c|*.h`:
  common files currently reused by Linux/macOS (`clock`, `file`, `sync`, `private`, `resolve`, etc.).
- `include/hl/{linux.h,macos.h}`:
  public host constructors.

### 3.2 Proposed folder additions/updates

- `include/hl/windows.h` (new)
  - mirror pattern of `linux.h`/`macos.h` for opaque `hl_host_windows` type and create/destroy APIs.

- `src/host/windows/` (new implementation folder)
  - expected file set:
    - `host.c`
    - `directory.c`
    - `process.c`
    - `range.c`
    - `system.c`
    - `probe.h`
  - plus any Windows-specific helpers that cannot be satisfied from shared files.

- `src/core/target/native.h` + `src/core/target/native.c`
  - add explicit Windows branch and host-name mapping.
  - keep Apple/Linux branches and avoid silent fallback.

- `src/core/activation.c`
  - add Windows host typedef and `activation_host_create/destroy` path.

- `src/host/native_context.h` / `src/host/native_compat.h`
  - add guarded Windows support, or at least compile-safe stubs and explicit `#error` points for unsupported paths.
  - do not leave Linux-only aliases accidentally active on Windows.

- `Makefile`
  - extend `HOST` gate from `linux|macos` to include `windows`.
  - add `WINDOWS_HOST_SOURCES` and `PACKAGE_HOST_LIBRARY` equivalent to Linux/Mac.
  - make host library object routing deterministic and explicit.

- `flake.nix`
  - either add explicit `x86_64-windows` system output only if CI/toolchain can produce and test it;
  - or explicitly mark Windows runtime lane as `unsupported` until phase 2 complete.

---

## 4) Code-path migration phases (recommended)

### Phase 1 — Linux/AMD host consolidation (safety step)

- Verify and document explicit host/ISA compatibility by running existing linux-production builds on `x86_64` host.
- Keep guest ISA split unchanged (`AARCH64`, `X86_64`) and ensure backend dispatch never falls back unexpectedly.

### Phase 2 — Windows host service skeleton (minimum viable for build)

- Add `include/hl/windows.h` and all Windows backend files in `src/host/windows/*` implementing full host tables.
- Implement only the capability subset required by `hl_engine_create_with_options` and `hl_engine_run`:
  - `memory`, `clock`, `sync` (baseline)
  - `file`, `process` (for executable loading and child launch)
  - `code mapping`, as required by `hl_native_host_bind`.
- Ensure `src/core/target/native.c` no longer fails compilation with Windows.

### Phase 3 — Compat hardening for Windows

- Extend `src/host/native_context.h` and `src/host/native_compat.h` with Windows branches.
- Explicitly define unsupported surfaces in comments and logs where parity is not guaranteed (e.g., watch/epoll-kqueue abstractions, signal metadata semantics).

### Phase 4 — Remove legacy routing ambiguity

- Resolve `src/core/target/dual.c` behavior to avoid accidental ISA mismatch:
  - either retire shim and require explicit ISA dispatch, or
  - keep shim behind an explicit compatibility flag with clear TODO + runtime assertion.

### Phase 5 — Build matrix integration

- `Makefile`:
  - add host output paths for Windows library and target products.
- `flake.nix`:
  - if toolchain exists, add system output and deterministic artifact naming.
  - avoid impacting current Linux/macOS CI behavior.
- Add migration checklist gates before unfreezing any runtime lane.

---

## 5) Risk register (explicit)

- **Linux ABI under Windows remains the largest architectural unknown**
  - Current linux_abi code expects Linux semantics (containers, signals, procfs-like behavior, namespace-related behavior).
  - A Windows implementation may need WSL or equivalent strategy before this is executable with true feature parity.

- **Signal context portability**
  - `native_context.h` is currently a hard Apple/Linux abstraction; replacing it requires careful correctness, not just compilation.

- **Event/watch compatibility semantics**
  - `native_compat.h` currently relies on Linux/BSD emulation behavior in places.

- **Activation parity**
  - Windows signal/child-adoption behavior must be validated even if typed API works.

- **Build complexity**
  - Adding host lanes affects both `make` outputs and packaging; mistakes can silently produce wrong defaults.

---

## 6) Concrete acceptance criteria

- [ ] `Makefile` and `src/core/target/native.h/.c` compile with `HOST=windows`.
- [ ] `src/core/activation.c` has a non-macOS/Linux branch and compiles on Windows host toolchain.
- [ ] `src/host/windows/*` provides `hl_host_services` with at least required baseline capabilities.
- [ ] `src/host/native_context.h` and `src/host/native_compat.h` no longer fail on Windows compilation in unchanged call-sites.
- [ ] Build artifacts include deterministic naming for each host/ISA matrix slice (as today for linux/mac outputs).
- [ ] Linux `x86_64` host execution path is documented as baseline-validated before Windows host milestone is marked complete.
