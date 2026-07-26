# amd64-host work: findings and merge handoff

Everything the x86-64-Linux-host work turned up that is **not** the feature itself: defects fixed, defects
found and deliberately left, code that looks load-bearing and is not, and documentation that actively
misleads. Plus what a reviewer or a later agent needs to know to merge this without re-deriving it.

`docs/amd64-host.md` is the design. This file is the debris list. It is deliberately blunt; none of it is a
criticism of anyone, and several items are artefacts of a same-day clean-room bootstrap followed by a
transfer of a mature engine (see §3.1).

Line numbers are as of the branch point and will drift. Every claim below was checked against the tree, not
inferred.

---

## 1. Defects fixed by this work

All of these are **pre-existing** and none is specific to amd64 — they were simply unreachable or invisible
while every host was AArch64.

| # | Where | Defect | Severity |
|---|---|---|---|
| 1.1 | `src/translator/guest/aarch64/cache.c:262`, `src/translator/guest/x86_64/cache.c:163` | Persistent-cache identity passed a hardcoded `host_isa = 1` to `hl_identity_configuration`, although the function takes it as a parameter and `DOCS.md:175` documents it as part of the key. Two hosts sharing a cache directory would each have accepted the other's **host machine code** and executed it. | High — arbitrary execution of foreign host code |
| 1.2 | `src/linux_abi/syscall/sysv.c:295` | `size_t pg = (size_t)sysconf(_SC_PAGESIZE); if (pg == 0) pg = 16384;` — `sysconf` reports failure as **-1, not 0**, so the cast yields `SIZE_MAX`, the guard never fires, and the page mask degenerates to `& 1`. `hl_ipc_pground` then returns 0 or 1, collapsing both the `ftruncate` and the `mmap` length in the SysV shm path. Latent on **every** host. | High, low probability — needs `sysconf` to fail |
| 1.3 | `src/linux_abi/syscall/mem.c:843` (and the offset fallback at `:795`) | The MAP_FIXED reconciliation rounded down with a literal `~0x3fff`. On a 4 KiB host that is 12 KiB into a live neighbouring mapping, and the save/restore then replaces the neighbour with an anon mapping. Worse: on such a host the mismatch it reconciles is *unreachable by construction*, so every `MAP_FAILED` reaching it is a genuine kernel verdict being converted into a bogus success — including `MAP_FIXED_NOREPLACE`'s `EEXIST`, where it overwrites the exact mapping the kernel just refused to replace. | High on a 4 KiB host, incl. 4 KiB-page aarch64 Linux |
| 1.4 | `src/host/native_context.h:6` | The `__APPLE__` arm carried **no CPU test** and defined the AArch64 accessors unconditionally, so an Intel Mac compiled `__ss.__pc` against a register file with no such member. | Compile-time; Intel macOS is not a target |
| 1.5 | `src/linux_abi/signal.c:991` | `#elif defined(__x86_64__)` used the **Darwin** mcontext shape (`u->uc_mcontext->__ss.__rip`). Cannot compile against a Linux `ucontext_t`. A guard that had never been compiled by anything. | Compile-time |
| 1.6 | `src/core/dispatch.c:112`, `src/translator/guest/x86_64/translate.c:6266` | The trampoline guards conflated "is the host AArch64" with "is this GCC rather than clang". `dispatch.c`'s `#else` arm was ARM64 asm reached on GCC/x86-64; `translate.c`'s guard had **no arch test at all**. | Compile-time |
| 1.7 | `cmake/Phase2Production.cmake:141,153`, `cmake/Phase3Gates.cmake:416`, `cmake/Phase4Mac.cmake:125,134` | `package/linux-aarch64` was a path literal naming the **host** CPU. An x86-64 host would overwrite the aarch64 artifact under its name, and `tools/refresh_crate_archives.sh:117` would install it as the *aarch64* crate asset. The only thing that would have caught it is a link test one line later. | High — mislabelled published binary |
| 1.8 | `tools/refresh_crate_archives.sh:110` | The `--linux` guard tested only `is_darwin()` while its own error text said it "needs an aarch64 Linux host". | Same as 1.7 |
| 1.9 | `cmake/toolchains/x86_64-linux.cmake:34-37` | `CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER` set unconditionally, which breaks the unconditional `find_program(HL_BASH_EXECUTABLE ... REQUIRED)` when the file is the *native* toolchain. Its sibling `aarch64-linux.cmake` set no find-root modes at all — the asymmetry encoded "the host is always aarch64". | Configure-time |
| 1.10 | `cmake/Phase4Install.cmake:28` | `HL_HAVE_ACTIVATION` tested `CMAKE_SYSTEM_PROCESSOR` for aarch64, while `Phase2Production.cmake` gates the activation archive on the host **OS** alone. So a non-aarch64 Linux host *built* `libhl-engine-activation.a` and then refused to install it or its header — a build producing an artifact it declares does not exist. | Inconsistency |
| 1.11 | `src/translator/guest/x86_64/x87state.c:82,128` | `hl_x86_fxsave`/`fxrstor` were `#if defined(__aarch64__)`-guarded with no other arm, so on any other host they compiled to nothing and **silently** dropped the guest's rounding mode and raised-exception flags. Guest `FXSAVE` would report round-to-nearest and no exceptions, always. | Silent wrong results |
| 1.12 | `src/translator/guest/x86_64/x87state.c` (new x86-64 arm) | `LDMXCSR` `#GP`s on reserved bits. `fxrstor` hands a **guest-controlled** memory image to it, so without masking, a malicious or merely odd guest image kills the *engine* where real hardware faults the *guest*. Fixed by masking on the way in. | Guest-triggerable engine kill |
| 1.13 | `src/translator/guest/x86_64/emit.c:15`, `src/translator/guest/aarch64/translate.c:159` | `g_host_lrcpc` from `getauxval(AT_HWCAP) & (1<<15)` and `g_host_i8mm`/`g_host_bf16` from `AT_HWCAP2` bit 13 — AArch64 feature bits read on any host. On x86 those bits mean something unrelated, so the flags were set from foreign data and would select codegen paths for features the host does not have. | Latent wrong codegen |
| 1.14 | `src/translator/cache.c:665` | `ibtc_publish`'s 16-byte atomic pair store depends on `sizeof(ibtc_ent) == 16` (AArch64 `stp` is single-copy atomic only within a granule; x86-64 `movdqa` `#GP`s when misaligned) and nothing asserted it. A member added to `ibtc_ent` would silently reintroduce the torn-dispatch hazard **on AArch64**, the host every lane tests. Static assertions added. | Latent, high impact |

---

## 2. Defects found and deliberately NOT fixed

Left alone on purpose. Each is out of scope, or fixing it would change behaviour on a host this branch
cannot test.

| # | Where | Issue | Why left |
|---|---|---|---|
| 2.1 | `src/linux_abi/syscall/mem.c:637-644` | `#if defined(__linux__)` with the comment "Linux host pages have the same granularity as this Linux ABI". That is a **compile-time assertion about a runtime property**. True on x86-64 Linux and on 4 KiB aarch64 Linux, false on a 16 KiB-page aarch64 Linux host. | Pre-existing; belongs to the Linux-host lane; cannot be tested here |
| 2.2 | `src/linux_abi/syscall/sysv.c:943` | `shmat` `SHM_RND` uses `sysconf(_SC_PAGESIZE)` with no fallback (same `(size_t)-1` hazard as 1.2), and on a 16 KiB host rejects a guest-legal 4 KiB-aligned `shmaddr` with `EINVAL` — `SHMLBA` should be the *guest* page. | ABI-visible path; fixing it changes 16 KiB behaviour, untestable here |
| 2.3 | `src/linux_abi/syscall/mem.c:315-318`, `:1422-1424` | `munmap` rounds length **down**, leaving a trailing partial guest page mapped where Linux releases it; `MADV_DONTNEED`'s partial tail `memset`s only the requested bytes where Linux zeroes to the page end. | Pre-existing and host-independent; strictly *less* divergent at 4 KiB than at 16 KiB |
| 2.4 | `src/core/target/dual.c:10-13` | `hl_run_linux_guest` hardcodes `hl_aarch64_run_linux_guest`, commented "the **native** AArch64 default used by the macOS command launcher". The guest ISA is not "native" to anything; that equivalence is exactly what inverts on a new host. Harmless today (it is the legacy untyped config launcher's default guest), but it is a landmine. | Behaviour change on a legacy entry point, no test coverage to protect it |
| 2.5 | `src/linux_abi/signal.c:836-846,961-967` | Uses `#if G_GPC_HASH_SHIFT == 2` as a proxy for "the guest is aarch64", to decide whether to print `c->x[...]` and dereference `G_PC(c)` as a `uint32_t*`. `G_GPC_HASH_SHIFT` is a **hash-tuning constant** (`abi.h:35`), not a guest-ISA discriminator. It happens to be 2 for aarch64 and 0 for x86. | Diagnostics only; needs a proper guest-ISA predicate, which is a separate cleanup |
| 2.6 | `src/host/linux/host.c:654-714` | `hl_linux_memory_reserve_code`'s non-dual fallback maps the arena `PROT_READ\|WRITE\|EXEC` — plain RWX — and `hl_linux_memory_code_write` is an unconditional no-op, so in fallback mode there is **no W^X enforcement at all**. The dual-alias memfd path is preferred and normally taken. | Pre-existing security posture question, not an amd64 issue; worth a deliberate decision |
| 2.7 | `pkgs/rust/Cargo.toml:12-28` | The crate ships prebuilt archives in `include` against a documented 10 MB crates.io budget, and each archive is ~24 MB. A third host triple cannot simply be added. This branch makes the target *accepted* and the asset *expected*, with an actionable error when absent; it does not commit a binary. | Publication decision, not an engineering one |

---

## 3. Code that looks load-bearing and is not

### 3.1 The IR and the per-host-CPU codegen backends

`include/hl/ir.h`, `src/translator/ir/`, `src/translator/codegen.c`, and the symmetric
`src/translator/host/{aarch64,x86_64}/codegen.c` pair read exactly as "lower guest code to IR, then select a
host-CPU backend". **They are on no execution path.** `hl_codegen_block` / `hl_codegen_function` have no
caller anywhere in `src/` — only `tests/unit/test_codegen.c`.

The production path is per-guest-ISA frontends emitting host code directly: `guest/x86_64/emit.c` ("arm64
host emitters") and `guest/aarch64/translate.c` (a same-ISA transliterator that copies guest instruction
words verbatim).

**Why**, from `git log`, all on 2026-07-13: `13108f44` bootstrapped the portable skeleton including
`ir.h`; `e2e2496a` added `src/translator/ir/interpreter.c` as an *"executable IR oracle"*, i.e. a reference
executor to differentially validate a lowering backend; `9f09d683` and `420f808e` added the two host
backends. Then `23e771b9`/`ce8f1e0b` transferred in the real pre-existing engine — `DOCS.md` section 12
roadmap item 1, *"Transfer and separate the complete working macOS-hosted Linux engine"* — whose mature
direct-to-ARM64 frontends superseded the IR path hours after it was written, before its 17-opcode IR could
grow to express flags, vectors or atomics.

So it is not abandoned code; it is a design that was overtaken on its first day. Two things follow:

- **Do not plan host portability around it.** Growing it into a real DBT IR is a bigger project than
  writing backends, and would put a new abstraction underneath the tuned, corpus-green ARM64 path.
- **Do not delete it either.** `include/hl/codegen.h` is published `HL_API` and ships via `hl-engine.pc`, so
  removal is an ABI decision. And the neighbouring `src/translator/reloc.c` **is** live —
  `hl_reloc_slide` serves the persistent cache from `guest/*/cache.c` — while looking like part of the same
  layer and hardcoding AArch64 `MOVZ`/`MOVK`.

One real gain: `tests/unit/test_codegen.c` executes its emitted bytes only on a matching host
(`#if defined(__aarch64__)` / `#elif defined(__x86_64__)`), and every CI runner is ARM64. The x86-64
backend's output had therefore **never been executed on any machine**. It ran on an x86-64 host for the
first time during this work, and passed.

### 3.2 Smaller instances

- `src/translator/host/x86_64/README.md` says *"Reserved for the x86-64 host-code backend"*. The directory
  contains a 623-line backend at full IR-opcode parity with the aarch64 one. The README is the single most
  misleading file in the tree for this task.
- `src/host/windows/` is genuinely README-only, and `flake.nix`'s `hostBackends.windows.supported = false`
  correctly reflects that. This one is honest.

---

## 3.3 `lower/*.c` is a link-time landmine on any non-AArch64 host

The highest-priority item in this document that is still **worked around rather than fixed**.

The nine files under `src/translator/guest/x86_64/lower/` are compiled *independently* into
`libhl-translator` via `IR_SOURCES` (`CMakeLists.txt`), **not** `#include`d into the unity TU. They are built
on every host, and every one of them calls the ARM64 emitters (`e_ldr`, `e_rrr`, `emit_exit_const`,
`hl_x86_emit_spill`, …) that only the AArch64 arm of `src/core/target/x86_64.c` defines.

This is invisible on an AArch64 host because a linker pulls an archive member only on demand, and nothing
demanded them. On an x86-64 host, `engine_global_init` unconditionally calls
`hl_x86_rep_set_store_commit()` and `hl_x86_rep_set_access_validators()` — host-neutral *runtime*
configuration hooks that happen to live in `lower/repstr.c`, the same object as its ARM64 emitter. That pulls
the object in and fails the link on 21 undefined symbols.

`guest/x86_64/interp.c` currently carries 21 aborting emitter stubs to unblock this. They are dead by
construction — nothing calls the lowering entry points on this host — but they are debris.

**Proper fix**, in preference order: split the host-neutral runtime halves (`hl_x86_rep_movs`,
`hl_x86_rep_stos`, and the two setters) out of the lowering objects; or gate `lower/*.c` out of
`IR_SOURCES` on a non-AArch64 host. Either deletes the stub section *and* lets the interpreter reuse the
bulk `rep` helpers, a real speedup it currently has to decline.

Note `src/translator/host/aarch64/{asm,codegen}.c` are in `IR_SOURCES` unconditionally too. They happen to
link anywhere because they are pure byte-emission C with no inline asm — but the same
on-demand-archive luck is load-bearing there, and it is luck.

## 3.4 Cross-file invariants with nothing enforcing them

Found while writing the interpreters. Each is a place where two files must agree and no mechanism makes
them.

- **`cpuid.c` and `xgetbv` must agree.** `cpuid.c` withholds AVX, so XCR0 must report x87+SSE only (3). If
  they disagree, a guest takes a path neither backend implements.
- **The JIT's address biasing and `hl_x86_guest_pointer` use different predicates.** `address.c`'s
  `emit_bias` biases whenever the computed effective address has zero high 32 bits, with a special case for
  disp-only absolutes outside `[nonpie_lo, nonpie_hi)`; `hl_x86_guest_pointer` does the precise range check.
  For a biased ET_EXEC these are not the same predicate, so the two backends can differ on a non-PIE guest
  whose EA is below 2^32 but outside the link range. The interpreter uses the precise one. Worth a decision.
- **The interpreter's system-register values must match the JIT's.** `DCZID_EL0` and `CTR_EL0` in
  particular: guests branch on them to choose between `DC ZVA` and byte-loop `memset`, so a different answer
  sends the same guest down a different path on the two backends and would diverge the exact-golden
  cross-ISA comparison for a reason nobody would find quickly.
- **`G_DISPATCH_REASON` in `guest/x86_64/dispatch.h` has no `R_SOFTSPAN` arm**, though `soft_tlb_miss` can
  set it; it falls through to the `R_BRANCH` default. Benign for the JIT (the block re-runs) but it is an
  unhandled reason. The interpreter handles it explicitly.
- **`R_TIER2` would spin forever on an interpreter** if left on the JIT's arm — `tier2_promote(); continue;`
  with a promoter that cannot change anything. Normalised to `R_BRANCH` in `interp_dispatch.h`.

## 3.5 Bugs the interpreters' own test guests caught

Recorded because they show which parts of the ISA space are treacherous, and because hand-written
freestanding test guests (no libc, one layer at a time) found all of them where re-reading did not.

- **A non-contiguous opcode range.** `PUNPCKLQDQ` is `0F 6C` and sits *above* the high-unpack group, so a
  `op >= 0x68` test silently swapped low for high. That is exactly the `movq`+`punpcklqdq` pointer-duplication
  idiom glibc's `INIT_LIST_HEAD` uses: it zeroed `_dl_stack_user` and crashed in `__tls_init_tp` roughly 400
  instructions later. Prefer explicit per-opcode cases over range tests.
- **A wrong barrier constant.** `0xD5033000` instead of `0xD503301F` — the group pins `Rt=11111`. Every
  `ISB`/`DMB`/`DSB` fell into the MRS/MSR catch-all, which silently broke the SMC commit point.
- **`CASP` is discriminated by bit 31, not the size field**, so a `size < 2` test rejected every `CASP`.
- **`SMAXP`/`SMINP` are opcode `0x14`/`0x15`**; `0x1A`/`0x1B` are the floating-point pair.
- **A folded `CLS`** that computed `x ^ (x << 1)` without shifting down, returning 62 for all-ones.

## 3.6 Host-neutral-looking C whose correctness depends on the host CPU

A class of its own, distinct from "unguarded inline asm", because nothing about the source hints at a host
dependency. Two instances, both in `src/translator/guest/x86_64/avx.c`, both found only by differential
testing against native hardware:

- **`sse_round_d`/`sse_round_f` used `__builtin_rint` for the MXCSR-controlled rounding mode.** Without
  `-msse4.1` there is no `ROUNDSD` to expand to and GCC emits no libm call either; at `-O2` it expands the
  builtin inline as `|x| + 2^52 - 2^52` with the sign re-applied — i.e. it rounds the **magnitude**. That is
  valid only under round-to-nearest, which `-fno-rounding-math` (the default) entitles GCC to assume. So
  under round-toward-negative-infinity a negative operand's round-*down* became round-toward-*zero*: −2.5
  gave −2.0 where hardware gives −3.0. On AArch64 the identical builtin lowers to `FRINTX`, which genuinely
  reads `FPCR.RMode` — so **the JIT does not have this bug, and it is not a missing step**. This is what
  `fpedge` was failing on.
- **`avx_f32_to_f16`'s MXCSR-controlled arm used `fegetround()`**, and glibc's x86-64 `fegetround` reads the
  **x87 control word** — a different register from the guest's MXCSR, which `LDMXCSR` writes and `FLDCW`
  does not. No test covers it. Latent landmine, now closed.

The lesson for a future host: `grep` for inline asm is not sufficient. A `__builtin_` whose lowering is
mode-sensitive, or a libc call that reads a control register, is equally host-specific and completely
invisible to that grep.

## 3.7 `visibility("hidden")` is not local linkage

Worth stating because it cost a link failure and the distinction is easy to get wrong. The dual activation
archive links **both** target objects into one binary, and `src/core/target/namespace.h` — which renames the
per-ISA symbols precisely so the two can coexist — does **not** cover `run_block`/`block_return`.

The JIT gets away with that only because its trampolines come from a file-scope `__asm__` block with
`.hidden`, which GCC emits as a genuinely **local** symbol: `nm` on either aarch64 target object shows a
lowercase `t`. A C definition marked `__attribute__((visibility("hidden")))` is still `STB_GLOBAL` — hidden
visibility governs export from a shared object, not linkage within a static link. So two backends each
defining one collide, the linker then rejects the whole object, and it cascades into ~300 undefined
ARM64-emitter references from `lower/*.c`. Both interpreters are now `static`.

## 3.8 Pre-existing checkpoint/restore defects (fixed here)

Found because the same five cases failed on **both** guest ISAs, which is impossible for a bug in either
interpreter. Full detail in the commit; the important one for anyone running an AArch64 host:

**The guest-PROT_NONE ledger is captured with the wrong predicate, and it is host-neutral.** Capture set
`reg.is_gna = gna_hit(addr, 1)` — "does this region's *first page* happen to be guest-PROT_NONE" — while
restore answers the same question with `gna_add` over the **whole** region. glibc's `allocate_stack` mmaps a
pthread stack readable and then mprotects its lowest page into the guard page, so **every thread stack
begins PROT_NONE**, and restore poisoned the entire 8 MiB. Any syscall handed a pointer into it returns
`-EFAULT`, and `pthread_join`'s `futex` on `pd->tid` — which lives at the *top* of the block — is exactly
such a call; glibc treats `EFAULT` there as unrecoverable and calls `futex_fatal_error()`. **This should be
failing on aarch64 hosts too. Somebody with one should confirm it.**

The other three are Linux-host-specific rather than amd64-specific, so they would hit an aarch64 **Linux**
host identically: a shim kqueue's identity not following its descriptor across restore's `dup2` relocation
(three missed sites); `SO_RCVBUF`/`SO_SNDBUF` not round-tripping because Linux stores twice what
`setsockopt` is given and reports the doubled value, so every checkpoint generation doubled the buffer; and
a queued-epoll placeholder that was never hoisted into the private descriptor band, landing on fd 4 — a
child's socketpair endpoint — which a later seeds-close then destroyed.

## 3.9 Two golden files that cannot be met

- **`tests/compat/isa/x86_64/expected/isa-regress.out` differs from real x86-64 hardware on 107 lines.**
  Every difference is two-NaN-operand selection or NaN sign (`subps-nan2` golden `7fc00001` vs hardware
  `ffc00001`; `subps-nan3` taking src2's NaN where hardware takes src1's; the whole
  `addpd/mulpd/haddpd/hsubpd/addsubpd` NaN block). The interpreter is byte-identical to running the fixture
  natively on this host, so the **golden** is what disagrees with the hardware — it was evidently captured
  from the AArch64-hosted JIT or an emulator and encodes ARM NaN semantics. Consequence:
  `compat.isa-x86-64` is **unreachable** on an amd64 host, not merely unimplemented. Regenerating it on real
  x86-64 hardware is the fix, and doing so would expose a genuine two-NaN-selection gap in the AArch64 JIT.
- Related JIT defects found the same way, not fixed here: **`FCOMI`/`FUCOMI` are wrong for unordered
  operands** (a bare ARM `FCMP` sets `NZCV=0011` for unordered, so x86 `CF` comes out 0 and `ZF` 0 where
  both must be 1, and `PF` is never written), and **x87 `C1` is not modelled at all** — a stale `C1` left by
  glibc's `fmod` FPREM loop surfaced in an unrelated `FNSTENV` hundreds of instructions later.

## 4. Documentation that misleads

These cost real time on this task and will cost it again.

| Where | Claim | Reality |
|---|---|---|
| `DOCS.md:169` | *"Lower IR to the selected host CPU"*, present tense | Describes the bootstrap intent (§3.1), not the code. Nothing lowers to IR. |
| `DOCS.md:444` | *"`x86_64-linux` is first class today"* | The build did not compile on x86-64 Linux: `src/host/native_context.h:32` `#error` was reached by `hl-translator`, so every configure, every unit test and `packages.x86_64-linux.default` failed. |
| `DOCS.md:414-415` | The toolchain table calls `cmake/toolchains/x86_64-linux.cmake` *"x86_64 guest fixtures"* | The file's own header says host archives. It is a host toolchain file. |
| `DOCS.md:487` | *"On a Linux/AArch64 host:"* for the production lanes | Now conditional on the host CPU. |
| `docs/arch.md` §4, *"Seams that gate a new host platform"* | Lists host selection (`target/native.h`, `activation.c`), compat shims, ISA routing, and the build | **These are the wrong seams.** Host selection is OS-only and already passed on x86-64 unchanged; `native_compat.h` is entirely CPU-neutral. The seams that actually gate a new host are the `run_block`/`block_return` trampolines, the register model, `ibtc_publish`, and the fact that neither frontend has a non-ARM64 back end. None of those are listed. |
| `packaging/embedded/README.md:5-6` | Hardcoded `linux-aarch64` / `macos-aarch64` output paths | Now derived from `HL_HOST_ARCH`. |
| `src/translator/guest/aarch64/abi.h:29` | *"the NATIVE audit arch"*; *"aarch64's ABI numbers are already the native ones"* | "Native" here means "canonical", and conflating the two is the same elision as 2.4. |

---

## 5. Architectural observations

Not bugs. Things a later reader should know before extending this.

1. **"Host platform" was two axes wearing one name.** `src/host/<os>/`, `CMAKE_SYSTEM_NAME` and
   `hostBackends` all name the host **OS**. Nothing named the host **CPU**, because there was only ever one
   answer. This branch names it: `HL_HOST_CPU_*` / `HL_HOST_ARCH` / `hostCPUs`. Every future host is a
   composition of the two, and Windows/amd64 in particular reuses the x86-64 CPU work with a new OS backend.
2. **`linux_abi/x86.c` is named for the guest ISA but carries host-ARM64 code.** Its `nonpie_fixup`,
   `lse_align_fixup` and `ldapr_align_fixup` decode the faulting **host** ARM64 instruction word
   (`*(uint32_t *)HL_HOST_UC_PC(uc)`, then `PC += 4`) to emulate the load the JIT emitted. That is host-CPU
   code in a guest-ISA-named file, and the `+= 4` fixed-width advance is unportable by construction. Now
   guarded; it should eventually move.
3. **Three ISA vocabularies now coexist** and are easy to confuse: `HL_GUEST_ISA_*` (`config.h`, what
   program are we running), `HL_HOST_ISA_*` (`codegen.h`, a runtime codegen-target value used for cache
   identity), and `HL_HOST_CPU_*` (`host_cpu.h`, compile-time host CPU). A static assertion in
   `identity.c` ties the last two together. A future cleanup could merge the middle one away with the IR.
4. **`struct cpu` is a three-way ABI**: baked offsets in emitted code, literal offsets inside the
   `run_block`/`block_return` assembly, and the checkpoint format (`sizeof(struct cpu)` is written into the
   image and validated on restore). Two `_Static_assert`s in `cpu.h` encode AArch64 `ldr/str` imm12 limits.
   The interpreter backends share the layout unchanged **on purpose**, so checkpoints stay compatible
   between backends; the cost is carrying `host_save[12]`/`host_v[16]`, which are AArch64 callee-saved
   slots an interpreter never uses.
5. **`G_OWN_TRAMPOLINES` is the real extension point** and predates this work. The dispatcher only ever
   calls `run_block(c, code)` and reads `c->reason`; nothing requires `code` to be machine code. That is why
   an interpreter backend is additive rather than invasive, and it is where a Stage-2 transliterator plugs
   in too.
6. **`sigframe_capture_fault` is per-backend, not per-guest-ISA.** The JIT reconstructs guest state from the
   host register file; an interpreter's `struct cpu` is already authoritative and its PC already exact. The
   JIT's most delicate fault machinery — `jit_instruction_guest_pc`, the folded-fault `mscratch[4..7]`
   replay — has no interpreter analogue because the problem does not arise.

---

## 6. Merge guidance

### What is safe
Almost all of it is **additive**: new `#if` arms whose AArch64 branch is the pre-existing code verbatim, plus
new files that no AArch64 build compiles. No existing line of ARM64-host logic was moved or rewritten.

**Verified**: cross-compiling with `cmake/toolchains/aarch64-linux.cmake` builds 235/235 targets with zero
errors and produces genuine ARM64 binaries for both production engines with the JIT trampolines present.
That exercises the `HL_HOST_CPU_AARCH64` arm of every guard added here. **Run this before merging** — it is
about a minute and it is the only thing that checks the arm CI does not:

```sh
nix develop --command bash -c '
  cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake &&
  ninja -C build-arm-check -k 0'
```

### Where behaviour genuinely changes on an existing host
Two places, both stated in their commits. Do not let these slip past review as "amd64-only":

- **1.3 / mem.c**: also changes an **aarch64 Linux host configured with 4 KiB pages**, which was already
  supported. Same bug, same corrective direction — the `~0x3fff` mask was equally wrong there. 16 KiB hosts
  (macOS arm64, 16 KiB aarch64 Linux) are bit-identical.
- **1.1 / cache identity**: any existing persistent cache directory is invalidated, because the key now
  includes the real host ISA. Warm caches go cold once. That is the point.

### Conflict hot spots
Files most likely to collide with concurrent work, and why:

- `src/core/target/{aarch64,x86_64}.c` — the host-CPU fork was inserted here. Both are ~1200-line unity TUs
  that everything else is `#include`d into, so any other change to them lands near this one.
- `src/host/native_context.h` — rewritten from a two-arm `#if` into a total OS×CPU matrix.
- `src/core/dispatch.c` — the trampoline guard was restructured (1.6).
- `src/translator/cache.c` — `ibtc_publish` gained an arm; the arena alignment request became dynamic.
- `flake.nix` — `canRunGuests` was **renamed** to `hasCrateArchive` because the two ideas were only
  accidentally the same. A merge that reintroduces `canRunGuests` will silently re-gate the Rust outputs.

### What is NOT done
See the host table in `README.md`, which is deliberately conservative. The interpreter backends are the
remaining work; `production.smoke-x86_64` and `production.smoke-aarch64` are the milestones. Do not mark
the host "Supported" until the exact-golden compat matrices pass for both guest ISAs, which is what that
word means everywhere else in this repo.

Also outstanding, and tracked rather than hidden:

- `checkpoint.x86_64.threads`, the one remaining checkpoint failure, needs a **design decision**: the
  x86-64 guest pins only its main stack, so anonymous guest mmaps -- which is what glibc pthread stacks are
  -- get kernel-chosen host addresses, and a re-forked child restores after engine init when those VAs are
  no longer reliably free. The aarch64 guest is immune only because its mmaps live in a biased window the
  host never allocates from. It needs a reserved VA window for x86-64 anonymous guest mmaps, the way
  `HL_CHECKPOINT` already pins the main stack. Deliberately not patched.
- Nested engine-in-engine (an amd64 host interpreting the **AArch64 build of hl-engine**, which in turn
  JIT-compiles an x86-64 guest) gets as far as the inner engine reaching `main`, parsing argv, writing its
  usage to stdout and exiting 2 -- so the loader, libc startup, TLS and stdio all work through the
  interpreter on a dynamically-linked PIE. With an inner guest it fails at `HL_STATUS_RESOURCE_LIMIT` during
  the inner engine's own setup. Note the obvious suspect has been **ruled out**: a bare guest doing
  `memfd_create` + `ftruncate(64 MiB)` + both `mmap` aliases + a `PROT_NONE` over-reserve + `MAP_FIXED` RWX
  behaves identically to native under the interpreter.
- `lower/*.c` is still worked around with 21 aborting stubs rather than fixed; see section 3.3.
- The `perf-linux` and per-case-timeout items are handled: `perf-linux` is record-only on this host and all
  six runners now scale their per-case budget from `HL_MATRIX_TIMEOUT_SCALE`. Neither weakens the aarch64
  lanes, which are byte-identical.
- `isa-fuzz.aarch64-*` needs an ARM64 host as its differential oracle, so half the fuzz coverage is
  host-conditional. A green `isa-fuzz` on an amd64 host is **not** full coverage — on that host
  `isa-fuzz.x86_64-*` becomes the native-oracle lane for the first time, which is a gain, but the aarch64
  half is absent.
