# x86-64 Linux host: what the interpreter actually costs

Reconnaissance, not optimisation. `docs/amd64-host.md` predicted "roughly 10-50x" and nobody had
measured it. This file is the measurement: per-case engine-vs-native, a profile with named cost
centres, and a ranked list of what to fix. Nothing here changes `src/`.

Every claim is marked **measured** or **estimated**. Measurement conditions are given per number,
because this machine had three other agents and a `ctest -L compat` run on it and the load average
moved between 1.4 and 30 during the session.

---

## The three things to do first

**1. Stop the fault pad from issuing a syscall on every guest basic block.**
`run_block()` in *both* interpreters begins `sigsetjmp(pad, 1)`. The `savemask=1` argument makes glibc
issue `rt_sigprocmask(SIG_BLOCK, NULL, &saved)` — a real host syscall — once per guest block.
Measured: **271.7 ns** per call on this box, against a measured **2.57 ns** for the same `sigsetjmp`
with `savemask=0`. On the compute case that is **44% of total CPU time** and **99.96% of all host
syscalls the process makes**. It is the single largest cost in the engine and it buys a mask save that
only the rare fault path ever reads.

**2. Memoise the instruction-fetch and mapping-validation path.**
Every guest instruction is fetched through `hl_guest_fetch_exec()`, which walks its range **twice**
(once to validate, once to copy), calling `hl_logical_vma_resolve_exec()` on each pass, plus
`host_range_mapped()` — which itself does `gna_hit()`, `hl_linux_bus_hit()`, its own `sigsetjmp`, and a
volatile probe load per page. Measured: **38.2% of user-mode cycles** on the compute case, making it a
*larger* cost centre than decoding. A per-page memo keyed on the logical-VMA snapshot collapses it to a
bounds compare. It does not cache guest bytes, so SMC stays coherent by construction — the property the
design deliberately bought.

**3. Then cache decoded instructions in the block descriptor.**
Measured: **25.2% of user-mode cycles**. This is the brief's headline candidate and it ranks *third*,
behind something the brief did not ask about. It is also the only one of the three that trades away
SMC-coherence-by-construction, so it should be done after the two cheaper, safer wins have been taken
and re-measured.

Ordered this way because #1 is arithmetic on a measured constant, #2 is bigger than the thing everyone
assumed was biggest, and #3 costs a correctness property. #1 and #2 together are estimated to take the
compute case from 605x native to roughly 200-260x with no change to the fault or SMC models.

---

## 1. How this was measured

### Binaries, pinned

Other agents rebuild `build-amd64` continuously, so everything was copied to a scratch directory first
and every measurement below used that copy. Recorded at pin time:

| artefact | md5 |
|---|---|
| `hl-engine-linux-x86_64` | `e1007a9dd34ef459423e896cf32ecbf0` |
| `hl-engine-linux-aarch64` | `fa7009261cef05a6cf3db8872be992c9` |
| `perf-runner` | `1c64000c6e8d687946d4b3c1d73e5241` |
| `config-e2e-runner` | `d8edc335f11839ed782e8009fdea78f5` |
| `compat/core/workload/x86_64/busyloop` | `50e539ff5b9b56baa9d5acf05afcc0ab` |
| `compat/core/workload/aarch64/busyloop` | `2bdc0025f02f1c9fccfa4df112c86ba1` |
| `e2e/guest-exit-x86_64` | `7338587ec00e0fefe2ad7bd3f5c08e81` |
| `perf/syscall-x86_64` | `fecb6fcfa7124505955d4743d65692dc` |
| `perf/translate-x86_64` | `4d9adee2ff57955b42dedc6a623ada53` |

The full manifest (32 files) is in the session scratchpad as `pinned.md5`. Working tree at pin time:
`ab821f791a83dcd19e0b3818b24182b001698192`, branch `feat/amd64-linux-host`.

This was not a formality. `src/translator/guest/x86_64/interp.c` and `.../translate.c` were both
modified by other work **during** this session, after the pin. Every number below therefore describes
one fixed pair of binaries, and a rebuild will not necessarily reproduce them exactly — re-pin and
re-measure rather than comparing against these figures across a rebuild.

### Machine

AMD Ryzen 7 7800X3D, 8 cores / 16 threads, Linux 6.18.7-76061807-generic. Core clock calibrated at
**4.83 GHz** from a pure-user-mode reference run (see §3). Mitigations enabled include Enhanced IBRS,
IBPB-before-exit-to-userspace and "Clear CPU buffers", which is why a null syscall costs ~270 ns here
rather than the ~80 ns an unmitigated box would show — that inflates the *absolute* syscall cost but is
the cost this host really pays.

### Method, and why CPU time rather than wall time

Wall time on a box at load average 25 is not a measurement. Everything below reports **child CPU time
(user+sys, `getrusage(RUSAGE_CHILDREN)` deltas)** at microsecond resolution alongside wall time, from a
harness that runs native and engine **interleaved** so drift hits both equally, and reports **medians**
over 5-21 repetitions. `/usr/bin/time` was abandoned early: its 10 ms resolution rounds most of these
cases to zero.

Load average is recorded per case. The headline table was taken at load **1.4-5.7**, after the compat
matrix run finished. An earlier full pass at load **26-30** produced ratios within about 15% of these,
which is the honest error bar on the ratios; the absolute numbers from that pass are discarded.

One caveat that turned out to matter: **`ru_utime`/`ru_stime` is tick-sampled and unusable here.** Two
identical 10-million-syscall runs split as 122/150 ns and 242/31 ns user/sys. The *total* is stable to
0.3%. No user/sys split from `getrusage` is quoted anywhere below; the kernel fraction in §3 comes from
a hardware cycle counter instead.

### Profiling, with a substitution

**`perf` is not installed on this machine** (`perf`, `gdb`, `valgrind`, `eu-stack` all absent;
`perf_event_paranoid=2`, `yama/ptrace_scope=1`). Two substitutes were built in the scratchpad:

* A ptrace poor-man's sampler. **Discarded** — `PTRACE_INTERRUPT` stops threads preferentially at
  syscall boundaries and it reported 100% of samples in one libc frame. Recorded here so nobody
  repeats it.
* A `perf_event_open` sampler with a mmap'd ring buffer. Three corrections were needed and each is a
  finding in its own right:
  - `inherit=1` makes this kernel refuse `mmap()` on a sampling event, so events are opened per task.
  - `PERF_COUNT_SW_TASK_CLOCK` **cannot profile this engine**: the hrtimer driving it is stopped and
    restarted on every user/kernel transition, so on a workload that syscalls every ~600 ns it never
    reaches its period. It collected 3 samples in 3 seconds. Switched to `PERF_COUNT_HW_CPU_CYCLES`.
  - The engine **forks a child process** to run the guest (the direct child sits in `wait4` while a
    grandchild named after the guest burns 100% CPU), so the sampler walks
    `/proc/<pid>/task/<tid>/children` and arms every descendant.

`perf_event_paranoid=2` permits user-space sampling only, so the profile in §3 is a **user-mode**
profile and the kernel fraction is derived separately. That is stated where it matters.

---

## 2. Per-case: engine versus native

x86-64 guest, both columns on this host, `perf-native` payloads run directly as the baseline they exist
to be. Medians; load average 1.4-5.7 throughout. **Measured.**

| case | native wall (us) | native CPU (us) | engine wall (us) | engine CPU (us) | **x wall** | **x CPU** |
|---|---:|---:|---:|---:|---:|---:|
| ipc-throughput | 16,814 | 18,299 | 43,013 | 61,685 | 2.6 | **3.4** |
| mmap | 56,031 | 55,874 | 220,075 | 220,048 | 3.9 | **3.9** |
| ipc-latency | 69,213 | 98,020 | 319,906 | 511,453 | 4.6 | **5.2** |
| syscall-1m | 261,297 | 261,200 | 2,933,507 | 2,933,358 | 11.2 | **11.2** |
| file | 6,829 | 6,782 | 82,633 | 82,181 | 12.1 | **12.1** |
| pipe | 82,298 | 82,220 | 1,964,632 | 1,964,493 | 23.9 | **23.9** |
| event | 75,572 | 75,515 | 2,014,199 | 2,013,954 | 26.7 | **26.7** |
| startup | 183 | 142 | 6,584 | 6,604 | 36.0 | **46.5** |
| fork-stress | 125,742 | 132,864 | 7,218,415 | 7,452,691 | 57.4 | **56.1** |
| translation | 338 | 295 | 27,725 | 27,774 | 82.0 | **94.1** |
| syscall-startup | 320 | 270 | 37,304 | 37,300 | 116.6 | **138.1** |
| compute | 304,270 | 304,165 | 184,045,690 | 184,033,223 | 604.9 | **605.0** |
| warm-cache | — | — | 51,091 | 29,919 | — | — |

**Distribution, not a number:** min **3.4x**, p25 **11.2x**, median **25.3x**, p75 **94.1x**, max
**605x**, geometric mean **26.6x**.

The predicted 10-50x band is right for the *middle* of the distribution and wrong at both ends. Three
workload classes fall out cleanly:

* **Kernel-bound (3-12x).** `mmap`, `ipc-throughput`, `ipc-latency`, `syscall-1m`, `file`. The host
  kernel does the work in both columns and the interpreter only adds the marshalling. The floor is the
  syscall service layer, not translation — a JIT would not help much here either.
* **Mixed (24-57x).** `pipe`, `event`, `fork-stress`. Real guest code between the syscalls.
* **Guest-execution-bound (94-605x).** `translation`, `syscall-startup`, `compute`. This is where the
  interpreter is actually the interpreter. `startup` at 46x is a special case: the engine's fixed
  start-up cost is ~6.5 ms against a 0.14 ms native process, and only **one** guest block executes.

**`warm-cache` measures nothing on this host.** Warm 29,919 us CPU versus cold 29,852 us — a 0.2%
difference, inside noise, over 15 and 9 repetitions. The interpreter emits no host code, so the
persistent code cache has nothing to persist. The case should not be read as a warm-path number here.

### Against the tracked (AArch64-host, JIT) thresholds

Run through `perf-runner` itself with the real `PERF_LIMIT_*` pairs applied, so this is the lane's own
verdict rather than a re-derivation. **Measured**, load 1.4-3.1.

| case | cold / p99 (us) | threshold cold / p99 | verdict |
|---|---:|---:|---|
| startup | 6,649 / 7,194 | 15,000 / 10,000 | **pass** |
| translation | 27,562 / 28,419 | 40,000 / 30,000 | **pass** |
| ipc-throughput | 42,387 / 43,509 | 75,000 / 60,000 | **pass** |
| fork-stress | 7,213,276 / 7,306,270 | 9,000,000 / 8,000,000 | **pass** |
| warm-cache | 51,091 (median) | 100,000 / 80,000 | **pass** |
| file | 82,597 / 83,045 | 75,000 / 60,000 | fail, 1.1x / 1.4x |
| syscall-startup | 36,708 / 38,630 | 30,000 / 25,000 | fail, 1.2x / 1.5x |
| mmap | 218,652 / 222,411 | 150,000 / 120,000 | fail, 1.5x / 1.9x |
| ipc-latency | 325,984 / 335,363 | 150,000 / 120,000 | fail, 2.2x / 2.8x |
| syscall-1m | 2,891,063 / 2,987,711 | 500,000 / 400,000 | fail, 5.8x / 7.5x |
| event | 1,917,847 / 1,975,489 | 250,000 / 200,000 | fail, 7.7x / 9.9x |
| pipe | 1,943,434 / 1,930,496 | 250,000 / 200,000 | fail, 7.8x / 9.7x |
| compute | 184,045,690 (median) | 750,000 / 650,000 | fail, 245x / 283x |

**`cmake/Phase3Gates.cmake` says "Enforced there, all thirteen cases fail for the one reason that is
already known and documented, which measures nothing." That is measurably false: five of thirteen pass
unchanged, and four more are within 3x.** The record-only *decision* still looks right — `compute` at
283x over means no single threshold set can serve both hosts, and a case that passes today could
regress silently — but the stated justification should be corrected to match what the lane actually
does.

### Two defects in the perf lane itself

1. **`perf.linux-compute-x86_64.record-only` cannot finish inside its own `TIMEOUT`.** `perf-runner`
   does 1 cold + `HL_PERF_WARMUPS` (3) + `HL_PERF_HEAVY_SAMPLES` (7) = **11 runs**. At the measured
   median of 184.0 s that is **2,024 s** against `TIMEOUT 1800`. It will fail as a timeout, not as a
   measurement. At the load average this machine ran at earlier in the session the same case took
   348 s per run, i.e. 3,828 s. **Measured.** The aarch64 leg is 11 x 135.6 s = 1,492 s, inside 1800 s
   but with only 17% margin — it will flake under load.
2. The `translation` case measures something different here. On the JIT it measures isolated
   translation; on the interpreter `translate_block()` only bump-allocates a 24-byte descriptor, so the
   case measures 1,024 first-executions of never-repeated blocks. The `metric=` label is still
   comparable across hosts, which is what matters, but the name no longer describes the mechanism.

---

## 3. Where the time goes

Profiled on `compat/core/workload/x86_64/busyloop` — the `compute` payload, and the cleanest
representative of guest execution in the tree. Its hot loop is exactly six instructions ending in a
`jne`, executed 300,000,000 times:

```
8840:  48 31 c2              xor  %rax,%rdx
8843:  48 83 c0 01           add  $0x1,%rax
8847:  48 0f af d1           imul %rcx,%rdx
884b:  48 c1 c2 07           rol  $0x7,%rdx
884f:  48 3d 00 a3 e1 11     cmp  $0x11e1a300,%rax
8855:  75 e9                 jne  8840
```

Because the x86-64 interpreter ends a block at **every** control transfer (`interp_execute()` returns
`STEP_END` on any Jcc — "Both edges end the block: the fall-through too, or a guest loop never reaches
the safepoints"), each iteration is exactly one `translate_block` lookup and one `run_block` call. So
per-iteration cost and per-block cost are the same number here: **613 ns**, against **1.01 ns** native.

### Kernel versus user

**Measured.** 137,942 samples at 4M-cycle periods = 551.8 G user cycles; total CPU 185.71 s x 4.83 GHz
= 896 G cycles. Kernel is the remainder: **~44%**.

Cross-checked independently: a standalone micro-benchmark puts `sigsetjmp(pad, 1)` at 271.7 ns against
the measured 613 ns per iteration = **44.4%**. Two unrelated routes agreeing to within 0.5 points.

And the kernel time is one thing. `strace -c` over 25 s of the compute run:

```
99.96%  1,643,380  rt_sigprocmask
 0.01%          3  pread64
 0.01%         89  read
```

**Measured: 99.96% of all host syscalls issued during a compute-bound guest run are
`rt_sigprocmask`.** Of 35,908 `rt_sigprocmask` calls in a trivial `gettid` run, 35,896 are the
`SIG_BLOCK, NULL` query form.

### The user-mode profile

137,942 samples, hardware cycle counter, of which 12,203 (8.8%) carry censored kernel IPs (PMI skid)
and are excluded. Percentages are of the 125,739 user-mode samples. **Measured.**

| share of user cycles | cost centre | what it is |
|---:|---|---|
| **38.2%** | instruction fetch + mapping validation | `hl_guest_fetch_exec` 8.2, `hl_logical_vma_resolve_exec` 9.8, `hl_logical_vma_resolve` 8.1, `gna_hit` 5.5, `host_range_mapped` 3.8, `hl_linux_bus_fault` 1.2, `pthread_once` 0.9, `hl_linux_bus_hit` 0.7 |
| **25.2%** | instruction decode | `decode_bytes` 21.6, `hl_x86_decode` 3.6 |
| **22.1%** | instruction semantics | `interp_step_one_byte` 5.4, `interp_imul_truncating` 3.0, `interp_alu_sub` 2.7, `interp_alu_to_rm` 2.6, `interp_alu_kind` 2.3, `interp_alu_add` 1.5, `interp_rm_read` 1.4, `interp_reg_write` 1.1, `interp_shift` 1.1, rest |
| **8.5%** | dispatcher round trip | `run_guest` 4.1, `run_block` 3.9, `stw_dispatch_safepoint` 0.5 |
| **3.9%** | fault-pad arming, user side | `__sigsetjmp` 2.6, `__sigjmp_save` 0.9, `pthread_sigmask` 0.4, `sigprocmask` 0.1 |
| 2.2% | unattributed | addresses below the first symbol, misc |

Folding in the ~44% kernel share, as fractions of **total** CPU on the compute case:

| share of total CPU | cost centre |
|---:|---|
| **~44%** | `rt_sigprocmask` from `sigsetjmp(pad, 1)` — kernel side |
| ~21% | instruction fetch + mapping validation |
| ~14% | instruction decode |
| ~12% | instruction semantics |
| ~5% | dispatcher round trip |
| ~2% | fault-pad arming, user side |

### The fetch path, since it is the surprise

Per guest instruction, `hl_x86_decode()` calls `instruction_fetch()` → `hl_guest_fetch_exec()`, which:

1. **validation pass** — `hl_logical_vma_resolve_exec()` (a `pthread_once` plus a binary search over the
   VMA snapshot), then, for an ordinary direct mapping, `g_direct_validator` →
   `guest_fetch_direct_valid` → `host_range_mapped()`, which does `gna_hit()`, `hl_linux_bus_hit()`, its
   own `sigsetjmp(g_hrm_jb, 0)` and **a volatile probe load per page**;
2. **copy pass** — `hl_logical_vma_resolve_exec()` **again**, then `memcpy` of up to 15 bytes.

Two full resolutions and a page probe, per instruction, to fetch bytes that in the overwhelming common
case come from the same page as the previous instruction.

### A profile of a syscall-bound case, for contrast

`perf/syscall-x86_64` (1M `gettid`), 3,516 user-mode samples after excluding 281 censored kernel IPs.
**Measured.**

| share of user cycles | cost centre |
|---:|---|
| **36.2%** | guest-syscall service (`svc_fs` 14.6, `svc_io` 12.3, `svc_proc` 4.2, `service_local`, fd snapshot, filemap replay) |
| **26.2%** | instruction fetch + mapping validation |
| **15.0%** | instruction decode |
| **10.7%** | dispatcher round trip |
| 6.4% | instruction semantics |
| 2.6% | fault-pad arming, user side |
| 2.9% | unattributed |

Here the syscall service layer is the top cost and translation is secondary — which is why this case
sits at 11.2x rather than 605x, and why a Stage-2 transliterator would move it much less.

---

## 4. The four candidates in the brief, answered

### "The block descriptor caches no decoded instructions. How much is that costing?"

**Measured: 25.2% of user cycles, ~14% of total CPU, on the compute case.** Standalone, `hl_x86_decode`
costs **17.2 ns per instruction** — 103 ns for the six-instruction busyloop block — against 0.42 ns for
the `memset` of `struct insn` (184 bytes) plus a 15-byte fetch, so the cost is genuinely the decode
tables, not the buffer handling.

Real, but **it is not the largest cost centre, and it is not even the largest part of "getting the next
instruction"**: the fetch-and-validate path in front of it is 38.2%. The correctness-first choice was
more defensible than it looks, and the cheaper half of the win can be taken without reversing it.

### "What does the `interp_access_begin`/`end` bracket cost per access?"

**Measured: essentially nothing per access.** A TLS store plus a compiler fence either side of a loop
body costs **0.36 ns** over the bare body (0.81 vs 0.45 ns/op at load 3.5; 1.24 vs 0.85 at load 15).
In the compute profile the ledger check inside it (`hl_linux_bus_fault` + `hl_linux_bus_hit`) is 1.9% of
user cycles.

**Caveat, flagged rather than glossed:** busyloop is a pure-register loop and makes almost no guest
memory accesses, so this profile does not exercise the bracket hard. The per-access cost above is from a
micro-benchmark, not from a memory-bound guest. A memory-heavy fixture would give a better number and I
did not isolate one. On the evidence available, **do not optimise this** — the brief's suspicion is not
supported.

### "The interpreter returns to the dispatcher at every block boundary. How much is the round trip?"

**Measured: 8.5% of user cycles, ~5% of total CPU** (`run_guest` + `run_block` + safepoint). `G_IBTC_FILL`
being a no-op is cheap.

**This corrects the intuition in the brief.** The block boundary is expensive, but almost none of the
expense is the dispatch round trip — it is the `sigsetjmp` that `run_block` performs on entry. Removing
the syscall (fix #1) makes the boundary cost about 5%, at which point chaining is a marginal win. Do #1
first and re-measure before touching chaining.

### "~49 `rt_sigprocmask` per guest syscall — confirm and quantify"

**Confirmed, and the framing is wrong in a way that makes it worse, not better.** It is not per guest
syscall; it is **exactly one per guest basic block executed**, and the ~49:1 ratio is a coincidence of
that particular fixture's block-to-syscall ratio. Measured counts of the `SIG_BLOCK, NULL` form:

| payload | x86-64 guest | aarch64 guest |
|---|---:|---:|
| `e2e/guest-exit` (executes one block) | 1 | 1 |
| `perf/translate` | 10,850 | 9,383 |
| `compat/syscall/gettid` | 35,896 | 32,411 |
| `busyloop` (25 s straced sample) | 1,643,380 | 1,697,570 |

The source is `sigsetjmp(g_interp_fault_pad, 1)` at `src/translator/guest/x86_64/interp.c:900` and
`sigsetjmp(g_interp_marker_jmp, 1)` at `src/translator/guest/aarch64/interp.c:4671`. glibc's
`__sigjmp_save` turns `savemask=1` into `rt_sigprocmask(SIG_BLOCK, NULL, &env->__saved_mask)`.

Cost, measured at load 3.5, medians of two runs of 2-10 million iterations:

| | ns/op |
|---|---:|
| `sigsetjmp(pad, 1)` | **271.7** |
| `sigsetjmp(pad, 0)` | 2.57 |
| `setjmp` | 2.71 |
| bare `sigprocmask(SIG_BLOCK, NULL, &old)` | 267.2 |

**It smells like an accident and it is one.** `savemask=1` exists so that a `siglongjmp` arriving from
a signal handler restores the mask in force before the handler ran. But both interpreters reach the pad
by exactly one route — `interp_signal_resume()`, called from the engine's own handler once the guest
signal frame is built — so the mask to restore is known at that point and can be restored explicitly on
the fault path, which is rare, instead of being queried on every block, which is not.

---

## 5. `HL_MATRIX_TIMEOUT_SCALE=30`: generous, and for the wrong reason

The scale was chosen as the midpoint of a predicted 10-50x range. The prediction was not right, but
**30 is comfortably adequate for the matrix runners** — for a reason that has nothing to do with the
midpoint.

A stratified sample of **69 of the 1,524 x86-64 compat fixtures** (every 22nd, all 18 suites
represented), each run natively and through the engine, wall clock, load 2.8-5.5. **Measured.**

One of the 69, `completeness/x86_64/syscall/seccomp_filter`, is excluded from the statistics below: it
ran *faster* under the engine (42.9 ms against 1.08 s native) because the native run took `SIGSYS` and
did different work. It is not a speedup. That leaves **n = 68**.

| statistic | engine/native |
|---|---:|
| min | 1.1x |
| p25 | 13.0x |
| **median** | **20.6x** |
| p75 | 22.0x |
| p90 | 26.9x |
| p95 | 161.6x |
| max | 529.9x |

Only **6 of 68 (8.8%)** exceed 30x. The slowest:

| ratio | native | engine | case |
|---:|---:|---:|---|
| 529.9x | 32 ms | 17.2 s | `memory/x86_64/dbt_conc_same` |
| 271.1x | 731 ms | **198.3 s** | `core/workload/x86_64/soak_codecache` |
| 170.0x | 1.8 ms | 314 ms | `abi-corpus/x86_64/x_avx2_shuf` |
| 161.6x | 2.3 ms | 379 ms | `completeness/x86_64/x86_64/repstring` |
| 42.5x | 2.0 ms | 84 ms | `abi/x86_64/ackermann` |
| 31.5x | 2.0 ms | 62 ms | `abi-corpus/x86_64/alloca` |

The median of 20.6x is **not** the interpretation rate — it is the engine's fixed ~6.5 ms start-up cost
against fixtures that take 1.8-2.5 ms natively. The tail is the interpretation rate.

**Verdict: 30 is generous for `matrix_runner` (120 s base → 3,600 s).** The slowest case measured
takes 198 s, i.e. **5.5% of budget**. Nothing in the sample comes close. For 30 to be too tight, a case
would need roughly 12 s of native CPU at a 300x profile, and the compat corpus contains nothing like
that — the heaviest, `soak_codecache`, is 731 ms native.

Three qualifications:

* The sample is 4.5% of the corpus and deliberately stratified, not exhaustive. The tail is where the
  risk is and a 69-case sample characterises a tail poorly. **This is a bound, not a proof.**
* The number that is *not* generous is the perf lane's `TIMEOUT 1800`, which the compute case exceeds
  (§2). That is a separate constant from `HL_MATRIX_TIMEOUT_SCALE` and it is wrong.
* `docs/amd64-host-gaps.md` F3 notes four runners that do not read the scale at all
  (`e2e_runner.c`, `config_e2e_runner.c`, `rootfs_e2e_runner.c` at 30 s, `checkpoint_tree_runner.c` at
  15 s). At the measured median of 20.6x a 30 s budget covers 1.45 s of native work — thin. That
  finding stands and this data supports it.

---

## 6. The aarch64 guest on this host

No native baseline exists for these — that quadrant is cross-ISA, and this machine cannot run an
aarch64 binary. So only the engine column is measurable, and it is reported against the x86-64 guest
engine column for shape. **Measured**, load 2.4-7.0.

| case | aarch64 guest CPU (us) | x86-64 guest CPU (us) | a64/x86 |
|---|---:|---:|---:|
| startup | 6,783 | 6,604 | 1.03x |
| syscall-startup | 31,928 | 37,300 | 0.86x |
| translation | 26,472 | 27,774 | 0.95x |
| compute | 135,457,054 | 184,033,223 | **0.74x** |
| syscall-1m | 2,864,186 | 2,933,358 | 0.98x |
| mmap | 246,423 | 220,048 | 1.12x |
| file | 95,966 | 82,181 | 1.17x |
| ipc-latency | 597,888 | 511,453 | 1.17x |
| ipc-throughput | 92,166 | 61,685 | 1.49x |

The two interpreters cost about the same. The aarch64 guest is 26% faster on `compute`, consistent
with fixed-width 32-bit decode being cheaper than `decode_bytes`' prefix/ModRM/SIB walk — the one place
the ISAs genuinely differ in interpreter cost.

The aarch64 interpreter has the **same** per-block `rt_sigprocmask` (measured above), so fix #1 applies to
both quadrants — but **not "unchanged", as this section originally claimed.** Measured after the fix landed
(`583ae490`): the x86-64 guest gains 1.85x and the **aarch64 guest gains 3.46x**. A fixed ~290 ns is a
larger fraction of a smaller per-block cost, so the quadrant this document treated as the lesser prize was
the greater one. Its `run_block` does have an inner loop bounded by the pre-scanned block
extent, which the x86-64 side lacks — but measurement shows it does not self-chain a backward branch in
practice (1,697,570 `rt_sigprocmask` in a 25 s busyloop sample), so that structural difference is not
currently buying anything.

---

## 7. Ranked optimisation list

Win estimates are derived from measured shares by arithmetic and are marked **estimated**; the shares
themselves are measured.

| # | change | estimated win | effort | risk |
|---|---|---|---|---|
| 1 | ~~`sigsetjmp(pad, 0)` + explicit mask restore on the fault path~~ **DONE `583ae490`** | predicted compute 1.80x; **measured 1.85x x86-64 guest, 3.46x aarch64 guest**. `rt_sigprocmask` 3,035,853 → 8 per 3M blocks | small | **medium-high** |
| 2 | ~~per-page memo for `hl_guest_fetch_exec` / `hl_logical_vma_resolve_exec`~~ **DONE `021c7fe2`** | predicted 1.2-1.3x; **measured 1.282x x86-64 guest, 1.329x aarch64 guest**. The `host_range_mapped` half is NOT done — see below | small-medium | low-medium |
| 3 | cache decoded `struct insn` in the block descriptor | compute **~1.15-1.2x** after #1+#2 | medium-large | **high** |
| 4 | shrink `struct insn` (184 bytes) and the decode tables | ~1.02-1.05x | small | low |
| 5 | block chaining / real `G_IBTC_FILL` | ~1.03-1.05x after #1 | medium | medium |
| 6 | fix `perf.linux-compute-*` `TIMEOUT`; correct the Phase3Gates justification | none (correctness of the lane) | trivial | none |

### 1. Drop `savemask` from the per-block `sigsetjmp`

**Measured cost:** 271.7 ns/block; 44% of compute CPU; 99.96% of host syscalls on a compute guest.
Applies to `src/translator/guest/x86_64/interp.c:900` and
`src/translator/guest/aarch64/interp.c:4671`.

**Win (estimated, from measured constants):** compute 184.0 s → ~102.4 s (**1.80x**). syscall-startup:
35,896 blocks x 271.7 ns = 9.75 ms of a 37.3 ms run, of which ~6.5 ms is engine start-up — so 1.36x on
the case, ~1.47x on the guest-execution part. translation: 10,850 x 271.7 ns = 2.95 ms of 27.8 ms,
1.12x. syscall-1m gains little because that case is dominated by the service layer.

**Done in `583ae490`, and the design proposed here was wrong.** This section suggested snapshotting the
mask on the theory that it is knowable and invariant across the run loop. **It is not invariant**, and two
sites prove it: `syscall/signal.c:779` mirrors the guest's SIGTSTP/TTIN/TTOU onto the *real* host mask for
job control — persistent and guest-driven, between blocks — and `thread.c:1559`'s `hrm_fault_hook` unblocks
SEGV+BUS from *inside* `run_block`, since the interpreter's fetch validates through `host_range_mapped`. A
restored snapshot would have silently un-blocked the stop signals bash blocks around `tcsetpgrp`.

The design that works needs no invariance: **the interpreter's `siglongjmp` is a hand-rolled
`rt_sigreturn`.** Leaving a handler by long jump means the kernel never runs sigreturn, so the mask restore
is a debt — and `ucontext->uc_sigmask` is by definition the value sigreturn would install, recorded by the
kernel on the faulting thread at the instant of the fault. Restore that, once, immediately before the jump.
The JIT owes nothing, because it rewrites `uc_mcontext.pc` and *returns*.

`siglongjmp` had to stay: on Darwin `setjmp`/`longjmp` are the mask-**saving** pair, so `sigsetjmp(pad, 0)`
+ `siglongjmp` is the only portable spelling of "this pad does not touch the mask".

The old code was also slightly **wrong** at `thread.c:1559` — `savemask=1` re-blocked what the fault hook
had deliberately unblocked.

### 2. Memoise the fetch and validation path

**Measured cost:** 38.2% of user cycles on compute — the largest user-mode centre. Two
`hl_logical_vma_resolve_exec()` calls plus one `host_range_mapped()` (with `gna_hit`,
`hl_linux_bus_hit`, a `sigsetjmp` and a volatile probe load) per guest instruction.

**Done in `021c7fe2`: 1.282x on the x86-64 guest, 1.329x on the aarch64 guest.** The shape proposed here
was keyed on `hl_logical_vma_ledger.current`, the snapshot pointer, and that is **unsafe**: retired
snapshots are `free()`d at the next quiescent reclaim, and `malloc` can hand the same address to the next
publication — an ABA that makes a stale entry look *fresh*. It uses a new monotonic ledger generation
instead.

The invalidation obligation named here was also the wrong frame. The memo is **revalidated on every use**
rather than notified, so the set has exactly one element: equal generation implies equal snapshot implies
a hit returns bit-for-bit what a fresh resolve would. The memo cannot be stale, only absent.

**Only the ledger-derived interval is cached; the ordinary/direct verdict is not**, and
`host_range_mapped` still runs per fetch. That is what makes `munmap`/`MAP_FIXED`/`mremap`/`mprotect`/
`G_SMC_UNMAP` hook-free — and it is the half still on the table, worth a further **1.137x** measured.
Taking it needs a generation over `g_gna`, the bus registry and host unmap, because
**`mprotect(PROT_NONE)` over an ordinary exec page calls `gna_add` without touching the ledger or firing
`G_SMC_UNMAP`** (which fires only for `PROT_WRITE`). A memo built on the obligation this document
originally stated would have executed a page the guest had made inaccessible.

`host_range_mapped`'s own `sigsetjmp` stays. It is disjoint from the block pad by construction — the pad
is claimed only when `g_interp_pad_armed && g_interp_guest_access`, and a probe load is not a marked guest
access — and the two exist for opposite purposes: the probe must turn a fault into `EFAULT`, the pad must
deliver it to the guest. After #1 it costs 2.57 ns rather than 271.7, so there is nothing left to win.

Note the baseline moved underneath this: measured before #1 landed, the same patch gave only **1.132x**.
Removing the kernel share made the fetch path a larger fraction of a smaller total. Cumulative on the
same fixture, 12.55 s → 4.88 s = **2.57x**.

**The compat suite cannot cover this path at all.** `logical_candidate` in `mem.c` requires
`host_page > guest_page`, so on an x86-64 Linux host the ledger is always empty and the indirect path is
unit-test-only; it is live on the 16 KiB-page AArch64/macOS lane. `tests/unit/test_guest_fetch.c` carries
the invalidation cases instead.

### 3. Cache decoded instructions in the block descriptor

**Measured cost:** 25.2% of user cycles, ~14% of total.

**Risk: high, and it is the design property the brief asks about.** `struct interp_block`'s comment is
explicit: "no decoded instructions: `run_block` re-decodes from guest memory every execution, which
makes self-modifying guest code coherent by construction". Reversing that means reinstating the JIT's
write-protect-and-drop machinery on the interpreter path, which currently has `smc_protect` absent and
`G_AFTER_TRANSLATE` empty precisely because it has nothing to protect. The aarch64 side already
maintains `txpg_mark` and the 64-byte `txln_put` line set in `translate_block`, so half the substrate
exists there and none of it does on the x86-64 side.

Do this **third**, and re-measure after #1 and #2 — the estimate of 1.15-1.2x is against the reduced
baseline, not today's, and it may look different once the two cheaper wins have landed.

### 5. Block chaining

Listed for completeness because the brief asks. **Measured at 8.5% of user cycles / ~5% of total**, so
it is the smallest of the four candidates, not the largest. The reason the block boundary looks
expensive is item #1, not the dispatch. Reassess after #1.

---

## 8. What Stage 2 would buy

### x86-64 guest: estimated 0.7-0.9x native, i.e. ~600-800x faster than today on compute

**Estimated**, by mechanism, since no transliterator exists.

A same-ISA transliterator per `docs/amd64-host.md` §3.1 copies guest instructions verbatim into the code
cache. Against the measured profile that eliminates, per guest instruction, the fetch-and-validate 38.2%
and the decode 25.2% (both move to translate time, paid once per block rather than once per execution),
and replaces the 22.1% of interpreter semantics with the guest instruction itself — roughly one host
instruction where the interpreter runs a switch, an operand decode, a flag computation and a register
write-back. Per block it removes the `sigsetjmp` entirely: the JIT fault model uses host-PC provenance,
not a per-block landing pad.

The reference point is the AArch64 host's own same-ISA diagonal. `docs/perf-simd-findings.md` §A
measures `guest/aarch64` on an ARM64 host at **0.85x native** on `float_simd`, with the residual being
the `cpu->irq` poll the folded back edge must still execute. There is no reason the x86-64 diagonal
should land in a different band, so **0.7-0.9x native** is the estimate, i.e. compute moving from 605x
to order 1.2x.

Two adjustments specific to this diagonal, from §3.1, and they point in opposite directions:

* **Cheaper than the ARM64 diagonal:** with `%gs` carrying the `struct cpu` pointer, no guest GPR is
  stolen, so there is no `is_stolen()` / `emit_mangled_x18()` rewrite cost at all. The ARM64 side pays
  that on every instruction naming x18/x28/x30.
* **More expensive than the ARM64 diagonal:** the engine owns the real `%gs`, so guest `%gs` accesses
  must be rewritten against `cpu->gs_base`. Guest `%fs` — the ubiquitous one, TLS — stays untouched.
  Guest `%gs` use is rare, so this is small.

The 128-byte red zone constraint is a correctness obligation on the spill sequences, not a speed cost.

**What Stage 2 would *not* fix:** the kernel-bound quadrant. `mmap` at 3.9x, `ipc-throughput` at 3.4x
and `ipc-latency` at 5.2x are the syscall service layer, and the measured syscall-bound profile (§3) puts the
guest-syscall service layer at 36.2% of user cycles with fetch-and-decode secondary. Those cases would
improve by a small factor at best.

### aarch64 guest: Stage 2 buys it nothing — stated explicitly so nobody assumes otherwise

The aarch64-guest-on-x86-64-host quadrant is **cross-ISA**. There is no verbatim copy to make. Every
argument in §3.1 — the `%gs` segment base, stealing no GPR, spilling only at block boundaries, the red
zone — is about the same-ISA diagonal and **none of it transfers**. That quadrant needs a full
aarch64 → x86-64 JIT: a new frontend *and* a new backend, which `docs/amd64-host.md` §3 correctly costs
as "very large". `src/translator/guest/aarch64/` is not a translator at all today, it is a
transliterator whose default case writes the guest instruction word straight out, so there is not even a
frontend to reuse.

Consequence for the ranking above: the aarch64 guest will be on the interpreter for the foreseeable
future, and items #1-#4 are the **only** thing that will ever make it faster. #1 applies to it verbatim
(same `sigsetjmp(…, 1)`, same measured per-block syscall). That raises the value of the shared fixes
relative to Stage 2, and it is an argument for doing them before Stage 2 rather than after.

---

## 9. Cross-check against the AArch64 host

I cannot run one. What follows is inference from tracked artefacts, labelled as such.

**Measured on this host, compared against tracked numbers:**

* The `PERF_LIMIT_*` pairs in `cmake/Phase3Gates.cmake` are AArch64-host JIT ceilings. Five of thirteen
  cases meet them on this host's interpreter (§2). Because they are *ceilings* with unknown headroom,
  this bounds the AArch64 host's true numbers from above but does not give them.

**Inferred from `docs/perf-simd-findings.md`:**

* aarch64 guest on ARM64 host, same-ISA: **0.85x native** on `float_simd` (§A). That is the target band
  for a Stage-2 x86-64 diagonal.
* x86-64 guest on ARM64 host, JIT: **53 host instructions per iteration** for a 7-instruction guest SSE
  loop before the fix in §B, 33 after. So the JIT quadrant on the ARM64 host is roughly an order of
  magnitude off native on SSE-heavy code, and that document's own measured A/B table (float_simd 0.827,
  sqlite 0.924, string 0.943, compute 1.002) is about *relative* improvement, not an absolute native
  ratio.
* That document also warns its own numbers were taken at load 7-10 with 10-90% run-to-run spread and
  that "anything inside about 5% is not a claim". The same discipline applies here: nothing in this
  file rests on a difference smaller than 10%, and the ratios I lean on hardest (605x, 44%, 38.2%) are
  large enough that load could not manufacture them.

**What I explicitly did not verify:** that the AArch64 host's `perf-linux` numbers sit where the
thresholds suggest. Nobody has recorded a `metric=` line from that host anywhere in the tree, so the
comparison in §2 is against ceilings, not against measurements. **Recording one run of `ctest -L
perf-linux` on an aarch64 host and committing the output would make every comparison in this document
concrete, and it costs one CI run.**

---

## 10. What I could not measure cleanly

Stated rather than reported as fact.

* **The access bracket under a memory-bound guest.** `busyloop` is pure register work. The 0.36 ns
  figure is a micro-benchmark, not a guest measurement. A memory-heavy fixture would give a better
  number; I did not isolate one, and the conclusion "do not optimise this" rests on the micro-benchmark
  plus the 1.9% ledger share, not on a memory-bound profile.
* **Anything below ~2% of a profile.** The sampler is user-mode only (`perf_event_paranoid=2`), so all
  kernel attribution in §3 is a derived total, not a per-symbol breakdown. I can say 44% of compute CPU
  is kernel and that 99.96% of syscalls are `rt_sigprocmask`; I cannot show a kernel stack.
* **The `[unmapped]` 8.8%.** Censored kernel IPs from PMI skid. Excluded from the user-mode
  denominator, which slightly *under*-states the kernel share if any of them are genuinely user
  addresses. They are all `0xffffffff…`, so they are not.
* **The compat tail.** 69 of 1,524 fixtures. §5's verdict on `HL_MATRIX_TIMEOUT_SCALE=30` is a bound
  from a stratified sample, not an exhaustive result.
* **The first full pass of the per-case table**, taken at load 26-30, is discarded. Its ratios agreed
  with the final numbers to within ~15%, which is the honest error bar; its absolute numbers were
  inflated by up to 2x on wall time.
* **`perf.linux-*-aarch64` cases other than the nine in §6.** `pipe`, `event` and `fork-stress` were
  not run for the aarch64 guest for time reasons.
