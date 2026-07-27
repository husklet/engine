# Testing the AArch64 host under qemu-user

The engine's AArch64 host backend is cross-compiled on x86_64 CI (`build-arm-check`)
and then never executed there. Every aarch64-host change on this branch has shipped
with "needs a run on an aarch64 host" attached. `qemu-aarch64` closes most of that
gap on the machine that already builds the tree.

This document records **what that is worth**, measured rather than assumed. A green
run under emulation is evidence, not proof, and the boundary matters: a false green
is worse than no result.

Lane: `emulated.*`, `cmake/Phase3Gates.cmake` section 9b, driver
`tools/emulated_aarch64_gate.sh`. Two labels — `emulated-aarch64` is every cell,
`emulated-aarch64-gated` is the subset `.github/workflows/linux-x86_64.yml` runs.
See "Status" below for which cell is in neither and why.

## Running it

`qemu-aarch64` comes from the flake devShell (`toolchainFor.emulators`, x86_64
Linux only). Nothing names a `/nix/store` path; **flake.lock is the pin**.

```sh
cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake
ninja -C build-arm-check
nix develop --command ctest --test-dir build -L emulated-aarch64 -V
```

`HL_QEMU_AARCH64=/path/to/qemu-aarch64` overrides the devShell's copy.
Use `-V`: CTest prints nothing at all for a skipped test — verified, a skip under
`--output-on-failure` produces only a "did not run" line — and the skip message is
the part that names what is missing.

The emulator version is this lane's **oracle**, so the gate prints it on every run
and a nixpkgs bump that moves it invalidates the table below until re-measured.

## What was measured

Probes were compiled for both aarch64 (run under qemu) and x86_64 (run natively as
an oracle for the arch-neutral behaviour), then compared. Verified against
qemu 11.0.2, and the lane re-measured on **qemu-user 10.2.2** — the version
flake.lock's nixpkgs serves, and the one CI will therefore use. Every cell gave
byte-identical results on both, down to the offset of each failing diff, so the
two versions are interchangeable as oracles for this corpus.

### Category A — vouched for

| Mechanism | Probe result |
|---|---|
| Dual-alias W^X arena: `memfd_create` + RW and RX `MAP_SHARED` aliases | first execution correct |
| Rewriting code through the RW alias, re-executing at RX | correct after `__builtin___clear_cache` |
| **Missing** cache maintenance | executes STALE code — the emulator is *stricter* than the x86 host, so a forgotten flush fails here rather than passing |
| `mprotect` RW↔RX transitions with in-place rewrite | correct |
| `MAP_FIXED` dropped over a page that has already executed | correct |
| `mremap` of a code mapping | correct |
| Cross-thread patching of code another thread is **executing** (tier-2 promotion, chain back-patch) | 3 threads, 2000 patches, 507786 calls, 0 wrong results |
| `sigaction` + `siglongjmp` out of the handler, repeated | correct |
| `sigsetjmp(pad, 1)` vs `(pad, 0)` mask semantics | both correct |
| `uc_mcontext.pc` / `.regs[]` / `.sp` at a fault | correct |
| `fpsimd_context` located by walking `uc_mcontext.__reserved` for `FPSIMD_MAGIC` | correct — V registers readable |
| Resuming by advancing `uc_mcontext.pc` | correct |
| SIGSEGV raised **inside JIT arena code**, host pc reported inside the arena | correct, and resumable |
| SIGILL from a reserved encoding (Q=0 `ZIP1 .1D`) | raised |
| `sigaltstack` + `SA_ONSTACK` | correct |
| `PTHREAD_PROCESS_SHARED` mutex/condvar in a `MAP_SHARED` region across `fork()` — the engine's actual guest-futex mechanism | correct |
| Arena inherited and rewritten after `fork()` | correct |
| `mrs tpidr_el0` agrees with `__builtin_thread_pointer()` | correct |
| Threaded atomic RMW | correct |
| FP semantics, including NEON NaN propagation | see below — the strongest single result |

The FP result deserves its own note. The previous ISA golden
(`tests/compat/isa/x86_64/expected/isa-regress.out` before commit `1387b1df`) was
captured from the ARM64 JIT **on real aarch64 hardware**. Running the same fixture
through the same JIT under qemu reproduces that file **byte-for-byte on all 1048
lines**, NaN cases included. That is an independent hardware oracle agreeing with
the emulator across the whole ISA corpus, and it is far better evidence than any
synthetic probe.

### Category B — not vouched for

- **Weak memory ordering.** qemu-user on an x86_64 host executes guest loads and
  stores under the host's ordering, which is strictly stronger than AArch64's. A
  missing `dmb`/`dsb`/acquire-release in the engine's lock-free code (the IBTC
  publish, the STW flush protocol, `ibtc_publish`'s 128-bit release) will pass here
  and can still fail on silicon. Nothing in this lane can find such a bug.
- **Timing, and anything derived from it.** Emulation is roughly an order of
  magnitude slower and the slowdown is not uniform, so no perf lane belongs here.
  It also makes real races far less likely to be *observed* even where they exist.
- **Host CPU feature detection.** `ctr_el0` reads `0x80038003` (32-byte cache
  lines) and `dczid_el0` reads `0x7` under qemu; real hardware commonly differs.
  Code that sizes loops from these sees qemu's values.

  This produces a **false RED**, not a false green. One was observed:
  `completeness/dotprod` failed under the lane with `hwcap_dp=0` where the golden
  says `1`, while the same fixture run *directly* under `qemu-aarch64` with no
  engine reported `1` — the divergence was in what the engine saw and re-advertised
  while itself emulated, not in the dotprod lowering. It **passes as of
  2026-07-27**, after the feature-advertisement and auxv work (`23afa33e`,
  `bb57d321`); the caveat stands even though its example no longer fires. Treat any
  HWCAP/`mrs`-derived case that fails only here as unproven in both directions
  until it runs on hardware.
- **`tpidrro_el0`.** Reads 0 under qemu. That matches Linux/aarch64, which zeroes it
  for 64-bit tasks — but it means the legacy `hl_a64_load_cpu` path
  (`src/translator/host/aarch64/asm.c:98`, emitted only when `!stealfast_on()`)
  cannot be distinguished here from a working one. It is a Darwin-shaped idiom.
- **LSE/atomic contention behaviour** beyond correctness of the result.
- **`execve` of an aarch64 binary from inside the emulated process.** The probe
  exec'd a native binary, which the kernel handles directly; qemu's own re-entry
  path is untested here. The engine forks far more than it execs, so this has not
  mattered, but it is not covered.

### Not applicable

The engine uses `memfd_create` **without** `MFD_ALLOW_SEALING` and calls
`F_ADD_SEALS` nowhere, so seal behaviour — which the probes initially chased — is
not part of its contract. Noted so the next person does not re-derive it.

## Status

Measured 2026-07-27 against `build-arm-check`, twice, on qemu-user 10.2.2 and 11.0.2.

| Cell | Result | Gated |
|---|---|---|
| `emulated.isa-x86-64` | 8/8 pass, 37s | yes |
| `emulated.abi` | 71/71 aarch64, 73/73 x86-64, 63s | yes |
| `emulated.completeness` | 117/118 aarch64, 150/154 x86-64, 264s | **no** |

`emulated-aarch64-gated` carries the two green cells and is run by
`.github/workflows/linux-x86_64.yml`. `emulated-aarch64` carries all three and stays
in `HL_CI_REGISTRY_LINUX`. Both are reserved to `Linux-x86_64` by
`HL_CI_HOST_CPU_ONLY`.

`emulated.abi` is the **control**: without a cell that must be green, total
breakage and a correctly-reported defect look alike.

The five defects that originally kept the lane out of a workflow are fixed, and every
one was found **by this lane** — none was reachable before it existed. The 107-line
`isa-x86-64` failure was the two-NaN operand rule: the engine implemented
qemu-x86_64's softfloat `float_2nan_prop_x87`, where real hardware is plain
src1-priority-quieted (measured on Zen 4 over all 64 ordered NaN pairs; the goldens
were right all along). `movq-mmx-xmm` (MOVQ2DQ/MOVDQ2Q not lowered at all),
`cvt-mmx` (all 160 lines), `x87-compare-codes` (FCOM/FCOMI did not set FSW.IE),
`mmx-width` and `div-overflow` now pass too.

### Why `completeness` is not gated

Four cases fail. Three are real defects in the shipped aarch64 host, all three
passing on the native x86-64 host engine and failing identically under both qemu
versions — so neither an emulator artefact nor a wrong golden:

- `x87-stack-faults` — the engine aborts with `UNIMPL 1B opcode 0xdd` at the guest's
  `dd b5 60 ff ff ff`, i.e. **FRSTOR (DD /4) is not lowered**.
- `x87-fprem-loop` — FPREM results diverge from byte 47.
- `x87-precision-rounding` — precision-control results diverge from byte 204.

All three fixtures arrived with `5d28c1ca`, which fixed the x86-host arm alone: the
aarch64 arm shipped in the same commit without ever running. That is exactly the
failure mode this lane exists to end, so the cell is left red and visible rather
than excluded from the registry.

The fourth, `priority`, is environmental — the harness is already at nice 12 and
cannot lower it, and it fails identically on both ISAs and on the native lane
(`docs/ci-green.md`, "Host-environment preconditions").

Add `GATED` to the cell in `cmake/Phase3Gates.cmake` section 9b once the three x87
defects are fixed.
