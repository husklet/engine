# Differential compat: dispositions, exclusions, open gaps

Every case here was written as a small self-contained C program, run **natively on the host
Linux kernel** to produce its golden output, and only then run through both production
engines. Where the engine disagreed with the kernel, the kernel is the specification.

A suite is green because cases pass or self-skip on a detected incapability — never because
a failure is hidden. `.github/workflows/*.yml` contain no `continue-on-error` and no
`--skip` list.

## Fixed divergences

| Case | Divergence | Fix |
|---|---|---|
| `sc-dup3-edges` (`tests/compat/syscall_edges/dup3_edges.c`) | `dup3(fd, 200, 0x40000000)` returned the descriptor instead of `EINVAL`. `translator/guest/x86_64/legacy.c` rewrote the x86-only `dup2` into `dup3` and signalled it by setting **bit 30 of the flags argument**, which the shared handlers stripped before validating — so a guest passing `0x40000000` itself got `dup2` semantics, including the `oldfd == newfd` no-op `dup3` must reject. | The signal moved out of band, mirroring `g_x86_forksave`: per-thread `g_x86_dup2_compat` in `legacy.c`, refreshed on every normalized syscall, read through `hl_x86_legacy_is_dup2()` (`legacy.h`) and the `G_IS_DUP2_COMPAT()` seam (`0` for aarch64, which has no legacy `dup2`). Flags now reach the validator as the guest wrote them. |
| `signalfd-state` (`tests/compat/signals/signalfd_state.c`) | Two bugs in one line. `sigq_push` coalesces a second pending instance of a standard signal, but `raise_guest_signal_si` wrote the signalfd self-pipe wake byte unconditionally, so the second `read(2)` returned a fabricated 128-byte record where Linux returns `EAGAIN`. And `thread_kill` stamped `SI_TKILL` only when routing to another thread; a self-directed `tkill`/`tgkill` (glibc `raise()` lowers to `tgkill`) fell through to `SI_USER`. | `sigq_push` reports whether it enqueued and `sfd_deliver` is gated on that; the self-signal path calls `raise_guest_signal_si(..., HL_SI_TKILL, ...)`. |
| `guard-page-efault` (`tests/compat/memory/guard_page_efault.c`) | `read(2)` into a buffer straddling a guest `PROT_NONE` page was all-or-nothing. hl force-maps guest anonymous pages host-writable and models `PROT_NONE` in the `g_gna` registry, so `io.c` returned `EFAULT` on *any* overlap; Linux `copy_to_user` is byte-granular and returns the short count. Separately `writev`/`pwritev` (66/70) skipped the guard that `write`/`pwrite` (64/68) had, so the mac host read the force-mapped `PROT_NONE` page and returned a byte count instead of `EFAULT`. | `gna_prefix()` (`linux_abi/thread.c:1221`) returns the leading non-`PROT_NONE` byte count; the read family clamps to it and only `EFAULT`s on an empty prefix. The write family keeps all-or-nothing deliberately — the native oracle shows Linux's pipe/`writev` paths fail the whole call — and the guard was extended to `writev`/`pwritev`. |
| `pf-comm-status` (`tests/compat/procfs/comm_status.c`) | Writing `/proc/self/comm` renames the task on Linux exactly as `prctl(PR_SET_NAME)` does, visible through `PR_GET_NAME` and `/proc/self/{comm,status:Name,stat}`. The engine accepted the write into the synthetic backing file and dropped it. | Write intercept on the `self:comm` tagged descriptor in `linux_abi/syscall/io.c`, mirroring `self:oom_score_adj`: truncate to `TASK_COMM_LEN-1`, drop one trailing newline, update `g_procname` + `set_guest_comm_name()`, re-render the backing file, return the full count. `synth_stat_raw` now reports mode 0644 for `/proc/self/comm`. |
| `sc-iov-limits` (`tests/compat/syscall_edges/iovmax_edges.c`) | Two host-passthrough bugs. `readv/writev/preadv/pwritev` with `iovcnt==0` must return 0; the mac/BSD host libc returned `EINVAL`. A segment whose `base+len` overflows the user address ceiling must be `EFAULT` (Linux `access_ok`); the mac host returned `EINVAL`. | Both emulated in `src/linux_abi/syscall/io.c`, so the surface is host-invariant. Golden unchanged (== native oracle). |

## Exclusions

`excluded-macos` in a manifest runs a case as active on the ELF/Linux engine and skips it
only on the Mach-O/macOS engine (`tools/linux_matrix.c`, `tools/matrix_runner.c`). It masks
nothing on the Linux lane. Three IPC cases hold that disposition; each was verified to pass
on the Linux engine and to fail on macOS only for Linux-only kernel behaviour:

| Case | Only divergence |
|---|---|
| `pipe-fill-exact` (`ipc_pipe_fill.c`) | macOS pipes have no `F_SETPIPE_SZ`, no Linux page accounting, no negative-size `EINVAL`, and a different `PIPE_BUF` atomic-refusal boundary. |
| `socketpair-peek` (`ipc_socketpair_peek.c`) | `MSG_PEEK\|MSG_TRUNC` on an AF_UNIX datagram: Linux reports the full datagram length (40) into a short buffer, macOS reports the copied length (16). Everything else matches. |
| `fork-fd-locks` (`ipc_fork_fd_locks.c`) | macOS has no OFD locks, so `F_OFD_SETLK` fails. |

## Host-conditional dispositions: an x86_64 Linux host

Every disposition above is keyed on the **engine** under test. These two are keyed on the
**host CPU**, which only became a second axis when x86-64 Linux became a host. Neither guest
frontend has an amd64 back end — both emit ARM64 — so that host's execution backend
interprets rather than emits, at roughly 10-50x the cost of the ARM64-host JIT
(`docs/amd64-host.md` sections 2-3). Two gates were calibrated against a JIT and cannot be
read there the way they are read on an ARM64 host.

Neither is disabled. One is rescaled and says so; the other measures instead of judging and
says so. `HL_HOST_ARCH` (derived once in `CMakeLists.txt`) is the only test either makes, so
an aarch64 host takes neither branch: with the scale at 1 and thresholds enforced, the
generated `build/CTestTestfile.cmake` is byte-for-byte the file it was before these existed,
which is a property of the build rather than a claim about it (`hl_matrix_timeout_scale()`
writes nothing at 1, and the enforced perf command line is unchanged).

| Gate | aarch64 host | x86_64 host | What names it in the output |
|---|---|---|---|
| per-case guest timeout (`matrix_runner.c` 120s, `linux_matrix.c` 20s, `e2e_runner.c`/`config_e2e_runner.c`/`rootfs_e2e_runner.c` 30s, `checkpoint_tree_runner.c` 15s) | unchanged | multiplied by `HL_MATRIX_TIMEOUT_SCALE` (30), and so is the CTest `TIMEOUT` of the 226 tests the six runners drive | `per-case timeout scaled x30 ...` on stdout at the start of the run, repeated after the pass/fail summary; a timeout diagnostic names the budget that expired |
| tracked cold/p99 perf thresholds (`PERF_LIMIT_*`, `cmake/Phase3Gates.cmake`) | enforced, 26 cases | recorded, not enforced (`HL_PERF_ENFORCE=OFF`) | the case is *named* `perf.linux-<case>-<arch>.record-only`, and echoes `RECORD-ONLY ...: measured but NOT gated` with the thresholds that were not applied |

### Why the timeout is scaled rather than raised for everyone

20s and 120s are hang detectors, and on an ARM64 host they are unambiguous: every case in
every suite these runners drive is milliseconds of guest work, so "did not finish" could only
mean hung. Interpreted, a correct case can outrun the budget and be killed by the
process-group kill — reporting a hang, which is both false and indistinguishable from the
real thing, and destroying the stdout that would have told them apart. Raising the constants
for every host would blunt the detector where it still works.

The scale is set once where the lanes are registered (`cmake/Phase3Compat.cmake`) and passed
in the environment; it is deliberately not detected inside the runners. Whether the engine a
runner execs interprets is a property of that binary, and inferring it inside the harness
would couple the harness to backend internals and silently re-tune itself whenever they
changed. A malformed or out-of-range value is a hard refusal in both the runners and the
configure, never a fallback to 1, because a silent fallback presents as a suite full of
unexplained timeouts — exactly the diagnostic this exists to prevent. It multiplies
`HL_MATRIX_CASE_TIMEOUT_MS` rather than replacing it: that variable says how much work *this
suite* does (`.github/workflows/linux.yml` gives `compat-soak` 600s), the scale says how much
slower *this host* executes any of it.

30 is the middle of the range `docs/amd64-host.md` predicts, not a measurement — the
interpreters are being written. It is a `-D` cache variable so it can be corrected once there
are numbers; what must not happen is that it silently stays at 1.

### Why the perf thresholds are recorded rather than re-set or skipped

`PERF_LIMIT_*` are tracked JIT numbers, and DOCS.md's "Performance and release" roadmap item
commits to enforcing them. Enforced on an interpreter, all thirteen cases per guest ISA fail
for the one reason already known and written down, which measures nothing. Three dispositions
were possible:

* **a second, host-conditional set of thresholds** — rejected. Nobody has measured them. Any
  number written today blesses whatever a half-written interpreter happens to do, and the
  first real optimisation would "regress" against it.
* **not-applicable, skip the lane** — rejected. It stops exercising the path: `perf-runner`
  drives one cold launch plus warmups plus samples — 29 real guest launches for a normal case
  — through the production engine, the densest execution coverage in the tree outside the
  compat matrix. It would also empty `perf-linux`, a lane
  `cmake/CiLanes.cmake` declares for Linux, so `gate.ci-lane-parity` would have to be taught
  an exemption for a lane that can in fact run.
* **record-only** — chosen. The Stage-2 same-ISA transliterator (`docs/amd64-host.md` §3)
  needs a number to beat, and an interpreter baseline taken by the same runner, on the same
  fixtures, in the same format as the aarch64-host numbers is exactly that. The measurement
  is the deliverable; only the verdict is what cannot be honest yet.

Record-only cases are **renamed**, not merely relabelled, because a CTest summary line is
often all anyone reads and `Passed perf.linux-startup-x86_64` would imply a threshold was
met. The `--label` `perf-runner` records is *not* renamed: `metric=linux-startup-x86_64` has
to stay comparable across hosts and across Stage 2, and being record-only is a property of
the run, not of the metric — the `host_arch=` field on the same line already identifies which
host produced it.

What still enforces on an x86_64 host, and must: `perf.linux-resource-<arch>`. Its assertions
live inside `tests/perf/resource.c` and bound retained RSS growth (2048 pages) plus a return
to the descriptor and thread baseline after teardown. Those are leak bounds, and a leak is not
a function of how fast the backend executes — an interpreter with no code cache should retain
less, not more. `--expect` still checks every guest's exit status in the record-only cases
too, so a case that stops producing the right answer still fails; only the timing verdict is
suspended.

### What a green x86_64 lane therefore does not prove

It does not prove comparable speed, and it does not prove threshold compliance. Two other
per-case budgets are *not* covered by the scale, because they live in runners outside the
matrix pair and will need the same knob before their lanes can run interpreted:
`tools/e2e_runner.c`, `tools/config_e2e_runner.c` and `tools/rootfs_e2e_runner.c` (30s, so
`e2e-oracle`, `production.config-*`, `dynamic-e2e` and the `warm-cache` perf case) and
`tests/integration/checkpoint_tree_runner.c` (15s, every `checkpoint.*` case). Those lanes are
absent from `.github/workflows/linux-x86_64.yml` today for a different reason — the engine
cannot execute a guest there yet — so nothing is currently hidden by it.

## Runtime self-skips

Detected, printed, and applicable-environment-only.

* **checkpoint suites** — `pkgs/rust/tests/support/checkpoint_env.rs` runs one real capture
  probe and skips the checkpoint tests only where capture cannot publish a manifest.
* **`typed_machines_cross_the_old_private_descriptor_ceiling_and_release_files`** —
  `private_descriptor_band_width()` in `pkgs/rust/tests/spec.rs` reads `/proc/self/limits`,
  computes the engine's private fd band
  (`floor = min(soft_nofile - 4096, 65536); band = soft_nofile - floor`) and self-skips with
  a printed reason when the band cannot hold 4097+128 slots. The GitHub hosted runner's soft
  `RLIMIT_NOFILE` of 65536 collapses the band to 4096; a dev host has ~10^6. The underlying
  band-layout gap in `src/host/private.c` is a separate engine fix.

## Open gap: epoll edge state for emulated objects

On the macOS backend epoll is emulated over kqueue (`src/linux_abi/syscall/event.c`), which
maps `EPOLLET -> EV_CLEAR` and `EPOLLONESHOT -> EV_ONESHOT` for kqueue-able fds. An
**emulated object** registered in an epoll set is instead served by the object-readiness
path (`event.c:989`), which samples current readiness once per wait. That path disarms
`EPOLLONESHOT` (`event.c:1012`, `ep_object_free` after one delivery) but holds no
edge-transition state, so `EPOLLET` on an emulated object degrades to level-triggered
reporting: the observed sequence was `n=1,1,1,1,1,1,1,1` where Linux gives `1,1,1,0,1,1,0,1`.

`sc-epoll-semantics` (`tests/compat/syscall_edges/epoll_semantics.c`) therefore asserts only
the host-invariant contract — `EEXIST` dup-ADD, `ENOENT` MOD/DEL, `EINVAL` self-add,
level-triggered readable-until-drained — with the golden regenerated from the native oracle.
Asserting an exact oneshot/edge readiness sequence there would encode a host-specific
outcome. The gap needs its own fix: apply edge state to the emulated-object epoll path.
