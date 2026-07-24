# sqlite phase: why hl-engine runs it at ~0.4x native (aarch64 guest)

Investigation of the `sqlite` phase of `tests/perf/combined_bench.c`, which is the only
phase of the combined benchmark that is far off parity.

Reproduced baseline (`make bench BENCH_ENVS="native hl-engine" BENCH_ARCHES="arm64"
BENCH_REPEATS=2`, this machine):

| phase   | hl-engine | native | ratio |
|---------|-----------|--------|-------|
| sqlite  | 121,260us | 48,300us | **2.51x slower** |
| calls   | 151,272us | 93,439us | 1.62x slower |
| malloc  | 301,534us | 227,710us | 1.32x slower |
| string  | 216,153us | 180,319us | 1.20x slower |
| compute |  45,010us |  46,016us | parity |
| memory  | 129,547us | 126,304us | parity |
| syscall | 416,874us | 792,561us | hl 1.9x faster |

A standalone repro of just the sqlite phase (4 x 300k INSERTs into `:memory:`) reproduces
it outside the harness: **native 177ms, hl-engine 425ms (2.40x)**.

## What it is NOT

All four hypotheses in the original investigation brief are ruled out by direct measurement.
The engine's existing `[prof]` counters (`src/linux_abi/syscall/proc.c` case 94, behind
`if (0)`) were enabled together with `g_prof = 1`, for the whole 425ms sqlite run:

```
[prof] crossings=4540 syscalls=249 ibtc_miss=1407 branch_cross=2884 translations=4253
       lse=20 wx_toggles=0 dualmap=1 xlate_ms=9.705 mtibtc=1 mtfill=0
```

* **Not syscalls / fs / locking.** 249 syscalls in the entire run. The phase opens
  `:memory:` — there is no file, no `fcntl` locking, no `fsync`, no `mmap` churn. The
  brief's "sqlite is fsync/mmap/lock heavy" premise does not hold for this benchmark.
* **Not path/fd cache misses.** No path resolution happens at all (249 syscalls total).
* **Not JIT re-translation.** 4253 blocks translated, 9.7ms total translation time
  (2.3% of the run), and translation happens once — there is no flush and no thrash.
* **Not dispatcher round-trips.** 4540 host<->guest crossings for a 425ms run, i.e. one
  crossing per ~94us of execution. The IBTC is effectively always hitting (1407 misses
  are the cold fills).

Everything except ~10ms is spent **inside steady-state translated code in the arena.**
That was confirmed independently with a SIGPROF sampling profiler added temporarily to the
dispatcher (samples the host PC, maps arena offsets back to guest PCs through `g_map`):
**409/419 samples landed in the code arena, 10 in engine C code, 0 elsewhere.**

Two further specific candidates were checked and cleared:

* **glibc IFUNC selection is identical.** hl and native resolve `memcpy`/`memmove`/`memset`/
  `strlen`/`strcmp`/`memchr` to the same implementations (identical offsets from the image
  base), so the guest is not falling back to slower string routines. Worth noting anyway:
  hl reports `HWCAP=0x1fb HWCAP2=0` where the host reports `HWCAP=0x2fb3ffff
  HWCAP2=0x100186181`. It does not change selection for this glibc, but it is a large gap
  and could change selection for another libc/version.
* **The return path is already at its optimum.** `shadowgate()` was A/B'd across all four
  levels on the sqlite repro; the current default is the best by a wide margin, confirming
  the tuning note already in `translator/guest/aarch64/translate.c`:

  | shadowgate | sqlite repro (ms) |
  |------------|-------------------|
  | **-1 (default, §B off, ret -> IBTC)** | **452 455 445** |
  | 0 (original §B-on gate) | 674 700 716 |
  | 1 (widen) | 719 708 737 |
  | 2 (widen more) | 743 733 746 |

## What it IS

The arena profile is **flat and unremarkable** — the hottest single block is 3.5%, and the
top blocks are exactly the functions sqlite ought to be running:

```
[samp]   3.50% __pthread_mutex_unlock+0x0     [samp]   1.64% sqlite3Get4byte+0x0
[samp]   2.57% __memcpy_generic+0x0           [samp]   1.64% getAndInitPage+0x0
[samp]   1.87% sqlite3VdbeHalt+0x1fc          [samp]   1.40% __pthread_mutex_lock+0x0
[samp]   1.87% sqlite3VdbeExec+0x55ac         [samp]   1.17% sqlite3VdbeMemGrow+0xf0
[samp]   1.64% __printf_buffer_write+0xa4     [samp]   1.17% sqlite3_step+0x284
```

There is no single pathological hot spot. The slowdown is **diffuse, and it tracks
instruction mix.** Microbenchmarks (same static-pie toolchain, native vs hl, on this host)
isolate which constructs hl loses on:

| construct | native | hl | ratio |
|-----------|--------|-----|-------|
| ALU loop, single basic block | 35,626us | 35,443us | 1.00 |
| GPR load / store / `ldp`+`stp` | 11,741us | 11,757us | 1.00 |
| LSE `casa`, `ldar`, `stlr`, `dmb`, TLS, `mrs tpidr_el0` | — | — | 1.00 |
| raw `ldaxr`/`stlxr` exclusives | 74,689us | 33,395us | hl 2.2x faster |
| non-inlinable direct calls | 22,949us | 24,730us | 1.08 |
| **computed-goto dispatch (the VDBE model)** | 6,373us | 10,608us | **1.66** |
| **variable-length `memcpy`** | 24,904us | 46,987us | **1.89** |
| **uncontended `pthread_mutex_lock`/`unlock`** | 51,435us | 109,118us | **2.12** |

sqlite's inner loop is made of *precisely and only* the three constructs in bold:

* `sqlite3VdbeExec` is a giant computed-goto/switch opcode dispatcher — one indirect
  branch per VDBE opcode, and each one terminates a translated block and pays an IBTC
  probe. Nothing chains across it.
* This sqlite is `SQLITE_THREADSAFE=1`, so every public API call
  (`bind_int`/`bind_text`/`step`/`reset`, 4 per row x 300k rows) brackets itself in
  `sqlite3_mutex_enter`/`leave`. `__pthread_mutex_lock`+`__pthread_mutex_unlock` are
  the two hottest blocks in the arena profile.
* Value handling (`sqlite3VdbeMemGrow`, `sqlite3VdbeMemSetStr`, `bindText` with
  `SQLITE_TRANSIENT`, record serialization) is a stream of short `memcpy`s.

And on top of that, sqlite is a dense mesh of very short functions, which is the `calls`
phase (1.62x) — with §B disabled every guest `ret` goes through the IBTC (per-site IC
compare + branch) instead of a 1-cycle hardware-RAS-predicted `ret`.

**Root cause statement.** There is no bug and no pathological slow path in the sqlite
phase. hl-engine reaches parity on long straight-line loops (compute, crypto, memory,
float) because per-block overhead amortizes to nothing there. sqlite has effectively *no*
straight-line loops: it is a mesh of short basic blocks joined by indirect dispatch,
returns, and short calls, so the per-block and per-indirect-edge costs (irq poll, the
`cpu->vdirty` set-store, `bl` link-register materialization + chain exit, and the IBTC
probe on every `ret` and every VDBE opcode) are paid every few guest instructions instead
of every few thousand. The 2.5x is the compounded product of the 1.6-2.1x measured on each
of those constructs, not one thing.

This is why every knob that was already tuned for it (shadowgate, IBTC, tier-2, IRQSLIM)
is at its optimum and none of them move the number: they are each already the best
available answer to one edge of the mesh.

## Adjacent real defect found (measured, NOT the sqlite cause)

While isolating the above, one genuine codegen defect turned up that is worth fixing on
its own merits — it just does not help sqlite.

`translator/guest/aarch64/translate.c` emits, at the first V-touching instruction of every
region, a `str x28, [x28, #OFF_VDIRTY]` so a later chained-to syscall exit takes the full
V-register spill. For a **tier-1 self-loop** whose body touches a vector register, that
store sits inside the loop and re-executes on every iteration. Its address is in the
engine's `struct cpu`, which under ASLR may 4K-alias the guest's own data addresses,
producing a store-to-load-forwarding false dependency. The result is a **bimodal**
slowdown: the same binary and same test alternates run-to-run between parity and ~1.6x.

Measured, 50M-iteration loops, three runs each (native is stable at ~11,750us in all cases):

```
              hl (current)                 hl (vdirty store removed)
gpld    11996  12237  11755          11836  11926  11821  11804
qld     12225  17235  12309          11763  11764  11746  12307
qld31   12269  18319  18194          11841  11825  11814  11837
qstp    18431  17481  12320          11828  11811  11773  11888
dld     18529  12432  18308          12279  11797  12426  11798
```

Removing the store restores exact native parity and removes the bimodality entirely. Only
V-touching loops show it, which is the signature — GPR-only loops never emit the store and
are always at parity.

This is *already known and already fixed for tier-2*: `g_t2_loop_top` in `translate.c`
hoists the store above a fresh async poll and folds the back-edge past it, with the comment
"measured ~1.7x on a 1 MiB memcpy vs native". The gap is that the hoist is applied **only
in the tier-2 recompile** (`g_tier2_build`) and **only when the V-touching instruction is
the block's first**. Loops that never reach tier-2 promotion, or whose vector instruction
is not the block's first, keep paying it per iteration.

**Proposal (not applied here).** Generalize the existing tier-2 hoist to tier-1: when the
vdirty store is emitted at a block whose back-edge folds to itself, record a loop-top after
the store (as `g_t2_loop_top` already does) and fold the back-edge there. The store is
idempotent — vdirty is sticky and cleared only by a full spill — so running it once per
loop *entry* rather than once per *iteration* is semantically identical. For the general
case (vector instruction not first in the block) a test-and-set form
(`ldr x16,[cpu,#vdirty]; cbnz x16, skip; str ...`) also removes the repeated store from the
steady state and replaces it with a load, which cannot create the store-forwarding hazard.

This was deliberately **not** landed in this change: it does not improve the target
workload (sqlite measured 451/460/458ms with the store removed vs 445/452/455ms with it —
no gain, the store is not on sqlite's critical path), and it touches self-loop back-edge
folding, which is exactly the codegen the correctness matrix is most sensitive to. It
deserves its own change with its own matrix run.

## What was deliberately not attempted

* No change was made to shadowgate/§B — measured, current default is best by 1.5x.
* The vdirty hoist above was documented rather than landed (see reasoning).
* Making the engine report a fuller `HWCAP`/`HWCAP2` was not attempted; it does not affect
  this guest's IFUNC selection, and widening advertised CPU features has correctness
  implications well beyond this benchmark.
* No attempt was made to reduce the fundamental per-indirect-branch or per-call cost. That
  is not a contained fix — it is a translator design change (e.g. superblock/trace
  formation across indirect dispatch, or inlining short callees into the caller's block) and
  should not be attempted as part of a benchmark investigation.

## How to reproduce the measurements

The `[prof]` counter dump already exists in `src/linux_abi/syscall/proc.c` case 94 behind
`if (0)`; flip both to `if (1)` and set `g_prof = 1` in `src/core/target/aarch64.c` (note
`g_prof=1` itself costs ~5% via `emit_prof_bump`, so take final timings with it off). The
arena sampling profiler was a temporary addition to `src/core/dispatch.c` (SIGPROF handler
recording `uc_mcontext.pc`, mapped back through `g_map` at `exit_group`); it is not part of
this commit. Note that a *guest-side* SIGPROF profiler is NOT a valid way to profile under
hl-engine: the engine only delivers async signals at dispatcher safepoints, so guest-side
samples attribute to block boundaries rather than to the interrupted instruction, and the
resulting profile is badly skewed (it showed `_init` at 3.5% and lost `_int_malloc`
entirely).
