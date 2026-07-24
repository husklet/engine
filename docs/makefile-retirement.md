# Retiring the Makefile

Status: **migration in progress. The Makefile is still authoritative and must not
be deleted.** This document is the coverage ledger that has to reach "no gaps"
before the switchover is a safe, deliberate step.

The build has two systems in parallel:

* `Makefile` — ~2900 lines, ~120 phony targets, the historical authority.
* `CMakeLists.txt` + `cmake/Phase1..Phase4` — progressively taking over; the
  test matrix is registered as CTest cases with labels.

Everything below was established by reading `CMakeLists.txt` and every file in
`cmake/`, and by configuring/building the CMake tree on an aarch64 Linux host
and enumerating the registered tests (`ctest -N`). Nothing is inferred from
target names.

## Headline

| | count |
|---|---|
| CTest cases registered on a Linux host (after this change) | **377** |
| Makefile phony targets | ~120 (of which ~215 are mechanical `run-unit-*` / `run-e2e-compat-*` fan-outs) |
| Makefile targets with no CMake equivalent (bucket ii) | **6 remaining**, all opt-in / non-CI |

CMake covers every lane CI depends on, on both hosts. The remaining gaps are
mac-remote-transport lanes and one packaging family owned by another change.

---

## Bucket (i) — already covered by CMake

Select with `ctest -L <label>`. Counts are from a Linux aarch64 configure.

### Unit lane — 96 CTest cases, label `unit`

| Makefile | CMake |
|---|---|
| `unit` | `ctest -L unit` (`cmake/Phase3Units.cmake`) |
| `run-unit-<name>` (~88 targets) | `unit.<name>` |
| `run-unit-linux` | `unit.linux` |
| `test-native-capacity` | `unit.native-capacity` |
| `test-debug-log`, `test-debug-fatal` | `unit.debug-log`, `unit.debug-fatal` |
| `sanitize` | separate configure: `-DHL_SANITIZE=ON` then `ctest -L unit` |

### Compat matrix — 26 CTest cases, label `compat`

The `typed-<suite>` lane and the mac `compat-<suite>` lane are the *same*
matrix-runner invocation with a different bridge/engine pair, and CMake models
both from one definition (`cmake/Phase3Compat.cmake`, `hl_compat_suite()`).
Host guard picks `env` + `build/linux-production/*` on Linux, `mac` +
`build/production/*` on Darwin — exactly the Makefile's `HOST` conditional.

| Makefile (Linux) | Makefile (macOS) | CMake test | label |
|---|---|---|---|
| `typed-abi` | `compat-abi` | `compat.abi` | `compat-abi` |
| — | `compat-abi-corpus` | `compat.abi-corpus` | `compat-abi-corpus` |
| `typed-completeness` | `compat-completeness` | `compat.completeness` | `compat-completeness` |
| `typed-filesystem` | `compat-filesystem` | `compat.filesystem` | `compat-filesystem` |
| `typed-ipc` | `compat-ipc` | `compat.ipc` | `compat-ipc` |
| `typed-isolation` | `compat-isolation` | `compat.isolation` | `compat-isolation` |
| `typed-libc` | `compat-libc` | `compat.libc` | `compat-libc` |
| `typed-memory` | `compat-memory` | `compat.memory` | `compat-memory` |
| `typed-network` | `compat-network` | `compat.network` | `compat-network` |
| `typed-network-icmp` | (same target) | `compat.network-icmp-bridge` | `compat-network` |
| `typed-posix` | `compat-posix` | `compat.posix` | `compat-posix` |
| `typed-process` | `compat-process` | `compat.process` | `compat-process` |
| `typed-procfs` | `compat-procfs` | `compat.procfs` | `compat-procfs` |
| `typed-signals` | `compat-signals` | `compat.signals` | `compat-signals` |
| `typed-syscall` | `compat-syscall` | `compat.syscall` | `compat-syscall` |
| `typed-syscall_edges` | `compat-syscall-edges` | `compat.syscall-edges` | `compat-syscall-edges` |
| `typed-threads` | `compat-threads` | `compat.threads` | `compat-threads` |
| `typed-time` | `compat-time` | `compat.time` | `compat-time` |
| — | `compat-core-abi` | `compat.core-abi` | `compat-core-abi` |
| — | `compat-core-syscall` | `compat.core-syscall` | `compat-core-syscall` |
| — | `compat-core-regress` | `compat.core-regress` | `compat-core-regress` |
| — | `compat-core-workload` | `compat.core-workload` | `compat-core-workload` |
| — | `compat-isa-x86-64` | `compat.isa-x86-64` | `compat-isa-x86-64` |
| — | `compat-isa-aarch64` | `compat.isa-aarch64` | `compat-isa-aarch64` (**ported here**) |
| — | `compat-soak` | `compat.soak` | `compat-soak` |
| `test-linux-production-typed` | `e2e-compat` (partly) | `ctest -L compat` | — |
| `compat-native` | `compat-native` | `compat-native` | `compat-native` |

**Note on the naming shift:** the Makefile's suite-directory name is
`syscall_edges` (underscore) while the CTest label is `compat-syscall-edges`
(hyphen), following the mac `compat-syscall-edges` target rather than the typed
one. `.github/workflows/linux.yml` shards by `typed-<suite>` names, so anyone
converting the workflow to CTest must map that one name.

### Production / linux-matrix lane — 51 cases, label `production`

| Makefile | CMake |
|---|---|
| `run-linux-production-smoke`, `run-linux-production-aarch64-smoke` | `production.smoke-{aarch64,x86_64}` |
| `test-linux-production-matrix` | `production.matrix` (24 triples, one process) |
| `test-linux-production-config` | `production.config-env`, `.config-supervisor`, `.config-exit70` |
| `test-linux-production-full` | `ctest -L production-full-x86_64` |
| `test-linux-production-aarch64-full` | `ctest -L production-full-aarch64` |
| `test-linux-production-core-abi` | `production.full-x86_64.core-abi` |

### Gates

| Makefile | CMake | label | count |
|---|---|---|---|
| `test-linux-lifecycle` | `lifecycle.*` | `lifecycle` | 10 |
| `e2e-checkpoint-*` (all 40+ scenarios) | `checkpoint.<arch>.<scenario>` | `checkpoint` | 76 |
| `dynamic-e2e` | `dynamic-e2e.{aarch64,x86_64}` | `dynamic-e2e` | 2 |
| `remote-supervisor-test` | `remote-supervisor` | `integration` | 1 |
| `perf-linux` | `perf.linux-*` | `perf-linux` | 28 |
| `perf-native-aarch64` | `perf.native-*` | `perf-native` | 11 |
| `bench` | `ninja -C build bench` (custom target; no pass/fail) | — | — |
| `run-e2e-compat-<case>` (34) | `e2e-oracle.<case>-<arch>` | `e2e-oracle` | 68 (**ported here**) |
| `isa-fuzz-regress`, `isa-fuzz-arm-regress` | `isa-fuzz.*` | `isa-fuzz` | 3 (**ported here**) |
| `compat-core-workload-extended`, `compat-soak-extended` | `compat.*-extended` | `compat-extended` | 2 (**ported here**) |

### macOS host lane (`cmake/Phase4Mac.cmake`, Darwin configure only)

| Makefile | CMake | label |
|---|---|---|
| `test-macos` (13 host-service binaries) | `macos.*` | `macos` |
| `run-unit-macos-destroy`, `test-native-capacity-macos`, `test-dns-objc-fork-macos` | `macos.macos-destroy`, `macos.native-capacity-macos`, `macos.dns-objc-fork` | `macos` |
| `e2e-mac-gates`, `e2e-lifecycle-{signal,control,hygiene}`, `e2e-embedding-{fd,stdio,dir}` | `e2e.*` | `e2e-mac` |
| `e2e-mac-build*` (build-only prerequisites) | the targets those tests depend on | — |
| bridge jobserver hygiene | `e2e.bridge-jobserver` | `e2e-mac` |
| `test-dual-backend` | `dual-backend.mac-link` | `embedding` (**ported here**) |
| `perf-macos` | `perf.mac-*` | `perf-macos` (**ported here**) |

### Packaging / install

| Makefile | CMake |
|---|---|
| `install` | `cmake --install <build> --prefix <p>` (`cmake/Phase4Install.cmake`) |
| `package-test` | `package.consumer-link` (label `package`) |
| `package-activation-installed-test` | the activation leg inside `package.consumer-link` |
| `test-dual-backend` (Linux dual archive) | `dual-backend.link` (label `embedding`) |
| `format`, `format-check` | `cmake/Format.cmake` targets |

---

## Bucket (ii) — NOT covered; still must be ported

Six items. **None of them is on the CI critical path** (`.github/workflows/*.yml`
never invokes any of them), but each is real coverage that would be lost.

1. **`perf-macos` / `perf-linux` driven over the OrbStack `mac` remote
   transport.** The mac perf lane is now registered *when configuring on
   macOS*. The Makefile can also drive a macOS machine **from a Linux host**
   via the `mac` command prefix (`MAC ?= mac`). CMake models no remote
   execution — see the header of `cmake/Phase4Mac.cmake`. Retiring the Makefile
   means deciding that lane is dead or moving it to a script.

2. **`e2e-compat`'s own six direct launches** (`e2e-runner` /
   `config-e2e-runner` against `build/production/hl-engine-linux-*` with
   expected statuses 42 and 70). These are the mac-engine equivalents of
   `production.config-*`; not registered on the mac side.

3. **`package-activation-macos-test`** — the mac activation-archive consumer
   link. `PackageTest.cmake` runs only on Linux
   (`if(HL_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")`).

4. **`uninstall`** — no CMake uninstall target. `package.consumer-link` asserts
   install does not clobber foreign files, but nothing asserts removal parity.

5. **`isa-fuzz` / `isa-fuzz-arm` open-ended campaigns.** Deliberately *not*
   ported as tests (see bucket iii); if anyone depends on `make isa-fuzz` as an
   entry point, that entry point disappears with the Makefile. The underlying
   `tests/fuzz/isa/<arch>/run.sh` is unaffected and takes `--seeds N`.

6. **Archive/package regeneration targets** — `package-embedded`,
   `package-embedded-linux`, `package-embedded-macos`, and the rules producing
   `build/package/<platform>/libhl-engine.a`.
   **OWNED ELSEWHERE: a concurrent change is adding archive-freshness
   automation over exactly these. Deliberately untouched here.** CMake does
   build `build/package/linux-aarch64/libhl-engine.a` (Phase 2) and
   `build/package/macos-aarch64/libhl-engine.a` (Phase 4), so the artefacts
   exist; the *regeneration/freshness policy* is the open part.

---

## Bucket (iii) — obsolete, should die with the Makefile

| Target | Why it should not be ported |
|---|---|
| `all`, `linux-compile`, `compat-build`, `compat-engines`, `compat-core`, `e2e-mac-build*` | Pure build aggregations. `ninja` builds everything a registered test needs, by dependency, and `ctest` builds nothing it does not need. Aggregation targets are a make-ism for expressing "these prerequisites"; CMake expresses it with `add_test` dependencies. |
| `clean` | `rm -rf <build-dir>`. Out-of-tree builds make a clean target meaningless. |
| `help` | A hand-maintained list of targets that drifts. `ctest -N`, `ctest --print-labels` and `ninja -t targets` are generated from the real graph and cannot drift. |
| `FORCE` | An implementation detail of make's dependency model. |
| `run-unit-<name>` (~88 targets) | One-per-binary fan-out that exists only because make has no test selector. `ctest -R unit.<name>` replaces all of them. |
| `test` (= `unit compat-native`) | An arbitrary pairing. `ctest -L 'unit|compat-native'`. |
| `e2e-compat` (umbrella) | The Makefile itself marks it `.NOTPARALLEL` and CI never runs it — `mac.yml` shards `compat-*` instead. Its content is covered case-by-case; the umbrella is a sequencing hack that CTest's `RESOURCE_LOCK` expresses precisely. |
| `perf-compat` (= `e2e-compat perf-macos`) | Composition of two other targets. `ctest -L 'compat|perf-macos'`. |
| `test-linux-production-typed` | The sequential chain of all `typed-*`. `ctest -L compat` with resource locks does the same and reports per-suite. |
| `isa-fuzz`, `isa-fuzz-arm` | Open-ended random-seed campaigns. A search, not a pass/fail test; a suite must be deterministic. The deterministic `*-regress` seed sets ARE ported. |
| `sanitize` | Recursive `make` into a second BUILD dir. The idiomatic CMake form is a second configure (`-DHL_SANITIZE=ON`), already documented in `CMakeLists.txt`. |
| `typed-$(1)` | An artefact of `$(eval)`; appears in `.PHONY` but is not a real target. |

---

## What was ported in this change

| Ported | CMake test(s) | Makefile equivalent |
|---|---|---|
| aarch64 ISA regression suite | `compat.isa-aarch64` + the `isa/aarch64/isa_regress` guest fixture (non-PIE) | `compat-isa-aarch64` |
| native-oracle differential e2e | 68 × `e2e-oracle.<case>-<arch>` | 34 × `run-e2e-compat-<case>` |
| warm code-cache perf | `perf.linux-warm-cache-{aarch64,x86_64}` | `HL_PERF_CACHE_LINUX` inside `perf-linux` |
| ISA fuzz regression seeds | `isa-fuzz.x86_64-regress`, `isa-fuzz.aarch64-regress`, `isa-fuzz.aarch64-regress-pie` | `isa-fuzz-regress`, `isa-fuzz-arm-regress` |
| repeat-10 stress variants | `compat.core-workload-extended`, `compat.soak-extended` | `compat-core-workload-extended`, `compat-soak-extended` |
| mac dual-backend gate | `dual-backend.mac-link` | `test-dual-backend` |
| macOS perf lane | 28 × `perf.mac-*` | `perf-macos` |

### Deliberate difference: `e2e-oracle`

`make run-e2e-compat-<case>` drives the **`build/production`** engines — the
macOS-built binaries, reached over the `mac` remote transport — and gates the
whole family on `HOST=linux`, because the oracle (`build/fixtures/<case>`, the
same source built with the host compiler) has to be a natively runnable Linux
binary.

CMake models no remote execution, so `e2e-oracle.*` uses the **local
`build/linux-production` engine** of the matching guest ISA. The case set, the
guest binaries, the oracle fixtures and the comparison are identical; only which
build of the engine runs them differs. This is stated rather than hidden: it is
a coverage *difference*, not a coverage *loss* — every case `make` runs is run,
and the mac-engine leg remains available through the Makefile until the remote
lane is resolved (bucket ii item 1).

---

## Equivalence evidence

Measured on an aarch64 Linux host inside `nix develop`. `matrix-runner` prints
the number of cases it actually executed, so the two paths can be compared
directly rather than by inspection.

| Suite | `make typed-<suite>` | `ctest -L compat-<suite>` | verdict |
|---|---|---|---|
| ipc | `matrix-runner: 122 active cases passed with 1 repetition(s); 0 manifest cases excluded` | `matrix-runner: 122 active cases passed with 1 repetition(s); 0 manifest cases excluded` | **122 = 122** |
| syscall_edges | `matrix-runner: 38 active cases passed ...; 0 manifest cases excluded` | `matrix-runner: 38 active cases passed ...; 0 manifest cases excluded` | **38 = 38** |

Both paths pass through the identical `matrix-runner env <a64-engine>
<a64-bindir> <x64-engine> <x64-bindir> tests/compat/<suite>` invocation, and the
runner loads the case list from `tests/compat/<suite>/manifest.tsv`. Case
identity therefore follows from identical arguments, and the counts confirm the
fixture sets built by the two systems agree — a missing fixture would show up as
a lower "active cases" number or an outright failure, not as a silent skip.

Newly ported lanes, verified green:

* `ctest -L compat-isa-aarch64` → `1 active case passed` (the `isa_regress`
  fixture, non-PIE). No Makefile-side comparison is possible on a Linux host:
  `make compat-isa-aarch64` needs `build/production/*`, i.e. the mac engines.
* `ctest -L e2e-oracle` → 68 registered; spot-run of the
  `atomics`/`eventfd`/`timerfd` cells across both arches: 6/6 passed.
* `ctest -L unit` → 96/96 passed.
* Full registry: **377** CTest cases (was 301 before this change).

The Makefile is unchanged and still works:
`make -s -j$(nproc) CC="$NATIVE_CC" AR=ar unit` → rc 0;
`make typed-ipc`, `make typed-syscall_edges` → rc 0.

---

## Before the Makefile can be deleted

1. Resolve the six bucket (ii) items above (or explicitly declare the
   mac-remote-transport lane dead).
2. Land the archive-freshness change that owns the packaging targets.
3. Convert `.github/workflows/linux.yml`'s `typed-<suite>` shards and
   `mac.yml`'s `compat-<suite>` shards to `ctest -L compat-<suite>`, keeping
   the shard buckets — note the `syscall_edges` → `syscall-edges` rename.
4. Convert `mac.yml`'s `make unit` / `package-test` / `test-macos` /
   `e2e-mac-gates` steps to `ctest -L unit|package|macos|e2e-mac`.
5. Only then delete `Makefile`, in its own commit, with nothing else in it.
