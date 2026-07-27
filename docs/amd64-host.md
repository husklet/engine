# The x86-64 Linux host

One document for the whole of this work: the design, what it measured, every defect it turned up, what is
still missing, and what a reviewer or a later agent needs in order to merge it without re-deriving any of it.

`DOCS.md` is normative. This file is the record.

Every number here was measured on this branch, not estimated. Where something was not measured, it says so.
Line numbers drift; claims were checked against the tree.

---

## 1. Status

| | |
|---|---|
| Compat corpus | **3031/3036 = 99.84%** — 24 manifests, both guest ISAs, per case, nothing sampled |
| `checkpoint` / `checkpoint-io` / `ckpt-cross` | **82/82**, **34/34**, **11/11** |
| `nested` (engine-in-engine) | **5/5** |
| `unit` | 114/114 · `package` 7/7 steps · `emulated.isa-x86-64` 8/8 |
| Build | 0 errors; the aarch64 cross build is 0 errors |

Both guest ISAs execute on an x86-64 Linux host. The engine hosts *itself* — an amd64 host interpreting the
AArch64 build of hl-engine, which in turn JIT-compiles an x86-64 guest — and that is a gate, not an
experiment.

The corpus figure was measured in an **isolated worktree**, HEAD plus one diff, because four agents were
mutating `src/` concurrently and a shared-tree measurement cannot be attributed. Of the five residual legs,
four are environmental (a `nice ≤ 5` precondition the harness does not enforce; the fixtures fail *natively*
under the same condition) and one is a real defect, listed in §7.

**The host is still not "Supported"** in README's sense, and should not be marked so until the exact-golden
compat matrices pass for both guest ISAs on a machine CI controls. See §7.

---

## 2. What was actually missing

The obvious reading of "add an x86-64 Linux host" is that `src/host/` gains a backend. That reading is
wrong, and being precise about why determines the size of the work.

`src/host/<os>/` implements `hl_host_services` — files, processes, clocks, memory, sync. That layer was
already host-CPU-neutral: `src/host/linux/` compiles and passes its provider tests on x86-64 unchanged. The
host CPU does not appear there at all, apart from one `cntfrq_el0` read.

What is host-CPU-specific is the **translator**, and both production frontends emitted ARM64 directly:

- `src/translator/guest/x86_64/` is a complete x86-64 frontend whose back end is ARM64.
- `src/translator/guest/aarch64/` is not a translator at all. Its own header calls it *"the aarch64-Linux →
  arm64-host transliterator. Same-ISA: copy most instructions verbatim; MANGLE only stolen-register
  users."* Its default case writes the guest instruction word straight into the host code cache. There is
  no decoder, because on an AArch64 host it does not need one.

So on an x86-64 host, neither guest ISA had a back end, and the aarch64 guest did not even have a front end.
This was new code generation, not a port.

### 2.1 Three axes, not two

`DOCS.md` §1 lists guest OS, guest ISA, and host platform. "Host platform" is two axes and only the first
had a name. Both do now, once per language:

| axis | C | CMake | nix |
|---|---|---|---|
| host OS | `src/host/<os>/`, `__APPLE__`/`__linux__` | `CMAKE_SYSTEM_NAME` | `hostBackends` |
| host CPU | `HL_HOST_CPU_*` (`src/host/host_cpu.h`) | `HL_HOST_ARCH` | `hostCPUs` |
| guest ISA | `HL_GUEST_ISA_*` | per-lane `hl_linux_production()` | `guestISAs` |

Use `HL_HOST_CPU_*` rather than compiler predefines. They are spelled differently per compiler
(`__aarch64__` vs `_M_ARM64`), and a bare `defined(__x86_64__)` says nothing about which OS's context layout
and calling convention apply — which is exactly how an Apple-shaped `uc_mcontext->__ss.__rip` came to sit
under a plain `#elif defined(__x86_64__)` in `linux_abi/signal.c`, unable to compile against a Linux
`ucontext_t`.

A guest ISA equal to the host CPU permits same-ISA transliteration. It is never a reason to conflate the
two.

---

## 3. The seam that makes it additive

The whole of `core/dispatch.c`'s contract with a backend is two calls:

```c
code = translate_block(G_PC(c));   /* produce something callable for this guest PC */
run_block(c, code);                /* call it */
/* on return: c->reason says why it stopped, G_PC(c) is the next guest PC,
   and every piece of guest architectural state is back in *c */
```

Nothing requires `code` to be host machine code, and `G_OWN_TRAMPOLINES` already existed as the escape hatch
for a backend whose register model differs. A backend that *decodes and executes* satisfies the same
contract, and everything on the other side is reused verbatim: the dispatcher, the block cache and its
STW/generation machinery, all of `src/linux_abi`, and checkpoint/restore.

Both target translation units fork on the host CPU and nothing else about them changes:

```c
#if defined(HL_HOST_CPU_AARCH64)
#include ".../host/aarch64/asm.h"   /* + the e_* wrappers */
#include ".../guest/<isa>/translate.c"
#else
#include ".../guest/<isa>/interp.c"  /* + translit/ on the x86-64 diagonal */
#endif
```

**`struct cpu` is deliberately shared** rather than specialised per backend. It *is* the checkpoint format —
`sizeof(struct cpu)` is written into the image and validated on restore — so one layout is what lets the two
backends read each other's guest state. The interpreter consequently carries `host_save[12]` and
`host_v[16]`, AArch64 callee-saved slots it never uses. That waste buys a stable image format, and
`ckpt-cross` (§5.3) proves the property it exists for.

The dispatch seam forks the same way: `guest/<isa>/dispatch.h` belongs to the JIT and patches ARM64 branch
encodings; `interp_dispatch.h` is its interpreter counterpart; `abi.h` above them is pure guest ABI and is
shared. The seam is per *(guest ISA, host CPU)*, not per guest ISA — visible only once a second host CPU
existed.

---

## 4. The four backends

|  | **ARM64 host** | **x86-64 host** |
|---|---|---|
| **aarch64 guest** | transliterator *(pre-existing)* | interpreter |
| **x86-64 guest** | JIT — real x86→ARM64 translation | interpreter, **+ transliterator** |

The diagonals can transliterate; the off-diagonals cannot. The empty-looking cell is the bottom-left: an
aarch64 guest on an x86-64 host is cross-ISA in the direction nobody has written a backend for, so it is
interpreted and will stay so without a new frontend *and* backend. That is why the interpreter-side wins of
§6 mattered: they are the only thing that will ever speed that quadrant up.

### 4.1 Why interpreters first

Correctness. An interpreter's `struct cpu` is *always* authoritative: a guest fault lands in ordinary C with
`cpu->pc` already exact, so `signal_capture` needs no host-register reconstruction and no
instruction-boundary provenance map — the two most delicate parts of the JIT's fault path simply do not
arise. Its fault model is a thread-local marker around every guest access, `sigsetjmp` at the top of
`run_block`, and a `siglongjmp` out of the host handler once the guest signal frame is built.

### 4.2 The transliterator (`HL_X86_TRANSLIT=1`, off by default)

Same-ISA fast path for the x86-64 diagonal: the guest's instructions already *are* the host's, so copy them
into the code cache and let the CPU run them. Mirror of what `guest/aarch64/` does on an ARM64 host.

```
core/workload/busyloop    105.32 s -> 7.01 s   15.0x   100.0% host blocks
core/abi/recursion          0.95 s -> 0.08 s   11.9x   100.0%
core/abi/sortbig            7.49 s -> 1.15 s    6.5x    80.3%
core/workload/allocchurn  186.60 s -> 54.50 s   3.4x    93.5%
```

`busyloop` runs 300,005,801 blocks, so per-block is exact: **351 ns → 23.4 ns**. The fallback rate is quoted
beside every number deliberately — `allocchurn`'s 3.4× against `busyloop`'s 15× *is* its fallback rate.

**Register model.** An x86-64 host has 16 GPRs and an x86-64 guest wants all 16, so unlike the AArch64
diagonal — which steals `x18`/`x28`/`x30` and mangles the rare instruction naming one — there is nothing
free. It steals **none** and reaches `struct cpu` through the `%gs` segment base (`arch_prctl(ARCH_SET_GS,
cpu)` per guest thread), since a Linux guest's TLS lives in `%fs`. Scratch comes only from `cpu->mmscratch`;
the 128-byte red zone is never written.

Only three things are rewritten; everything else is `memcpy`. The block prologue/epilogue — flags survive
the whole spill because every spill instruction is a `mov`, so a host `pushfq` captures them, plus a `cld`
so a guest `std` cannot leak DF into host C. Rip-relative displacements, re-aimed at the RX alias. And the
guest stack accesses `CALL`/`RET` perform, written so no register is clobbered before the store can fault.

**Correctness is structural**: it falls back to the interpreter **per block**, so anything unhandled costs
speed and never correctness. Differential on every x86-64 fixture: 246/250 static-PIE and 299/300 sampled
non-PIE byte-identical, the differences being three richer `[HLFATAL]` diagnostics, one case where the
interpreter timed out and the transliterator finished, and one address-normalisation artefact in the
comparison script.

Declined today: guest `%fs`, `0x67`, all VEX/EVEX/`0F38`/`0F3A`, legacy SSE, x87, `div`/`idiv`, `cpuid`,
`rdtsc`, `pushfq`/`popfq`, memory-indirect branches, and anything at all while an emulated `MAP_SHARED` or
`PROT_EXEC` guest mapping is live. **Guest `%fs` is the largest single fallback source** — §2's design note
assumed it could stay untouched, and that was wrong: keeping it live means putting the guest FS base in the
*host's*, and every host signal handler then reads its own TLS through it with no way to restore the base
before the handler's first instruction.

Not to be assumed working: no superblocks, chaining, IBTC, persistent cache, tier-2 or fast-clock inlining.
SMC coherence is by refusal — parity with the JIT, but a step down from the interpreter, which re-decodes
and had no gap. 23× against native rather than the 0.7–0.9× predicted, and the whole gap is the block
boundary: 4.8 guest instructions per block, ~1 ns of work, with no chaining.

---

## 5. Verifying the AArch64 host from an x86-64 box

The branch's deepest structural problem was that **CI's aarch64 runners never compile the x86_64 arms, and
the x86-64 runner had never executed the aarch64 host.** Several JIT defects were written, reviewed and
merged without ever running — the MMX width bug, the NaN rule, `cvt-mmx`, and the three x87 defects of §6.

`qemu-user` is now in the flake devShell (`qemu-user`, not `qemu`: same binaries, 208 MB closure instead of
3.7 GB; `flake.lock` unchanged, since the pinned rev already serves it). Two of the three `emulated.*` cells
are CI-gated, with `emulated.abi` kept as a **green control** so total breakage and a correctly-reported
defect cannot look alike. The gate prints the qemu version on every run, so the oracle is in the CI log.

### 5.1 What emulation vouches for

Established by 3 probe programs, ~25 checks, each compiled for **both** architectures — the aarch64 build
under qemu, the x86_64 build natively as a control. An inventory of what the engine actually does came
first, and it mattered: the engine **never calls `futex(2)`** (it uses `PTHREAD_PROCESS_SHARED` primitives
in a `MAP_SHARED` table across fork) and never uses `F_SEAL_*`, so two initial probes were testing the wrong
primitive.

**Vouched for:** the dual-alias W^X arena; rewrite-through-RW-then-execute-at-RX; `mprotect` RW↔RX;
`MAP_FIXED` over already-executed pages; `mremap` of code; **cross-thread patching of code another thread is
executing** (tier-2 promotion's exact shape — 3 threads, 2000 patches, 507,786 calls, 0 wrong results);
`siglongjmp` out of handlers; `sigsetjmp` mask semantics; `uc_mcontext` including `fpsimd` via the
`__reserved` magic chain; SIGSEGV raised *inside* arena code, resumable; `sigaltstack`; pshared primitives
across fork.

Notably it is **stricter** than the x86 host on cache maintenance: without `__builtin___clear_cache`, qemu
executes stale code where native x86 does not. A forgotten flush fails rather than passing.

**NOT vouched for — and this is the load-bearing caveat: weak memory ordering.** qemu-user inherits the x86
host's stronger model, so a missing `dmb` in the IBTC/STW paths passes under it and can still fail on
silicon. This was *reasoned*, not measured — the litmus harness was never completed. Also not vouched for:
timing, host CPU feature registers, `tpidrro_el0`, execve re-entry, and the HINT space above `hint #1`
(qemu aborts on `hint #2`).

The strongest single piece of evidence is not a probe: the ISA golden captured from the ARM64 JIT on **real
aarch64 hardware** reproduces byte-for-byte under qemu across all 1048 lines, NaN cases included.

### 5.2 What it caught

Immediately, on its first gated run: `FRSTOR` (`DD /4`) not lowered on the AArch64 host at all, `FPREM`
diverging, and precision control diverging — all three shipped in a commit that fixed only the x86-host arm.
Both were proved real rather than emulator artefacts by the test the doc itself prescribes: they pass on the
native x86-64 engine and fail identically on two qemu versions.

### 5.3 Cross-backend checkpoint restore

Untested before this branch, and it is the property the shared `struct cpu` exists for: an image captured by
the interpreter and restored by the ARM64 JIT under qemu, and the reverse, both guest ISAs — **11/11**,
including a double round-trip. A byte-wise diff of the two backends' `cpu` images differs only in the
`pf`/`af` flag substrates and address-valued fields.

---

## 6. Performance

Profiled with a purpose-built `perf_event_open` sampler, because `perf`, gdb and valgrind are all absent
from this box.

Overhead is not one number: **min 3.4×, median 25.3×, max 605×** (geomean 26.6×) across 12 perf cases.
Kernel-bound work is 3–12×; guest-execution-bound work is 94–605×. `warm-cache` measures nothing here —
warm and cold agree to 0.2%, because an interpreter emits no code to cache.

Two cost centres dominated, and **neither was the one everyone assumed**:

1. **`sigsetjmp(pad, 1)` issued a real `rt_sigprocmask` syscall on every guest basic block** — 271.7 ns
   against 2.57 ns for `savemask=0`. 44% of the compute case's CPU and **99.96% of every host syscall the
   process makes** while compute-bound. Fixed: **1.85× on the x86-64 guest, 3.46× on the aarch64 guest**,
   `rt_sigprocmask` 3,035,853 → 8 per 3M blocks.
2. **Instruction fetch and validation cost 38.2% of user cycles — more than decode's 25.2%.** Fixed with a
   memo: **1.282× / 1.329×**. Cumulative with (1) on the same fixture: **12.55 s → 4.88 s = 2.57×**.

Caching decoded instructions — the obvious candidate — ranks third and remains undone, because it is the
only one that trades away SMC-coherence-by-construction.

Three assumptions were measured and discarded: dispatch round-trip is 8.5% of user cycles, not a major cost;
the guest-access bracket is ~0.36 ns and should be left alone; and `HL_MATRIX_TIMEOUT_SCALE=30` is generous,
not tight (median 20.6× over 68 fixtures; the slowest case uses 5.5% of its budget).

### 6.1 Two design notes worth keeping

**The mask restore is a debt, not a snapshot.** The proposed fix for (1) was to snapshot the signal mask per
thread, on the theory that it is invariant across the run loop. **It is not** — `syscall/signal.c` mirrors
the guest's SIGTSTP/TTIN/TTOU onto the *real* host mask for job control, persistently and between blocks,
and `thread.c`'s `hrm_fault_hook` unblocks SEGV+BUS from *inside* `run_block`. A restored snapshot would
have silently un-blocked the stop signals bash blocks around `tcsetpgrp`.

The design that works needs no invariance: **the interpreter's `siglongjmp` is a hand-rolled `rt_sigreturn`.**
Leaving a handler by long jump means the kernel never runs sigreturn, so the mask restore is a debt owed —
and `ucontext->uc_sigmask` is by definition the value sigreturn would install. The JIT owes nothing, because
it rewrites `uc_mcontext.pc` and *returns*. (`siglongjmp` had to stay: on Darwin `setjmp`/`longjmp` are the
mask-**saving** pair, so plain `setjmp` would have put the syscall back on macOS.)

**Revalidate, don't notify.** The memo for (2) was proposed keyed on the logical-VMA snapshot *pointer*.
That is unsafe: retired snapshots are `free()`d at the next quiescent reclaim and `malloc` can hand the same
address to the next publication — an ABA that makes a stale entry look *fresh*. It uses a monotonic
generation instead, and is revalidated on every use rather than notified, which collapses the invalidation
set to one element: equal generation ⟹ equal snapshot ⟹ a hit returns bit-for-bit what a fresh resolve
would. **The memo cannot be stale, only absent.**

Only the ledger-derived interval is cached; the ordinary/direct verdict is not, and `host_range_mapped` runs
per fetch. That is what makes `munmap`/`MAP_FIXED`/`mremap`/`mprotect`/`G_SMC_UNMAP` hook-free — and it is
the half still on the table, worth a further measured 1.137×. Taking it needs a generation over `g_gna`, the
bus registry and host unmap, because **`mprotect(PROT_NONE)` over an ordinary exec page calls `gna_add`
without touching the ledger or firing `G_SMC_UNMAP`** (which fires only for `PROT_WRITE`).

---

## 7. What is still missing

Ranked by what a reader should care about.

### Correctness

- **293 unallocated aarch64 encodings execute instead of trapping.** qemu SIGILLs them; the interpreter
  invents behaviour. Dominated by unallocated *scalar* spellings of three-same FP opcodes. A guest probing
  for a feature by executing an encoding and catching SIGILL gets the wrong answer — which is how libcs and
  JITs do feature detection. Needs an allocation table per opcode; the encoding sweep has produced enough
  per-encoding data to build one.
- **`hl_x86_fxsave` leaves stale bytes in its image** — FOP/FIP/FDP, MXCSR_MASK and each register slot's
  6-byte tail come back as the *caller's previous buffer contents*. Both hosts. A guest reading MXCSR_MASK
  gets garbage. This is a second, independent hole in a function already fixed once this branch (the
  register area was written in *physical* order where hardware is TOP-relative).
- **Checkpoint restore does not repopulate `g_gro`.** It MAP_FIXEDs every region back as anon RW and
  restores only `g_gna`/`anon_track`, so after a round trip `.rodata` is writable again and a store into it
  is silently dropped — re-opening the defect §8 closed on the load path. `elf_protect.h` was written to be
  exactly the second caller this needs.
- **MMX aliases XMM rather than the x87 stack** — 5,386 encodings, damage measured on both sides. Not fixed
  because the ARM64 JIT shares the model and the completeness suite enforces byte-identical output between
  engines, so one backend cannot move alone.
- **x87 FSW exception bits are projected from MXCSR** (8,300 encodings), so any SSE instruction's exceptions
  appear in `fnstsw`. Separating them means the interpreter owning an x87 exception accumulator.
- **`#D` on denormal inputs is missing for every SSE op on the aarch64 host** — *declined with numbers*, not
  overlooked: 14 fast-path instructions across ~40 sites, taking a gated packed `FADD` from 7 to 21
  instructions. `translate.c`'s own comment records the previous 16-instruction NaN gate being cut for
  exactly this reason.
- **`FNSTENV`'s FIP/FCS/FOP/FDP/FDS stay zero**, both backends, honestly. Reproducing them needs two more
  64-bit `struct cpu` fields — the format change the tag word was arranged to avoid — and the group is read
  only by 16/32-bit unmasked-`#FPU` handlers that cannot exist under this ABI. **Writing the two selectors
  beside a zero FIP would be a plausible-looking lie.**
- **PC=64's 11 bits of long-double tail** are unreachable while `ST` is a C `double`.
- `wild-highva` fails in-lane on the x86_64 guest but passes 5/5 standalone; deterministic **only** under the
  runner's ASLR-off environment.
- Known-environmental, not defects: `completeness/priority` and `process/sched-attr` need `nice ≤ 5` and
  fail *natively* under the same condition; `syscall/memfd-seals` needs `HL_MATRIX_SCRATCH_DIR` on tmpfs
  while `core-syscall/fallocate` needs it **not** on tmpfs — no single value satisfies both.

### Coverage and gating

- **The compat corpus is not gated by CI on this host.** `cmake/CiLanes.cmake` still omits `Linux-x86_64`
  from `HL_CI_COMPAT_HOSTS`. The four-step sequence is written at the decision site; the part easy to get
  wrong is that **declaring the token alone turns I20 off**, leaving that workflow with no structural guard.
- **`emulated.completeness` is now gateable** (1 of 183, `priority`, on both ISAs) and should be wired.
- **No fixture yet for the AVX fault bracket, the gathers, or XSAVE.** The native oracles exist and are
  validated; converting them is mechanical. One constraint already worked out: print the written-range map
  and `XSTATE_BV`, **never** MXCSR_MASK or FIP/FDP — those are CPU-dependent (`0x0002ffff` on AMD,
  `0x0000ffff` on Intel) while the range map is not.
- **The AArch64 host has none of the `0F AE` work**: `translate.c` treats `sub >= 5` as `dmb` regardless of
  `is_mem`, so XRSTOR and XSAVEOPT are silently no-ops there.
- `isa-fuzz.aarch64-*` needs an ARM64 host as its differential oracle, so a green `isa-fuzz` on an amd64
  host is **not** full coverage.
- The `.rodata` fixture is registered `-static` only; `-static-pie` and dynamic were verified by hand.

### Performance

- Decode caching (§6) — the largest remaining item, and the one that trades away SMC coherence.
- The direct-validity half of the fetch memo — a further measured 1.137×, blocked on a generation over
  `g_gna`.
- Transliterator: guest `%fs`, then chaining/IBTC. 23× off native today, and the gap is the block boundary.
- `HL_PCACHE` loses block revivability for non-PIE on Linux (`pcache_note_fixed_img` filters on `base >=
  4 TB`). Perf only, verified identical cold/warm.
- **x87 lowering is now considerably larger per instruction** on the AArch64 host — tag RMW, `#IS`
  predicate, an `FPCR` save/restore around every arithmetic op. `msr fpcr` is a pipeline flush on real
  silicon and no aarch64 perf lane runs here, so this is **untested**. The cheap win, if it matters, is a
  runtime skip when `FCW == 0x037f`.

### Not done, deliberately

- **The reciprocal-estimate tables rest on qemu alone.** The ARM ARM text could not be retrieved, so the
  transcription is corroborated by exhaustive measurement (640 entries via six independent instruction
  paths) rather than by a second reading of the specification. Note the usual "qemu reproduces the
  hardware-captured golden" argument does *not* apply: that golden contains no `frecp*` case.
- **Everything aarch64 in this branch never ran on silicon.** The differentials prove *interpreter ≡ qemu*,
  not *interpreter ≡ ARM*. The fixtures self-check against in-fixture ARM ARM transcriptions, so each
  becomes a genuine silicon differential the moment someone runs it on hardware. **That is the first thing
  to do on an aarch64 host.**
- **Memory ordering is untested by construction.** The interpreter emits the fences and lowers guest atomics
  to host `__atomic` at SEQ_CST, but the x86-64 host's TSO hides a missing `dmb` and qemu inherits the same
  model. Needs a weakly-ordered host or an explicit litmus harness.
- **The engine and guest share one address space**, so a guest can read engine memory through any ordinary
  load. This is a property of the in-process model, not of any one instruction; the validators reject
  unmapped and `PROT_NONE` pages but nothing cheap separates engine-private from guest pages under an
  identity map.

---

## 8. Defects fixed

### 8.1 Pre-existing, unreachable or invisible while every host was AArch64

| Where | Defect |
|---|---|
| `guest/*/cache.c` | Persistent-cache identity passed a hardcoded `host_isa = 1`, although the function takes it and DOCS.md documents it as part of the key. **Two hosts sharing a cache directory would each have accepted and executed the other's host machine code.** |
| `syscall/sysv.c` | `sysconf` reports failure as **−1, not 0**, so the cast yielded `SIZE_MAX`, the guard never fired, and the page mask degenerated to `& 1`. Latent on every host. |
| `syscall/mem.c` | MAP_FIXED reconciliation rounded down with a literal `~0x3fff` — 12 KiB into a live neighbouring mapping on a 4 KiB host, converting genuine kernel verdicts (including `MAP_FIXED_NOREPLACE`'s `EEXIST`) into bogus successes. |
| `x87state.c` | `hl_x86_fxsave`/`fxrstor` were `#if defined(__aarch64__)` with no other arm, so elsewhere they compiled to nothing and **silently** dropped the guest's rounding mode and exception flags. |
| `x87state.c` | `LDMXCSR` `#GP`s on reserved bits and `fxrstor` hands it a **guest-controlled** image — a guest-triggerable engine kill, fixed by masking on the way in. |
| `emit.c`, `aarch64/translate.c` | `g_host_lrcpc`/`i8mm`/`bf16` read from `AT_HWCAP` **on any host**; on x86 those bits mean something unrelated, so codegen paths were selected from foreign data. |
| `translator/cache.c` | `ibtc_publish`'s 16-byte atomic pair store depends on `sizeof(ibtc_ent) == 16` and nothing asserted it. A new member would silently reintroduce the torn-dispatch hazard **on AArch64**, the host every lane tests. |
| `Phase*.cmake`, `refresh_crate_archives.sh` | `package/linux-aarch64` was a path literal naming the *host* CPU; an x86-64 host would have published its artifact as the aarch64 crate asset. |
| `dispatch.c`, `translate.c`, `native_context.h`, `signal.c` | Four guards that conflated host-CPU with compiler, or used a Darwin mcontext shape under a bare `__x86_64__`. None had ever been compiled by anything. |

### 8.2 Found by this work

**The store that landed twice.** `memcpy(dst, src, bytes)` with a **runtime** size is a call into glibc,
whose 4..7 and 8..15 paths copy head and tail separately — and at n == 4 both stores hit **the same
address**. Every 4- and 8-byte guest store reached memory twice, resurrecting whatever a peer guest thread
had committed in between: a spinlock's release store undid the next acquirer's acquire, and a third thread
acquired the now-zero word. **Two holders.** Measured on the host with no engine involved: ~294 of 3.4M
racing CASes silently undone, matching the observed guest breach rate. Both interpreters.

The diagnosis preceded the mechanism, which is why it is trustworthy: a lock word carrying the owner's id
showed the holder finding *another thread's* id; plain data under a provably sound lock never lost an update
over 5×200k×4 threads, while the same data under a plain-store release lost **exactly as many updates as
there were owner violations** — 2/2, 3/3, 4/4. Ruled out and recorded so nobody repeats them: the exclusive
monitor (value-based, so a stale reservation is benign), the STW machinery, atomic RMW atomicity, every
mapping type, and plain-store replay in isolation.

**Guest addresses dereferenced outside any fault bracket.** `hl_x86_avx_run`/`hl_x86_sse_run` are called
from the dispatch loop, *not* from inside `run_block`, so no fault pad was armed and **every ordinary r/m
operand** did a raw `memcpy` at a guest address — `vmovdqu`, `pshufb`, `fxsave` all killed the engine. Filed
as "the VEX gathers segfault"; the gathers were not special. Now every access is *proven before it is made*,
exiting `R_SOFTMISS` with the same protocol the JIT's memory guard uses.

**The non-PIE bias family — nine instances, one cause.** A non-PIE `ET_EXEC` maps HIGH at `+g_nonpie_bias`
while every guest-visible address stays at its LOW link value. Nine separate bugs came from that, found days
apart: a deadlock inside glibc several frames from the cause; `/proc/self/maps` silently losing every image
row; a guest SIGSEGV before a signal handler's first instruction; a **silently skipped robust-futex
`OWNER_DIED` walk**; `si_addr` handed to the guest in storage coordinates; a guest-triggerable engine kill
in `arch_prctl`; `select`'s `timeval` read with neither fold nor guard; two protection registries keyed in
*different coordinate systems*; and `G_DISPATCH_DEBUG` delivering a queued signal to an unfolded handler
address. §9 records how it was retired.

**Guest visibility of the engine.** `/proc/self/{numa_maps,smaps_rollup,map_files,syscall,mountstats}` and
`/proc/<any other pid>/*` were not intercepted at all and returned the *engine's* mappings, host paths, load
address and ASLR slide — and **`/proc/self/mem` was the engine's own address space**, where a `pwrite` to an
`r--p` page succeeded and read back. That is an escape, not a leak. The guard is identity-based, not
path-based: in bare mode the namespaces are identical, so a guest cannot detect a host path by inspection.

**Two NaN rules copied from an emulator.** `avx_dnan_*` implemented qemu's softfloat `float_2nan_prop_x87`,
agreeing with silicon on only 40 of 64 ordered pairs; the code comment recorded that "oracle and manual
disagree here" and chose the oracle. FMA was wrong on **both** backends — 28,560 and 13,008 triples of
43,680 — and they disagreed *with each other* on 21,312. Measured rules: first NaN in `a*b+c` order, wins
verbatim-quieted; MIN/MAX take src2 **not even quieted**; `#I` iff *some* operand is an SNaN.

**x87 had no model.** No tag word (so no `#IS`, `FFREE` a no-op, `FXAM` unable to say "empty"), `FPREM`
completing in one step so `C2` was never set — which glibc's `remainderl` *loops* on — precision control
ignored, and `FCW.RC` unapplied to arithmetic. Fixed on both backends, and **without a format change**:
emptiness cannot be derived (proved: `FFREE` punches an arbitrary hole without moving TOP), but valid/zero/
special *are*, leaving one bit per slot, which fits in `cpu->fptop`'s unused high bits.

**Decoder, both backends.** `REX.R` extended a group's `/reg` opcode extension, so `44 F6 /0 ib` decoded as
`NOT` — which takes no immediate — the length came out a byte short, and **the immediate executed as the
next instruction**.

**The archive nobody could link.** `libhl-translator.a` was installed and named in `hl-engine.pc` while
carrying 89 unresolved engine-internal symbols *on the aarch64 host too*. It linked only because a linker
pulls archive members on demand and nothing demanded them.

Plus: the ARM64 JIT lowering MMX at 128 bits (over-reading a page and computing wrong low halves); nine
FP-convert defects across shared helpers; eleven silently mis-executed aarch64 encodings; five silently
mis-executed x86-64 classes; the entire AdvSIMD indexed-element box and the reciprocal-estimate family
unimplemented; and `.rodata` stores silently dropped.

---

## 9. What actually worked

The transferable part.

**Replace a plausible oracle with a measured one.** Three results overturned settled beliefs this way, and
the rule they earn is: **when a comment says the manual and an emulator disagree, the emulator is the
suspect.**

**Measure the ISA, not the corpus.** A scan over 3012 fixture-runs reported 9 unimplemented aarch64
encodings. A slot-per-encoding differential over **2,882,308 encodings / ~4.0M full-state comparisons** found
~39,000 unimplemented and — decisively — **11 silently mis-executed encodings, a class the corpus scan
cannot see at all.** A fixture scan finds only instructions the corpus executes *and* which report; an
encoding that falls into a neighbouring case and returns a plausible wrong answer is invisible to it. The
x86-64 sweep repeated it against native silicon: 1,663,104 encodings, ~8.3M comparisons, five more classes.

**Retire the cause, not the instance.** After eight instances of the non-PIE family had been fixed one at a
time, the cause turned out to be a macOS constraint (`__PAGEZERO`) that Linux does not have — stated in the
code's own words, arrived through a bulk transfer commit, with `x86.c:433` already recording that mapping at
the link address was the robust fix. Placing non-PIE images there made the family **inert by construction**,
and lifted the compat corpus 99.34% → 99.84% while unblocking 1292 of 1542 fixtures for the transliterator.
The load-bearing line is not the placement but arming the fold on `bias != 0` rather than `etype == 2`.

**Distinguish specified from unspecified.** The same question — "hardware disagrees with us, who is right?"
— got three different correct answers, each argued from what the specification actually says. NaN
propagation is *specified*, so hardware was the architecture and the emulator was wrong. ARM's `FRECPE` is a
*defined* table, so the exact table is the architecture. x86's `RCPPS` specifies only a *bound*, so exact
satisfies it and "match hardware" would mean "match one vendor's ROM". Undefined flags are undefined, so the
engine keeps its own model — and the two backends agreeing with each other matters more than either
agreeing with one host, because `ckpt-cross` restores one backend's image on the other.

**Refuse to guess.** An honest `interp_undefined` report is a known unknown; a plausible wrong answer is a
bug in a guest nobody will trace back here. The 535-encoding reciprocal family sat reporting for a whole
cycle rather than being reconstructed from memory, and one fixture case was *removed* rather than have its
golden weakened.

**Traps that cost real time, and will again.** GCC **if-converts a ternary between two intrinsics into both
instructions plus a select**, ORing the not-taken one's exceptions into MXCSR — this had corrupted every
scalar SSE op. GCC constant-folds NEON at `-O2`: one fixture emitted 1 `mul` where it intended 37, i.e. it
tested the compiler. `FPSR` must be read in the *same* asm block as the instruction or the scheduler hoists
the op out of the window. `COMISD` and glibc's rounding helpers raise the very flags a probe is measuring.
And in a harness: the output struct must be **static, not a stack local**, or an encoding naming the
harness's own reserved register reads a differing stack address and thousands of encodings report false
differences.

**Concurrency discipline.** Several measurements were nearly reported wrong because another process
relinked `build-amd64` mid-run. Pin binaries and record their md5 before measuring; build and copy in a
*single* command; and A/B inside **one** binary behind a runtime switch where the effect is timing-sensitive.

---

## 10. Merge guidance

### What is safe

Almost all of it is **additive**: new `#if` arms whose AArch64 branch is the pre-existing code verbatim,
plus new files no AArch64 build compiles.

**Run this before merging.** It is about a minute and it is the only thing that checks what CI does not:

```sh
nix develop --command bash -c '
  cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake &&
  ninja -C build-arm-check -k 0'
```

### Where behaviour genuinely changes on an existing host

Do not let these past review as "amd64-only":

- **`mem.c`'s MAP_FIXED mask** also changes a 4 KiB-page aarch64 Linux host. Same bug, same direction;
  16 KiB hosts are bit-identical.
- **Cache identity** now includes the real host ISA, so any existing persistent cache directory is
  invalidated once. That is the point.
- **Non-PIE guests on Linux now load at their link address.** Zero goldens moved — guest-visible addresses
  were *always* the low link values — but it changes address-space layout on the interpreter lane too.
- **`.rodata` is now read-only**, so a guest that was silently getting away with writing to it will now take
  SIGSEGV. That is correct, and it is a behaviour change.
- **x87 lowering on the AArch64 host is larger per instruction** (§7).

### Conflict hot spots

- `src/core/target/{aarch64,x86_64}.c` — the host-CPU fork was inserted here; both are ~1200-line unity TUs
  everything else is `#include`d into.
- `src/host/native_context.h` — rewritten from a two-arm `#if` into a total OS×CPU matrix.
- `src/translator/cache.c`, `src/core/dispatch.c` — guard and assertion changes.
- `flake.nix` — `canRunGuests` was **renamed** to `hasCrateArchive` because the two ideas were only
  accidentally the same. A merge that reintroduces `canRunGuests` will silently re-gate the Rust outputs.
- `tests/compat/completeness/manifest.tsv` — appended by many changes; merge by row, never wholesale.

### Where to look first on an aarch64 host

1. Run the fixtures that have never touched silicon: `neon-misc`, `neon-indexed`, `neon-recip`,
   `mmx-width`, `cvt-flags`, `denorm-flags`. Each self-checks against an in-fixture ARM ARM transcription,
   so a divergence is a real answer, not a harness artefact.
2. Run `perf-linux` — the x87 and `#D` changes are unmeasured there.
3. Build a litmus harness for memory ordering. Nothing on an x86-64 box can test it.
