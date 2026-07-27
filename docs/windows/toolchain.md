# The Windows host toolchain

The verified build environment for the Windows host backend, and the gate that says it is viable.
This document records what was *measured*, not what is expected to work.

Branch: `feat/windows-amd64`, based on `feat/amd64-linux-host`.

## 1. Why mingw-w64 clang and not MSVC

The engine source is GNU C. Three constructs are load-bearing and MSVC rejects all three:

| Construct | Sites on this branch | Used for |
| --- | --- | --- |
| `__attribute__((constructor))` | 11 | Production backend self-registration — `src/core/lifecycle.c` compiled once per guest ISA calls `hl_target_register_backend()` from a constructor (`docs/arch.md` §2) |
| file-scope `__asm__` | 3 | The `run_block` / `block_return` entry trampolines (`src/core/dispatch.c`, `src/translator/guest/x86_64/translate.c`) |
| `__uint128_t` | 5 files | AArch64 vector-register accessors, incl. `hl_host_uc_vregs` in `src/host/native_context.h` |

Making these MSVC-clean is a source-wide change well outside `src/host/windows/`, so it is not phase-1
work. mingw-w64 clang accepts them and still emits a native PE binary against the UCRT — no POSIX
emulation layer, no runtime DLL dependency of the Cygwin kind.

Cygwin was rejected as the *build* target for the same reason it is valuable as *prior art*: it solves
the problem by not being a native Windows port. It remains the reference for how `fork()` can be done
(`docs/windows/prior-art-cygwin-fork.md`).

## 2. Verified environment

Installed on the development host 2026-07-27:

| Component | Version / location |
| --- | --- |
| MSYS2 | `C:\msys64` |
| clang | 22.1.8, target `x86_64-w64-windows-gnu` (`C:\msys64\clang64\bin\clang.exe`) |
| ld.lld | present, same prefix |
| CMake | `C:\Program Files\CMake` |
| Ninja | winget-installed |
| rustup / cargo | `C:\Users\hutta\.cargo` |
| Rust targets | `x86_64-pc-windows-msvc` (**default host**), `x86_64-pc-windows-gnu` |

Note the Rust default host triple is **MSVC**, while the engine archive will be **GNU**. Static archives
are not ABI-compatible across that boundary. That is the crate's central problem, not the engine's, and
it is settled in `docs/windows/rust-crate.md`.

## 3. The viability gate

`tools/windows/toolchain_probe.c` compiles the three GNU constructs above plus the two Win32 mechanisms
the port cannot proceed without. It is built with the engine's own warning set, unmodified:

```sh
clang -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
      -Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration \
      -D_WIN32_WINNT=0x0A00 tools/windows/toolchain_probe.c -o probe.exe -lmincore
```

`-fvisibility=hidden` is omitted deliberately: it is an ELF concept and a no-op on PE, where symbol
export is governed by `dllexport`/`--export-all-symbols`. Resolving what replaces it in
`hl_engine_cflags` belongs to `docs/windows/build-system.md`.

Result — **compiles with zero warnings**, and at run time:

```
constructor attribute        : ok
file-scope __asm__ trampoline: ok
__uint128_t                  : ok
VEH fault + resume           : ok
W^X dual-alias JIT mapping   : ok

TOOLCHAIN VIABLE
```

### 3.1 What the last two lines actually prove

These are the two results that carry real weight; the first three were expected.

**VEH fault + resume.** The probe installs `AddVectoredExceptionHandler`, stores to a `PAGE_READONLY`
page, and the handler makes the page writable and returns `EXCEPTION_CONTINUE_EXECUTION`. The store then
completes and the written value is observable. This is the Windows analogue of mutating a `ucontext_t`
and returning from a POSIX signal handler, which is exactly what `sigframe_resume_dispatch`
(`src/core/target/{aarch64,x86_64}.c`) does today. It establishes that guest memory faults and JIT
write-protect faults have a Windows delivery path. It does **not** yet establish that the handler can
meet the async-safety contract `repair_signal_page` demands (`include/hl/host_services.h`: no userspace
allocation, locks, logging, ownership registries, or errno-dependent decisions) — see
`docs/windows/signals-and-faults.md`.

**W^X dual-alias JIT mapping.** The probe reserves an 8K placeholder with `VirtualAlloc2`
(`MEM_RESERVE_PLACEHOLDER`), splits it with `VirtualFree(MEM_PRESERVE_PLACEHOLDER)`, maps one section
twice with `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` — one view `PAGE_READWRITE`, one `PAGE_EXECUTE_READ`
— writes machine code through the RW view, `FlushInstructionCache`s, and calls it through the RX view.
It returns the right answer.

This matters because it is the direct equivalent of the macOS backend's `MAP_JIT` dual-alias
arrangement, which is already a first-class concept in the host contract: `hl_host_code_mapping` carries
both `writable_address` and `executable_address`, and `HL_HOST_CODE_DUAL_ALIAS` already exists as a flag.
So the Windows backend can satisfy `reserve_code` with the *same* shape the contract was designed
around, rather than needing a new one. `begin_code_write`/`end_code_write` — the per-thread W^X gate —
may then legitimately no-op on Windows, which the contract explicitly permits ("Dual-alias hosts may
no-op").

`VirtualAlloc2` and `MapViewOfFile3` require Windows 10 1803 or later and link against `mincore`.
That sets the minimum supported host.

## 4. What this does not prove

The probe is a floor, not a ceiling. Untouched by it, and each owned by another document:

- `fork()` — the port's hardest blocker. `docs/windows/fork-model.md`, `docs/windows/prior-art-cygwin-fork.md`.
- Whether a VEH handler can be made async-safe enough for `repair_signal_page`. `docs/windows/signals-and-faults.md`.
- The readiness-vs-completion mismatch for `hl_host_event_services`. `docs/windows/host-services-map.md`.
- Guest fixture acquisition: compat tests need cross-built static-glibc Linux binaries for both guest
  ISAs, and there is no nix or cross-glibc toolchain on Windows. `docs/windows/testing-and-ci.md`.
- Whether CFG and CET/shadow stacks interfere with emitted code once a real JIT runs, and whether x64
  dynamic unwind info (`RtlAddFunctionTable`) must be registered for JIT frames.

## 5. Reproducing the environment

```powershell
winget install --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements
winget install --id Kitware.CMake
winget install --id Ninja-build.Ninja
winget install --id Rustlang.Rustup

C:\msys64\usr\bin\bash.exe -lc "pacman -Syuu --noconfirm"
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed \
    mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-cmake \
    mingw-w64-clang-x86_64-ninja base-devel git"

rustup target add x86_64-pc-windows-gnu
rustup toolchain install stable-x86_64-pc-windows-gnu
```
