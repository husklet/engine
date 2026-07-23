# Differential x86-64 ISA fuzzer

Instruction-level differential testing of the x86-64 -> AArch64 lowering. Before this existed the
entire instruction translator was covered only by whole-program fixtures in
`tests/compat/isa/x86_64`, which exercise the instruction mix a compiler happens to emit and
nothing else.

## How it works

`isafuzz_gen.c` is a host tool: given a seed it writes a **self-contained x86-64 C program** whose
body is a long randomized straight-line instruction sequence, emitted as file-scope basic inline
assembly so the compiler never reorders, reallocates or reinterprets any of it. The program seeds
all sixteen XMM registers and the fourteen usable GPRs from constants baked into the source, runs
the sequence, and prints a canonical fixed-width state image:

```
RAX 0000000000000000     ... 14 GPRs (RSP and R15 are reserved: stack and scratch base)
FLG 0000000000000845     RFLAGS, masked to the bits that are architecturally DEFINED here
MXC 00001fa3             MXCSR
XMM0  <32 hex digits>    ... all 16 vector registers
MEM 0f0a...              FNV-1a over the 8 KiB scratch buffer
```

`run.sh` cross-compiles that program once and runs the **same binary** under `qemu-x86_64` (the
reference oracle) and under `build/linux-production/hl-engine-linux-x86_64` (the engine under
test). Any difference in the dump is a translator divergence.

Determinism is total: the same seed produces a byte-identical source file, and the guest reads
nothing from the environment (no `rdtsc`, no `cpuid`, no syscalls beyond the final `printf`, no
addresses in the dump).

### Undefined flags

Rather than fencing undefined flags away, the generator *tracks* which of CF/PF/ZF/SF/OF are
architecturally defined after every instruction it emits. Flag consumers (`jcc`, `setcc`,
`cmovcc`, `adc`/`sbb`, `rcl`/`rcr`) are only emitted when the bits they read are defined, and the
final definedness set is folded into a constant mask the generated program applies before printing
RFLAGS. AF is always masked. Every *defined* flag bit therefore stays under test while no
architecturally-open bit is ever compared -- which is what makes the lazy-flag / dead-flag-elision
paths testable at all.

### Instruction classes

ALU (r/r, r/imm, r/m, m/r at all four widths) and the lazy-flag consumer chains; inc/dec/neg/not;
shl/shr/sar/rol/ror/rcl/rcr in by-1, by-imm, by-CL-known and by-CL-dynamic forms; shld/shrd;
mul/imul/div/idiv (1-, 2- and 3-operand, 8/16/32/64-bit, guarded so `#DE` is impossible);
bt/bts/btr/btc/bsf/bsr/popcnt; movsx/movzx/movsxd/bswap/lea; AH/AL/AX subregister writes;
xchg/cmpxchg/xadd incl. `lock`; SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2 packed and scalar integer and
floating point, shuffles, pack/unpack, compares, `pmovmskb`, `pcmpistri`, `ptest`, conversions,
and a seeded supply of NaN / infinity / denormal / signed-zero operands. x87 is available behind
`--gen-args "+x87"`; the BMI count instructions behind `+bmi` (the guest CPUID is Westmere, so
they are off by default).

## Running

```sh
# campaign over 200 fresh seeds
tests/fuzz/isa/x86_64/run.sh --seeds 200 --ignore-mxcsr

# one seed, and re-run it later reproducibly
tests/fuzz/isa/x86_64/run.sh --list 137 --ignore-mxcsr

# shrink a failing seed to a minimal diverging instruction group
tests/fuzz/isa/x86_64/run.sh --minimize 137 --ignore-mxcsr
```

Via make (Linux only -- needs `qemu-x86_64` and the cross compiler):

```sh
make isa-fuzz                     # ISA_FUZZ_SEEDS fresh seeds (default 200)
make isa-fuzz-regress             # fixed, deterministic seed set for CI
```

Failing seeds leave the generated source, both dumps and the diff under `build/isafuzz/repro/sN/`.

### Minimization

`--minimize` delta-debugs the generated body down to the smallest instruction group that still
reproduces. Two properties make it sound: each generated *step* is emitted as one source line, so
a guard (an index clamp before a scaled memory operand, the non-zero divisor setup before a `div`,
the `or $1` before a `bsf`) can never be deleted away from the instruction it protects; and the
reduction is targeted -- the shrunken program must reproduce the same failure *mode* and differ on
one of the same state keys as the full-size one.

Note that the `FLG` line is only trustworthy at full size: the undefined-flag mask is a constant
baked from the complete sequence, so deleting instructions can leave it stale. Pass
`--ignore-flags` when minimizing a non-flag divergence.

## Oracle caveats

Three behaviours are deliberately kept out of the corpus because `qemu-x86_64` (10.2) and the
engine disagree in a way that is not a translator bug:

* `RCL`/`RCR` at 8/16-bit width with a masked count of 9 / 17. The count is reduced modulo 9 / 17,
  making the instruction a no-op; qemu instead zeroes the operand and clears CF. The engine is
  correct, so the generator keeps the immediate inside one rotation period.
* `SHLD`/`SHRD` with a masked count of 0, which is a documented no-operation. qemu still writes the
  destination back, which for the 32-bit form zero-extends bits 63:32.
* `CMPXCHG` with a 32-bit **register** destination on the failing-comparison path: the SDM's
  `DEST <- DEST` write zero-extends on hardware, qemu preserves the upper half.

`MXCSR` is excluded from the make targets. The engine *does* project the host FPSR cumulative flags
into MXCSR bits 0..5 (`emit_fpsr_to_mxcsr`, translate.c), so the divergence is narrower than "not
modelled at all". Measured over 126 diverging seeds:

| differing bit | seeds |
| --- | --- |
| DE (bit 1, denormal operand) | 126 / 126 |
| UE (bit 4) | 5 |
| OE (bit 3) | 4 |
| IE (bit 0) | 3 |
| PE (bit 5) | 2 |

DE is present in *every* one and is the blocker. x86 sets DE whenever a SOURCE OPERAND is denormal;
AArch64 has no equivalent -- FPSR.IDC is raised only when FZ=1 flushes a denormal input, and the
engine must run with FZ=0 to keep results bit-exact. Modelling DE therefore means testing the inputs
of every SSE FP instruction inline (a per-lane `|x| < min_normal && x != 0` probe plus a sticky OR
into a new cpu-side MXCSR shadow, on the hot path of every FP kernel) and rebuilding
stmxcsr/ldmxcsr/fxsave/fxrstor around that shadow instead of the FPSR. That is a real per-instruction
cost for a flag no portable software can observe -- C99 exposes FE_INVALID / FE_DIVBYZERO /
FE_OVERFLOW / FE_UNDERFLOW / FE_INEXACT and has no name for DE at all. The mask therefore stays,
deliberately; this is the assessment behind it rather than an unexamined default.

## Two-NaN operand selection

When BOTH inputs of an FP lane are NaN, `qemu-x86_64` selects the surviving operand with softfloat's
`float_2nan_prop_x87` rule -- a QNaN beats an SNaN; between two NaNs of the same kind the LARGER
significand wins; on a significand tie the POSITIVE one wins -- and it applies that rule to SSE as
well as to x87. The Intel SDM (Vol. 1, "Rules for handling NaNs") instead specifies plain
FIRST-OPERAND priority for SSE: `QNaN(SRC1)` whenever SRC1 is any NaN. Oracle and manual disagree
here, and the engine's C softmulator (`avx_dnan_f32`/`avx_dnan_f64`, avx.c) follows the ORACLE so the
campaign stays usable. Two consequences worth knowing: the implemented rule is COMMUTATIVE, which is
why the horizontal ops need not reproduce x86's odd-lane-first addend order; and this is a
deliberate oracle-compatibility choice, not an accident.

## Mode coverage

`--gen-args "+bmi"` is clean (400/400 seeds). `--gen-args "+x87"` still reports ~18% of seeds
diverging, and every one that has been minimized bottoms out in the DOCUMENTED 80-bit gap (H11,
lower/x87.c): the engine carries ST(0..7) as binary64, so an `fdiv`/`fsqrt` chain double-rounds
against a real 64-bit-mantissa FPU. Those are not new findings. Two x87 oracle caveats:

* `F2XM1` outside [-1,+1] is architecturally UNDEFINED (the SDM constrains the source range); qemu
  returns the indefinite, the engine returns `exp2(x)-1`. The generator does not emit `f2xm1`.
* the x87 divergences show up only in the `MEM` hash, because the generator's x87 results reach the
  dump through `fstpl` stores.
