# The x86-64 same-ISA transliterator

`docs/amd64-host.md` §3.1 settled the register model for an x86-64 guest on an x86-64 host and left the code
unwritten. This is what got built, what it declines, and what a reader must not assume works.

It is the **third arm** of the fork in `src/core/target/x86_64.c`: ARM64 host → the JIT
(`guest/x86_64/{emit,translate,cache}.c`), any other host → the interpreter (`guest/x86_64/interp.c`), and now
x86-64 host → the interpreter **plus** `guest/x86_64/translit/`, which `interp.c` includes. Off by default
(`cmake -DHL_TRANSLIT=ON` moves the default, `HL_X86_TRANSLIT=1`/`0` decides at run time), compiled in
unconditionally so one binary reaches both backends.

Reproducing either lane:

```
HL_X86_TRANSLIT=1 HL_X86_TRANSLIT_STATS=1 build/linux-production/hl-engine-linux-x86_64 <guest>
HL_X86_TRANSLIT=1 ctest --test-dir build -R "compat\.(core-abi|signals|threads)"
```

`ctest` passes its environment through unchanged (`matrix_runner.c` `execlp` → `remote_supervisor.c`
`execv`), so the same variable reaches the engine under every harness.

## 1. The register model as built

Exactly option 3 of §3.1, with one divergence.

| | doc | built |
|---|---|---|
| guest GPRs | all 16 in the matching host GPRs | same; guest RSP **is** host RSP |
| `struct cpu` | `mov %gs:0, %reg` | `%gs` base **is** the cpu pointer, so `mov %gs:OFF, %reg` reaches a field directly — one instruction, no pointer load |
| stolen GPRs | none | none |
| scratch | block boundaries only | `cpu->mmscratch[0..1]` through `%gs`; the guest red zone `[rsp-128, rsp)` is never written |
| guest `%gs` | rewritten against `cpu->gs_base` | **declined** — the block ends before it |
| guest `%fs` | "ubiquitous and stays untouched" | **declined** — see below |

`%gs` base is published per guest thread with `arch_prctl(ARCH_SET_GS, cpu)`, re-published lazily whenever
`run_block` sees a cpu it has not published for (a cloned thread, a fork child, a restored checkpoint).

**The `%fs` divergence is the important one.** The doc assumed guest `%fs` could pass through untouched. It
cannot. Leaving guest `%fs` live means setting the *host* FS base to `cpu->fs_base` for the duration of a
block, and every host signal handler that then runs is C code that reads its own TLS through `%fs`. There is
no way to restore the host base before the handler's first instruction. So guest `%fs` is not passed through
and not rewritten either — it ends the block, and the interpreter serves it. Guest `%fs` use really is
ubiquitous (stack-protector prologues, `errno`, every TLS variable), which makes this the single largest
source of fallback and the first thing a v2 should fix, by rewriting the memory operand against
`cpu->fs_base` with a `%gs`-spilled scratch register.

## 2. The bias rule, and why this backend refuses rather than implements it

`docs/amd64-host-findings.md` §3.11/§3.12/§3.16 record six defects from one rule: a non-PIE `ET_EXEC` is
mapped HIGH at `+g_nonpie_bias` while its baked pointers stay LOW, so **address materialisation must be
un-biased and rip-relative accesses must stay biased**.

The JIT satisfies the access half with `emit_bias` (`guest/x86_64/address.c`): a *runtime conditional on every
memory access* — if the address has no bits above 32, add the bias. The interpreter satisfies it with
`hl_x86_guest_pointer` on every access. **Verbatim copying can express neither.** A copied instruction carries
its own addressing mode; there is no place to put the fold, and no register to fold into.

So the transliterator declines any image with `g_nonpie_lo != 0`, wholesale, at every block. Both directions
of the rule become vacuous rather than approximated. This is a refusal, and it is the honest one — the
alternative shapes are a guess:

- Skipping the fold and hoping low addresses never reach a copied instruction. They do: `linux_abi/x86.c`
  re-relocates the image's baked pointer words precisely because they otherwise stay low.
- Faulting on low addresses and fixing up in the handler. Correct in principle (the interpreter is right
  there in the same TU and could step the one instruction at `+bias`), but it turns an unknown fraction of
  data accesses into signal round-trips, and "unknown fraction" is not a performance model.

**What that cost, and why it no longer does.** The refusal above is still the right shape for a *folded*
image, but on Linux there are none. The bias existed only because macOS `__PAGEZERO` reserves the low 4 GB;
the loaders now place a non-PIE `ET_EXEC` at its own link address on Linux (`MAP_FIXED_NOREPLACE`, see
`linux_abi/thread.c` "non-PIE image placement, per host"), so `g_nonpie_lo` stays 0 and every image is
admitted. That took the previously-refused **1292 of 1542** x86-64 fixtures — including 7 of the 8
`compat.isa-x86-64` cases — from 0 % host blocks to the rates below. A macOS host, and a Linux host
restoring a checkpoint captured folded, still take the refusal.

| previously-refused case | interpreter | transliterator | vs interp | host blocks |
|---|---|---|---|---|
| `busyloop` rebuilt `-static -no-pie` | 132.5 s | **13.6 s** | **9.7x** | 100.0 % |
| `isa/x86_64/go_goro_x86` | 29.9 s | **1.3 s** | **23.2x** | 99.8 % |
| `isa/x86_64/go_heapgc_x86` | 31.9 s | **4.8 s** | **6.7x** | 91.1 % |
| `isa/x86_64/isa_regress` | 0.45 s | 0.17 s | 2.6x | 83.7 % |
| `core/regress/nonpie_vec` | 0.02 s | 0.02 s | — | 91.2 % |

USER+SYS CPU, `taskset` to one core, on a box running three other agents — so the ratios are conservative
and not comparable to §6's quiet-box figures. Output is byte-identical with the switch off and on in every
row.

## 3. What transliterates

A block is a maximal run of whitelisted instructions ending in a terminator. The whitelist
(`translit_classify`) is baseline-x86-64 integer work: the eight ALU groups, `mov`/`movzx`/`movsx`/`movsxd`,
`lea`, `test`, `xchg`, `cmpxchg`, `xadd`, `bt`/`bts`/`btr`/`btc`, `bsf`/`bsr`, `shld`/`shrd`, the shift and
rotate groups, `imul`, `inc`/`dec`/`neg`/`not`/`mul`, `push`/`pop`, `setcc`, `cmovcc`, `bswap`, `leave`,
`cld`/`std`/`clc`/`stc`/`cmc`, `endbr64`, the `nop` forms, and the string ops (`movs`/`stos`/`lods`/`scas`/
`cmps`, `rep`-prefixed, which run natively at hardware speed). `lock` is copied and executes natively.

Terminators: `jcc rel8/rel32` (a host `jcc` to a second epilogue — the guest's flags *are* the host's flags,
so a compare and its branch cost nothing extra), `jmp rel8/rel32`, `call rel32`, `call`/`jmp *%reg`, `ret` /
`ret imm16`, and `syscall`. No inter-block edge is emitted: every block returns to the shared dispatcher, so
the signal, checkpoint and stop-the-world polling in `core/dispatch.c` keeps working unchanged.

**Declined, ending the block** (the interpreter takes over from that instruction): `%fs`/`%gs` prefixes,
`0x67` 32-bit addressing, every VEX/EVEX/`0F38`/`0F3A` instruction, legacy SSE, x87, `div`/`idiv` (`#DE` is a
dispatcher exit), `cpuid`, `rdtsc`, `int3`/`ud2`, `pushfq`/`popfq`/`lahf`/`sahf`, indirect `call`/`jmp`
through memory, `loop`/`jrcxz`, and any rip-relative operand whose re-aimed displacement leaves int32 range.

A block also declines wholesale while an emulated `MAP_SHARED` mapping or a `PROT_EXEC` guest mapping is
live (`jit86_store_alias_observation_active`): the interpreter queues every store for writeback and the JIT
write-protects source pages, and a verbatim store does neither. That is re-tested in `run_block`, not only at
translate time, because both can turn on mid-run.

## 4. The three things that are rewritten

Everything else is `memcpy`.

1. **Block entry and exit.** `hl_x86_translit_enter` pushes the six host callee-saved registers, records
   `cpu->host_sp` after them, installs the guest EFLAGS with `push %gs:mmscratch[0]; popfq`, loads all 16
   guest GPRs from `%gs` (RSP last), and jumps. The epilogue stores all 16 back, switches to `cpu->host_sp`,
   captures flags with `pushfq` on the *host* stack, `cld`s (a guest `std` must not leak DF into host C),
   writes `cpu->rip` and `cpu->reason`, pops and returns. Every instruction in the spill is a `mov`, so the
   guest's flags survive untouched from the last guest instruction into that `pushfq` — nothing is
   materialised. `cpu->nzcv` + the PF/AF/DF side lanes are converted in C, once per boundary.
2. **rip-relative displacements.** `next_guest_rip + disp32` must still name the same address after the bytes
   move, so `disp32 += guest_next - host_next`, with `host_next` taken at the **RX** alias. Out of int32
   range ends the block; nothing places the code arena near the guest image, so that is a real limit rather
   than a rarity.
3. **The guest stack accesses `CALL` and `RET` perform.** `CALL` writes its return address as two 32-bit
   immediate stores below RSP and then `lea`s RSP down, which clobbers no register — so if the store faults
   (a guest stack overflow) every host GPR still holds its guest value and the fault path restarts the
   `CALL` exactly. `RET` saves RAX through `%gs` (never through the stack — the red zone is the guest's),
   loads the return address while the guest register file is still live, then pops.

## 5. Faults

A synchronous fault whose host PC is inside the code cache came from a guest access in a transliterated
block. Because guest GPRs *are* host GPRs, `*cpu` is reconstructed exactly from `uc_mcontext.gregs` plus
`REG_EFL`, and the JIT's own per-instruction provenance map (`translator/cache.c`,
`jit_instruction_guest_pc`) recovers the faulting guest RIP — with the same block-granular fallback the ARM64
JIT takes when that bounded ring has wrapped. Delivery then reuses the interpreter's path unchanged: the
`sigsetjmp` pad `run_block` already arms, and `interp_signal_resume`'s `siglongjmp`, which restores the host
callee-saved registers the block is currently holding guest values in.

One handler change is required and is gated on the switch: **`SA_ONSTACK` on SIGSEGV/SIGBUS**. This is the
only backend whose host stack *is* the guest stack, so a guest stack overflow leaves no room to build the
signal frame. Without it `compat/core/regress/stackoverflow_catch` dies of a host SIGSEGV (exit 139) instead
of delivering the guest's handler; with it, both backends print `caught SIGSEGV addr=1`.

## 6. Measured

**Correctness first.** Same pinned binary, every x86-64 compat fixture run twice — switch off, switch on —
comparing exit status and combined output:

* **246 of 250 static-PIE fixtures** (`core`, `abi`, `signals`, `process`, `threads`, `ipc`, `network`,
  `filesystem`, `isolation`, `isa`) are byte-identical. All four remainders were inspected and none is a
  behavioural difference:
  * `process/execfault`, `signals/sigbus_mmap_eof`, `core/regress/stackoverflow` — same exit status, same
    guest stdout; the engine's own `[HLFATAL]` line gains `hpc=/hblk=/hoff=/hinsn=` because the host PC is
    now inside the code cache. Strictly more diagnostic.
  * `core/syscall/edge_times` — the *interpreter* hits the 180 s harness timeout (exit 124) and the
    transliterator finishes (exit 0). A speedup showing up as a diff.
* **299 of 300 sampled non-PIE fixtures** are byte-identical, as they must be: the transliterator declines
  them. The exception, `memory/dbt_codecache_straightline`, fails identically both ways (exit 70,
  `unimplemented one-byte opcode`) and differs only in the ASLR'd `rip` the message prints — an artifact of
  the comparison script's address normalisation, not of the engine.
* One real regression was found and fixed this way: `core/regress/stackoverflow_catch` (see §5).

The named suites were then run twice on that binary, switch off and switch on. **The two runs are identical**
— `production.{smoke-x86_64,matrix,full-x86_64.core-abi}`, `compat.{core-abi,isa-x86-64,signals,threads}` and
`unit` (114/114) pass both ways, and `compat.memory` fails both ways on the same case, `wild-highva`, with
byte-identical output down to the reported `pc`/`sp`. That case is pre-existing and environmental: it asserts
`handler_maperr=1` for an access at `0x7ffffffff000`, the harness runs the engine with randomisation off (the
`pc`/`sp` are fixed across runs where a standalone run randomises them), and with no ASLR that address is
inside the guest stack's own mapping, so the kernel reports `SEGV_ACCERR` instead of `SEGV_MAPERR`. Run
standalone it passes 5/5, and under `matrix-runner … --repeat 3 wild-highva` it passes 3/3. It is a non-PIE
`ET_EXEC`, so the transliterator declines it and never executes one of its instructions.

**Then speed.** Pinned binary, `taskset`-ed to one core, USER+SYS CPU seconds (wall clock is unusable here — several agents
share this box). `HL_X86_TRANSLIT_STATS=1` reports the fallback rate beside every number; a speed number
without it is meaningless, because a workload that fell back is just the interpreter.

| case | linkage | native | interpreter | transliterator | vs interp | host blocks |
|---|---|---|---|---|---|---|
| `core/workload/busyloop` (the documented `compute` payload) | PIE | 0.310 s | 105.32 s | **7.01 s** | **15.0x** | 100.0% |
| `core/abi/recursion` | PIE | <0.01 s | 0.95 s | 0.08 s | 11.9x | 100.0% |
| `core/abi/sortbig` | PIE | 0.030 s | 7.49 s | 1.15 s | 6.5x | 80.3% |
| `core/workload/soak_allocchurn` | PIE | 0.760 s | 186.60 s | 54.50 s | 3.4x | 93.5% |
| `core/abi/heap` | PIE | <0.01 s | 0.05 s | 0.02 s | 2.5x | 88.9% |
| `core/abi/regex` | PIE | <0.01 s | 0.02 s | 0.01 s | 2.0x | 94.5% |
| `core/abi/{qsort,strings,math}` | PIE | <0.01 s | 0.01 s | 0.01 s | too short to time | 90.6–94.2% |
| any non-PIE `ET_EXEC` | non-PIE | — | unchanged | unchanged | 1.00x | 0% |

The spread is the fallback rate, and it is why the rate has to be reported: `busyloop`'s inner loop is pure
integer work and never leaves host code, while `soak_allocchurn` spends 55.7M of its 860M blocks in the
interpreter — malloc/free reach `%fs` TLS and SSE on almost every path — and gains only 3.4x for it.

`busyloop` executes 300,005,801 blocks, so the per-block cost is exact: **351 ns/block on the interpreter →
23.4 ns/block transliterated.** (351 matches the ~330 ns/block `docs/amd64-host-performance.md` records after
`583ae490`.) Against native the transliterator is **23x**, not the 0.7–0.9x that document predicts for the
diagonal — and the whole of that gap is the block boundary, not the guest work:

* 5,087 guest instructions over 1,052 blocks is **4.8 instructions per block**, and 4.8 native integer
  instructions cost about 1 ns. Everything else in the 23.4 ns is the dispatcher round-trip plus this
  backend's prologue/epilogue, which every block pays because nothing is chained.
* Within that, the two known-expensive items are `popfq` on entry and `pushfq` on exit (POPFQ is ~18 cycles
  on most microarchitectures, so ~6–8 ns of the 23 between them), and the 32 `%gs` loads/stores of the
  register file.

The 0.7–0.9x band therefore needs the three things §7 lists, in this order: **superblock formation** (keep
transliterating through a conditional branch's fallthrough — this alone should multiply instructions per
block several times), **block chaining and in-block loops** (which needs an emitted `cpu->irq` check so a hot
loop still reaches a dispatcher safepoint), and only then boundary micro-cost. None of them is started.

## 7. What a reader must not assume works

- **Non-PIE guests are not transliterated at all.** §2.
- **No `%fs` guest TLS, no SSE/AVX, no x87.** §1, §3. A memcpy-heavy or float-heavy guest spends most of its
  time in the interpreter even when the switch is on.
- **No superblocks, no block chaining, no IBTC.** A conditional branch ends the block, so blocks average
  4.8 guest instructions, and every one of them is a dispatcher round-trip — the dispatcher's own per-block
  cost is the floor, not the guest work (§6).
- **No persistent code cache.** `HL_PCACHE` is a JIT feature; a transliterated block is never saved or
  revived, and the cache identity already includes the host ISA.
- **No tier-2, no self-loop folding, no fast-clock inlining.** Those are `translate.c`'s.
- **SMC coherence is by refusal, not by protection.** A transliterated block caches guest bytes, so it
  registers its true source range `[gpc, guest_end)` for mmap/munmap invalidation — but a guest that
  rewrites its own code without ever taking a `PROT_EXEC` mapping is not covered. The JIT has the same gate
  (`g_rwx_guest`); the difference is that the interpreter had no gap here at all, because it re-decodes.
- **The whitelist must stay inside the intersection of the advertised CPUID and the host baseline.** `bsf`
  with an `F3` prefix is `tzcnt` on a BMI1 host and is declined for exactly this reason. Anything added to
  `translit_classify` inherits that obligation (`05298926`: one CPUID model, and a feature is advertised only
  when every backend implements it).
