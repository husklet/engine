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

> **Resolved on this branch: all of it was deleted.** `include/hl/{ir,codegen}.h`, `src/translator/codegen.c`,
> `src/translator/ir/`, both `src/translator/host/<cpu>/codegen.c` + `<cpu>_codegen.h`, and the two unit tests
> that were its only callers (`unit.codegen`, `unit.ir`; 115 unit tests → 113). The "do not delete it" reasoning
> below was an ABI argument, and this library has never been published, so there is no ABI to preserve — while the
> misdirection the rest of this section documents was a recurring, measurable cost. `HL_HOST_ISA_*` survived the
> deletion and moved to `src/translator/identity.h`, cache identity being its only live consumer; the
> `_Static_assert` pinning it to `src/host/host_cpu.h` moved with it. `src/translator/reloc.c` and
> `src/translator/host/aarch64/asm.{c,h}` were untouched — both are live. **The analysis below is retained as
> written, because why the code existed and why it read as load-bearing is the part still worth knowing.**

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
  misleading file in the tree for this task. (Resolved with 3.1: the backend is gone and the directory now
  holds only a README saying it is deliberately empty and why.)
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

## 3.10 What the corpus actually scores, and how the harness hid it

A corpus-wide survey (24 manifests, 1580 active cases, **3013 (case, guest-ISA) runs**, complete coverage —
nothing skipped for privilege, rootfs or network) measured, on the binaries of the day:

**2632/3013 = 87.4% overall. aarch64 guest 1282/1496 = 85.7%. x86-64 guest 1350/1517 = 89.0%.** Zero
cross-ISA stdout divergences among cases passing on both. Fully green on both guest ISAs: `syscall-edges`
(104/104). Green on the x86-64 guest only: `ipc`, `signals`.

**Superseded by a second sweep after the fixes landed.** Same method, same completeness, pinned binaries,
one `matrix_runner` invocation per case so both legs always run:

**2993/3013 = 99.34%. aarch64 guest 1488/1496. x86-64 guest 1505/1517.** Again zero cross-ISA stdout
divergences and zero host-resource-restoration failures. **18 of the 24 matrix suites fully green on both
guest ISAs**; with the five x86-64 encodings of `2b926d2f` it is 20, leaving `completeness`, `core-regress`,
`process` and `procfs`. The residue is 16 cases / 20 legs: six engine defects (§3.12), three unadvertised
aarch64 extensions, five encodings since fixed, and two environmental.

**There are no hangs.** The stall detector never fired and no case reached its budget. The slowest correct
cases are `soak/callgraph` 1095 s, `core-syscall/times` 939 s, `core-workload/allocchurn` 866 s — so a
triage run capped below ~1100 s manufactures false timeouts. The x86-64 futex-across-fork hangs recorded
below were the non-PIE `LEA` bug (§3.11) and are gone.

Producing that number required **patching the harness**, and the reasons are defects in their own right:

- **`tools/matrix_runner.c:1063` short-circuits the second ISA.**
  `if (!case_failed && (isa == ISA_X86_64 || isa == ISA_BOTH) && run_one(...))` — an `ISA_BOTH` case that
  fails on aarch64 never runs the x86-64 leg, and the cross-ISA byte-identity comparison is skipped with it.
  One lane run therefore cannot produce per-ISA numbers, and the axis it hides is exactly the one that
  matters on a host where the two backends have diverged.
- **`tools/linux_matrix.c --suite` is fail-fast** where `matrix_runner` continues. The
  `production-full-{aarch64,x86_64}` lanes (21 suites each) use it, so each reports only its FIRST failing
  case — which is why broad `-L production` sweeps were so uninformative. It also silently skips every row
  with argv/env/rootfs and reports "N active cases passed" without saying how many it skipped.
- **The hang detector no longer detects hangs.** `matrix_runner`'s base budget is **120 s** (not the 20 s
  this document previously claimed — that is `linux_matrix`), so scale 30 gives **3600 s per case**, and
  CI's `compat-soak` override gives **5 hours per case** here; the ctest `TIMEOUT` reaches 30 h per suite.
  Scaling was the right fix for slow-but-correct being called a hang, but nothing now bounds a real one.
  The x86-64 futex-across-fork cases that motivated this entry (`threads/futex-fork-stale-waiter`,
  `process/{fork-blocked-io,fork-child-futex,shared-key-futex}`) turned out to be the non-PIE `LEA` bug of
  §3.11 and now pass; the harness observation stands regardless, since nothing would have caught them.
- `matrix_runner.c`'s `CASE_MAX = 256` has no bound check while `completeness/manifest.tsv` is at 167 rows;
  overflow reports *"invalid manifest row"* — a parse error for a file that parses fine.
- `HL_MATRIX_SCRATCH_DIR` is set only by the workflow, not by CMake, so a local `ctest -L compat-syscall` on
  an ext4 build tree fails `syscall/memfd-seals` on both ISAs for reasons unrelated to the engine.

**And the claim that gates it all is now false.** `cmake/CiLanes.cmake` omits `Linux-x86_64` from
`HL_CI_COMPAT_HOSTS` because *"the engine cannot yet EXECUTE guests on an x86_64 host … a compat shard there
would fail every case rather than measure anything."* It passes 99.34%. That comment is why
`.github/workflows/linux-x86_64.yml` runs only `ctest -L unit` — so **none of those 3013 runs is gated by CI
on this host today.** `docs/ci-green.md` repeats the same false premise.

Three correctness bugs the survey flagged loudly were fixed before it reported, because it measured pinned
binaries from an earlier commit; all three are verified byte-correct against native on current binaries:
the x86-64 `PACKSSWB`/`PACKUSWB`/`PACKSSDW` high-half corruption (non-deterministic, ~27 runs across 12
suites), the aarch64 `MOVI Vd.2D` per-bit-to-per-byte expansion, and the `/proc/self/maps` engine SIGSEGV.
That is a caution about pinning, not about the survey: pinning was correct and necessary, because
concurrent rebuilds otherwise make a multi-suite sweep measure several different binaries.

## 3.11 The non-PIE rip-relative address rewrite is a per-backend obligation, and nothing enforces it

The sharpest bug found on this branch, and the one most worth reading before writing another backend.

A non-PIE `ET_EXEC` is mapped HIGH (the low 4 GB is reserved) and the dispatcher biases the guest PC to that
high mapping. But the image's own **baked absolute pointers are LOW** — a non-PIE has no dynamic relocations,
so the linker's addresses are final. Any instruction that *materialises* an address from the PC therefore has
to produce the LOW value, or it will not compare equal to a pointer the image stored at link time.

The AArch64 side has always known this: `guest/aarch64/translate.c`'s `pcrel_base()` un-biases `ADR`/`ADRP`,
with a comment about gcc ICEing when an `adrp`-computed pointer fails to match a baked one. The x86-64 JIT
learned it too — `guest/x86_64/lower/mov.c` rewrites rip-relative `LEA`, added when glibc's thread exit
compared a tcache pointer against `lea __tcache_dummy(%rip)`, and its comment calls itself "the exact
analogue of the aarch64 engine's adr/adrp PC un-biasing".

**The x86-64 interpreter did not have it, and the symptom was a deadlock rather than a wrong number.**
glibc's `__malloc_fork_lock_parent` walks the arena list with `lea main_arena(%rip),%r12` as the loop
sentinel and `main_arena.next` — a baked LOW self-pointer — as the cursor. HIGH never equals LOW, so the
single-arena loop never terminated: it took `main_arena.mutex`, came round again, found it already 1, and
parked in `__lll_lock_wait_private` **on a lock it was itself holding**. Nothing wakes that. Every `fork(2)`
from an x86-64 non-PIE guest that had ever started a thread hung at zero CPU, which is three whole compat
suites — and every fixture in the corpus is built `-static`, i.e. non-PIE.

Two things generalise:

- **Address *materialisation* is un-biased; rip-relative *accesses* stay biased.** Only the value handed to
  the program changes. Getting that backwards breaks memory access instead of pointer comparison.
- **`call_return_pc` / `interp_call_return_pc` is the sibling obligation** and is duplicated the same way.
  A new backend must implement both, and there is no test that fails loudly if it implements neither —
  it fails as a hang in glibc, several frames from the cause.

For diagnosis: the signature is a process at `State: S`, `/proc/<pid>/wchan` = `futex_do_wait`, and
**`00:00:00` CPU time over many minutes**. A spin or a lost update burns CPU; this does not. That distinction
is what separates "the interpreter is slow" from "the interpreter deadlocked", and no amount of timeout
scaling addresses the second — which is why the harness now has a progress-based stall detector rather than
a larger number.

## 3.12 The residue after the second sweep

Every one of these has a minimal reproducer; none is a mystery. Ranked by cases unblocked, except that an
engine crash outranks a wrong answer.

| # | Defect | ISA | Cases |
|---|---|---|---|
| 1 | **`/proc/self/maps` omits the guest ELF image entirely** — no `r-xp` text row, no `r--p` RELRO row, where native shows the full `r--p`/`r-xp`/`r--p`/`rw-p` set with a pathname. Reaches far past the two fixtures: libunwind, `backtrace()`, ASan, V8's free-range scan and jemalloc's arena probe all parse this file. Follow-up to `88d19298`. | both | 2 (4 legs) |
| 2 | **The x86-64 interpreter SIGSEGVs on repeated async signal delivery** — `sigaction` + a 2 ms repeating `setitimer` + a long arithmetic loop, 10 lines, no inline asm, `wait=0x8b00`, deterministic 3/3. A guest-triggerable engine kill. | x86-64 | 1 |
| 3 | **Non-PIE `mov r64,imm32` materialisation is not rebased** — `same_half=0`. The exact sibling of §3.11's `LEA` obligation in the opposite direction: `LEA` must un-bias DOWN, this must stay HIGH. The JIT does it in `lower/mov.c`; the interpreter does not. §3.11 predicted this class would recur, and it did. | x86-64 | 1 |
| 4 | **`CRC32` with a high-byte source** — `crc32 %ah, %r32` gives `968c7e88`, hardware `86d2b9e7`. Every other line of a 14 kB output matches, so it is the `%ah/%ch/%dh/%bh` decode, not CRC32. | x86-64 | 1 |
| 5 | **`MRS` of an inaccessible ID register does not trap** — HWCAP bit 11 (CPUID emulation) is clear, so `mrs x, id_aa64isar0_el1` must SIGILL; the interpreter returned a value. **Fixed** (`23afa33e`): raises a guest `SIGILL`/`ILL_ILLOPC` with the PC on the instruction, as an architectural trap rather than through `interp_undefined`. | aarch64 | 1 |
| 6 | **BF16 / DotProd / I8MM unimplemented** — **fixed** (`23afa33e`), and my description of the cause was wrong; see §3.13. | aarch64 | 3 |

Two failures are **environmental, not engine defects**, and are now documented in `docs/ci-green.md` under
"Host-environment preconditions the harness does not enforce":

- `completeness/priority` and `process/sched-attr` need **nice ≤ 5**. Verified by running the x86-64
  fixtures *natively* at nice 12: they fail identically to the engine (`priority set=0 nice=12`,
  `sched_attr ok=0`), and `RLIMIT_NICE` is 0 here so they cannot recover. One genuine divergence hides
  underneath and is not fixed: at nice 12 the engine prints `set=1` where native prints `set=0`, i.e.
  `setpriority` reports success where the kernel returns `EACCES`. Host-neutral, invisible at nice 0.
- `syscall/memfd-seals` needs `HL_MATRIX_SCRATCH_DIR` on tmpfs, which only the workflow sets. Re-verified
  both ways.

### 3.13 The feature-advertisement contradiction, and how I mis-stated it

**Corrected.** I wrote here that the engine advertised three aarch64 features it did not implement. It did
not. `AT_HWCAP = 0x1fb` is bits 0,1,3-8 — FP, ASIMD, AES, PMULL, SHA1, SHA2, CRC32, ATOMICS — and contains
no `HWCAP_ASIMDDP`; `AT_HWCAP2`, where `HWCAP2_I8MM` and `HWCAP2_BF16` live, was absent entirely. `elf.c`
was telling the truth. The outlier was `tests/compat/completeness/manifest.tsv`, which carried three
`active` rows with hardware-derived goldens for instructions the interpreter aborted on. The real defect was
smaller than I claimed and in a different file.

One genuine ABI defect did fall out of it: **an absent `AT_HWCAP2` is a shape no kernel produces.** arm64's
`create_elf_tables` emits it unconditionally. Now emitted explicitly with value 0.

The rule now recorded in `guest/aarch64/cpu.h` is the durable part: **a HWCAP bit is set only when both
backends implement the whole feature exactly.** Applied per feature, it gives three different answers —
DotProd advertised (mandatory from Armv8.4-A, so the transliterator always lands on silicon that has it, the
same assumption the already-advertised LSE/AES/SHA2 bits make); I8MM implemented but not advertised (Apple
Silicon before M4 lacks it, so the JIT falls to a baseline lowering covering 3 of 6 forms); BF16 with BFCVT
implemented and **BFDOT left an honest gap**, because with `FPCR.EBF=0` `BFDotAdd` computes in
**round-to-odd**, the soft-FP has no such mode, and RNE would be subtly wrong. That last one independently
confirms `translate.c`'s own note that its BFDOT lowering is not bit-exact.

No host-CPU-keyed harness disposition was needed in the end, because implementing all three made the rows
pass on both hosts. §3.12's entry 6 is closed.

### 3.14 The whole AdvSIMD vector × indexed-element box was unimplemented

Found in passing, and **far bigger than the three named features**. The aarch64 interpreter implemented none
of the "AdvSIMD vector x indexed element" encoding group — indexed `FMLA`, `MLA`, `MUL`, `SQDMULL`,
`SQRDMULH` and the rest. Confirmed with a hand-built binary: `0x4f9e8bff` aborts.

This is **baseline Armv8.0** that any compiled NEON code uses constantly. The 3012-run scan missed it
because no fixture in the corpus reaches it — which is the sharpest available illustration of what that scan
does and does not prove: it measures the corpus, not the ISA. The box is now open for the dot products only;
everything else in it still reports honestly.

### 3.15 x86-64 guest auxv defects, found during the aarch64 audit

`src/linux_abi/x86.c`:

- **`AT_HWCAP` is hardcoded 0**, where a real kernel puts `CPUID.1:EDX`. Masked in practice because glibc
  special-cases `getauxval(AT_HWCAP)` to return `GLRO(dl_hwcap)` from CPUID directly — which is why the
  `auxval` fixture still reads `hwcap_nz=1`. Anything parsing `/proc/self/auxv` sees 0.
- **`AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID` are hardcoded 0** while the aarch64 path uses `cuid()`/`cgid()`.
  Live divergence: on this host an aarch64 guest sees 1000 and an x86-64 guest sees 0. The x86 side carries
  a "container root" comment, so it reads as deliberate rather than a slip — but it is guest-visible and the
  two guest ISAs disagree.

And in `src/linux_abi/container/vfs.c:4998`: **`/proc/cpuinfo` reports `Features: fp asimd` only**, not
derived from the CPU model, so it omits the aes/pmull/sha1/sha2/crc32/atomics the engine *does* advertise in
`AT_HWCAP`. That contradicts `cpu.h`'s claim that every discovery surface derives from one model.

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

- `checkpoint.x86_64.threads` is still the one remaining checkpoint failure (77/78), and it needs a
  **design decision**. Note the non-PIE `LEA` fix in section 3.11 did NOT close it, although that fixture
  forks after threading and so looked like a candidate: it still fails, consistently and in 2.2 s rather
  than by timeout. The original diagnosis stands: the
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
