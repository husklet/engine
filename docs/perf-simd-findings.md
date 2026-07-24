# float/SIMD performance: measured root causes

Benchmark: `tests/perf/combined_bench.c`, phase `float_simd`, run in-guest-timed on an
Apple-silicon aarch64 host. All numbers in microseconds.

The phase is **plain C**, auto-vectorised by the cross compilers — it is not intrinsics and
it does not go through glibc IFUNC selection, so the guest CPUID/XCR0 story is irrelevant
here. The instruction mix is what the two `-O2` builds actually emitted:

* aarch64 guest, `phase_float_simd+0xb8` — an 8-instruction NEON loop:

  ```
  ldr  q29, [x4, x1]
  ldr  q30, [x3, x1]
  mov  v31.16b, v28.16b
  fmla v31.4s, v29.4s, v30.4s
  str  q31, [x2, x1]
  add  x1, x1, #0x10
  cmp  x1, #0x4, lsl #12
  b.ne <top>
  ```

* x86_64 guest, `phase_float_simd+0x120` — a 7-instruction **baseline SSE** loop (no AVX,
  no FMA):

  ```
  movaps (%rcx,%rax,1),%xmm0
  mulps  (%rdx,%rax,1),%xmm0
  addps  %xmm1,%xmm0
  movaps %xmm0,(%rsi,%rax,1)
  add    $0x10,%rax
  cmp    $0x4000,%rax
  jne    <top>
  ```

Native aarch64 runs its loop at roughly one iteration per cycle, so both cases are
**host-instruction-throughput bound**: every host instruction the translator adds to the
loop body costs almost linearly.

A and B are unrelated. A is a signal-liveness poll; B was SSE NaN semantics.

---

## A — aarch64 guest at 0.85x native: the tier-2 back-edge async poll

The aarch64 frontend is a same-ISA transliterator: it copies the loop body **verbatim**,
and the tier-2 promoter folds the back edge so there is no trampoline. What it adds is the
`cpu->irq` async-signal poll that the folded back edge must still execute (a guest loop
that never issues a syscall would otherwise be uninterruptible).

Emitted tier-2 body (dumped from the code cache, `blk_*_1.bin`):

```
  c4:  str  x28, [x28, #26464]     ; vdirty, hoisted out of the loop (runs once per entry)
  c8:  ldr  x16, [x28, #1040]      ; <-- loop top: cpu->irq poll          ADDED
  cc:  cbz  x16, 0x1b8             ; <--                                  ADDED
       ... 0xd0..0x1b4: the R_BRANCH spill/exit stub ...
 1b8:  ldr  q29, [x4, x1]          ; the 8 guest instructions, byte-for-byte
 1bc:  ldr  q30, [x3, x1]
 1c0:  mov  v31.16b, v28.16b
 1c4:  fmla v31.4s, v29.4s, v30.4s
 1c8:  str  q31, [x2, x1]
 1cc:  add  x1, x1, #0x10
 1d0:  cmp  x1, #0x4, lsl #12
 1d4:  b.ne 0xc8
```

So the loop is 10 host instructions instead of 8, executes 3 loads instead of 2, and takes
two taken branches per iteration instead of one (the poll's `cbz` jumps forward over the
~0x120-byte exit stub to reach the body).

**Evidence it is the whole gap.** Suppressing only that poll in the tier-2 recompile
(measurement build, not a shippable change — it makes the loop uninterruptible):

| build                                | float_simd (us)   |
|--------------------------------------|-------------------|
| native aarch64                        | 123,556          |
| hl-engine, current                    | 147,810 / 157,432 |
| hl-engine, tier-2 back-edge poll off  | 129,707 / 130,272 |

Removing the two instructions recovers essentially all of the 15%. Nothing else on the
aarch64 path costs anything on this loop: no per-instruction fixup, no flag sync, no
guest-base fold (the guest is `-static-pie`, so `guestbase_on()` is false and
`emit_fold_mem` is inert), and the `cpu->vdirty` store is already hoisted out of the loop.

**Attempted fix, rejected.** The obvious layout cleanup is to give the hoisted tier-2 poll
the same out-of-line exit stub the block-entry poll uses (`cbnz x16, <end-of-block stub>`),
which makes the loop 10 *contiguous* instructions inside one 64-byte line with a single
taken branch. It was implemented and measured, and it is **50% slower**:

| tier-2 poll layout                                   | float_simd (us)          |
|------------------------------------------------------|--------------------------|
| current (`cbz` forward over an inline exit stub)      | 163,028 / 162,448 / 153,292 |
| shared out-of-line stub, compact loop                 | 246,455 / 245,086 / 245,216 |

Same instruction count, same architectural behaviour, better layout on paper, much worse in
practice — an Apple-silicon front-end effect that was not chased further. The change was
reverted; **no aarch64 change is shipped**. If A is picked up again, the direction to
explore is reducing the poll *frequency* (e.g. polling every N-th back edge with an
in-register countdown) rather than re-laying-out the two instructions, and any such change
has to be argued against async-signal latency.

---

## B — x86_64 guest 6.8x slower than Rosetta: the SSE NaN-input gate (FIXED)

The x86 loop compiled to **53 host instructions per iteration** (dumped from the code
cache). 30 of those 53 came from two guest instructions, `mulps` and `addps`, and none of
that 30 was arithmetic:

```
  6c:  ldr  x16, [x28, #672]       ; irq poll (2)
  70:  cbnz x16, ...
  74:  str  x28, [x28, #3312]      ; vdirty (1; not hoisted on the x86 path)
  78:  add  x17, x1, x0            ; movaps load  (2)
  7c:  ldr  q0, [x17]
  80:  add  x17, x2, x0            ; mulps memory operand (2)
  84:  ldr  q16, [x17]
  ---- mulps: NaN-INPUT gate, 8 instructions ----
  88:  fcmeq v24.4s, v0.4s, v0.4s
  8c:  fcmeq v25.4s, v16.4s, v16.4s
  90:  and   v24.16b, v24.16b, v25.16b
  94:  ext   v25.16b, v24.16b, v24.16b, #8
  98:  and   v24.16b, v24.16b, v25.16b
  9c:  fmov  x16, d24
  a0:  mvn   x16, x16
  a4:  cbz   x16, 0x13c            ; ... over a 39-instruction R_SSE3B spill/exit stub
  ---- mulps: emit_dnan_pre (4) + arithmetic (1) + emit_dnan_post (3) ----
 13c:  fcmeq v20.4s, v0.4s, v0.4s
 140:  fcmeq v21.4s, v16.4s, v16.4s
 144:  and   v20.16b, v20.16b, v21.16b
 148:  shl   v20.4s, v20.4s, #31
 14c:  fmul  v0.4s, v0.4s, v16.4s
 150:  fcmeq v21.4s, v0.4s, v0.4s
 154:  bic   v20.16b, v20.16b, v21.16b
 158:  orr   v0.16b, v0.16b, v20.16b
  ---- addps: the identical 8 + 4 + 1 + 3 again ----
 15c .. 22c
  ---- store, index update, x86 flag synthesis ----
 230:  add  x17, x6, x0
 234:  str  q0, [x17]
 238:  mov  x19, #0x10
 23c:  adds x0, x0, x19
 240:  mov  x16, #0x4000
 244:  sub  w25, w0, w16
 248:  str  x25, [x28, #2728]      ; lazy-flag operand spill
 24c:  eor  w26, w0, w16
 250:  eor  w26, w26, w25
 254:  str  x26, [x28, #2736]
 258:  cmp  x0, x16
 25c:  mrs  x20, nzcv
 260:  str  x20, [x28, #136]
 264:  b.ne 0x6c
```

The NaN-input gate exists because NEON and SSE disagree on which operand wins when a lane
has **two** NaN inputs, and `emit_dnan_pre`/`post` exist because x86's generated default NaN
has the sign bit set and ARM's does not. Both were paid unconditionally, on every packed FP
op, in code that never sees a NaN.

**The fix (shipped).** For packed `addps/pd`, `subps/pd`, `mulps/pd`, `divps/pd` both
divergences are visible in the *result*: these four ops propagate a NaN operand to a NaN
result unconditionally, so "some result lane is NaN" is a sound superset of "this
instruction needs x86-exact handling". So do the arithmetic into scratch `v18` — leaving
the architectural `vd`, and therefore the `R_SSE3B` spill, exactly as the guest instruction
found it — test the *result*, and on any NaN lane exit to the same x86-exact C softmulator
that the input gate already used. Emitted now:

```
  88:  fmul  v18.4s, v0.4s, v16.4s
  8c:  fcmeq v21.4s, v18.4s, v18.4s   ; all-ones per NON-NaN lane
  90:  uminv b21, v21.16b             ; zero iff ANY lane is NaN
  94:  fmov  w16, s21
  98:  cbnz  w16, 0x130               ; clean -> commit
       <R_SSE3B exit>
 130:  mov   v0.16b, v18.16b
```

7 host instructions where the old input gate plus `emit_dnan_pre`/`post` took 16, and
bit-identical on both paths: the old fast path required no NaN *input*, which for these ops
implies a non-NaN result except for a generated default NaN — and that case now routes to
C, which produces exactly the value `emit_dnan_post` used to stamp. Scalar `ss`/`sd` forms
are untouched (their gate is already 6 instructions and their fixup is a
predicted-not-taken FCMP branch); packed `sqrtps/pd` is untouched (single operand, no
two-NaN case).

Loop body: **53 -> 33 host instructions per iteration.**

| build                              | float_simd (us)              |
|------------------------------------|------------------------------|
| hl-engine before                    | 848,633 / 853,120 / 846,043 |
| hl-engine after                     | 405,770 / 405,521 / 405,109 |
| Docker/amd64 (Rosetta 2), reference | 124,197                      |
| qemu-user, reference                | 6,375,415                    |

**2.09x faster**, phase checksum unchanged (`ok=2302728104500`), every other phase
unchanged. Gap to Rosetta on this phase: 6.8x -> 3.3x.

Correctness: `unit`, `compat-isa-x86-64` and `compat-isa-aarch64` all pass. In addition, a
purpose-built differential test (all pairwise combinations of QNaN / SNaN / +-inf / +-0 /
denormal / normal patterns through `addps addpd subps subpd mulps mulpd divps divpd`, 244
result lines) is **bit-identical between hl-engine and `qemu-x86_64`**, which exercises
exactly the paths this change touches — two-NaN-input lanes, single-NaN propagation, and
generated default NaNs (`0*inf`, `inf-inf`, `0/0`).

### What is left on the x86 side (not done)

The remaining 33 instructions per iteration, in descending order of value:

1. **x86 flag synthesis, ~11 instructions** for one `add` + one `cmp`: two immediate
   materialisations (`mov x19,#0x10` / `mov x16,#0x4000` — both fit `add`/`cmp` imm12), the
   lazy-flag operand spill (`sub`/`str`/`eor`/`eor`/`str`), *and* a real `cmp` plus
   `mrs`/`str` of NZCV. The deferred-operand spill and the eager NZCV materialisation look
   redundant with each other here. This is not SIMD-specific and would also help `compute`.
2. **Memory operands, 3 instructions**: `add x17, base, index` + `ldr q` per operand. ARM
   has a register-offset `ldr q0,[x1,x0]`; folding it is blocked by `g_ldr_q`'s `rn == 17`
   bus-guard convention and by fault-PC provenance, both of which want the EA in x17.
3. **`str x28,[x28,#vdirty]` every iteration**: the aarch64 frontend hoists this out of a
   tier-2 self-loop (`g_t2_loop_top`); the x86 frontend has no equivalent hoist.
4. The `cpu->irq` poll, same 2 instructions as A.
