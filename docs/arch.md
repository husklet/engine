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
  deterministic unit-test backend; `src/host/windows/` is README-only. This layer is keyed on
  the host **OS** only; it is host-CPU-neutral and compiled on x86-64 unchanged.
- **Translator** (`src/translator/*`) — the production frontends
  `src/translator/guest/{aarch64,x86_64}/` emit host machine code directly; there is no
  intermediate representation and no host-neutral lowering seam a new host CPU could select.
  `src/translator/host/aarch64/asm.{c,h}` is the ARM64 instruction assembler they emit through.
  DOCS.md 3.3 has the detail. (An unused IR + per-host-CPU lowering pipeline used to sit under
  `src/translator/host/<cpu>/codegen.c`; it was deleted — docs/amd64-host-findings.md 3.1.)
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

## 4. Seams that gate a new host

"Host platform" is two independent axes, and the seams do not overlap — decide which one you
are adding before reading further. DOCS.md section 11 keeps the two checklists apart. The host
OS is `src/host/<os>/`, `__APPLE__`/`__linux__`, `CMAKE_SYSTEM_NAME`, `hostBackends`; the host
CPU is `HL_HOST_CPU_*` (`src/host/host_cpu.h`), `HL_HOST_ARCH` (CMakeLists.txt), `hostCPUs`
(flake.nix). Branch on `HL_HOST_CPU_*`, never on a bare `__aarch64__`/`__x86_64__`.

### 4.1 A new host OS

1. **Service backend.** `src/host/<os>/` implementing `hl_host_services`.
2. **Host selection.** `src/core/target/native.h` (`HL_NATIVE_HOST_NAME`),
   `src/core/target/native.c` (`hl_host_macos_create`/`hl_host_linux_create`) and
   `src/core/activation.c` each split on `__APPLE__`/`__linux__` with an `#error` default.
3. **Compat shim.** `src/host/native_compat.h` — BSD/Linux primitive aliasing, OS-only.
4. **Build.** `hostBackends` in `flake.nix`.

None of these gated x86-64 Linux: they are all OS-axis, and `src/host/linux` plus all three
selection sites compiled on a new host CPU unchanged.

### 4.2 A new host CPU

This is the axis that costs, and it is code generation, not selection: both production frontends
emit ARM64 directly, so on any other host CPU **each guest ISA needs a new back end**.
`guest/x86_64/emit.c`'s own header says "arm64 host emitters"; `guest/aarch64/translate.c` is a
same-ISA transliterator that copies guest instruction words verbatim and has no decoder at all.
The composition point is a `HL_HOST_CPU_AARCH64` fork near the top of `src/core/target/aarch64.c`
and `src/core/target/x86_64.c`, selecting the JIT files or `guest/<isa>/interp.c`.

1. **The entry trampolines.** `run_block` / `block_return` — hand-written ARM64 with `struct cpu`
   byte offsets baked in as decimal literals, in four copies: `src/core/dispatch.c` (GCC
   file-scope `__asm__` vs clang `naked`) and `src/translator/guest/x86_64/translate.c` (the same
   pair at different offsets, because `struct cpu` differs per guest ISA). A backend supplies its
   own pair and defines `G_OWN_TRAMPOLINES` to suppress the shared ones; the `#else` arms exist
   only to abort with a diagnostic. `dispatch.c`'s whole contract with a backend is
   `translate_block()` + `run_block()`, so `code` need not be host machine code.
2. **The register model** — whether guest registers live in host registers at all.
   `src/core/target/x86_64.c`'s header states the x86-64 JIT's (guest `rax..r15` in host
   `x0..x15`, `cpu` pinned in `x28`); the aarch64 transliterator keeps 31 guest GPRs in the
   matching host GPRs and steals `x18`/`x28`/`x30`. The assumption reaches past codegen:
   `sigframe_capture_fault` / `sigframe_resume_dispatch` (`src/core/target/{aarch64,x86_64}.c`)
   fork on the host CPU because a JIT must reconstruct guest state from the host mcontext
   (`hl_aarch64_signal_capture`, `hl_x86_signal_capture`, `jit_instruction_guest_pc`) while an
   interpreter's `struct cpu` is already current; `mach_resolve_fault`
   (`src/core/target/aarch64.c`) types on `arm_thread_state64_t` under a bare `__APPLE__`; and
   `install_host_sigaltstack` (`src/linux_abi/thread.c`) exists only because the aarch64 frontend
   runs with host SP == guest SP.
3. **`ibtc_publish`** (`src/translator/cache.c`) — a 16-byte single-copy-atomic pair store paired
   with a lock-free reader in emitted code. The correctness argument is per host CPU: `stp` under
   FEAT_LSE2 on AArch64, `movdqa` on x86-64. A new backend's indirect-branch reader must be an
   aligned 16-byte load, not two 8-byte ones.
4. **Per-backend obligations with nothing enforcing them.** Non-PIE address *materialisation* must
   be un-biased to the LOW link address while accesses stay biased — `pcrel_base`
   (`guest/aarch64/translate.c`), the rip-relative `lea` rewrite (`guest/x86_64/lower/mov.c`),
   `interp_lea_value` (`guest/x86_64/interp.c`) — and `call_return_pc` / `interp_call_return_pc`
   is the sibling rule for pushed return addresses. Omitting either fails as a hang inside glibc,
   several frames from the cause: docs/amd64-host-findings.md 3.11.
5. **Signal-context extraction.** `src/host/native_context.h` is an (OS × CPU) matrix.
   `HL_HOST_UC_PC`/`HL_HOST_UC_SP` are total; everything else sits behind
   `HL_HOST_HAS_A64_CONTEXT` or `HL_HOST_HAS_X64_CONTEXT` and needs a host-neutral counterpart.
6. **Build and CI.** `HL_HOST_ARCH`, `hostCPUs`, and `HL_CI_HOSTS` / `HL_CI_COMPAT_HOSTS` in
   `cmake/CiLanes.cmake`. DOCS.md section 11 gives the order to touch them in.

Not a seam, despite appearances: `src/core/target/dual.c` routes the legacy launcher's untyped
`hl_run_linux_guest` to `hl_aarch64_run_linux_guest`. That is **guest**-ISA routing and it needed
nothing here — but its comment calls that the "native AArch64 default", and reading "native" as
the host CPU rather than the default guest ISA is exactly the conflation this section exists to
prevent.

### 4.3 Where the detail lives

- `docs/amd64-host.md` — the host-CPU seam, the (host CPU × guest ISA) matrix, and why each cell
  starts as an interpreter before a transliterator.
- `docs/amd64-host-findings.md` — the defects and the traps. 3.11 (the non-PIE
  address-materialisation obligation) and 3.7 (`visibility("hidden")` is not local linkage) are
  the two that bite a new backend first.
- `docs/amd64-host-architecture.md` — the proposed cleanups, with risk and sequencing.
