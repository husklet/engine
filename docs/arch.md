# Engine runtime map

Where the layers DOCS.md section 3 defines actually live, at symbol level. DOCS.md is
normative; this file is a navigation aid.

## 1. Ownership

- **Public API** (`include/hl`, `src/core/config.c`, `src/runner/main.c`) — ABI versions,
  config schema, host-service contracts; `hl_engine_create*`, `hl_engine_run`,
  `hl_engine_destroy`, plus the host constructors `hl_host_linux_create` and
  `hl_host_macos_create`.
- **Core runtime** (`src/core/*.c`) — validation, lifecycle, process contract execution,
  backend dispatch, engine state machine, result materialization.
- **Target layer** (`src/core/target/{aarch64,x86_64,run,native,services,bus}.c`,
  `src/core/target/dual.c`, `src/core/lifecycle.c`) — guest entry points
  (`hl_run_linux_guest`), status publishing, run contracts, namespace init
  (`src/core/target/namespace.h`), and constructor registration of the production backend.
- **Linux ABI** (`src/linux_abi/*`) — the syscall and environment surface a Linux guest
  image expects. Host-OS-neutral at the call site, Linux-semantic throughout.
- **Host services** (`src/host/*`) — portable service interfaces mapped to OS primitives.
  `src/host/linux` and `src/host/macos` are complete backends; `src/host/fake` is the
  deterministic unit-test backend; `src/host/windows/` is README-only.
- **Activation** (`src/core/activation.c`) — the config-file/embedded supervisor launch
  path; initializes subsystems and delegates into the core runtime.

## 2. Entry paths

### Typed API (library)

1. Build a host backend: `hl_host_macos_create` (`include/hl/macos.h`,
   `src/host/macos/host.c`) or `hl_host_linux_create` (`include/hl/linux.h`,
   `src/host/linux/host.c`). `hl_host_services` (`include/hl/host_services.h`) is the only
   transport contract into the runtime.
2. `hl_engine_create_with_options` (`src/core/engine.c`) validates ABI/size fields and
   capabilities (`hl_host_services_validate`, `src/core/host_services.c`) and selects
   `engine->backend = production_backends[config->guest_isa]`.
3. `hl_engine_run` (`src/core/engine.c`) forwards to `backend->start_process`
   (`hl_production_start_process`, `src/core/lifecycle.c`), waits via
   `host->process->wait`, then calls `backend->finish_process` where present, publishes the
   unified exit through `hl_engine_exit`, and tears down.

The production backend is one `hl_engine_backend` per translation unit:
`src/core/lifecycle.c` is compiled once per ISA with `-DHL_PRODUCTION_GUEST_ISA`, and its
`__attribute__((constructor))` calls `hl_target_register_backend()`.

### Native CLI

`hl_native_engine_run` (`src/core/target/run.c`) creates a native host with
`hl_native_host_create` (`src/core/target/native.c`), builds `hl_engine_config` with
stdin/stdout/stderr as transferred file handles, delegates into
`hl_engine_create_with_options`, and optionally writes `hl_launch_result` through
`hl_native_result_store` using the same host `file` service.

### Activation

`hl_activation_start` / `hl_activation_child` (`src/core/activation.c`):

1. read the activation descriptor and request block from the supervisor channel;
2. register both target backends (`hl_aarch64_target_register_backend`,
   `hl_x86_64_target_register_backend`);
3. run one-time init for the requested ISA (`hl_aarch64_target_runtime_init` /
   `hl_x86_64_target_runtime_init`);
4. construct services with the host-specific `activation_host_create`, then
   `hl_engine_create_with_options` + `hl_engine_run`.

## 3. Where data moves

- **Config** (`src/core/engine.c`) — config strings, executable image, box config, fd
  bindings and options are deep-copied into engine state; ABI and field checks precede any
  host or process launch.
- **Capabilities** (`src/core/host_services.c`) — `memory + clock + sync` is required for
  every run; `executable`, `file` and process paths add further required bits.
- **Execution** (`src/core/lifecycle.c`) — the parent maps a shared
  `hl_engine_child_result`; the child runs `hl_run_linux_guest` and publishes status; the
  parent reifies that record together with `wait()` detail.
- **Linux ABI** (`src/linux_abi/`) — `hl_linux_abi_spawn` owns namespace/container setup,
  translation-cache interaction, and fd-table wiring.

## 4. Seams that gate a new host platform

1. **Host selection.** `src/core/target/native.h` defines `HL_NATIVE_HOST_NAME` only for
   `__APPLE__`/`__linux__` and errors elsewhere; `src/core/target/native.c` dispatches only
   to `hl_host_macos_create`/`hl_host_linux_create`; `src/core/activation.c` repeats the
   same split with matching `#error` guards.
2. **Compat shims.** `src/host/native_context.h` (signal-context extraction) and
   `src/host/native_compat.h` (BSD/Linux primitive aliasing) are Apple/Linux only.
3. **Legacy ISA routing.** `src/core/target/dual.c` routes `hl_run_linux_guest` to
   `hl_aarch64_run_linux_guest`.
4. **Build.** `flake.nix` carries `hostBackends` and `guestISAs` as data tables, so a new
   host backend is an entry there plus source; the Makefile's `HOST` gate is `linux|macos`
   (see docs/makefile-retirement.md).
