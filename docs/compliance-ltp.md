# LTP compliance lane

A differential lane under `tests/compliance/ltp/`: build a curated subset of the Linux Test
Project's CORE syscall tests as static-PIE binaries for both guest ISAs, run each one under
an oracle and under the engine, and compare LTP verdicts. It is **not** wired into the
Makefile, CMake or CI — it is a manual investigation lane driven by three shell scripts.

## Layout

| Path | Role |
|---|---|
| `tests.list` | The curated set: `<category> <syscall> <relpath-under-testcases/kernel/syscalls>`, 430 tests across 13 categories (`fileio` 148, `proc` 60, `mm` 41, `poll` 31, `ipc` 26, `timer` 23, `sched` 21, `cred` 21, `xattr` 20, `time` 15, `signal` 12, `net` 6, `misc` 6). |
| `build.sh` | Fetch + cross-compile stage. |
| `run.sh` | Run + score stage, one test at a time. |
| `run-parallel.sh` | Same matrix in bounded per-category batches (`LTP_JOBS`, default 8), assembling one deterministic table. |
| `config.h` | Hand-built `include/config.h` standing in for `./configure` on a modern glibc host. |
| `results-baseline-v0.9.25.tsv` | A recorded run (see below). |
| `out/` | Working tree: `ltp-src/`, `bin/<arch>/<test>`, `logs/`, `results.tsv`. |

## Build

`build.sh` clones LTP at a pinned commit (`LTP_PIN`, default
`ae4a01208fa2ce31f4f0a7a92b6e71a32299eb94`) and compiles the "new API" libltp plus the
curated tests for aarch64 and x86_64 (`CC_ARM`, `CC_X86`). LTP's normal build needs
autoconf/automake plus a generated per-arch syscall table; neither is available here, so the
script drives the two pure-shell generators (`generate_syscalls.sh`, `ltp-version.h`)
directly and supplies `config.h`. An existing `out/ltp-src/` checkout is reused. It raises
its own `ulimit -n` because ~900 sequential `cc` invocations exhaust a small inherited
soft limit partway through.

## Run and score

Oracle: on arm64 the same static binary runs natively; x86_64 runs under `qemu-x86_64`.
Engine: `$HL_ENGINE_DIR/hl-engine-linux-<arch>` (default `build/linux-production`), directly
on a Linux host or through `HL_ENGINE_RUNNER` as a bridge command elsewhere. Each execution
gets a fresh session and every process still carrying that sid is reaped, so a timed-out
fork test cannot leak orphan engines into the next category.

Each run is classified from the test's own TPASS/TFAIL/TBROK/TCONF counts plus exit status
into `{PASS,FAIL,BROK,CONF,CRASH,TIMEOUT}`, then diffed:

* oracle not PASS → `skip`. No valid ground truth; never counted against the engine.
* oracle PASS, engine PASS with all assertions and the oracle's exit status → `ok`.
* oracle PASS, engine PASS with all assertions but a nonzero exit → `TEARDOWN`. The
  assertions are correct and the harness exit path is not; reported apart from per-syscall
  gaps because it is one systemic root cause.
* anything else → `DD-GAP`, a real compliance gap.

A run that produced no result lines is retried up to three times, because a contended host
can kill an unrelated launch; a run that emitted assertions is final.

The scorecard reports, per arch, `syscall-assertion PASS` (`ok + TEARDOWN`) and `clean-run`
(`ok`) over oracle-PASS tests, a per-category table, and the `TEARDOWN` and `DD-GAP` lists.

Selectors: `LTP_ARCHES`, `LTP_CATEGORY`, `LTP_ONLY` (space-separated test names),
`LTP_TIMEOUT` (default 20s), `LTP_OUT`, `LTP_BIN_ROOT`.

## Recorded baseline

`results-baseline-v0.9.25.tsv` is a **partial** run: 149 rows, not the full 860
(arch × test) matrix — arm64 47 `ok` / 13 `TEARDOWN` / 35 `DD-GAP` / 24 `skip`, x86_64
14 / 3 / 7 / 6. Treat it as a snapshot of the categories that were executed, not as a
coverage number.

## Skipped / out-of-scope

`tests.list` is curated to the surface the engine emulates. Deliberately excluded: tests
needing multiple binaries or external helpers, and device-, hugepage-, cgroup-, module- and
privilege-heavy tests. Anything whose oracle does not itself pass on the host is additionally
excluded at scoring time as `skip`, since there is no ground truth to compare against.
