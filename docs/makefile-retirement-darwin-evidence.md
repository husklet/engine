# Makefile retirement: measured equivalence

The Makefile is deleted. This records what was measured before deleting it, so
a later regression has a baseline. Apple silicon through the shared checkout,
and an aarch64 Linux host, 2026-07-25.

## Registry

| host | CTest cases |
|---|---:|
| Linux (`nix develop`, both cross compilers) | 396 |
| Darwin | 195 |

`ctest -N` and `--print-labels` enumerate both. `gate.ci-lane-parity` passes on
each host: every lane declared in `cmake/CiLanes.cmake` selects at least one
test.

## Suite-for-suite equivalence

Each compat suite is one CTest case driving `tools/matrix-runner` over the whole
suite manifest, exactly as the Makefile recipe did. "active/excluded" is the
runner's own verdict line, so identical numbers mean identical case selection.

### Linux — `make typed-<suite>` vs `ctest -L compat-<suite>`

| suite | make | ctest |
|---|---|---|
| memory | 106 active / 2 excluded | 106 / 2 |
| process | 71 / 4 | 71 / 4 |
| signals | 69 / 1 | 69 / 1 |
| threads | 54 / 1 | 54 / 1 |

### Darwin — `make compat-<suite>` vs `ctest -L compat-<suite>`

| suite | make | ctest |
|---|---|---|
| memory | 91 / 17 | 91 / 17 |
| process | 66 / 9 | 66 / 9, plus the 2 forkserver cases `make` also ran |
| filesystem | 84 / 5 | 84 / 5 |

Darwin excludes more cases than Linux in the same suites because the manifests
gate them on host; the totals agree (memory 108 both, process 75 both).

### Darwin — the non-compat lanes

| make | ctest | cases |
|---|---|---|
| `unit` (95 unit binaries + 2 debug + check-ci-workflows) | `-L unit` | 100 — same set, with `check-ci-workflows` split into `unit.ci-workflow-invariants` + `unit.publish-gating`, plus `gate.ci-lane-parity` |
| `test-macos` (13 binaries) | `-L macos` | 14 — adds `macos.macos-destroy`, which the Makefile exposed only as a separate `run-unit-macos-destroy` target |
| `e2e-mac-gates` (6 aggregate targets) | `-L e2e-mac` | 16 — the same gates split per guest ISA, plus `e2e.bridge-jobserver` |
| `package-test` | `-L package` | 1 |

Coverage grows on both counts; nothing was dropped.

## Defects the pairing found

Measuring the Darwin pairs is what exposed these; none had ever been run.

1. `ninja` could not build all targets on Darwin: 27 host-Linux-only targets
   (`checkpoint-tree-runner`, `deny-icmp`, the host-native compat fixtures)
   registered unconditionally. `compat-native` and `compat.network-icmp-bridge`
   were therefore registered but unbuildable there.
2. The matrix bridge was `mac` on a Darwin host. `mac` is OrbStack's
   Linux-to-macOS forwarder, not a command on the mac itself, so every case
   exec-failed (`wait=0x7f00`, 0 bytes of stdout). The Makefile had it right:
   `MAC ?= $(if $(filter Darwin,$(shell uname -s)),env,mac)`.
3. The four `*_DYNAMIC_LOADER` / `*_DYNAMIC_LIBC` paths were hardcoded to
   `/usr/...`. The nix devShell exports all four and the Makefile picked them up
   through `?=`; CMake ignored the environment, so rootfs-staging cases staged a
   loader that exists on no host here.

## What guards the result

`cmake/CiLanes.cmake` is the single source of truth for the CI lane set. See
DOCS.md §7.5.1 for the three assertions that hang off it.
