# Differential AArch64 ISA fuzzer

Instruction-level differential testing of the AArch64-Linux guest frontend
(`src/translator/guest/aarch64`). The aarch64 guest is often assumed "safe" because the guest ISA
equals the host ISA -- but it is still TRANSLATED, and the parts that are *not* a verbatim copy are
exactly the parts no whole-program fixture reaches.

## Why the oracle here is perfect

The generated program is a **static AArch64 binary**, and it is run twice:

1. **natively** on this ARM64 Linux host, and
2. under `build/linux-production/hl-engine-linux-aarch64`.

Same binary, same ISA, same silicon. Any difference in the final state is unambiguously an engine
bug. There is no emulator-vs-emulator ambiguity (the x86-64 side has to argue with `qemu-x86_64`
about NaN propagation and undefined flags), no second implementation's undefined behaviour, and no
grey area at all. **Nothing is masked from the comparison.**

## How it works

`isafuzz_gen.c` is a host tool: given a seed it writes a self-contained AArch64 C program whose
body is one long randomized straight-line instruction sequence, emitted as a single file-scope
inline assembly block so the compiler never reorders, reallocates or reinterprets any of it. The
program seeds all 29 usable general-purpose registers and all 32 vector registers from constants
baked into the source, runs the sequence, and prints a canonical fixed-width state image:

```
X00 0102030405060708     ... 29 GPRs (x24/x25 reserved: scratch base and writeback base)
NZCV f0000000            the four condition flags
FPSR 0000009f            cumulative FP exception bits + QC
FPCR 07ffff00            rounding mode / FZ / DN / trap enables
V00  <32 hex digits>     ... all 32 vector registers
MEM  0f0a...             FNV-1a over the 8 KiB scratch buffer
```

`run.sh` compiles that program once and diffs the two runs. Determinism is total: the same seed
produces a byte-identical source file; the guest reads nothing from the environment; and **no
address ever reaches the dump** -- the two reserved registers `x24`/`x25` hold every pointer the
body needs, are never operands of a generated instruction, and are never printed.

## Where the bugs are, and how the generator aims at them

The frontend is a same-ISA transliterator: it copies most instructions verbatim and MANGLES only
those naming a register the engine has stolen (`x16`, `x17`, `x18`, `x28`, `x30` -- `is_stolen()`,
`cpu.h`). So the generator picks a register from the stolen set **38% of the time**, roughly ten
times what a compiler would emit. The other hand-written surfaces it targets:

* `emit_mangled_x18` / `emit_casp_mangled` -- the stolen-register rewrite, including the CASP pair
  partners that are implicit in the encoding.
* `emit_fold_mem` / `emit_fold_advsimd_struct` -- the non-PIE low-address bias fold, which runs on
  every memory op of a non-PIE image. `run.sh` builds `-static -no-pie` by DEFAULT for this reason;
  `--pie` covers the other link model, which takes a different path.
* `try_lse_atomic` -- the ldxr/stxr-retry-loop -> single-LSE-atomic pattern matcher. The generator
  emits the exact idioms it recognizes (swap, the five register RMW loops, the add-immediate loop,
  the CAS loop) plus `ldxp`/`stxp` near-misses that must not be upgraded.
* `gpr_field_mask` -- the per-encoding classification of which register fields are general-purpose.
  Getting it wrong on any instruction that crosses the SIMD/GPR boundary (`umov`/`smov`/`dup`/`ins`,
  every scalar FP<->GPR conversion, the LSE value operand, the AdvSIMD structure post-index stride)
  silently emits a stolen register verbatim. All of those classes are generated.
* `emit_i8mm_mmla` / `emit_bf16_bfcvt` / `emit_bf16_bfdot` -- the only real lowerings in the file.
* `+hot` makes every block hot, so the run also crosses tier-2 specialization / trace formation.

### Instruction classes

add/sub (immediate, shifted-register, extended-register with every UXTB..SXTX and shift, and the
flag-setting forms); adc/sbc/adcs/sbcs; the full logical family incl. bitmask immediates and ROR
shifts; movz/movn/movk at every shift; ubfm/sbfm/bfm (ubfx/sbfx/bfi/bfxil/ubfiz/sbfiz), the
sxtb..sxtw aliases and the lsl/lsr/asr immediates; extr/ror; rbit/rev/rev16/rev32/clz/cls;
udiv/sdiv (incl. divide-by-zero and INT_MIN/-1 from the corner-value seeds) and lslv/lsrv/asrv/rorv;
madd/msub/smulh/umulh/smaddl/umaddl/smsubl/umsubl/smull/umull; ccmp/ccmn in both register and
immediate form; csel/csinc/csinv/csneg/cset/csetm/cinc/cneg; b.cond/cbz/cbnz/tbz/tbnz skips;
adr/adrp (compared as a page difference, so no address leaks); loads and stores at all four widths
with sign extension, in unsigned-immediate, register-offset-with-extend-and-scale, pre-index,
post-index and unscaled ldur/stur forms; ldp/stp/ldpsw incl. writeback; the whole LSE set
(ldadd/ldclr/ldeor/ldset/ldsmax/ldsmin/ldumax/ldumin/swp/cas/casp) in all four acquire/release
flavours; ldxr/stxr, ldaxr/stlxr, ldxp/stxp and ldar/ldapr/stlr; AdvSIMD integer arithmetic,
saturating arithmetic, halving/rounding arithmetic, min/max, absolute difference and accumulate,
compares (register and zero forms), shifts by register and by immediate incl. saturating and
insert-shift, widening/narrowing/saturating-narrowing, pairwise and across-lane reductions,
zip/uzp/trn/ext/tbl/tbx, rev/cnt/rbit/abs/neg/cls/clz, movi/mvni/orr-imm/bic-imm, the
vector<->GPR moves; AdvSIMD FP arithmetic, compares, absolute compares, conversions with and
without a fixed-point immediate, fcvtl/fcvtn/fcvtxn, and the FP reductions; scalar FP arithmetic,
the fused multiply-adds, fcmp/fcmpe/fccmp/fcsel, every FP<->GPR conversion and fmov;
ld1..ld4/st1..st4 (multiple-structure, single-lane and ld1r) with immediate and register
post-index; vector ldr/str/ldp/stp at every element width; mrs of NZCV/FPSR.

Behind `--gen-args`: `+i8mm` (SMMLA/UMMLA/USMMLA), `+bf16` (BFCVT/BFDOT), `+dczva` (DC ZVA),
`+fpcr` (MSR FPCR -- rounding mode, FZ, DN), `+hot`.

## Running

```sh
# campaign over 200 fresh seeds
tests/fuzz/isa/aarch64/run.sh --seeds 200 --gen-args "+i8mm +bf16 +dczva +fpcr"

# the other link model
tests/fuzz/isa/aarch64/run.sh --seeds 200 --pie

# one seed, reproducibly
tests/fuzz/isa/aarch64/run.sh --list 93

# shrink a failing seed to a minimal diverging instruction group
tests/fuzz/isa/aarch64/run.sh --minimize 93
```

Via make (ARM64 Linux only):

```sh
make isa-fuzz-arm                 # ISA_FUZZ_ARM_SEEDS fresh seeds (default 200)
make isa-fuzz-arm-regress         # fixed deterministic seed set, non-PIE and PIE
```

Failing seeds leave the generated source, both dumps and the diff under
`build/isafuzz-arm/repro/sN/`.

### Minimization

`--minimize` delta-debugs the generated body down to the smallest instruction group that still
reproduces. Two properties make it sound: each generated **step** is emitted as one source line, so
a guard (the index clamp in front of a scaled memory operand, the `add x24,x25,#off` that
establishes a writeback base, a conditional branch and its target label, the retry loop around a
`stxr`) can never be deleted away from the instruction it protects; and the reduction is targeted --
the shrunken program must reproduce the same failure *mode* and differ on one of the same state
keys as the full-size one.

## Determinism caveats the generator handles

* **Store-exclusive status.** `stxr` may report a SPURIOUS failure, so a bare `ldxr`/`stxr` pair is
  not a deterministic oracle. Every exclusive sequence is emitted as a retry loop, which can only
  exit with the status at 0.
* **Addresses.** `x24`/`x25` are reserved and never dumped, so no run-to-run address ever enters the
  comparison. `adr`/`adrp` are exercised but only their page DIFFERENCE (a link-time constant)
  survives into a dumped register.
* **FPCR.** The epilogue restores FPCR before returning to libc, so a fuzzed rounding mode cannot
  leak into `printf`.
* **Alignment.** Atomics and exclusives fault when unaligned, so their operands are always
  naturally aligned; every generated address is constructed to land inside the 8 KiB scratch buffer.

## Oracle caveats

There is only one, and it is a property of the ENGINE rather than of the oracle:

* **FEAT_I8MM / FEAT_BF16.** The engine's CPU model does not advertise these, but it still lowers
  `SMMLA`/`UMMLA`/`USMMLA` and `BFCVT`/`BFDOT` to baseline AdvSIMD for guests that use them anyway.
  The BFDOT decomposition is not exact: BFDOT is architecturally defined to add both bf16 products
  and the addend with a SINGLE rounding under forced FZ/DN and to raise no FP exceptions, which a
  widen/fmul/pairwise-add sequence cannot reproduce (measured: 1-ulp results, wrong NaN payloads,
  a spurious `FPSR.UFC`). The fix applied was to probe the HOST for the extension
  (`AT_HWCAP2` on Linux, `hw.optional.arm.FEAT_*` on macOS) and copy the instruction verbatim when
  it is present -- bit-exact and faster. The lowerings remain as the fallback for hosts without the
  extension, and on such a host `--gen-args "+bf16"` is expected to diverge; that fallback is a
  best-effort shim for an out-of-contract guest, not a supported surface.
