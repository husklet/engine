# The Windows host: build system

The plan for making this tree configure, build and test on a native Windows host (Win32/NT, mingw-w64
clang from MSYS2, host CPU x86-64). This document covers the **build system only** — toolchain, CMake
wiring, fixture supply, shell dependency, phase gating, flake. The engine-side work (`src/host/windows/`,
the translator, the ABI) is named where it bounds a build decision and otherwise left alone.

`DOCS.md` is normative. This file is a plan, not a record: nothing here has been compiled. Every claim
about *this tree* was read out of the tree and is cited by file:line. Every claim about *mingw-w64* is
marked as verified-by-search or as an assumption to check at first compile. Where I do not know, it says
so.

---

## 1. What the tree does today, before anything changes

Facts, so the plan below is not argued against a guess.

**Configure dies immediately.** `CMakeLists.txt:27` is
`find_program(HL_BASH_EXECUTABLE NAMES bash REQUIRED)`. Without `bash.exe` on `PATH` the configure fails
before a single source file is considered. The same call is repeated at `cmake/Phase3CiConfig.cmake:8`
and `cmake/LaneParity.cmake:14`.

**The host-CPU axis already works.** `CMakeLists.txt:32-40` matches `^(x86_64|amd64|AMD64)$`, and a
native Windows CMake sets `CMAKE_SYSTEM_PROCESSOR` from `PROCESSOR_ARCHITECTURE`, i.e. `AMD64`. So
`HL_HOST_ARCH` resolves to `x86_64` with no change. `src/host/host_cpu.h` keys on `__x86_64__`, which
clang-mingw defines. Nothing on the CPU axis needs touching for x86-64. (Windows-on-ARM would: three
regexes in `cmake/Phase3Gates.cmake:602,662,686` spell the ARM case `^(aarch64|arm64)$` and omit `ARM64`,
which is what a Windows ARM host reports — `CMakeLists.txt:32` accepts it, those three do not. Latent
inconsistency, not this milestone's problem.)

**Every `else()` in the tree means "Linux".** There are five, and each silently mislabels a Windows
configure:

| site | what the `else()` does on Windows |
|---|---|
| `CMakeLists.txt:45-49` | `HL_PACKAGE_ARCH_DIR` = `package/linux-x86_64` |
| `cmake/Phase4Install.cmake:27-31` | `HL_PACKAGE_HOST` = `linux`, so `hl-engine.pc` emits `-lhl-host-linux` |
| `cmake/Phase4Install.cmake:53-58` | `hl-engine-runner` links via `$<LINK_GROUP:RESCAN,...>` |
| `cmake/Phase3Units.cmake:123-127` | `_hostl` = `hl-host-linux` — **a target that does not exist**, so configure errors |
| `cmake/Phase3Compat.cmake:404-408` | engine dir = `build/linux-production`, which `Phase2Production.cmake:65` never builds |

`cmake/Phase4Install.cmake:119-124` has neither arm: `HL_INSTALL_LIBS` gets no host archive at all, while
the `.pc` file above still names one.

**What already self-excludes correctly.** `cmake/Codesign.cmake:38` returns off Darwin.
`cmake/Phase4Mac.cmake:24` returns off Darwin. `cmake/Phase2Production.cmake:65` returns off Linux.
`cmake/Phase4Install.cmake:192` guards `gate.archive-closure` to Linux. `cmake/Format.cmake:9`,
`cmake/Lint.cmake:8-17` and `cmake/RustLint.cmake:8-15` all degrade with a `STATUS` message when their
tool is absent. `cmake/GuestFixtures.cmake:74-86` returns early with no cross compilers, which in turn
means `CMakeLists.txt:218-237` never includes `Phase3Compat`, `Phase3Gates` or `Phase4Mac`. That last one
matters a lot: **a Windows configure with no `*_LINUX_CC` exported already skips the two largest and least
portable CMake files.**

**The build-time bash dependency is inert on Windows.** `cmake/ArchiveStamp.cmake:20` runs
`tools/gen_archive_stamp.sh` in an `add_custom_command`, but its output is only pulled in by
`hl_stamp_archive_object()`, called from `cmake/Phase2Production.cmake:133` (Linux) and
`cmake/Phase4Mac.cmake:116` (Darwin). Neither runs on Windows, and `hl-archive-stamp` is not an `ALL`
target, so the command is declared and never executed. This is worth knowing because it means the *only*
mandatory bash on a Windows configure is the `find_program` itself.

**Which archives can actually compile.** I audited the four Phase-1 source lists against what mingw-w64
provides. mingw-w64 ships `unistd.h`, `fcntl.h`, `sys/stat.h`, `sys/types.h`, `sys/time.h`, `dirent.h`,
`setjmp.h`, and `pthread.h`/`sched.h`/`semaphore.h` via winpthreads. It does **not** ship `sys/mman.h`,
`sys/socket.h`, `sys/un.h`, `poll.h`, `dlfcn.h`, `ucontext.h`, `sys/wait.h`, or `termios.h`
([mingw-w64 discussion](https://sourceforge.net/p/mingw-w64/mailman/message/37725974/),
[msys2/MINGW-packages#6002](https://github.com/msys2/MINGW-packages/issues/6002)).

| archive | blocking translation units | verdict |
|---|---|---|
| `hl-translator` (`IR_SOURCES`) | none — only `<setjmp.h>` (`guest/x86_64/avx.c`) and `<sys/time.h>` (`guest/x86_64/legacy.c`) | **compiles, probably unmodified** |
| `hl-host-fake` (`FAKE_HOST_SOURCES`) | none — `src/host/fake/host.c` includes only `<string.h>` and `<sched.h>` | **compiles, probably unmodified** |
| `hl-engine` (`CORE_SOURCES`) | `src/core/checkpoint_channel.c` (`poll.h`, `sys/mman.h`, `sys/socket.h`), `src/core/provider/demux.c` (`sys/mman.h`, `sys/socket.h`, `signal.h`) | 2 of 20 files block |
| `hl-linux-abi` (`LINUX_ABI_SOURCES`) | `container/vfs/gmap.c`, `logical_vma.c`, `fdcache.c` (`sys/mman.h`); `dns.c` (`dlfcn.h`) | 4 of 31 files block |

The heavy POSIX (`sys/shm.h`, `sys/msg.h`, `sys/sem.h`, `sys/prctl.h`, `sys/syscall.h`, `termios.h`,
`ucontext.h`) is **not** in these lists. It is in `src/linux_abi/syscall/*.c`, which reach the build only
through the Phase-2 unity TU `src/core/target/x86_64.c`. That layering is the single most useful fact in
this document: it is why a Windows build can produce real artifacts long before anything can run a guest.

---

## 2. Toolchain

### 2.1 Environment: UCRT, and clang from MSYS2

Use **UCRT**, not msvcrt. MSYS2 itself now recommends `ucrt64`/`clang64` over the legacy `mingw64`
environment; msvcrt is not C99-clean and UCRT ships in-box on Windows 10+
([MSYS2 environments](https://www.msys2.org/docs/environments/)). UCRT and msvcrt binaries cannot be
mixed, so this is a whole-project commitment, made once.

Either `CLANG64` (clang + lld + compiler-rt + libunwind, UCRT-based) or `UCRT64` with
`mingw-w64-ucrt-x86_64-clang` installed alongside gcc will do. I recommend **CLANG64**: it is a single
coherent LLVM toolchain, the project is C-only so the libc++/libstdc++ difference is irrelevant, and
`lld-link`'s diagnostics on the archive-recursion problems this tree has (see §2.4) are better than
`ld.bfd`'s. **Unverified**: whether `$<LINK_GROUP:RESCAN>` (used at `Phase4Install.cmake:57` and
`Phase3Units.cmake:96-98`) is supported by CMake for the GNU-driver-on-Windows link line. It maps to
`--start-group`/`--end-group`, which mingw-w64's `ld` documents, but CMake's feature table is per
`CMAKE_C_COMPILER_ID`/platform and I have not confirmed it registers `RESCAN` here. **Check this at
milestone M5**; if it does not, the Darwin branch's technique (repeat `$<TARGET_FILE:...>` twice) is the
fallback and is already written at `Phase3Units.cmake:80-91`.

### 2.2 Which of `hl_engine_cflags` survives

`CMakeLists.txt:93-99`:

| flag | on mingw-w64 clang | action |
|---|---|---|
| `-O2 -g` | fine (`-g` emits DWARF in the PE; `gdb`/`lldb` read it) | keep |
| `-std=c11` | fine | keep |
| `-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes` | fine | keep |
| `-Wpedantic` | fine on this tree's own code; **noisy** once `windows.h` is included (Microsoft's headers use anonymous structs/unions and `__int64`) | keep, expect noise in `src/host/windows/` only |
| `-Wconversion` | fine, but very noisy against Win32 types (`DWORD`/`SIZE_T`/`HANDLE` round-trips) | keep; the noise is a feature, this tree treats conversions seriously |
| `-Werror=implicit-function-declaration -Werror=implicit-int` | fine | keep |
| `-fvisibility=hidden` | **drop on Windows** | see below |

**`-fvisibility=hidden` must go.** ELF/Mach-O visibility has no PE/COFF equivalent. GCC-mingw warns
`visibility attribute not supported in this configuration; ignored`; clang-mingw generally accepts it
silently but it does nothing for a static archive or an executable. Windows exports are per-symbol
(`__declspec(dllexport)`) or by a `.def` file, and are only meaningful at a DLL boundary
([MaskRay, PE/COFF linker notes](https://maskray.me/blog/2023-12-03-linker-notes-on-pe-coff)).

**Nothing replaces it, and specifically: do not introduce dllexport/dllimport.** This tree builds static
archives and executables — `add_library(... STATIC ...)` at `CMakeLists.txt:182-185`, no `SHARED`
anywhere. There is no export macro in `include/hl/`, and adding one is a public-ABI decision, not a
Windows port detail. If a `hl-engine.dll` is ever wanted, that is a separate design with a generated
`HL_API` macro and `WINDOWS_EXPORT_ALL_SYMBOLS` or an explicit export list. Not now.

The same flag list is duplicated at `cmake/Lint.cmake:22-27` for the `hl_lint` host tool and needs the
same treatment, or `hl_lint` will emit a warning per TU on every Windows configure with `-DHL_LINT=ON`.

### 2.3 `-municode`: no

`-municode` switches the entry point to `wmain`/`wWinMain` and requires the source to define `wmain`.
`src/runner/main.c` and every `tools/*_runner.c` define `int main(int, char **)`. Adopting `-municode`
means editing all of them and threading `wchar_t **` through code that is otherwise byte-oriented, which
is a large and irreversible change to portable files.

The problem `-municode` solves — non-ASCII paths in `argv` — is better solved by the **UTF-8 active code
page manifest**: an application manifest with `<activeCodePage>UTF-8</activeCodePage>` makes the ANSI
code page UTF-8 process-wide on Windows 10 1903+, so `main`'s `argv`, `fopen`, and every `-A` Win32 call
become UTF-8. That is a linker input (a `.rc`/manifest), not a source change, and it keeps one `main`
signature across all three hosts.

**Unverified**: whether the mingw-w64 CRT startup (`__getmainargs`) honours the manifest's active code
page when constructing `argv`. It builds `argv` from `GetCommandLineA` semantics, so it *should*, but I
have not confirmed it and it is the kind of thing that silently doesn't. **Test it explicitly at M5** with
a path containing a non-ASCII character; if it fails, the fallback is `GetCommandLineW` +
`CommandLineToArgvW` + `WideCharToMultiByte(CP_UTF8, ...)` inside one Windows-only shim called from
`main`, which is still smaller than `-municode`.

### 2.4 Static runtime

Link `-static`. mingw-w64's C runtime is the OS's UCRT either way; what `-static` removes is the
dependency on `libwinpthread-1.dll` (and `libunwind`/`libc++` under CLANG64). Do not use
`-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic`: it is documented as unreliable because the flag ordering is
overridden by later implicit dependencies
([mingw-w64 discussion](https://sourceforge.net/p/mingw-w64/discussion/723797/thread/d3d2068e/)).

This is not cosmetic here. The test harness copies binaries into scratch directories and runs them from
varying working directories (`Phase3Gates.cmake:529,539` do exactly this on Linux), and Windows resolves
DLLs relative to the executable and the current directory. A DLL-dependent test binary that works in the
build tree and fails after a copy is a failure mode this tree should not acquire.

### 2.5 Proposed `cmake/toolchains/x86_64-windows.cmake`

Two design points first.

**The compiler-flag delta does not belong in the toolchain file.** `hl_engine_cflags` is an INTERFACE
library (`CMakeLists.txt:91-99`) and a toolchain file cannot subtract from it. The `-fvisibility=hidden`
removal has to be a branch in `CMakeLists.txt`. That is consistent with the file's own reasoning at
`CMakeLists.txt:80-85` — flags live in one place — so the toolchain file stays minimal and the flag
change is a three-line edit there.

**The toolchain file is optional for a native build.** `DOCS.md` §7.0 says so for Linux and Darwin, and
the same holds here: inside a CLANG64 shell, `clang` on `PATH` *is* the intended host compiler and a bare
`cmake -G Ninja -B build-win` works. The file exists to pin the compiler explicitly (for a CI runner whose
`PATH` also has MSVC or a Chocolatey LLVM), and to hold the link-line decisions of §2.4.

Nix is the toolchain authority for the Linux and Darwin toolchain files because nix supplies those
compilers. It supplies nothing here, so this file reads a plain optional environment variable and falls
back to `PATH` rather than pretending to a nix contract it does not have. That asymmetry is honest and
should be stated in the file's header comment.

```cmake
# CMake toolchain file — x86-64 Windows (mingw-w64 clang, MSYS2 UCRT/CLANG64).
#
# NOT a nix-authored toolchain, unlike its two siblings. Nix does not run
# natively on Windows and supplies no compiler here, so this file reads an
# optional $WINDOWS_X86_64_CC and otherwise takes clang from PATH. Inside an
# MSYS2 CLANG64 or UCRT64 shell no toolchain file is needed at all; this one
# exists to pin the compiler when PATH also carries MSVC or a stray LLVM, and to
# hold the static-runtime link decision.
#
# Usage (MSYS2 CLANG64 shell):
#     cmake -G Ninja -B build-win \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-windows.cmake
#     ninja -C build-win

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT "$ENV{WINDOWS_X86_64_CC}" STREQUAL "")
  separate_arguments(_hl_cc UNIX_COMMAND "$ENV{WINDOWS_X86_64_CC}")
  list(POP_FRONT _hl_cc CMAKE_C_COMPILER)
  if(_hl_cc)
    string(JOIN " " _hl_cc_extra ${_hl_cc})
    set(CMAKE_C_FLAGS_INIT "${_hl_cc_extra}")
  endif()
else()
  find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
endif()

# Static runtime: no libwinpthread-1.dll / libunwind DLL beside the binaries.
# The test harness copies executables into scratch directories and runs them
# from elsewhere; Windows resolves DLLs relative to the image and the cwd, so a
# dynamic link turns a copy into a load failure.  -static rather than
# -Wl,-Bstatic -lwinpthread, whose ordering is overridden by later implicit deps.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Windows 10 1903+ UTF-8 active code page, so main()'s argv and every -A call
# are UTF-8 without -municode and without a wmain in portable sources.
# (Verify at M5 that the mingw CRT honours it when building argv.)
# set(CMAKE_RC_COMPILER ...) / the manifest is attached per-target, not here.

# This file is only ever the NATIVE one: a Windows host cross-compiled from
# elsewhere is not a supported configuration, so nothing restricts find_root.
```

Deliberately absent: `CMAKE_FIND_ROOT_PATH_MODE_*` (this is never a cross file);
`CMAKE_EXECUTABLE_SUFFIX` (CMake sets `.exe` itself); `-fvisibility=hidden` removal (belongs in
`CMakeLists.txt`); `-municode` (§2.3).

---

## 3. Host backend wiring

### 3.1 The new arm

`CMakeLists.txt:192-200` gains a third arm:

```cmake
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_library(hl-host-windows STATIC ${WINDOWS_HOST_SOURCES})
  target_link_libraries(hl-host-windows PRIVATE hl_engine_cflags)
```

with `WINDOWS_HOST_SOURCES` mirroring the other two:
`src/host/windows/{directory,host,process,range,system}.c`.

`src/host/windows/` exists and contains only a `README.md` reserving the boundary. For scale: the two
existing backends are `src/host/linux/host.c` at 3961 lines and `src/host/macos/host.c` at 4839 lines,
implementing 14 service groups (`include/hl/host_services.h`). A Windows backend is comparable work and
is not a build-system task.

`src/host/native_context.h` currently `#error`s on any (OS, CPU) pair it does not know, and its four arms
are Darwin/Linux × aarch64/x86-64. A Windows arm is required before `hl-host-windows` or anything
including it will compile. On Windows there is no `ucontext_t`; the equivalent is
`EXCEPTION_POINTERS->ContextRecord` (`CONTEXT.Rip`, `CONTEXT.Rsp`, `CONTEXT.Xmm0..15`) delivered by a
vectored exception handler, so `HL_HOST_UC_PC`/`HL_HOST_UC_SP` need a differently-typed argument. That is
a header-shape question the engine agents should own; noted here because it blocks compilation.

### 3.2 `COMMON_HOST_SOURCES` audit

This is the part of the task where the current grouping is actually wrong, and saying so precisely is the
point. `CMakeLists.txt:167-178` defines:

- `COMMON_HOST_SOURCES` = `child.c fork_wire.c private.c range.c resolve.c sync.c`
- `LINUX_HOST_SOURCES` = the Linux files **plus** `clock.c file.c` **plus** `COMMON_HOST_SOURCES`
- `MACOS_HOST_SOURCES` = the macOS files only; the Darwin `add_library` at `CMakeLists.txt:196-198`
  re-adds `COMMON_HOST_SOURCES` **and** `clock.c file.c` at the call site

So `clock.c` and `file.c` are already common to both hosts but are spelled twice, once inside a list and
once at a call site. Any Windows arm inherits that asymmetry unless it is fixed. Fix it: they belong in
`COMMON_HOST_SOURCES`.

File by file:

| file | what it actually uses | verdict on Windows |
|---|---|---|
| `range.c` | nothing but `hl_host_page_size()` / `hl_host_address_mapped()`, both per-OS | **portable, unchanged.** Its two callees live in `src/host/<os>/range.c`; Windows supplies `GetSystemInfo().dwPageSize` and `VirtualQuery` |
| `clock.c` | the `services->clock` vtable and `struct timespec` | **portable, unchanged.** `struct timespec` is C11 and present in mingw's `<time.h>` |
| `file.c` | the `services->file` vtable only; `errno` for the return convention | **portable, unchanged** |
| `sync.c` | `pthread_mutex_*`, `PTHREAD_MUTEX_ERRORCHECK` | **portable via winpthreads**, with one semantic hole: `hl_host_sync_fork_prepare`/`_complete` (lines 198-207) exist to hold the registry lock across `fork()`. Windows has no fork. Keep the file common; the fork pair becomes a documented no-op or a typed unsupported. Do **not** fork the file for this |
| `resolve.c` | `openat`, `fstatat`, `readlinkat`, `O_DIRECTORY`, `O_NOFOLLOW`, `AT_SYMLINK_NOFOLLOW`, `S_ISLNK` | **needs a Windows sibling.** mingw has no `*at()` family. Do not split this into portable + per-OS halves: the loop looks portable but every primitive in it is the security property. The contract is "resolve beneath a pinned root, never escape, follow ≤40 links" (§4 of DOCS.md), which on NT means handle-relative opens (`NtCreateFile` with `RootDirectory`, or `CreateFileW` + `FILE_FLAG_OPEN_REPARSE_POINT` + `GetFinalPathNameByHandleW` and an explicit prefix check). Reimplement, do not port |
| `child.c` | `pipe()`, `fcntl(F_SETFL/F_SETFD)`, `sigaction(SIGCHLD)` | **needs a Windows sibling.** There is no `SIGCHLD`. The natural shape is `RegisterWaitForSingleObject` on each child `HANDLE` signalling a manual-reset event. But `child.h` exposes `hl_host_child_watch_descriptor()` returning an `int` fd, which is a POSIX shape leaking into the portable interface; a Windows arm needs either a widened handle type or a second accessor. **This is an interface change, not just a port** — flag it early |
| `fork_wire.c` | `AF_UNIX` + `sendmsg`/`recvmsg` + `SCM_RIGHTS` | **absent on Windows, at least initially.** Windows 10 1803+ has `AF_UNIX` for `SOCK_STREAM`, but there is **no `SCM_RIGHTS`**: handle passing is `DuplicateHandle` with the peer's PID, a different protocol with a different trust model. This file serves the fork-server, which exists to serve *guest* `fork()`. Guest fork semantics are owned by the Linux ABI, not the host, and what a Windows host does about guest fork is unsettled. Recommend: no Windows sibling until that is settled |
| `private.c` | `mmap(MAP_SHARED|MAP_ANONYMOUS)` for a cross-process table, `getpid`, `pthread_mutex`, `RLIMIT_NOFILE` | **needs a Windows sibling, whose design is downstream of the process model.** Its purpose is keeping engine-private descriptors out of the guest's fd namespace *across fork*. The shared anonymous mapping survives fork by inheritance; the Windows analogue is `CreateFileMapping(INVALID_HANDLE_VALUE)` plus inheritable handles — but with no fork there is nothing to inherit across. Do not write this until §3.1's process model exists |

**Recommended re-split** (a `CMakeLists.txt` edit; no source moves):

```cmake
# Host-neutral: pure vtable plumbing and page arithmetic. Every host.
set(HOST_PORTABLE_SOURCES
  src/host/clock.c src/host/file.c src/host/range.c src/host/sync.c)

# POSIX-flavoured: descriptor-passing, SIGCHLD, *at() resolution, the
# fork-inherited private-fd table. Linux and macOS only; Windows supplies
# siblings under src/host/windows/ or omits the capability.
set(HOST_POSIX_SOURCES
  src/host/child.c src/host/fork_wire.c src/host/private.c src/host/resolve.c)
```

with `LINUX_HOST_SOURCES` and `MACOS_HOST_SOURCES` taking both, `WINDOWS_HOST_SOURCES` taking
`HOST_PORTABLE_SOURCES` plus `src/host/windows/{child,private,resolve}.c` as they appear. `cmake/Format.cmake:19-21`
names `COMMON_HOST_SOURCES` and must be updated in the same edit or four files silently stop being
format-checked.

Note that `COMMON_HOST_SOURCES` is also swept by `cmake/ArchiveSources.cmake:10` (`src/host/*.c` glob), so
the crate-archive manifest is unaffected by the re-split. Good — that is one fewer coupling to reason about.

---

## 4. Guest fixtures

This gates every compat test and there is no comfortable answer. Treating it seriously means starting with
what the problem actually is.

### 4.1 The problem, stated exactly

`cmake/GuestFixtures.cmake` cross-compiles Linux ELF guest programs for **both** guest ISAs, statically
linked against glibc (`flake.nix:87`, `staticCCBase = "${ccFor g} -L${(pkgsFor g).glibc.static}/lib"`),
some against a static sqlite (`flake.nix:84-89`). The compilers come from `$AARCH64_LINUX_STATIC_CC` /
`$X86_64_LINUX_STATIC_CC`, exported by the nix devShell. The corpus is **1621 `.c` files under
`tests/compat/` alone**, each built per ISA, plus e2e, perf, soak and checkpoint fixtures.

On a Windows host: no nix, and no MSYS2 package supplying an `aarch64-linux-gnu` or `x86_64-linux-gnu`
cross-gcc with a static glibc. (**Unverified in the strong sense**: I did not exhaustively search for a
prebuilt Windows-hosted Linux cross toolchain. Standalone tarballs exist — Bootlin, Linaro — but nothing
guarantees they produce the *same* glibc, and a fixture corpus whose libc differs from CI's is a corpus
that produces different goldens. Treat "no equivalent toolchain" as the working assumption and, if
someone wants to overturn it, the bar is byte-identical fixture output against the nix build, not "it
compiled".)

### 4.2 Options

**(a) Do nothing; fixtures off.** `cmake/GuestFixtures.cmake:74-86` already returns early with a `STATUS`
message when the compilers are absent, and `CMakeLists.txt:218` then skips Phase3Compat/Phase3Gates/
Phase4Mac entirely. A Windows configure today is *already* a valid no-fixture configure. Cost: zero. Buys:
the whole build-system milestone chain M0-M6. **This is correct for now and should be stated as the
supported Windows configuration rather than treated as a degradation.**

**(b) Commit prebuilt fixture binaries.** Rejected on volume. ~3200 static-glibc binaries at roughly
0.8-1 MB each is several gigabytes in git. The *mechanism* exists in this tree — `pkgs/rust/assets/lib/`
commits prebuilt `libhl-engine.a` per triple, with `PROVENANCE.md`, a refresh script
(`tools/refresh_crate_archives.sh`) and a currency gate (`tools/check_crate_archives.sh`) — so the pattern
is proven. The volume is three orders of magnitude off.

**(c) WSL2.** A real Linux kernel on the same machine, so the nix devShell runs and produces
byte-identical fixtures. Cost: path translation between `\\wsl$\...` and Windows-native CMake, and the
Windows build is no longer self-contained. Good as a **developer convenience**; unsuitable as a CI gate
because it makes the Windows lane depend on a Linux userland the workflow has to install and warm.

**(d) A container step on the Windows host** (Docker Desktop with Linux containers, or podman). Same
fixtures, reproducible, no second machine. Cost: a hypervisor dependency and a several-GB image; slow
first run. Also a developer convenience, not a gate.

**(e) Build the corpus once on a Linux runner, cache and publish it as an artifact.** The Windows
configure fetches and unpacks a tarball instead of compiling anything.

### 4.3 Recommendation

**(a) now, (e) as the production answer, (c)/(d) as documented developer conveniences, (b) rejected.**

The argument for (e) over the rest is that this tree has already built and debugged exactly this
machinery for a different artifact. `cmake/ArchiveSources.cmake` owns a source closure *in CMake*;
`tools/crate_archive_manifest.sh` reduces it to one SHA-256; `cmake/ArchiveStamp.cmake` force-includes
that digest into every shipped object so a stale artifact is discoverable from the artifact itself; and
`tools/check_crate_archives.sh` is the gate that fails when the committed thing no longer matches the
sources. The same four pieces, pointed at `tests/**` plus `flake.lock`, give a fixture archive that
**cannot be silently stale** — which is the only property that matters, because a stale fixture corpus
does not fail loudly, it produces a subtly different golden.

Concretely:

1. A `fixtures` job on the existing `linux.yml` runner builds the full corpus inside `nix develop` and
   tars it as `guest-fixtures-<digest>.tar.zst`, where `<digest>` is the SHA-256 of the sorted content of
   the fixture source closure plus `flake.lock` (the lock is what pins glibc and sqlite).
2. `cmake/GuestFixtures.cmake` grows a third mode beside "compilers present" and "compilers absent":
   `-DHL_GUEST_FIXTURE_ARCHIVE=<path-or-url>`. It unpacks into the same output layout
   `hl_guest_binary()` would have produced, registers the same `HL_GUEST_ALL_OUTPUTS`, and everything
   downstream is unchanged.
3. The digest is checked on unpack. A mismatch is a `FATAL_ERROR`, not a warning.
4. `HL_HAVE_GUEST_CC` is renamed or joined by `HL_HAVE_GUEST_FIXTURES`, since the condition
   `CMakeLists.txt:218` actually wants is "fixtures exist", not "a compiler exists". That rename mirrors
   `canRunGuests` → `hasCrateArchive` in `flake.nix`, done for exactly this reason (`docs/amd64-host.md`
   §10) — two ideas that were only accidentally the same.

**And the honest caveat, which is large.** Fixtures are necessary and nowhere near sufficient. Even with a
perfect corpus, a Windows compat run needs (i) a Windows production engine, which Phase 2 does not build
off Linux (`Phase2Production.cmake:65`), (ii) `matrix_runner.c` and its ~10 sibling runners, which between
them carry over 100 `fork`/`execvp`/`waitpid`/`setsid`/`setrlimit`/`AF_UNIX` call sites, and (iii) the
`execlp("env", ...)` bridge contract (`tools/e2e_runner.c:95`, `tools/config_e2e_runner.c:223`,
`tools/matrix_runner.c:862`) that `Phase3Compat.cmake:403` and six sites in `Phase3Gates.cmake` depend on.
**Do not do the fixture work before the engine work.** It would produce a corpus nothing can consume.

---

## 5. The bash dependency

### 5.1 Enumeration

15 scripts under `tools/`, 1720 lines. What matters is which of them a *Windows* configure actually
reaches.

**Configure-time, unconditional (blocks everything):**
`CMakeLists.txt:27`, `cmake/Phase3CiConfig.cmake:8`, `cmake/LaneParity.cmake:14` — three
`find_program(... bash REQUIRED)`.

**Build-time:** `cmake/ArchiveStamp.cmake:20` → `tools/gen_archive_stamp.sh` → `tools/crate_archive_manifest.sh`.
**Inert on Windows** for the reason given in §1.

**Test-time on the Windows configuration that will actually exist** (`HL_BUILD_TESTS=ON`, no guest CC):

| test | script | reachable on Windows? |
|---|---|---|
| `unit.ci-workflow-invariants`, `unit.publish-gating` | `tools/check_ci_workflows.sh` | **yes** — `Phase3CiConfig.cmake:10-20`, no host guard |
| `gate.archive-closure` | `tools/check_archive_closure.sh` | no — `Phase4Install.cmake:192` guards to Linux |
| `gate.ci-lane-parity` | `tools/check_lane_parity.sh` | no — `CMakeLists.txt:246` requires `HL_HAVE_GUEST_CC` |
| `compat-launch.*` | `tools/run_direct_launch.sh` | no — Phase4Mac, Darwin only |
| `nested.*`, `emulated.*`, `checkpoint-cross.*`, `isa-fuzz.*` | four gate scripts | no — `Phase3Gates.cmake:143` guards the whole file to Linux, and Phase3Gates is not included without guest CC |

**Custom targets:** `check-crate-archives`, `refresh-crate-archives{,-linux,-darwin,-provenance}`
(`CMakeLists.txt:264-287`) and `perf-macos-remote` (Linux-guarded). These are release/developer targets;
they are declared on Windows but nobody would invoke them, and `tools/refresh_crate_archives.sh:127-132`
refuses a non-Linux `uname -s` anyway.

So the *real* Windows bash surface is: **one `find_program` and one script**
(`tools/check_ci_workflows.sh`), and that script is deliberately POSIX `sh` with no bashisms and already
strips CR (`check_ci_workflows.sh:46`). It processes `.github/workflows/*.yml` and touches nothing
host-specific.

Two latent defects found while enumerating, both worth fixing regardless of Windows:

- `cmake/Phase3Gates.cmake:678,688,695` register `tests/fuzz/isa/*/run.sh` as a CTest `COMMAND`
  **directly**, relying on the shebang. Everything else in the tree routes through `HL_BASH_EXECUTABLE`
  precisely because a nix sandbox has no `/usr/bin/env` (`Phase3CiConfig.cmake:4-7`). These three are
  inconsistent with that, and on Windows `CreateProcess` cannot execute a `.sh` at all.
- `tools/gen_archive_stamp.sh:27` invokes a hardcoded `bash` from `PATH` rather than the configured
  `HL_BASH_EXECUTABLE`. Same class of bug.
- `tools/validate_crate_archive.sh:37-50` dispatches on `uname -s` knowing only `Darwin` and `Linux`.
  Under MSYS2 `uname -s` returns `MINGW64_NT-…`, matching neither, so `defer_reason` stays empty and it
  falls through to the native ELF path with GNU `nm --print-armap` and `aarch64-linux-gnu-gcc`. It needs
  an explicit third branch, or it will misbehave rather than refuse.

### 5.2 Policy: recommend a hybrid, in this order

**Do not port everything to CMake script mode, and do not port anything to PowerShell.**

PowerShell is rejected outright: it adds a third shell language to a project whose CI runs on Linux and
macOS, so every ported script would need a bash twin or the CI would stop running it. That is worse than
the problem.

Whole-scale CMake script mode is rejected on cost/benefit: `check_ci_workflows.sh` is 397 lines carrying a
large awk program implementing 21 named invariants, and rewriting it in `string(REGEX ...)` buys nothing
that anyone will ever run on Windows — its subject is `.github/workflows/*.yml`, which has no Windows
content until a `windows.yml` exists.

The recommendation, in dependency order:

1. **Make `HL_BASH_EXECUTABLE` optional and gate on it.** Replace the three `REQUIRED` `find_program`
   calls with one non-required call in `CMakeLists.txt` (delete the two duplicates), and wrap each
   bash-backed target/test in `if(HL_BASH_EXECUTABLE)` with a `message(STATUS ...)` naming what is not
   registered when it is absent. This follows the pattern `Format.cmake:11-14`, `Lint.cmake:14-17` and
   `RustLint.cmake:14-18` already use for clang-format, the linter and cargo. **This one edit unblocks the
   entire Windows configure.**
2. **Port the two build-graph scripts to CMake script mode**: `tools/crate_archive_manifest.sh` (54 lines)
   and `tools/gen_archive_stamp.sh` (44 lines). They are small, they are the only build-time shell
   dependency on *any* host, and the work they do is already half-CMake — the source list comes from
   `cmake/ArchiveSources.cmake` via `tools/export_archive_sources.cmake`, and the shell contributes only
   `sha256sum` + `sort` + a change-guarded write, all of which are `file(SHA256)`, `list(SORT)` and
   `configure_file`/`file(GENERATE)`. This removes a build-time external dependency for Linux and macOS
   too, which is the honest justification; the Windows benefit is incidental.
3. **Leave the CI-metadata gates as bash, prefer-when-present.** `check_ci_workflows.sh` and
   `check_lane_parity.sh` stay bash and are registered only when `HL_BASH_EXECUTABLE` was found. On a
   developer's MSYS2 shell bash is present and they run; on a bare Windows box they are skipped with a
   message. They lose nothing, because CI runs them on Linux and macOS on every push.
4. **Fix `Phase3Gates.cmake:678,688,695`** to prefix `${HL_BASH_EXECUTABLE}`, and
   `tools/gen_archive_stamp.sh:27` to take the interpreter as an argument. Correctness fixes that happen
   to help.
5. **Do not port the execution gates** (`nested_engine_gate.sh`, `emulated_aarch64_gate.sh`,
   `checkpoint_cross_gate.sh`, `tests/fuzz/isa/*/run.sh`). They are not shell problems: they need
   `qemu-user`, generated `#!/bin/sh` engine shims made executable with `chmod +x`, and Linux guest
   engines. Porting the shell would leave every one of those requirements intact.

The net policy statement for `DOCS.md` §7: *"MSYS2 bash is recommended on a Windows host and is required
to run the CI-metadata gates; it is not required to configure or build."*

### 5.3 The MSYS2 path-mangling trap

If bash is present, MSYS2's bash rewrites arguments that look like POSIX paths when invoking a *native*
Windows binary. `check_ci_workflows.sh` takes only the literal arguments `invariants` / `publish-gate`
and is safe. `check_lane_parity.sh` takes four, including `${CMAKE_CTEST_COMMAND}` and
`${CMAKE_BINARY_DIR}` (`LaneParity.cmake:21-24`), and would need `MSYS2_ARG_CONV_EXCL` handling if it is
ever registered on Windows. It is not registered today (guarded by `HL_HAVE_GUEST_CC`); note it so it is
not discovered the hard way later.

---

## 6. Phase gating

### 6.1 Must be gated OFF on Windows

| file | current guard | change |
|---|---|---|
| `cmake/Phase4Mac.cmake` | `:24` returns off Darwin | none needed |
| `cmake/Codesign.cmake` | `:38` no-op off Darwin | none needed |
| `cmake/Phase2Production.cmake` | `:65` returns off Linux | none needed **for now** (§6.3) |
| `gate.archive-closure` | `Phase4Install.cmake:192` Linux-only | none needed. GNU `nm --defined-only/--undefined-only` output and a probe link with `-pthread -ldl -latomic` (`check_archive_closure.sh:81`) — none of those three libs exist on mingw |
| `perf-macos-remote` | `CMakeLists.txt:253` Linux-only | none needed |
| `cmake/Phase3Compat.cmake`, `cmake/Phase3Gates.cmake` | reached only when `HL_HAVE_GUEST_CC` | none needed today; see the warning below |
| `cmake/LaneParity.cmake` | `CMakeLists.txt:246` requires `HL_HAVE_GUEST_CC` | none needed today. `check_lane_parity.sh` `exit 2`s on an OS token absent from `HL_CI_HOSTS`, and there is no `Windows-x86_64` token |

**Warning, load-bearing.** `Phase3Gates.cmake:927-948` raises `FATAL_ERROR` if any of six test-name
patterns matches zero registered tests. It sits inside the `Linux` guard at `:143` so it cannot fire
today. If anyone relaxes that guard to admit Windows without also registering those lanes, **configure
hard-fails**. Same shape at `Phase3Compat.cmake:487-490`, where a fixture-path prefix match
(`string(FIND "${_output}" "${_fixture_dir}/" _prefix)`) is a literal string comparison that a
path-separator difference would empty, and at `Phase3Compat.cmake:426-431` for the timeout scale.
Phase3Compat is otherwise **almost entirely unguarded**, so if guest fixtures ever do appear on Windows,
it will register ~30 compat suites pointing at `build/linux-production`, an engine directory nothing
builds. Phase3Compat needs a host guard mirroring `Phase3Gates.cmake:143` **before** fixtures arrive, not
after.

### 6.2 Must gain a Windows arm

| site | today | needs |
|---|---|---|
| `CMakeLists.txt:91-99` (`hl_engine_cflags`) | `-fvisibility=hidden` unconditional | drop it on Windows (§2.2) |
| `CMakeLists.txt:45-49` (`HL_PACKAGE_ARCH_DIR`) | `else()` → `package/linux-<arch>` | `package/windows-<arch>`. This is exactly the class of bug `docs/amd64-host.md` §8.1 records: *"`package/linux-aarch64` was a path literal naming the host CPU"* |
| `CMakeLists.txt:192-200` | Linux / Darwin | `hl-host-windows` from `WINDOWS_HOST_SOURCES` (§3.1) |
| `CMakeLists.txt:167-178` | `COMMON_HOST_SOURCES` | re-split into portable + POSIX (§3.2) |
| `cmake/Format.cmake:19-21` | names `COMMON_HOST_SOURCES` | follow the re-split, and add `src/host/windows/*.c` |
| `cmake/Phase4Install.cmake:27-31` | `else()` → `HL_PACKAGE_HOST=linux` | `windows` |
| `cmake/Phase4Install.cmake:53-58` | `else()` → `LINK_GROUP RESCAN` | verify RESCAN on the mingw link line (§2.1); fall back to the Darwin technique if unsupported |
| `cmake/Phase4Install.cmake:83-91` | activation `.pc` `else()` → `--whole-archive ... -pthread -ldl -lm -latomic` | no `-ldl`/`-latomic` on mingw. Moot until an activation archive exists on Windows (`HL_HAVE_ACTIVATION` is false, since Phase 2 does not run) |
| `cmake/Phase4Install.cmake:119-124` | no Windows arm | append `hl-host-windows` |
| `cmake/Phase4Install.cmake:218-248` (`package.consumer-link`) | registered on any host with `HL_BUILD_TESTS` | `cmake/PackageTest.cmake` is already CMake script mode, which is the good news; it passes `-DCC=` and links a consumer, so it needs a read-through for `.exe` suffix and the `-lhl-host-<host>` name. Probably works with the `HL_PACKAGE_HOST` fix alone — **verify, do not assume** |
| `cmake/Phase3Units.cmake:123-127` | `else()` → `hl-host-linux` (nonexistent) | `hl-host-windows`, plus a `_HL_WINDOWS_EXCLUDED_UNITS` list mirroring `_HL_DARWIN_EXCLUDED_UNITS` at `:28-30`. Expect it to be larger than Darwin's 12 entries |
| `cmake/Phase3Units.cmake:215` | `if(Linux)` extra Linux-host binaries | leave; add a Windows equivalent when the backend exists |
| `cmake/AssertExecutables.cmake:9` | `execute_process(COMMAND test -x ...)` | an external POSIX binary and a Unix mode bit. Replace with `if(EXISTS)` on Windows. Only reached from `Phase3Compat.cmake:568-571`, so not urgent |
| `cmake/CiLanes.cmake:24-28` | `HL_CI_HOSTS` | a `Windows-x86_64` token, but **only** at the milestone where a `windows.yml` exists. `DOCS.md` §11 spells out why the order matters: declaring the token turns invariant I20 off, leaving that workflow with no structural guard |

### 6.3 Not a build-system problem, named so it is not mistaken for one

`Phase2Production.cmake` builds the production engines from the unity TU `src/core/target/x86_64.c`,
which textually includes the whole engine tree — including `src/linux_abi/syscall/*.c`, which is where
`sys/shm.h`, `sys/msg.h`, `sys/sem.h`, `sys/prctl.h`, `sys/syscall.h`, `termios.h` and `ucontext.h` all
live. A "Phase2Windows.cmake" is three lines of CMake and several thousand lines of engine work. The build
system should not grow that arm until the unity TU compiles.

---

## 7. flake.nix

### 7.1 What `windows.supported = true` would and would not mean

Nix does not run natively on Windows. There is no nixpkgs `system` for a native Windows host; what exists
is `pkgsCross.mingwW64`, which cross-compiles Windows binaries *from* a Linux builder. Those are different
things, and conflating them is the failure mode to avoid.

Read `toolchainFor` (`flake.nix:63-118`). `backendName` is
`if host.isDarwin then "macos" else if host.isLinux then "linux" else "windows"` — derived from
`pkgs.stdenv.hostPlatform`. `systems` lists only Linux and Darwin, so `backendName` is **never** `windows`
today. Flipping `windows.supported = true` therefore changes nothing observable. That is not a reason to
flip it; it is a reason to be precise about what it would be for.

Two secondary observations from the same block:

- `hasCrateArchive = backend.supported && hostCpu == "aarch64"` (`flake.nix:107`). An x86-64 Windows host
  cannot accidentally enable a crate build with no archive to link, whichever way `windows.supported`
  goes. Correct by construction; worth recording so nobody "fixes" it.
- `backendName`'s final `else` is a fallthrough, so any future non-Linux, non-Darwin platform would be
  named `windows`. Harmless today, wrong the moment a third case exists. A `throw` on the unknown case
  would be more in the spirit of the rest of the file.

### 7.2 Recommendation

**The native Windows host is outside nix's remit, and the flake should say so rather than model it.**

- **Until the Windows arm compiles:** leave `windows.supported = false` and replace the bare entry with a
  comment stating (i) that nix supplies no compiler on Windows, so `cmake/toolchains/x86_64-windows.cmake`
  reads `PATH`/`$WINDOWS_X86_64_CC` rather than a nix variable, and (ii) what would flip it. An entry with
  no explanation invites someone to flip it because it looks like a switch.
- **Once the Windows arm compiles:** the useful role for nix is **cross-compiling and smoke-testing the
  Windows arm from Linux CI**, via `pkgs.pkgsCross.mingwW64`. That is not a new idea in this tree — it is
  exactly `DOCS.md` §11's step 5 for a new host CPU (*"cross-compile the other host CPUs' arms from this
  one, and have that be a CI step"*), and exactly how `cmake/toolchains/aarch64-linux.cmake` lets the
  x86-64 CI job compile the AArch64 arms without AArch64 hardware. `docs/amd64-host.md` §5 records what
  that discipline caught. Concretely: a `packages.<linux-system>.hl-engine-windows` output that builds
  `hl-translator`, `hl-host-fake`, `hl-engine` and eventually `hl-host-windows` under
  `pkgsCross.mingwW64`, gating that the Windows `#if` arms stay compilable from every push.
- At *that* point `windows.supported = true` acquires a meaning: **"the Windows host arm is compiled by
  CI"**, which is honest and testable. It should never mean "guests execute on Windows"; per `README.md`'s
  host table and `DOCS.md` §11 step 6, that requires the compat matrix, and it is a separate declaration.
- `canBuildGuests = backend.supported && cpu.supported` (`flake.nix:101`) gates `packages.default`
  (`flake.nix:164`). Any Windows entry must not cause a `packages.<windows-system>` to be emitted with no
  builder behind it. Since `systems` gains no Windows member under this recommendation, it will not —
  but say so, because "add the entry, then add the system" is the obvious wrong next step.

### 7.3 What does not change

`flake.lock`, `guestISAs`, `hostCPUs`, the devShell, and every `*_LINUX_CC` / `*_DYNAMIC_*` export. The
Windows host consumes none of them. If §4.3's fixture-archive plan is adopted, `flake.lock`'s hash becomes
an *input to the fixture digest* — it pins glibc and sqlite — but the flake itself still does not change.

---

## 8. Staged milestones

Each milestone is defined by a **command that succeeds** and produces a file. M0-M3 are build-system work.
M4 onward is engine work the build system only has to express, listed so the ordering is visible.

### M0 — configure succeeds
- Make `HL_BASH_EXECUTABLE` optional; delete the two duplicate `find_program` calls (§5.2 step 1).
- Add the Windows arms to the five `else()` sites of §1 that a `HL_BUILD_TESTS=OFF` configure reaches:
  `CMakeLists.txt:45-49`, `Phase4Install.cmake:27-31`, `Phase4Install.cmake:119-124`.
- Drop `-fvisibility=hidden` on Windows in `hl_engine_cflags` and in `Lint.cmake:22-27`.
- Add `cmake/toolchains/x86_64-windows.cmake` (§2.5).

```
cmake -G Ninja -B build-win -DHL_BUILD_TESTS=OFF \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-windows.cmake
```
**Artifact:** a configured tree. Nothing compiled. **This is the smallest real step and it is worth
landing alone**, because it is the one that has to be right before any compile error is even legible.

### M1 — the first compiling artifact: `libhl-translator.a`
No further edits expected. `IR_SOURCES` needs only `<setjmp.h>` and `<sys/time.h>`, both present in
mingw-w64 (§1). `HL_HOST_ARCH=x86_64` correctly keeps `guest/x86_64/lower/*` out of the list
(`CMakeLists.txt:138-149`) — those emit ARM64 and belong nowhere else.

```
ninja -C build-win hl-translator
```
**Artifact:** `libhl-translator.a`. **This is the answer to "smallest first step that produces a compiling
artifact".**

### M2 — `libhl-host-fake.a`, then `libhl-engine.a`
`hl-host-fake` is one file with `<string.h>` and `<sched.h>`; expect it to build unmodified. `hl-engine` is
20 files of which 2 block: `src/core/checkpoint_channel.c` and `src/core/provider/demux.c`, both on
`sys/mman.h`/`sys/socket.h`/`poll.h`. Decide per file — shim, split, or exclude from a Windows
`CORE_SOURCES` — and record the decision; excluding a file from one host's archive is the kind of thing
that later reads as an accident.

### M3 — `libhl-linux-abi.a`
4 of 31 files block: `container/vfs/gmap.c`, `logical_vma.c`, `fdcache.c` on `sys/mman.h`; `dns.c` on
`dlfcn.h`. Same decision procedure. At the end of M3 the tree builds **three of the four Phase-1 archives**
on Windows with no host backend at all.

### M4 — `libhl-host-windows.a`
The real work, and not a build-system milestone. Needs: the `WINDOWS_HOST_SOURCES` list and the third arm
(§3.1); the `COMMON_HOST_SOURCES` re-split (§3.2); a Windows × x86-64 arm in `src/host/native_context.h`,
which today `#error`s; and `src/host/windows/{directory,host,process,range,system}.c` plus Windows
siblings for `child.c`, `private.c`, `resolve.c`. Scale reference: `src/host/linux/host.c` is 3961 lines,
`src/host/macos/host.c` 4839, across 14 service groups.

### M5 — `hl-engine-runner.exe` links
Resolve the `LINK_GROUP RESCAN` question (§2.1). Verify the UTF-8 manifest actually reaches `argv`
(§2.3). Fix the `.pc` files' `-lhl-host-windows` and the missing `-ldl`/`-latomic`. `cmake --install`
should then produce a usable, if guest-less, SDK.

### M6 — host unit tests
`cmake/Phase3Units.cmake` with `_hostl = hl-host-windows` and a `_HL_WINDOWS_EXCLUDED_UNITS` list. This is
the first milestone that produces a green `ctest -L unit` on Windows and therefore the first that CI can
gate on. Also the point to add a `Windows-x86_64` token to `HL_CI_HOSTS` and a `windows.yml`, in that
order and in one change (`DOCS.md` §11 step 4, and `CiLanes.cmake:46-57` on why the order matters).

### M7 — a Windows production engine
The Phase-2 equivalent: the unity TU `src/core/target/x86_64.c` compiling against Win32. This pulls in the
entire `src/linux_abi/syscall/` tree. **This is the largest single item in the whole port** and it is
where "a Windows host" stops being a build question.

### M8 — guest fixtures and the compat matrix
Blocked on M7 and on §4.3. Requires, additionally: a host guard on `Phase3Compat.cmake` before it is ever
reachable (§6.1), the `execlp("env", ...)` bridge contract, `/tmp` replaced by a configured scratch
variable at the nine sites in `Phase3Gates.cmake`, `RunSequence.cmake:10`'s `separate_arguments(UNIX_COMMAND)`
replaced (it treats `\` as an escape, so any Windows path in those `-DCMD0=` strings is destroyed), the
`create_symlink` at `Phase3Compat.cmake:162-171` (needs Developer Mode or `SeCreateSymbolicLinkPrivilege`),
and Windows process-model answers for the ~100 `fork`/`exec`/`waitpid` sites across `tools/*_runner.c`.

---

## 9. Unknowns, stated as unknowns

Ranked by how much they would cost to be wrong about.

1. **Whether `$<LINK_GROUP:RESCAN>` is supported on the mingw link line.** Blocks M5. Falls back to the
   Darwin double-list technique, which already exists in the tree. Cheap to check, expensive to discover
   late.
2. **Whether the mingw CRT builds `argv` honouring the UTF-8 active-code-page manifest.** If not, a
   Windows-only `main` shim is needed. Test with a non-ASCII path at M5.
3. **Whether a Windows-hosted Linux cross toolchain exists that produces byte-identical fixtures.** I did
   not exhaustively search. The bar for overturning §4's assumption is identical output against the nix
   build, not "it compiled".
4. **Whether `cmake/PackageTest.cmake` works on Windows with only the `HL_PACKAGE_HOST` fix.** It is
   already CMake script mode, which is promising, but it links a consumer and re-invokes `cmake --install`;
   it needs reading, not assuming.
5. **How many `_HL_WINDOWS_EXCLUDED_UNITS` entries M6 needs.** Darwin excludes 12
   (`Phase3Units.cmake:28-30`). I did not audit `tests/unit/*.c`; the number bounds M6 and I have no
   estimate.
6. **Whether guest `fork()` has any Windows answer at all.** This is not a build question, but it
   determines whether `fork_wire.c` and `private.c` ever get Windows siblings (§3.2), and therefore
   whether the fork-server, the forkserver compat lane and `checkpoint`'s process-tree cases exist on this
   host. It should be settled before anyone writes those two files.
7. **Whether the Windows JIT/executable-memory model matches what the translator assumes.** The dual-alias
   W^X arena (`docs/amd64-host.md` §5.1) is `mmap`/`mprotect`/`mach_vm_remap` shaped. `VirtualAlloc2` +
   `MapViewOfFile3` can express a dual mapping, but the guarantees differ. Out of scope here; it bounds
   M7.
