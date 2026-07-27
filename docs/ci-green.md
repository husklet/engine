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
| `guard-page-efault` (`tests/compat/memory/guard_page_efault.c`) | `read(2)` into a buffer straddling a guest `PROT_NONE` page was all-or-nothing. hl force-maps guest anonymous pages host-writable and models `PROT_NONE` in the `g_gna` registry, so `io.c` returned `EFAULT` on *any* overlap; Linux `copy_to_user` is byte-granular and returns the short count. Separately `writev`/`pwritev` (66/70) skipped the guard that `write`/`pwrite` (64/68) had, so the mac host read the force-mapped `PROT_NONE` page and returned a byte count instead of `EFAULT`. | `gna_prefix()` (`linux_abi/thread.c:1331`) returns the leading non-`PROT_NONE` byte count; the read family clamps to it and only `EFAULT`s on an empty prefix. The write family keeps all-or-nothing deliberately — the native oracle shows Linux's pipe/`writev` paths fail the whole call — and the guard was extended to `writev`/`pwritev`. |
| `pf-comm-status` (`tests/compat/procfs/comm_status.c`) | Writing `/proc/self/comm` renames the task on Linux exactly as `prctl(PR_SET_NAME)` does, visible through `PR_GET_NAME` and `/proc/self/{comm,status:Name,stat}`. The engine accepted the write into the synthetic backing file and dropped it. | Write intercept on the `self:comm` tagged descriptor in `linux_abi/syscall/io.c`, mirroring `self:oom_score_adj`: truncate to `TASK_COMM_LEN-1`, drop one trailing newline, update `g_procname` + `set_guest_comm_name()`, re-render the backing file, return the full count. `synth_stat_raw` now reports mode 0644 for `/proc/self/comm`. |
| `sc-iov-limits` (`tests/compat/syscall_edges/iovmax_edges.c`) | Two host-passthrough bugs. `readv/writev/preadv/pwritev` with `iovcnt==0` must return 0; the mac/BSD host libc returned `EINVAL`. A segment whose `base+len` overflows the user address ceiling must be `EFAULT` (Linux `access_ok`); the mac host returned `EINVAL`. | Both emulated in `src/linux_abi/syscall/io.c`, so the surface is host-invariant. Golden unchanged (== native oracle). |

## Exclusions

`excluded-macos` in a manifest runs a case as active on the ELF/Linux engine and skips it
only on the Mach-O/macOS engine (`tools/linux_matrix.c`, `tools/matrix_runner.c`). It masks
nothing on the Linux lane. **58 cases across 11 suites** hold that disposition, not the three this
section used to claim: `memory` 15, `network` 8, `ipc` and `syscall` 7 each, `filesystem` and `process`
5, `posix` and `procfs` 4, and one each in `abi`, `completeness` and `syscall_edges`. Count them from the
manifests rather than from here — field 12 is the disposition:

```text
awk -F'\t' '$12=="excluded-macos"{print FILENAME"\t"$1}' tests/compat/*/manifest.tsv
```

Each carries its reason in field 13. The three IPC ones below are exemplars of the shape, not the
whole set; each was verified to pass on the Linux engine and to fail on macOS only for Linux-only
kernel behaviour:

| Case | Only divergence |
|---|---|
| `pipe-fill-exact` (`ipc_pipe_fill.c`) | macOS pipes have no `F_SETPIPE_SZ`, no Linux page accounting, no negative-size `EINVAL`, and a different `PIPE_BUF` atomic-refusal boundary. |
| `socketpair-peek` (`ipc_socketpair_peek.c`) | `MSG_PEEK\|MSG_TRUNC` on an AF_UNIX datagram: Linux reports the full datagram length (40) into a short buffer, macOS reports the copied length (16). Everything else matches. |
| `fork-fd-locks` (`ipc_fork_fd_locks.c`) | macOS has no OFD locks, so `F_OFD_SETLK` fails. |

## Host-conditional dispositions: an x86_64 Linux host

Every disposition above is keyed on the **engine** under test. These two are keyed on the
**host CPU**, which only became a second axis when x86-64 Linux became a host. Neither guest
frontend has an amd64 back end — both emit ARM64 — so that host's execution backend interprets rather
than emits. `docs/amd64-host.md` predicted "roughly 10-50x"; measured, the cost is
**3.4x to 605x, geometric mean 26.6x** (`docs/amd64-host-performance.md` §2, engine-vs-native CPU time
on the thirteen perf payloads). The prediction holds for the middle of the distribution and not at
either end: kernel-bound cases are 3-12x because the host kernel does the work in both columns, and
guest-execution-bound cases are 94-605x. Two gates were calibrated against a JIT and cannot be read
there the way they are read on an ARM64 host.

Neither is disabled. One is rescaled and says so; the other measures instead of judging and
says so. `HL_HOST_ARCH` (derived once in `CMakeLists.txt`) is the only test either makes, so
an aarch64 host takes neither branch: with the scale at 1 and thresholds enforced, the
generated `build/CTestTestfile.cmake` is byte-for-byte the file it was before these existed,
which is a property of the build rather than a claim about it (`hl_matrix_timeout_scale()`
writes nothing at 1, and the enforced perf command line is unchanged).

| Gate | aarch64 host | x86_64 host | What names it in the output |
|---|---|---|---|
| per-case guest timeout (`matrix_runner.c` 120s, `linux_matrix.c` 20s, `e2e_runner.c`/`config_e2e_runner.c`/`rootfs_e2e_runner.c` 30s, `checkpoint_tree_runner.c` 15s) | unchanged | multiplied by `HL_MATRIX_TIMEOUT_SCALE` (30), and so is the CTest `TIMEOUT` of the 231 tests the runners drive (count it: `grep -c HL_MATRIX_TIMEOUT_SCALE=30 build/CTestTestfile.cmake`) | `per-case timeout scaled x30 ...` on stdout at the start of the run, repeated after the pass/fail summary; a timeout diagnostic names the budget that expired |
| per-case **stall** detector (`matrix_runner.c`, `linux_matrix.c`) | inert by construction — its budget is ≥ the wall budget, which fires first | armed: a case whose process tree consumes no CPU and writes no output for the *unscaled* budget (≥60s) is killed and reported as hung | `HUNG -- no ... output and no CPU in its process tree for <n>ms, with the <n>ms per-case budget unexpired` |
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

30 was picked as the middle of the range `docs/amd64-host.md` predicted, before anything was
measured. There are numbers now, and the geometric mean (26.6x) lands within 15% of it — luck,
not derivation, and still wrong per case in both directions: `compute` needs 605x, `mmap` 3.9x.
It stays one `-D` cache variable rather than a per-case table, which would be thirteen numbers
to maintain against a backend under active change; the cases 30 cannot cover carry derived
per-case CTest budgets instead (`cmake/Phase3Gates.cmake`, `perf.linux-compute-*` at 4800s).
What must not happen is that it silently stays at 1.

### What re-arms the hang detector the scale disarmed

Scaling fixed a false positive and bought a false negative with it. At scale 30 the matrix
runner's budget is 3600s per case, and `compat-soak`'s `HL_MATRIX_CASE_TIMEOUT_MS=600000`
makes it **five hours**; the CTest `TIMEOUT` becomes 30h per suite. Nothing bounded a genuine
hang any more — and there was one to bound. The x86-64 futex-across-fork cases
(`threads/futex-fork-stale-waiter`, `process/{fork-blocked-io,fork-child-futex,shared-key-futex}`)
were observed on this branch sitting in `do_wait`/`futex_do_wait` indefinitely on the x86-64
guest, at **zero CPU** and with zero output — measured at 95s of elapsed time with `00:00:00`
of process time across the whole tree. (That engine defect has since been fixed; the detector
is for the class, not for those four cases, and the zero-CPU signature is what the class looks
like.)

No choice of number separates the two, because they differ in kind. An interpreted guest is
10–50x slower per unit of work; it is not one bit more *idle*. So both matrix runners measure
progress directly:

> **progress** := the guest's captured stdout/stderr grew, **or** the case's process tree
> consumed CPU.

Both signals are required. Output alone would kill a correct case that computes for minutes
before printing (`soak/reallocchurn` does exactly that); CPU alone would kill a correct case
blocked on a timer. They are only *both* absent when nothing in the launch is running or
producing anything — which is what a hang is. Neither signal can come from the runner's pipes:
`tools/remote_supervisor.c` writes a heartbeat byte to stderr every 250ms, so pipe traffic
proves the supervisor is alive and says nothing about the guest. The tree is walked by parent
link rather than by process group, because the supervisor deliberately puts the engine in a
group of its own; anything the host will not answer (a non-Linux host, an unreadable `/proc`)
reports *unknown*, and unknown counts as progress — missing evidence must never manufacture a
hang.

The stall budget is **derived, not invented**: it is the *unscaled* per-case budget, with a
60s floor. Two consequences, both wanted.

* Wherever the scale is 1, the stall budget is ≥ the wall budget, so the wall clock fires
  first and the detector cannot change a verdict. The aarch64 and Darwin lanes are unaffected
  **by arithmetic**, not by assertion.
* A suite that declares it does more work (`compat-soak`'s 600s) gets a proportionally longer
  stall budget for free. The one knob that means "this suite is big" already means "this suite
  may legitimately go quiet for longer", so there is no second variable to get wrong.

Why the floor cannot fire on a slow guest: firing requires 60s in which the whole process tree
burned **zero** CPU ticks and wrote **zero** bytes. The longest deliberate sleep in the corpus
these runners drive is 10s; the longest `poll`/`epoll_wait` timeout is under 10s; and the
100s/50s alarms and 100s/1000s timers in `tests/compat/time` are armed and then read back or
cancelled, never waited on. So the margin is ≥6x — and it is a margin against
idleness, which is the axis a slower host does not move along. A true hang is now bounded to
2 minutes per case instead of 5 hours.

What it deliberately does **not** catch: a livelock that burns CPU forever. That is
indistinguishable from a correct long computation without a semantics for the guest, so it
stays the wall clock's job — which is why the wall clock is still there.

### Why the perf thresholds are recorded rather than re-set or skipped

`PERF_LIMIT_*` are tracked JIT numbers, and DOCS.md's "Performance and release" roadmap item
commits to enforcing them.

This paragraph used to say that enforced on an interpreter "all thirteen cases per guest ISA fail".
That is measurably false and the number was never taken: **five of the thirteen pass the AArch64-host
JIT thresholds unchanged** — `startup`, `translation`, `ipc-throughput`, `fork-stress`, `warm-cache` —
**and four more are within 3x** (`docs/amd64-host-performance.md` §2, run through `perf-runner` itself
with the real `PERF_LIMIT_*` pairs applied, so it is the lane's own verdict). `cmake/Phase3Gates.cmake`
was corrected in `2e1b90bb`; this is the same fix.

The decision is unchanged, for the reason that is actually true. Enforcing the five that pass would buy
flaky red rather than signal:

* **Four of the five clear p99 on margin this host cannot hold.** `translation` by 5.6% and
  `fork-stress` by 9.5%; `startup` by 39% and `ipc-throughput` by 38%. The measured load spread on this
  machine is **1.9x** (`compute` at 348 s/run under load against a 184 s median), and several agents
  build here concurrently. A 5.6% margin against a 1.9x spread fails on the first busy run, and a
  threshold that fires on load reports a regression that did not happen.
* **`warm-cache`, the only one with real headroom, measures a tautology here.** Warm 29,919 us CPU
  against cold 29,852 us — **0.2%**, inside noise — because an interpreter emits no host code for the
  persistent code cache to hold. Enforcing a warm/cold threshold where warm and cold are the same
  measurement gates nothing.

And `compute` is 245x/283x over, so no single threshold set can serve both hosts anyway. Three
dispositions were possible:

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

It does not prove comparable speed, and it does not prove threshold compliance.

This section used to list four runners outside the matrix pair whose per-case budgets the scale did
*not* cover — `tools/{e2e,config_e2e,rootfs_e2e}_runner.c` (30s) and
`tests/integration/checkpoint_tree_runner.c` (15s). `988e2b11` closed that: all four read
`HL_MATRIX_TIMEOUT_SCALE`, and `cmake/Phase3Gates.cmake` section 10 sweeps them by CTest-name pattern
selected from the directory's `TESTS` property rather than by a repeated case list, with each pattern
asserted non-empty so a rename fails the configure instead of silently leaving a lane unscaled.
`^nested\.` joined the sweep with the `nested-engine` lane.

### What IS hidden by the lanes absent from `linux-x86_64.yml`

This paragraph previously said those lanes were absent "for a different reason — the engine
cannot execute a guest there yet — so nothing is currently hidden by it." Both halves are
false, and the second follows from the first only while the first holds. The engine executes
guests on this host through the two interpreter backends. A first sweep of all 24 compat
manifests at `fe722e2c` measured 2632/3013 (case, guest-ISA) runs passing — 87.4%. **That figure
is superseded.** A second sweep after the fixes landed, same method and same completeness against
pinned binaries, measures **2993/3013 — 99.34%** (aarch64 guest 1488/1496, x86-64 guest
1505/1517), with **20 of the 24 suites fully green on both guest ISAs**; the residue is
`completeness`, `core-regress`, `process` and `procfs`. `docs/amd64-host-findings.md` §3.10 is
the record. Still a snapshot, not a gate: re-measure before quoting it.

`linux-x86_64.yml` gates `ctest -L unit` (twice — once sandboxed through `.#checks.x86_64-linux.unit`,
once in the CMake tree), `ctest -L nested-engine`, `ctest -L emulated-aarch64-gated`, the `package`
nix check and a `cmake --install` assertion. No compat, production, perf, checkpoint or lifecycle
lane. So **none of those 3013 runs is gated**, and the ~20 that fail are invisible to CI rather than
absent from the tree. That is the real cost of the missing shards, and it is what the sentence above
used to deny.

The `emulated-aarch64-gated` step is the one lane that closed a gap of its own, and it is
**emulation**: it runs the cross-built aarch64 host arm under `qemu-user` on this x86-64 runner,
because CI otherwise never executes that arm anywhere. `docs/emulated-aarch64.md` is the category
list — weak memory ordering is explicitly *not* vouched for, since qemu-user inherits this host's
stronger model, so a missing barrier in the IBTC or STW paths passes there and can still fail on
silicon. It does not substitute for an aarch64 runner and does not touch the 3013 runs above.

What it takes to close the compat gap is written where the decision lives (`cmake/CiLanes.cmake`,
`HL_CI_COMPAT_HOSTS`), as an ordered four-step sequence: split `HL_CI_SHARDED_LINUX` per host CPU,
rework the OS-keyed guards, declare the host token and add a sharded matrix job to
`linux-x86_64.yml` in one change, then gate lane by lane. The order is not cosmetic — the gateable
subset is the suites measured green on **both** guest ISAs, because `cmake/Phase3Compat.cmake`
gives each compat label one CTest case covering both; `tools/check_ci_workflows.sh`'s I19 rejects a
second compat host on an OS whose sharded lane list cannot say which host it describes; and
declaring the token first switches I20 off and leaves that workflow with no structural guard at all.

## Host-environment preconditions the harness does not enforce

Two properties of the environment that invokes `ctest` are *inputs* to the corpus, and
neither is checked. Both produce failures that read as engine defects and are not. Both are
cheap to confirm on an x86-64 host: run the case's own x86-64 fixture binary
(`build-*/compat/<suite>/x86_64/<case>`) natively and diff it against the golden — on that
host it is an exact oracle for the x86-64 guest leg, and a native binary that fails the
golden the same way the engine does is proof the cause is the environment.

* **The scheduling nice level.** Two cases lower their own nice and assert the result:
  `completeness/priority` (`syscall/priority.c`, `setpriority(PRIO_PROCESS, 0, 5)`, golden
  `priority set=1 nice=5`) and `process/sched-attr` (`sched_attr.c`, whose
  `sched_getattr().sched_nice` check does the same). Lowering nice needs `CAP_SYS_NICE` or
  `RLIMIT_NICE` headroom, and the default `RLIMIT_NICE` is 0 — so from a shell already at
  nice > 5 the call returns `EACCES`, the golden is unreachable, and both cases fail on
  **both** guest ISAs. Measured from a nice-12 shell the native binaries fail identically
  (`priority set=0 nice=12`, `sched_attr ok=0`). The CI lanes run at nice 0, where both
  cases are green. Run `ctest` at nice ≤ 5, or read those two failures as environmental.

  One real divergence hides behind the first: at nice 12 the engine prints `set=1` where
  native prints `set=0`, i.e. the engine's `setpriority` reports success where the kernel
  refuses with `EACCES`. It is host-neutral and invisible wherever the call would have
  succeeded anyway, which is why no lane sees it.

* **The filesystem under the guest's `/tmp`.** `HL_MATRIX_SCRATCH_DIR`, covered by the
  comment block in `cmake/Phase3Compat.cmake` and by `matrix_runner.c`'s `scratch_note()`,
  which names the filesystem on any failing run. `.github/workflows/linux.yml` sets it per
  suite and deliberately unsets it for `compat-core-syscall`. A local `ctest -L compat-*`
  sets it for no suite at all, so on an ext4 build tree `syscall/memfd-seals` fails on both
  ISAs for that reason alone.

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
`EPOLLONESHOT` (`event.c:1014`, `ep_object_free` after one delivery) but holds no
edge-transition state, so `EPOLLET` on an emulated object degrades to level-triggered
reporting: the observed sequence was `n=1,1,1,1,1,1,1,1` where Linux gives `1,1,1,0,1,1,0,1`.

`sc-epoll-semantics` (`tests/compat/syscall_edges/epoll_semantics.c`) therefore asserts only
the host-invariant contract — `EEXIST` dup-ADD, `ENOENT` MOD/DEL, `EINVAL` self-add,
level-triggered readable-until-drained — with the golden regenerated from the native oracle.
Asserting an exact oneshot/edge readiness sequence there would encode a host-specific
outcome. The gap needs its own fix: apply edge state to the emulated-object epoll path.
