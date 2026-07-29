# x86-64 ISA differential fuzzer

`isafuzz_gen.c` creates deterministic x86-64 programs and `run.sh` compares
their defined register, flag, vector, and scratch-memory state under
`qemu-x86_64` and `hl-engine-linux-x86_64`.

```sh
tests/fuzz/isa/x86_64/run.sh --seeds 200 --ignore-mxcsr
tests/fuzz/isa/x86_64/run.sh --list 137 --ignore-mxcsr
tests/fuzz/isa/x86_64/run.sh --minimize 137 --ignore-mxcsr
```

Failures are retained under `build/isafuzz/repro/sN/`. Minimization removes
whole generated instruction groups while requiring the same failure class and
state key. Use `--ignore-flags` when minimizing a non-flag divergence because
the final defined-flag mask belongs to the original sequence.

Coverage includes integer ALU and flag chains, shifts and rotates, multiply and
divide, bit operations, atomics, scalar and packed SSE, conversions, shuffles,
NaNs, infinities, denormals, and memory operands. `+bmi` and `+x87` enable
additional instruction groups.

The default campaign ignores MXCSR because x86 denormal-operand status has no
equivalent AArch64 flag without per-instruction input probes. Known QEMU oracle
differences are excluded: narrow RCL/RCR full-period counts, zero-count
SHLD/SHRD writeback, and failed 32-bit register CMPXCHG zero-extension.

`+x87` remains limited by the engine's binary64 x87 representation; workloads
requiring architectural 80-bit intermediates can legitimately diverge.
