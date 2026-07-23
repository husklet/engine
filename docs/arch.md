# Engine Runtime Architecture (Current State, July 21, 2026)

## 1) Core ownership map

The implementation is split by responsibility across these layers:

- **Public API layer** (`include/hl`, `src/core/config.c`, `src/runner`)
  - Defines ABI versions, config schema, and host-service contracts.
  - Provides typed entrypoints (`hl_engine_create*`, `hl_engine_run`, `hl_engine_destroy`) and host-specific creation entrypoints (`hl_host_linux_create`, `hl_host_macos_create`).

- **Core runtime layer** (`src/core/*.c`)
  - Owns validation, lifecycle, process contract execution, and backend dispatch.
  - Maintains canonical engine state machine and result materialization.

- **Target layer** (`src/core/target/{aarch64,x86_64,lifecycle,run}.c`, `src/core/target/dual.c`, `src/core/target/native.*`)
  - Owns guest entry points (`hl_run_linux_guest`, status publishing, run contracts), namespace initialization, and startup constructor registration.

- **Linux ABI layer** (`src/linux_abi/*`)
  - Owns syscalls/translation environment expected by Linux guest images.
  - Independent of host OS choice at call-site level, but semantically Linux-centric.

- **Host service layer** (`src/host/*`)
  - Maps portable host service interfaces to OS primitives.
  - Linux and macOS backends are complete enough to build and register static libs.
  - Windows backend directory exists as README-only placeholder.

- **Activation path** (`src/core/activation.c`)
  - Config-file/embedded supervisor launch path that initializes multiple subsystems then delegates into core runtime.

---

## 2) Core communication graph

### 2.1 Typed API entry (library-style)

1. Host app builds a host backend
   - macOS: `hl_host_macos_create` (`include/hl/macos.h`, `src/host/macos/*.c`)
   - Linux: `hl_host_linux_create` (`include/hl/linux.h`, `src/host/linux/*.c`)
   - `hl_host_services` is the single transport contract passed into runtime (`include/hl/host_services.h`).

2. Host app builds config and calls
   - `hl_engine_create_with_options` (`src/core/engine.c`)
   - validates ABI/size fields and `hl_host_services` capabilities (`src/core/host_services.c`)
   - chooses backend from `engine->backend = production_backends[config->guest_isa]`.

3. Host app calls
   - `hl_engine_run` (`src/core/engine.c`)
   - forwards to `backend->start_process` (`src/core/target/lifecycle.c`/`src/core/target/*`) with config + options + host services.
   - waits via `host->process->wait`, then forwards to `backend->finish_process` when available.
   - publishes unified exit through `hl_engine_exit` and tears down resources.

4. Backend implementation currently available
   - constructor-registered production backends in each ISA translation unit (`src/core/target/lifecycle.c`)
   - dispatch index in `src/core/engine.c`.
   - start/finish logic (`spawn_cloned` or `hl_linux_abi_spawn`, shared memory result record, wait/finish reconciliation).

### 2.2 Native CLI entry

`hl_native_engine_run` (`src/core/target/run.c`) is the runner entry path for local CLI launches.

1. It creates a native host via `hl_native_host_create` (`src/core/target/native.c`).
2. It builds `hl_engine_config`, maps stdin/stdout/stderr as transferred file handles, and delegates into `hl_engine_create_with_options`.
3. It runs engine and optionally writes `hl_launch_result` (`hl_native_result_store`) using the same host `file` service.

### 2.3 Activation entry

`hl_activation_start` / `hl_activation_child` in `src/core/activation.c` follow this sequence:

1. Read activation descriptor and request block from process supervisor channel.
2. Register both target backends:
   - `hl_aarch64_target_register_backend`
   - `hl_x86_64_target_register_backend`
3. Run one-time target init for requested ISA:
   - `hl_aarch64_target_runtime_init` or `hl_x86_64_target_runtime_init`
4. Construct services with host-specific `activation_host_create` and run `hl_engine_create_with_options` + `hl_engine_run`.

---

## 3) Data movement and where it happens

- **Config validation (`src/core/engine.c`)**
  - Copying and owning config strings, executable image, box config, fd bindings and options happens in engine state.
  - ABI and field-level checks happen before host/process launch.

- **Runtime services access (`src/core/engine.c` + `src/core/host_services.c`)**
  - `hl_host_services_validate` gates required capabilities.
  - Minimum required set for all runs is `memory + clock + sync`.
  - `executable`, `file`, and process-related paths trigger additional required bits (`file`, `process`).

- **Execution (`src/core/target/lifecycle.c`)**
  - Parent creates a shared `hl_engine_child_result` mapping for child/parent agreement.
  - Child runs `hl_run_linux_guest`, then publishes status.
  - Parent reifies child status from shared record and `wait()` detail.

- **Linux ABI context (`src/linux_abi/`)**
  - `hl_linux_abi_spawn` and related helpers own namespace/container setup, translation cache interaction, and fd table wiring for Linux guest execution.

---

## 4) What “currently supported” really means

- Guest ISAs implemented at target layer: `AARCH64` and `X86_64` (`src/core/target/aarch64.c`, `src/core/target/x86_64.c`).
- Host backends implemented and selectable by compile-time/compile-target path: Linux and macOS (`src/host/linux`, `src/host/macos`).
- Windows is not yet a runnable host backend despite an empty placeholder directory.
- A legacy compatibility shim exists:
  - `src/core/target/dual.c` routes `hl_run_linux_guest` to `hl_aarch64_run_linux_guest`.

---

## 5) Architecture seams that gate platform expansion

1. **Host selection seam**
   - `src/core/target/native.h` + `src/core/target/native.c` define `HL_NATIVE_HOST_NAME` as `macos|linux` only and error elsewhere.
   - `src/core/activation.c` has matching `#error` and host typedef guards.

2. **Host service implementation seam**
   - `src/core/target/native.c` calls only `hl_host_macos_create` / `hl_host_linux_create`.
   - No Windows implementations for `hl_host_process`, `hl_host_file`, and related interface tables.

3. **Translation/compat seam**
   - `src/host/native_context.h` and `src/host/native_compat.h` are Apple/Linux only with hard errors or platform-shared emulation assumptions.

4. **Build and package seam**
   - `Makefile` gates `HOST` to `linux|macos` and derives package host objects from that.
   - `flake.nix` systems omit `x86_64-windows` and only builds/installs `linux` binaries and test matrix entries.
