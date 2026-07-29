# AArch64 ISA differential fuzzer

`isafuzz_gen.c` creates deterministic static AArch64 programs and `run.sh`
compares their register, flag, vector, and scratch-memory state natively and
through `hl-engine-linux-aarch64`. The identical binary runs on the same host
ISA, making any defined-state difference an engine defect.

```sh
tests/fuzz/isa/aarch64/run.sh --seeds 200 \
  --gen-args '+i8mm +bf16 +dczva +fpcr'
tests/fuzz/isa/aarch64/run.sh --seeds 200 --pie
tests/fuzz/isa/aarch64/run.sh --list 93
tests/fuzz/isa/aarch64/run.sh --minimize 93
```

Failures are retained under `build/isafuzz-arm/repro/sN/`. Minimization removes
whole generated instruction groups while requiring the same failure class and
state key.

The generator concentrates on stolen-register rewriting, non-PIE address
folding, LSE pattern recognition, SIMD/GPR field classification, scalar and
vector arithmetic, atomics, loads/stores, branches, flags, FP state, and
AdvSIMD structure operations. Reserved pointer registers and naturally aligned
scratch addresses keep the oracle deterministic.

Optional `+hot` exercises tier-two translation. `+i8mm` and `+bf16` are exact
when the host supports those extensions. BF16 fallback lowering on a host
without FEAT_BF16 is best-effort and may differ in rounding, NaN payloads, and
FP exception flags.
