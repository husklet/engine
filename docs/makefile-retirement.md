# Retiring the Makefile

Status: **the CMake/CTest registry is complete; the Makefile is still what CI
runs for the compat shards and for the whole macOS lane, so it must not be
deleted yet.** This document lists only what remains.

Two systems build the same tree:

* `Makefile` — 2969 lines, 132 explicit targets.
* `CMakeLists.txt` + `cmake/Phase2Production.cmake`, `cmake/Phase3*.cmake`,
  `cmake/Phase4*.cmake` — the whole test matrix as CTest cases with one label
  per historical lane. A Linux `nix develop` configure registers 394 tests
  (`cmake -G Ninja -B build-x && ctest --test-dir build-x -N`).

## What guards the migration

* `gate.makefile-lane-parity` (`cmake/LaneParity.cmake`, `tools/check_lane_parity.sh`)
  asserts that every historical CI lane resolves to a **non-empty** CTest label
  on the current host. `ctest -L <missing-label>` exits 0, so without this gate a
  renamed label would turn a converted workflow step silently green.
* `.github/workflows/linux.yml` additionally checks each `typed-<suite>` shard
  name against `ctest -N -L compat-<suite>`.

Both are registry checks, not suite runs. They cost seconds.

## Remaining work

1. **`.github/workflows/mac.yml` is entirely `make`.** Convert:
   `make unit` → `ctest -L unit`; `make package-test` → `ctest -L package`;
   `make test-macos` → `ctest -L macos`; `make e2e-mac-gates` → `ctest -L e2e-mac`;
   the five `mac-compat` shards (`compat-abi-corpus`, `compat-abi compat-core
   compat-isa-x86-64 compat-completeness`, …) → `ctest -L compat-<suite>`.
   Keep the shard buckets and the existing single retry.
2. **`.github/workflows/linux.yml` compat shards still run `make typed-<suite>`.**
   Convert to `ctest -L compat-<suite>`, keeping the buckets. One name changes:
   the suite directory is `tests/compat/syscall_edges` and the Makefile target is
   `typed-syscall_edges`, but the CTest label is `compat-syscall-edges`
   (hyphen), following the mac target name.
3. **`nix develop --command make check-crate-archives`** in `linux.yml` and
   `publish.yml` → `cmake --build <dir> --target check-crate-archives`. Both
   entry points exist and run the same `tools/check_crate_archives.sh`.
4. **Decide the `e2e-oracle` engine difference.** `make run-e2e-compat-<case>`
   drives the macOS-built `build/production` engines over the `mac` bridge;
   `e2e-oracle.<case>-<arch>` (68 cases) drives the local
   `build/linux-production` engine of the matching guest ISA. Same cases, same
   guest binaries, same native oracle fixtures, different build of the engine.
   Either accept the local-engine form or route it through
   `tools/run_remote_macos_ctest.sh`.
5. **`isa-fuzz` / `isa-fuzz-arm` open-ended campaigns** are `ninja` targets, not
   tests, on purpose: a random-seed search has no verdict. Only the deterministic
   seed sets are CTest cases (`isa-fuzz.x86_64-regress`,
   `isa-fuzz.aarch64-regress`, `isa-fuzz.aarch64-regress-pie`). Nothing to port;
   listed so it is not mistaken for a gap.
6. **Then delete `Makefile`**, in its own commit, with nothing else in it.

Note while converting: most labels the parity gate protects are not run by any
workflow today (`production*`, `checkpoint*`, `lifecycle`, `dynamic-e2e`,
`e2e-oracle`, `perf-*`, `integration`, `compat-extended`, `compat-direct`).
Converting CI is not the same as gating those lanes.

## No longer gaps

Previously tracked as unported; each now has a real entry point.

| Was missing | Now |
|---|---|
| `perf-macos` driven from a Linux host over the `mac` transport | `perf-macos-remote` target + `tools/run_remote_macos_ctest.sh` (configures, builds `perf-macos-build`, runs `ctest -L <label>` on the mac) |
| `e2e-compat`'s six direct production/config launches | `compat-launch.{cli-exit42,config-exit42,config-exit70}-{aarch64,x86_64}` (`cmake/Phase4Mac.cmake`, label `compat-direct`) |
| `package-activation-macos-test` | `package.consumer-link` registers on Darwin too and carries the activation leg (`cmake/Phase4Install.cmake`, labels `package;package-activation;package-embedded`) |
| `uninstall` | `uninstall` target (`cmake/Uninstall.cmake.in`, exact `install_manifest.txt` consumer); `cmake/PackageTest.cmake` runs it and asserts owned files went away while a planted foreign file survived |
| archive/package regeneration policy | `refresh-crate-archives` / `check-crate-archives` targets (`tools/refresh_crate_archives.sh`, `tools/crate_archive_manifest.sh`); see DOCS.md "Prebuilt crate archives" |

## Targets that should die with the Makefile, not be ported

| Target | Why |
|---|---|
| `all`, `linux-compile`, `compat-build`, `compat-engines`, `compat-core`, `e2e-mac-build*` | Build aggregations. `ninja` builds what a test needs by dependency; `ctest` builds nothing it does not need. |
| `clean` | `rm -rf <build-dir>`. Out-of-tree builds make it meaningless. |
| `help` | Hand-maintained target list. `ctest -N`, `ctest --print-labels`, `ninja -t targets` are generated from the real graph. |
| `run-unit-<name>` fan-out | Exists only because make has no test selector. `ctest -R unit.<name>`. |
| `test` (= `unit compat-native`) | An arbitrary pairing: `ctest -L 'unit|compat-native'`. |
| `e2e-compat` umbrella | Marked `.NOTPARALLEL`, never run by CI. Its content is covered case by case; the sequencing is `RESOURCE_LOCK`. |
| `perf-compat` (= `e2e-compat perf-macos`) | `ctest -L 'compat|perf-macos'`. |
| `test-linux-production-typed` | The sequential chain of all `typed-*`. `ctest -L compat` reports per suite. |
| `sanitize` | Recursive make into a second BUILD dir. Idiomatic form is a second configure: `-DHL_SANITIZE=ON`, then `ctest -L unit`. |
| `FORCE`, `typed-$(1)` | Artefacts of make's dependency model and `$(eval)`. |
