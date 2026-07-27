# The `hl-engine` crate on a Windows host

Plan for making `pkgs/rust` build on a native Windows host, under the requirement that *"the rust package
should compile as any other rust"* — a plain `cargo add hl-engine` / `cargo build` on a stock Windows box,
with no exotic prerequisites.

`DOCS.md` is normative; §7.7 describes the current crate model and this document proposes changes to it.
Nothing here is implemented. Every measurement below was taken on this branch; where something was not
measured or could not be checked on this box, it says so.

---

## 1. Status

| | |
|---|---|
| Windows host backend | `src/host/windows/` is a `README.md` and nothing else — no code, no CMake wiring |
| `flake.nix` | `hostBackends.windows.supported = false`; `systems` lists only the three unix systems |
| Crate targets accepted | 3 (`build.rs` `HOSTS`), archives committed for 2 |
| Crate Rust source portability to Windows | **does not compile** — see §3.1, this is the larger of the two problems |
| crates.io budget headroom | ~5 MB compressed of the 10 MiB limit — measured, §4.3 |

---

## 2. How the crate is built today

### 2.1 It links a frozen archive; it never compiles `src/`

`pkgs/rust/build.rs` is 61 lines and compiles nothing. It:

1. reads `TARGET`, looks it up in a three-row table

   ```rust
   const HOSTS: &[(&str, bool)] = &[
       ("aarch64-apple-darwin", false),
       ("aarch64-unknown-linux-gnu", true),
       ("x86_64-unknown-linux-gnu", true),
   ];
   ```

   The `bool` is "needs the GNU/Linux libraries the Mach-O link does not". An unlisted target `panic!`s with
   `hl-engine does not support target {target}`.
2. resolves `assets/lib/<triple>/libhl-engine.a`, emits `cargo:rerun-if-changed` for it *before* testing
   existence, so producing the archive re-runs the script;
3. if the archive is absent, prints nine `cargo:warning=` lines naming the CMake target that builds it and
   returns **without** emitting link directives. This is deliberate and the comment says why: emitting
   `rustc-link-lib=static:` makes rustc resolve the archive while producing the rlib, so a missing archive
   would fail `cargo clippy` as hard as a panic. "Supported host, no archive in this tree" is a third state,
   distinct from both "unsupported target" and "ready to link";
4. otherwise emits

   ```
   cargo:rustc-link-search=native=<assets/lib/triple>
   cargo:rustc-link-lib=static:+whole-archive=hl-engine
   ```

   plus `pthread dl m atomic` on the two GNU/Linux rows.

`+whole-archive` is load-bearing, not an optimisation. The archive's activation constructor
(`src/core/activation.c`, one of 11 `__attribute__((constructor))` sites) has no referenced symbol, so a
normal `-l` drops the member and the engine never activates. `cmake/Phase4Install.cmake` records the same
requirement for the installed `libhl-engine-activation.a`, and the two archives are the same artifact:
`build/package/<host-os>-<host-cpu>/libhl-engine.a` (`HL_PACKAGE_ARCH_DIR`, `CMakeLists.txt:46-49`).

The crate declares **zero dependencies** (`Cargo.lock` holds one package). There is no `cc` build-dependency,
no `bindgen`, no vendored C. `src/ffi.rs` hand-declares the C surface in a single `unsafe extern "C"` block.

### 2.2 Three sets, each strictly narrower than the last

This is the structure the Windows plan has to extend, and it is easy to conflate the layers:

| set | members | decided by |
|---|---|---|
| **compiles** | 3 | `src/lib.rs`'s `compile_error!` cfg gate |
| **links** (supported) | 3 | `build.rs` `HOSTS` |
| **published** | 2 | `Cargo.toml` `include` + `PROVENANCE.md` + the crates.io size budget |
| **nix-built** | 2 | `flake.nix` `hasCrateArchive = backend.supported && hostCpu == "aarch64"` |

`x86_64-unknown-linux-gnu` is the one that already sits in the gap: supported and compilable, but its archive
is a *local build product* (`refresh-crate-archives-linux`), never committed. `src/lib.rs`'s gate carries the
comment "Keep in step with the `HOSTS` table in build.rs: that decides what can be LINKED, this what can be
COMPILED."

### 2.3 What the stamp, manifest and closure checks are for

The archives are committed binaries, so they can silently fall behind the headers. Releases 0.1.17, 0.1.18
and 0.1.26 shipped an archive 478 commits behind (launch-config ABI 9 against 13) and every guest launch
failed with `launch config has an invalid prefix`. Four independent mechanisms now guard that:

- **source manifest** (`tools/crate_archive_manifest.sh`) — SHA-256 over the per-file SHA-256s of every
  `src/{core,host,linux_abi,translator}/**.{c,h}` and `include/hl/*.h`, with the file list produced by
  `cmake/ArchiveSources.cmake` in script mode so it cannot drift from the list the build's `OBJECT_DEPENDS`
  uses. This is the **currency** check. A byte-comparison of a rebuild is not used: the compiler records the
  absolute checkout directory in every object, so the same commit built at a different path yields different
  bytes.
- **build stamp** (`tools/gen_archive_stamp.sh`, `cmake/ArchiveStamp.cmake`) — every archive object
  force-includes a generated header holding `HL-ARCHIVE-SOURCE-MANIFEST:<digest>` as a `used static const
  char[]`. `check_crate_archives.sh` greps the raw archive bytes and demands exactly one distinct stamp, equal
  to the recorded manifest, present once per `ar -t` member. This is the **correspondence** check, and it
  catches the case the manifest cannot: a refresh in an incremental build directory that skipped a changed
  file passes both the manifest and the SHA-256 while carrying two stamps. The stamp is `static` and therefore
  invisible to `nm`; matching the byte string is what no linkage choice can hide.
- **structural/link validation** (`tools/validate_crate_archive.sh`) — `!<arch>` magic, non-empty member list,
  `hl_engine_create` / `hl_host_process_open` / `hl_production_clock_gettime` defined, and a real link of
  `tools/dual_backend_e2e_runner.c` against the force-loaded archive. It **defers** the link test with a
  printed reason when run on a host that cannot perform it (`no Darwin host; Mach-O link test deferred to
  macOS CI`), falling back to byte-level checks. This deferral pattern is the model Windows should reuse.
- **archive closure** (`tools/check_archive_closure.sh`, wired as `gate.archive-closure` in the `unit` lane,
  Linux only) — a different question from the above three: it asserts the *installed* archive set closes over
  itself plus the system toolchain, by nm-diffing defined against undefined symbols and offering the residue
  to a real linker. This exists because `libhl-translator.a` shipped for a long time carrying 89 unresolvable
  engine-internal references that a demand-driven archive link never surfaced.

`pkgs/rust/tests/packaged_archive.rs` is the fourth leg: it *launches a guest* through the committed archive,
which is the only check that can see an ABI mismatch, since that is a runtime rejection.

---

## 3. What "compiles like any other rust crate" requires on Windows

### 3.1 The problem that is not the ABI: the Rust source is Unix-only

Before any archive question, `cargo build --target x86_64-pc-windows-*` fails in `rustc`, not in the linker.
`src/lib.rs` says so explicitly:

```rust
#[cfg(not(any(
    all(target_arch = "aarch64", target_os = "macos"),
    all(target_arch = "aarch64", target_os = "linux"),
    all(target_arch = "x86_64", target_os = "linux")
)))]
compile_error!("hl-engine supports only the aarch64-apple-darwin, ...");
```

and removing that gate only moves the failure. Measured over `pkgs/rust/src` (11,977 lines, 10 files
affected):

| API | uses | Windows equivalent |
|---|---|---|
| `std::os::unix::net::UnixStream` | 13 | none in `std` (Windows AF_UNIX exists in the OS but not in `std`) |
| `std::os::unix::net::UnixDatagram` | 4 | none in `std`, and **SOCK_DGRAM AF_UNIX does not exist on Windows at all** |
| `os::unix::ffi::OsStrExt` / `OsStringExt` | 12 | `OsStr` is WTF-16 on Windows; there is no `as_bytes` |
| `os::fd::AsRawFd` / `FromRawFd` | 5 | `AsRawHandle` / `AsRawSocket` — two types, not one |
| `std::os::unix::fs::symlink` | 3 | `symlink_file` / `symlink_dir`, privilege-gated |
| `PermissionsExt`, `OpenOptionsExt`, `FileTypeExt` | 3 | different traits, different semantics |

`src/ffi.rs` additionally declares three raw libc entry points by name — `pipe`, `fcntl`, and `kill` (as
`process_signal`) — none of which exist in the MSVC UCRT, plus `__libc_current_sigrtmin` behind
`cfg(target_os = "linux")`. `src/ffi.rs` and `src/engine/launch.rs` pass bare `c_int` descriptors across the
FFI boundary (`Streams { input, output, error: i32 }`, `transport: c_int`, `master: *mut i32`), which is a
**wire-format decision inside `hl_activation_start_with_channels`**, not just a Rust-side type choice. A
Windows host backend hands out `HANDLE`s and `SOCKET`s, and `SOCKET` is `UINT_PTR` — 64-bit — so `i32` is not
merely the wrong name for it, it is the wrong width.

**Consequence for planning.** The C-side archive is the *second* half of this work. The first half is a
descriptor-abstraction decision in `include/hl/activation.h` and a Windows arm for ten Rust modules. Whoever
owns the Windows host backend and whoever owns the crate must agree on the descriptor representation before
either can finish; that is the single largest coupling in this whole task.

`AF_UNIX` deserves a specific note: `src/checkpoint_stream.rs` (broker over `UnixDatagram` + per-request
`UnixStream`) and `src/runtime/transport.rs` need a bidirectional, credential-free, datagram-capable local
channel. Windows AF_UNIX supports `SOCK_STREAM` only, has no `socketpair`, and cannot pass handles. The
likely shape is named pipes plus `DuplicateHandle` for descriptor transfer, which means the transport is a
**re-implementation**, not a port. This should be scoped separately.

### 3.2 MSVC vs GNU: which ABI does the archive have to be?

**`x86_64-pc-windows-msvc` is rustup's default host triple on Windows.** A user who runs `rustup-init` and
then `cargo add hl-engine` is on MSVC. Supporting only `-gnu` therefore fails the stated requirement outright:
the default path ends in `build.rs`'s `panic!`.

The two are not interchangeable. The *container* is not the problem — mingw-w64 and MSVC both emit PE/COFF
objects into `!<arch>` archives, and `llvm-ar`/GNU `ar`/`llvm-nm` read both. What breaks:

1. **Static initialisers, and this one is decisive.** The crate's entire mechanism is a pre-`main`
   `__attribute__((constructor))` retained by `+whole-archive`. Targeting MinGW, clang lowers
   `llvm.global_ctors` into `.ctors`, walked by mingw's `crtbegin.o`/`__do_global_ctors`. Targeting MSVC it
   lowers into a `.CRT$XCU` pointer walked by the UCRT's initialiser table. An MSVC link of a mingw-built
   archive either drops the constructors entirely or drags mingw CRT objects into the image. **The failure is
   silent** — everything links, and the engine is simply never activated. There is no diagnostic for it, which
   is exactly the failure class this repository has already been bitten by twice (§2.3).
2. **`__int128` helper symbols.** 100 uses across 13 files. `__udivti3`/`__umodti3`/`__divti3` come from
   `libgcc` on a mingw link; on an MSVC link they must come from Rust's `compiler_builtins` or
   `clang_rt.builtins-x86_64.lib`. *Unverified:* whether `compiler_builtins` exports all of the 128-bit
   division set on `x86_64-pc-windows-msvc`. Worth a 20-line probe before committing to the plan.
3. **CRT surface.** mingw supplies `libmingwex` (C99 `printf` conversions, `strtold`, `sincos`). The UCRT
   does not. Any use of those in the archive is an undefined symbol on an MSVC link.
4. **`setjmp`/`longjmp`.** 21 sites. mingw's and MSVC's `_setjmp` differ in the frame argument and in unwind
   interaction. (These are almost all `sigsetjmp` on the POSIX interpreter fault path and will be replaced by
   VEH/SEH on the Windows backend, but the ones that survive are CRT-coupled.)
5. **`long double`.** 80-bit x87 under mingw, 64-bit under MSVC. **Checked and benign here:** the only two
   occurrences in `src/` are a comment and a `uint64_t x87_ea` field name; `struct cpu`'s x87 stack is
   `double st[8]` (`src/translator/guest/x86_64/cpu.h:38`). So `sizeof(struct cpu)` — which is written into
   every checkpoint image and validated on restore — does **not** change between the two ABIs. This removes
   what would otherwise have been a checkpoint-format fork.

### 3.3 The four options, and the recommendation

**Recommendation: ship one Windows archive, for `x86_64-pc-windows-msvc`, built by `clang`/`clang-cl`
targeting `x86_64-pc-windows-msvc`. Do not use `cl.exe`. Do not ship a DLL. Treat
`x86_64-pc-windows-gnu` as a later, optional addition that must not gate the release.**

*Why not `-gnu` only.* It fails the requirement. The default Windows Rust user never reaches it.

*Why not `cl.exe`.* Two hard stops, both measured:
- **File-scope and inline `__asm__` in 3 files** (`src/core/dispatch.c`, `src/host/linux/host.c`,
  `src/linux_abi/x86.c`; 12 files reference `__asm__` overall). MSVC has removed inline assembly on x64
  entirely — there is no `__asm` block, only intrinsics. Replacing these means externalising them into
  `.asm` files assembled by `ml64`, per site.
- **`__int128`, 100 uses in 13 files.** `cl.exe` has no 128-bit integer type at all; there is no
  `#define` that fixes this, only a rewrite to `_umul128`/`_udiv128` intrinsics or a soft 128-bit type.

  `__attribute__((naked))` (4 sites), `cleanup` (17), `packed` (10), `weak` (4) and `vector_size` (1) would
  each also need a `__declspec` or a rewrite. Against that, the dialect surface is otherwise *narrow* and this
  is the useful finding: **zero** uses of `typeof`, statement expressions, computed `goto`, case ranges, VLAs
  or `alloca`. The GNU-ism problem is concentrated in a handful of constructs, and every one of them is
  accepted by clang.

*Why clang targeting MSVC works where `cl.exe` does not.* clang accepts GNU C regardless of which ABI it
targets. `clang --target=x86_64-pc-windows-msvc` (or the `clang-cl` driver) gives MSVC-ABI COFF objects,
`.CRT$XCU` constructors, MSVC name decoration and UCRT linkage, while still parsing `__attribute__`,
`__asm__` through the integrated assembler, and `__int128`. This is the only route that satisfies both
"the default Windows Rust user can link it" and "we do not rewrite the translator".

Residual risks on this route, all unverified and all cheap to probe:
- `__attribute__((naked))` on x86-64 in clang is restricted to bodies containing only `asm` statements. The
  4 sites are `run_block`/`block_return` trampolines. On an x86-64 host these are JIT-path code, which
  `docs/amd64-host.md` §3 shows is `#if defined(HL_HOST_CPU_AARCH64)`-gated — so they may not be compiled at
  all in a Windows x86-64 build. **Confirm before assuming either way.**
- `__attribute__((weak))` on *undefined* references (3 in `src/core/launch.c`, NULL-checked at runtime) has
  no COFF representation on either Windows ABI. This must be resolved by the host-backend work — most likely
  by giving `src/host/windows/` real definitions — and it is not MSVC-specific.
- `__attribute__((visibility("hidden")))` (9 sites) is ignored with a warning on COFF. Harmless in a static
  archive; expect noise.

*Why not both archives now.* §4.3's measurement: the budget admits roughly two more archives, and
`x86_64-unknown-linux-gnu` has a prior claim on one of them. Adding a second Windows ABI doubles the CI
matrix, the refresh tooling, the provenance block and the validation lanes for a target reachable only by a
user who has deliberately installed a non-default toolchain. `-gnu` can be added later at near-zero marginal
*C* cost once the sources compile with clang for Windows at all — the incremental work is packaging, not
porting. It should be added when there is a reason, not preemptively.

*Why not a DLL.* This is the option that most deserves consideration, because a C-API DLL genuinely is
ABI-tolerant: the CRT stays inside the DLL and one binary could serve both MSVC and GNU consumers through
their respective import libraries. It is rejected on three grounds, in order of weight:

1. **Deployment.** A `.dll` must be discoverable by the loader at run time. `cargo build` emits a bare
   `.exe`; `cargo test` runs binaries out of `target/<profile>/deps/`; `cargo install` copies one file. Cargo
   has no supported mechanism for a dependency to place a runtime DLL next to a downstream artifact —
   `OUT_DIR` is not on the search path. Every workaround (build-script copy into `target/`, `PATH`
   manipulation, `#[link(name=..., kind="raw-dylib")]` plus manual staging) is a step the user has to know
   about, which is precisely what "compiles like any other rust" forbids.
2. **The re-exec.** `README.md`: "Child isolation reexecutes the downstream application and activates the
   native backend before Rust `main`." The re-exec'd process must also find the DLL, from whatever working
   directory and environment the launch happened in.
3. **Loader lock.** Moving activation into `DllMain` runs it under the loader lock, where creating threads,
   spawning processes and loading further modules are all forbidden or deadlock-prone — and activation does
   all three. Keeping activation in a `.CRT$XCU` initialiser inside the DLL avoids the lock but reintroduces
   ordering questions relative to the host executable's own initialisers.

A static `.lib` has none of these properties. Keep it.

---

## 4. Target triple and asset layout

### 4.1 Naming

rustc's native-library search on `*-windows-msvc` looks for `<name>.lib`; on `*-windows-gnu` it looks for
`lib<name>.a`. The safest layout keeps the file name a property of the row rather than a constant:

```
pkgs/rust/assets/lib/x86_64-pc-windows-msvc/hl-engine.lib
pkgs/rust/assets/lib/x86_64-pc-windows-gnu/libhl-engine.a      (if/when added)
```

*Unverified:* whether rustc on `windows-msvc` also accepts `libhl-engine.a`. If it does, keeping one filename
everywhere is simpler; check before choosing.

### 4.2 `build.rs`

The current two-column table stops being expressive enough: the Windows rows share neither the Unix system
libraries nor the archive filename. Widen it to a struct-like row rather than adding a second `bool`:

```rust
struct Host {
    triple: &'static str,
    archive: &'static str,     // "libhl-engine.a" | "hl-engine.lib"
    link_name: &'static str,   // "hl-engine"
    system_libs: &'static [&'static str],
}
```

Everything else in `build.rs` generalises unchanged, and the three behaviours it already gets right must be
preserved verbatim:

- the `rerun-if-changed` before the existence test;
- the "supported host, archive absent" warning path that emits **no** link directives, so `cargo clippy`
  still passes — the remediation command it prints must become target-aware
  (`refresh-crate-archives-windows`, not `-linux`);
- the `panic!` reserved for genuinely unsupported targets.

`src/lib.rs`'s `compile_error!` cfg list must gain the Windows arm in the same commit, or the crate fails in
`rustc` with a message that points at host support instead of at the missing Rust arms of §3.1.

The `Cargo.toml` `include` list needs no change — `assets/lib/**` already globs.

### 4.3 The size budget, measured

| archive | on disk | gzip -9 |
|---|---|---|
| `aarch64-apple-darwin/libhl-engine.a` | 24,722,376 | 2,158,150 |
| `aarch64-unknown-linux-gnu/libhl-engine.a` | 27,079,506 | 2,610,650 |
| `src/` + `examples/` + metadata (uncompressed) | ~419,000 | ~0.1 MB compressed |

Published `.crate` today ≈ **4.9 MB against the 10 MiB crates.io limit**. `Cargo.toml`'s comment ("the two
here leave no room") and `PROVENANCE.md` ("a third is a publication decision that needs that budget solved
first") were written when 0.1.27 was 11.3 MB *including* the alpine rootfs and testdata; those are now
excluded and the constraint has moved. **There is room for one more archive comfortably, and two at the
edge.** That is an argument for the MSVC-only recommendation, and it is measured rather than assumed.

Two escape valves exist if a fourth archive is ever wanted: crates.io grants per-crate limit increases on
request, and the standard structural fix is per-target `hl-engine-sys-<triple>` companion crates selected by
`[target.'cfg(...)'.dependencies]`, which Cargo resolves without downloading the non-matching ones. Both are
larger decisions than this document should make; note that the second also retires the
`x86_64-unknown-linux-gnu` gap.

### 4.4 `flake.nix`

`hasCrateArchive = backend.supported && hostCpu == "aarch64"` should stop being an expression over the two
axes and become a lookup keyed on the triple the archive is actually filed under, so a newly supported host
cannot enable a crate build with nothing to link — which is what the current name already promises. Sketch:

```nix
rustTriple = { linux = "${hostCpu}-unknown-linux-gnu";
               macos = "${hostCpu}-apple-darwin";
               windows = "${hostCpu}-pc-windows-msvc"; }.${backendName};
crateArchiveTriples = [ "aarch64-apple-darwin" "aarch64-unknown-linux-gnu" ];
hasCrateArchive = backend.supported && lib.elem rustTriple crateArchiveTriples;
```

The list is then the same fact `build.rs`, `PROVENANCE.md` and `check_crate_archives.sh` each state
separately today — four copies, which is the reason `refresh_crate_archives.sh:203` has to hardcode
`host_arch != aarch64` to protect the provenance block. Consider deriving them from one file.

Note the merge hazard `docs/amd64-host.md` §10 already records: `canRunGuests` was **renamed** to
`hasCrateArchive` because the two ideas were only accidentally the same. Do not let a Windows change
re-conflate them.

Nix cannot build the Windows crate output regardless: `systems` has no Windows entry and nixpkgs has no
Windows `stdenv`. The flake's job here is only to not *claim* an archive that does not exist.

---

## 5. Windows link requirements

The Windows host backend does not exist yet, so this is a prediction from what `hl_host_services` covers
(files, processes, clocks, memory, sync — `src/host/*.{c,h}`) plus what the crate advertises (secure host
entropy, process domains, terminal, provider transport). Treat it as a starting list to be replaced by the
actual `nm` output of the first archive.

| library | why | note |
|---|---|---|
| `ntdll` | `NtCreateFile`, `NtQueryInformationProcess`, `NtMapViewOfSection`, `NtSuspendProcess` — anything below the Win32 layer | ships in the Windows SDK as `ntdll.lib`; mingw-w64 ships `libntdll.a` |
| `kernel32` | the bulk of Win32: `CreateFileW`, `VirtualAlloc2`, `MapViewOfFile3`, `QueryPerformanceCounter`, `CreateProcessW`, job objects | rustc's own link line already includes it; name it anyway rather than depending on that |
| `ws2_32` | sockets — the guest ABI's socket surface and the provider channel | Rust `std` links it, but the C archive must not rely on that |
| `synchronization` | `WaitOnAddress` / `WakeByAddressSingle` / `WakeByAddressAll` — the futex-shaped primitive `src/host/sync.c` needs | umbrella lib forwarding to `api-ms-win-core-synchronization-l1-2-0.dll`; Windows 8+ |
| `advapi32` | tokens, ACLs, `OpenProcessToken`, privilege adjustment (symlink creation, `SeDebugPrivilege`) | |
| `bcrypt` | `BCryptGenRandom` for the "secure host entropy" capability the README advertises | prefer over the deprecated `advapi32` crypto API |
| `userenv` | only if the container/namespace lowering resolves profile or environment blocks | probably not needed; confirm |
| `dbghelp` / `psapi` | module enumeration for diagnostics | almost certainly not needed; do not add speculatively |

`build.rs` emits these the same way as the Unix rows, from the row's `system_libs`:

```rust
for library in host.system_libs {
    println!("cargo:rustc-link-lib=dylib={library}");
}
```

Two Windows-specific points:

- Use `dylib=` (import library), not `static=`. `ntdll.lib` and friends *are* import libraries; asking rustc
  for `static=ntdll` makes it look for `libntdll.a`.
- `cargo:rustc-link-lib=static:+whole-archive=hl-engine` maps to `/WHOLEARCHIVE:` on `link.exe`. *Unverified
  on this box (no rustc installed):* that the `+whole-archive` modifier is honoured on `x86_64-pc-windows-msvc`
  in the crate's MSRV, `rust-version = "1.81"`. This is a hard prerequisite — if it is not, the activation
  constructor is dropped and the failure is silent. **Probe this first; it can invalidate the whole plan.**
- Nothing from the Unix row (`pthread dl m atomic`) applies. `m` in particular has no Windows counterpart —
  the math functions are in the UCRT.

---

## 6. The bash tooling

`tools/refresh_crate_archives.sh`, `check_crate_archives.sh`, `validate_crate_archive.sh`,
`crate_archive_manifest.sh`, `gen_archive_stamp.sh` and `check_archive_closure.sh` are all `#!/usr/bin/env
bash`. Bash exists on a Windows runner (Git Bash / MSYS2, and GitHub Actions supports `shell: bash` on
`windows-*`), so the scripts *run*; what breaks is narrower and enumerable:

1. **Host detection.** `uname -s` returns `MINGW64_NT-10.0-...` or `MSYS_NT-...`, matching neither `Linux` nor
   `Darwin`. `refresh_crate_archives.sh:100-111` and `validate_crate_archive.sh:37-50` both switch on it.
   Match with a `MINGW*|MSYS*|CYGWIN*)` glob, not an equality test.
2. **`ar` and `nm` are absent from Git Bash.** `check_crate_archives.sh:146` calls `ar -t`;
   `validate_crate_archive.sh` calls `ar`, `nm --print-armap`, `nm -g --defined-only`;
   `check_archive_closure.sh` takes `nm` as `$1` (already parameterised — follow that precedent). The fix is
   to honour `${AR:-ar}` / `${NM:-nm}` throughout and point them at `llvm-ar` / `llvm-nm` from the LLVM
   install that is building the archive anyway. GNU binutils and LLVM both read PE/COFF.
3. **Member counting.** `check_crate_archives.sh`'s stamp check requires `stamps == members` and filters only
   `^__\.SYMDEF` (the Mach-O case). A `lib.exe`- or `llvm-ar`-produced COFF archive lists the linker members
   `/` and `//` (and, from `lib.exe`, sometimes a `.drectve`-bearing member). Extend the filter, and do it as
   an explicit named set rather than a regex that happens to work — an over-broad filter here silently weakens
   the strongest of the four gates.
4. **Symbol prefix.** x86-64 PE/COFF has no leading underscore, like ELF and unlike Mach-O and 32-bit COFF.
   `validate_crate_archive.sh`'s `symbol_prefix=` default is already correct; the `grep -Eq
   "[[:space:]]_?${symbol}$"` form tolerates both.
5. **The link test.** Reuse the existing `defer_reason` mechanism verbatim: a Linux or macOS host cannot link
   a COFF archive against the Windows SDK, so it should do the byte-level half and print `no Windows host;
   COFF link test deferred to Windows CI`. This is the same shape as the Mach-O deferral, and the comment
   there records why demanding it on the wrong host was a mistake ("made `check-crate-archives`
   unconditionally red no matter how fresh the archive was").
6. **`refresh_crate_archives.sh` needs a `--windows` half**, plus a `refresh-crate-archives-windows` CMake
   target in the `foreach(_half linux darwin provenance)` loop. It must derive its package directory from
   `HL_PACKAGE_ARCH_DIR`, which today has no Windows arm (`CMakeLists.txt:46-49` — `Darwin` or else `linux`).
   `docs/amd64-host.md` §8.1 records exactly this class of defect: `package/linux-aarch64` spelled as a path
   literal meant an x86-64 host would have published its artifact as the aarch64 crate asset. Add
   `windows-${HL_HOST_ARCH}` deliberately.
7. **`--provenance` stays on the aarch64 Linux host.** It rewrites the generated block certifying the
   *published* archives, and `refresh_crate_archives.sh:203` refuses to run anywhere else. Adding a Windows
   archive to the published set means the Windows bytes must reach that host to be hashed — the existing
   `--darwin --emit` / `--from` transport pattern is the answer, generalised to `--windows --emit`. Note
   `--provenance` also uses `python3`, absent from a stock Git Bash; another reason to keep it off Windows.
8. **`crate_archive_manifest.sh` and `gen_archive_stamp.sh` need no Windows change** and should not get one.
   The manifest is a content digest of C sources with no host dependence, and it is deliberately an
   over-approximation (a macOS-only source change invalidates every archive). That property is what makes a
   Windows archive's staleness detectable by the same digest. `hl_stamp_archive_object` passes `-include`,
   which `clang-cl` accepts under `/clang:-include` or in `clang --driver-mode=gcc` form — check which driver
   the Windows build uses.

---

## 7. Publish flow

`publish.yml` today is `needs: [linux, mac]`, then runs three release-specific steps on `ubuntu-24.04-arm`
inside `nix develop`: version check, `check-crate-archives`, and `cargo test --test packaged_archive`.

What changes, in the order it should be done:

1. **A `windows.yml` reusable workflow** on `windows-2022`, building the engine with mingw-w64 clang (host
   backend work) and, once §3.3's clang/MSVC route is proven, the MSVC-ABI archive. No `nix` — nix does not
   run on Windows — so this lane provisions its toolchain directly, which is a departure from every existing
   lane and should be called out in review rather than discovered.
2. **`HL_CI_HOSTS` gains `Windows-x86_64`** (`cmake/CiLanes.cmake`), spelled
   `<CMAKE_SYSTEM_NAME>-<HL_HOST_ARCH>` per DOCS §7.5.1. The invariant that matters:

   > `HL_CI_COMPAT_HOSTS` is the subset that runs guests and therefore shards the compat matrix

   and I20 turns *"a host not in `HL_CI_COMPAT_HOSTS` shards nothing"* into a hard failure. So Windows enters
   as a declared host that shards no compat lane, and **only** joins `HL_CI_COMPAT_HOSTS` when it actually
   runs guests. `docs/amd64-host.md` §1 states the same rule for the x86-64 Linux host: it is not "Supported"
   and should not be marked so until the exact-golden compat matrices pass on a machine CI controls. Note the
   trap recorded there — *declaring the token alone turns I20 off*, so the guard that was protecting the
   workflow stops protecting it the moment the token appears. Add the token and the shards together.
3. **`publish.yml` `needs: [linux, mac, windows]`** — but only after (2). Until the Windows lane runs the
   product, adding it to `needs` makes it a release gate that can block a release without vouching for
   anything, which inverts the invariant.
4. **`check-crate-archives` must learn the Windows row.** The two `for target in aarch64-unknown-linux-gnu
   aarch64-apple-darwin` loops in `check_crate_archives.sh` (three of them, in fact: sha, stamp, validate)
   and `refresh_crate_archives.sh`'s Python provenance writer both hardcode the published pair. These should
   read one list. Until then, three edits per new published target, and forgetting the stamp loop is the
   silent one.
5. **The `packaged_archive` step is the release's correctness check** and it must run *on Windows* for the
   Windows archive. `cargo test --test packaged_archive` on the ubuntu-arm publisher exercises only the
   aarch64-linux archive. This is the same asymmetry §7.5.1 warns about for `isa-fuzz`: a green
   `packaged_archive` on one host is not evidence about another host's archive.

---

## 8. Testing

**`rust.fmt` and `rust.clippy`** (`cmake/RustLint.cmake`) are host-neutral CTest cases that shell out to
`cargo`. They register only when `cargo` is on `PATH`. On Windows they work as-is, *provided* `build.rs`'s
missing-archive path is preserved — clippy on a checkout with no Windows archive must warn, not fail, and
that is exactly what the current `return()`-without-link-directives branch buys. Preserve it or the Windows
lane is red on every commit until an archive lands.

**The crate's own integration tests are largely not portable.** `tests/spec.rs` alone uses `UnixListener` at
three sites and `std::os::unix::fs::symlink` at five; `tests/alpine.rs` symlinks `/dev/stderr`. These are
excluded from the published crate already (`Cargo.toml`'s `include` omits `tests/`, `testdata/` and
`assets/alpine/`, which is what took 0.1.27 from 11.3 MB to under the limit), so they are a repo-side concern
only. The realistic plan is `#![cfg(unix)]` on the ones that are structurally Unix-shaped, and a genuine
Windows arm only for `packaged_archive.rs`, `api.rs`, `contracts.rs` and `traits.rs` — the ones that test
contracts rather than host mechanisms. Audit rather than assume: that split was not verified here.

**`packaged_archive.rs`** is the one test that must run on Windows. It launches `testdata/exit42-{aarch64,
x86_64}` through the committed archive and asserts `Exit::Code(42)` on both guest backends, plus a static
`launch_abi` cross-check against `include/hl/config.h`. Its two fixtures are ELF guest binaries, which is
correct and unchanged — the *guest* is Linux on every host. It gates on nothing Unix-specific in the test
body itself.

**The `package` ctest lane** (`package.consumer-link`, `cmake/PackageTest.cmake`) is a different matter and
is **not** a small port. It is seven steps and every one of them is POSIX-shaped:

- step 3 links `tests/integration/package.c` with `-I`, `-L`, `-l`, `-pthread`, `-o` — GCC driver syntax that
  `clang-cl` does not accept (it needs `/I`, `/link`, `/OUT:`). Either the lane invokes `clang` in GCC-driver
  mode on Windows, or the flags become generator expressions;
- the link line is `hl-engine.pc`'s `Libs:` verbatim, which is the point of the step ("a change to one that
  is not made in the other fails here"). pkg-config on Windows is unusual; `Phase4Install.cmake` would need
  a Windows `HL_PACKAGE_HOST` (`windows`) and a Windows `HL_PACKAGE_SYSTEM_LIBS` (not `-pthread`);
- step 4's activation consumer uses `-Wl,--whole-archive`, which becomes `/WHOLEARCHIVE:`;
- `gate.archive-closure` is Linux-only by construction (`Phase4Install.cmake:192`) because it probe-links
  with `-pthread -lm -ldl -latomic`. A Windows arm needs its own system-library set, and
  `check_archive_closure.sh` hardcodes that list at line 81. Parameterise it rather than adding a branch.

Recommended sequencing: get `packaged_archive.rs` green on Windows first — it is the check that actually
vouches for the shipped bytes — and treat the `package` lane as a follow-on. A Windows `package` lane that is
skipped is honest; one that silently degrades to a link check is not (`PackageTest.cmake:147` already makes
that point about the guest-exec leg: "a silent skip here shrinks the gate to a link check").

---

## 9. Open questions, ranked

Everything below is a real unknown, not a formality. The first three can each invalidate the plan.

1. **Does rustc honour `static:+whole-archive` on `x86_64-pc-windows-msvc` at Rust 1.81?** If not, the
   activation constructor is dropped and the failure is silent — no link error, no runtime diagnostic, just an
   engine that never activates. One 30-line probe answers it. Could not be run here: no Rust toolchain on this
   box.
2. **Does clang lower `__attribute__((constructor))` to `.CRT$XCU` for the MSVC target, and does that survive
   `/WHOLEARCHIVE:` out of a static archive?** Same failure mode, same silence. Probe both together.
3. **Does the Rust source's descriptor model survive?** §3.1: `Streams { i32, i32, i32 }` and `transport:
   c_int` cross the FFI boundary. Windows `SOCKET` is `UINT_PTR`. This is a change to
   `include/hl/activation.h`, i.e. to the C ABI the archive exposes, and it must be agreed with the host-backend
   work *before* either side commits.
4. **Is `compiler_builtins` sufficient for `__int128` division on windows-msvc?** 100 uses, 13 files.
5. **What replaces `AF_UNIX` datagrams?** `src/checkpoint_stream.rs` needs a datagram broker with descriptor
   passing; Windows AF_UNIX is stream-only with no `socketpair` and no handle transfer.
6. **Are `__attribute__((naked))` sites compiled at all in an x86-64 Windows build?** If the
   `HL_HOST_CPU_AARCH64` guard excludes them, one risk disappears entirely.
7. **Does `x86_64-pc-windows-gnu` work on a stock rustup install without MSYS2?** Only matters if `-gnu` is
   ever added. `rust-mingw` ships CRT objects and import libs, but the default linker flavour is the `gcc`
   driver. There is also a third variant, `x86_64-pc-windows-gnullvm` (clang + lld), which would be the
   cheaper of the two GNU-side options if one is ever wanted.
8. **crates.io limit increase** — is one obtainable, and does the project want to depend on one? Relevant only
   if a fourth published archive is ever needed (§4.3).
