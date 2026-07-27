# Testing and CI for the Windows host

A plan for the test and CI infrastructure of a native Windows (Win32/NT, mingw-w64 clang) host, host CPU
x86-64 first. It covers what can be gated, in what order, and what has to change in the guards before the
host token can be declared at all.

`DOCS.md` is normative. This file is a plan, not a record: nothing here has been executed. Every claim about
the tree was read out of the tree on this branch and carries a file and line; every claim about a Windows
runner that was *not* read out of the tree is marked as an assumption in §8.

Scope boundary: another agent owns the build system. Where a build decision is a prerequisite for a test
lane, this document states the *requirement the test side has* and stops there.

---

## 1. Status of the premise

`src/host/windows/` is a README and nothing else (`src/host/windows/README.md`, 4 lines). `flake.nix:46-52`
declares `windows = { supported = false; }` and `flake.nix:11` lists three systems, none of them Windows.
So there is no Windows build to test yet, and this plan starts from a configure that does not exist.

The one thing that is already true and worth stating first: **a `CMAKE_SYSTEM_NAME=Windows` configure would
register 3 labels and ~110 tests today, and roughly ten of them would not link.** Everything else in the tree
is behind an OS guard. That is the shape of the problem — not "port the tests", but "almost every lane is
structurally absent, and the guards that are supposed to notice an absent lane are keyed to two operating
systems".

---

## 2. Lane inventory

### 2.1 What the guards consider a lane

`cmake/CiLanes.cmake` is the registry. It declares 24 sharded compat lanes (`HL_CI_SHARDED_LINUX` /
`HL_CI_SHARDED_DARWIN`, identical sets), 3 direct Linux lanes, 4 direct Darwin lanes, 19 registry-only Linux
lanes and 6 registry-only Darwin lanes. `gate.ci-lane-parity` (`cmake/LaneParity.cmake:20-24` →
`tools/check_lane_parity.sh`) asserts every declared lane selects at least one CTest case on the host it
applies to; it does not compare contents.

Labels that exist in the tree but are *not* declared — `compat`, `gate`, `perf`, `production-full`,
`checkpoint-aarch64`, `checkpoint-x86_64`, `e2e`, `rust` — are unguarded by parity and are not in scope for
the Windows lists either.

### 2.2 The single structural fact

`cmake/Phase3Gates.cmake:143` opens `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` and closes at line 950
(`endif() # native Linux lane`). **Every `add_test()` in that 951-line file is inside it.** Lines 1-142
compile guest fixtures and register nothing.

Above that, `CMakeLists.txt:218 if(HL_HAVE_GUEST_CC)` gates the inclusion of `Phase3Compat.cmake`,
`Phase3Gates.cmake` and `Phase4Mac.cmake` entirely, and `CMakeLists.txt:246` gates `LaneParity.cmake` on the
same flag. `HL_HAVE_GUEST_CC` is TRUE only when both guest cross-compilers are named by environment
variables the nix devShell exports (`cmake/GuestFixtures.cmake:32-51, 66-86`), and the file `return()`s at
line 85 otherwise — before `hl_guest_binary` is even defined.

Two consequences that drive the whole plan:

1. On a Windows runner with no guest cross-toolchain, **`gate.ci-lane-parity` is not registered at all**.
   Declaring `Windows-x86_64` in `HL_CI_HOSTS` while that holds buys a declaration nothing checks — the same
   class of hole the `CiLanes.cmake` comment warns about for I20.
2. Relaxing `Phase3Gates.cmake:143` is not a one-line edit. `Phase3Gates.cmake:927-948` sweeps six CTest-name
   patterns out of the directory `TESTS` property and raises `message(FATAL_ERROR)` at line 943 if any
   matches zero tests. Admitting a Windows configure into that block without also admitting those lanes
   fails the *configure*, not a test.

### 2.3 Per-lane disposition

Legend: **P1** = can run in phase 1 (host-only, no guest execution); **ADAPT** = needs harness or guard work
but no new engine capability; **GUEST** = blocked on guests executing on Windows; **NEVER** = structurally
impossible or meaningless on a Windows host.

| Lane | Registered at | Windows | Why |
|---|---|---|---|
| `unit` | `Phase3Units.cmake:111-118, 240`; `Phase3CiConfig.cmake:10,16` | **P1 (ADAPT)** | 106 of ~108 cases are plain executables with no shell, no `/tmp`, no locks. Blockers below. |
| `package` | `Phase4Install.cmake:221-241` | **ADAPT** | Registers under bare `if(HL_BUILD_TESTS)` — it *will* appear on Windows. `cmake/PackageTest.cmake:112-115` is a GCC-driver link line (`-L… -lhl-host-… -pthread`); mingw-w64 clang accepts that syntax, but the output has no `.exe` suffix and `PACKAGE_HOST` must name a Windows provider. |
| `package-activation`, `package-embedded` | same test, `Phase4Install.cmake:245-248` | **GUEST** | Conditional on `HL_HAVE_ACTIVATION`, whose target `hl-engine-activation` is created at `Phase2Production.cmake:136` — below that file's `if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux") return()` at line 65. Also declared Darwin-only today. |
| 24 × `compat-*` | `Phase3Compat.cmake:493-499` | **GUEST** | Each label is one CTest case driving `matrix-runner` over both engines and both guest binary roots (`Phase3Compat.cmake:494-497`). Needs a ported `matrix_runner.c` (§6.1), both production engines, and the guest corpus. |
| `compat-extended` | `Phase3Compat.cmake:559-565` | **GUEST** | Same, ×10 repeats. |
| `compat-native` | `Phase3Compat.cmake:389-390` | **NEVER** | Inside `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` at line 377. It runs *native Linux* fixture binaries as an oracle; there is no Windows analogue of "run this Linux C program natively". |
| `compat-direct` | `Phase4Mac.cmake:288` | **NEVER** | Darwin lane. |
| `production`, `production-config`, `production-full-*` | `Phase3Gates.cmake:216,259,292,301,309,326` | **GUEST** | Behind line 143. `production.smoke-<arch>` is the milestone to watch, exactly as `docs/amd64-host.md` §7 uses it. |
| `lifecycle` | `Phase3Gates.cmake:191,205` | **GUEST** | Behind line 143; POSIX signal expectations (`--expect-signal 11`), fork/exec lifecycle control. |
| `e2e-oracle` | `Phase3Gates.cmake:349` | **NEVER (as written)** | It diffs a guest against the *same C source compiled and run natively on the host* (`Phase3Gates.cmake:152-160`), over `epoll`/`signalfd`/`inotify`/`futex`/`seccomp`/`sysv_ipc`. The oracle cannot exist on NT. If the lane is wanted on Windows the oracle must be captured on Linux and committed, which is a different test. |
| `integration` (`remote-supervisor`) | `Phase3Gates.cmake:354-356` | **GUEST/ADAPT** | fork/exec + fd-passing supervisor. Needs a Win32 transport. |
| `checkpoint`, `checkpoint-io` | `Phase3Gates.cmake:386,396,404,417` | **GUEST** | 82 + 17 cases over pipes, unlinked-but-open files, memfd, eventfd, timerfd, inotify, epoll, socketpairs. All engine-emulated objects, so conceptually reachable — but only after guests run. |
| `ckpt-cross` | `Phase3Gates.cmake:904`, guard line 842 | **NEVER** | Guard is `Linux AND HL_HOST_ARCH x86_64`, and the gate needs `qemu-aarch64` plus a `#!/bin/sh` shim it writes and `chmod +x`es (`tools/checkpoint_cross_gate.sh:69-72`). |
| `emulated-aarch64`, `emulated-aarch64-gated` | `Phase3Gates.cmake:858`, guard line 842 | **NEVER** | qemu-user. Same guard. |
| `nested-engine` | `Phase3Gates.cmake:781` | **GUEST (late)** | Conceptually the strongest Windows acceptance criterion — a PE engine hosting an ELF `hl-engine` hosting a guest — but `tools/nested_engine_gate.sh` tests the executable bit (`[ -x ]`, lines 38/50), `mktemp`s under `${TMPDIR:-/tmp}`, `trap`s POSIX signal names and shells `diff -u`. Portable in principle under Git Bash; unproven. |
| `dynamic-e2e` | `Phase3Gates.cmake:446` | **GUEST (late)** | Synthesises a rootfs and links guests with `-Wl,--dynamic-linker,/lib/ld-linux-*` and `-rpath,/lib`. The rootfs is a directory tree the engine maps, so it survives on Windows; the loader paths are guest-side and fine. |
| `embedding` (`dual-backend.link`) | `Phase3Gates.cmake:424-429` | **GUEST** | Runs a prebuilt package binary against six ELF guest paths; the binary itself comes from `Phase2Production.cmake:146-154` (Linux-only). |
| `perf-linux` | `Phase3Gates.cmake:539,582,592` | **ADAPT + GUEST** | Hardcodes `/tmp` payload paths (lines 515, 529, 576-578, 588-589) and `WORKING_DIRECTORY /tmp` (539, 583, 592), and drives through `cmake/RunSequence.cmake`, which does `separate_arguments(... UNIX_COMMAND ...)` — a Windows path with backslashes and a drive colon is mangled. Every one of those is a mechanical fix, but it is a fix. |
| `perf-native` | `Phase3Gates.cmake:611`, guard 600-604 | **NEVER** | Guard is `CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"` and it executes the cross-built guest ELF binaries directly on the host. |
| `isa-fuzz` | `Phase3Gates.cmake:705` | **NEVER (as written)** | The tests invoke `tests/fuzz/isa/*/run.sh` **with no interpreter** (lines 678, 688, 695), relying on the kernel honouring a shebang and the exec bit. CTest on Windows cannot execute those. The x86_64 driver also wants `qemu-x86_64` as its oracle. Reachable only by rewriting the driver invocation to go through `${HL_BASH_EXECUTABLE}` and finding a differential oracle. |
| `macos`, `e2e-mac`, `perf-macos` | `Phase4Mac.cmake` | **NEVER** | Darwin. |

**Summary: exactly one lane (`unit`) is a phase-1 candidate, and `package` is a near-term second.** Everything
else is blocked on guests executing under a PE engine, and four lanes (`compat-native`, `perf-native`,
`e2e-oracle`, `isa-fuzz`) are blocked on something a Windows host will never have.

### 2.4 What blocks `unit` specifically

`Phase3Units.cmake` is the most portable file in the tree — no shell, no `/tmp`, no `RESOURCE_LOCK`, no
`SKIP_RETURN_CODE`. Three concrete blockers:

1. **`_hostl` falls through to `hl-host-linux`.** `Phase3Units.cmake:123-127` is a two-arm `if(Darwin) …
   else()`. On Windows the else branch names a target that `CMakeLists.txt:192-200` does not create, so
   CMake emits a bare `-lhl-host-linux` and the ~10 units that use `${_hostl}` fail at link
   (`native`, `private`, `directory_services`, `resolve_services`, `linux_fork`, `pipe_linux`,
   `eventfd_fork`, `fork_wire`, and the two `-SOURCES src/host/linux/*.c` cases at lines 185-189).
2. **There is no Windows exclusion list.** `_HL_DARWIN_EXCLUDED_UNITS` (`Phase3Units.cmake:29-31`) names 11
   units; the Windows set is almost certainly the same set plus whatever the Windows backend does not
   implement. This must be an explicit list mirroring the Darwin one, not a silent link failure.
3. **`unit.linux` and `unit.native-capacity`** are inside `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`
   (`Phase3Units.cmake:215-225`) and correctly drop.

`unit.ci-workflow-invariants` and `unit.publish-gating` (`Phase3CiConfig.cmake`) are pure YAML inspection
through `${HL_BASH_EXECUTABLE}` and should run unchanged under Git Bash. `find_program(NAMES bash REQUIRED)`
appears three times (`CMakeLists.txt:27`, `Phase3CiConfig.cmake:8`, `LaneParity.cmake:14`) — with Git Bash on
`PATH` it succeeds; without it the Windows configure fails outright.

`gate.archive-closure` (`Phase4Install.cmake:207-211`, labels `unit;gate`) is Linux-guarded at line 192 and
drops. It is worth re-enabling on Windows later — it is `llvm-nm` over the archives and a link probe — but it
is not required for parity, since `gate` is not a declared lane.

---

## 3. The host token

### 3.1 What "declare the token" means mechanically

Three things must be true together, and the order matters (§4):

1. `Windows-x86_64` in `HL_CI_HOSTS` (`cmake/CiLanes.cmake:24-28`). The token is
   `<CMAKE_SYSTEM_NAME>-<HL_HOST_ARCH>`. `CMakeLists.txt:32-40` already maps `AMD64` → `x86_64`, so the token
   spells itself correctly with no edit.
2. One workflow file, `.github/workflows/windows-x86_64.yml`.
3. New lane lists — `HL_CI_DIRECT_WINDOWS` and `HL_CI_REGISTRY_WINDOWS`, and later
   `HL_CI_SHARDED_WINDOWS`. `tools/check_lane_parity.sh:39-53` is a `case "$os"` over `Linux` and `Darwin`
   with `*) … exit 2`, so **a Windows configure fails the parity gate with exit 2, not a skip**, until that
   arm exists. It also refuses at line 33 any `$os-$cpu` not in `HL_CI_HOSTS`.

A non-obvious constraint: `check_lane_parity.sh:87-90` exits 1 if the label list is empty. So
`HL_CI_DIRECT_WINDOWS` cannot start empty — the first declaration must already have `unit` in it and `unit`
must already be non-empty and green on the runner.

And the prerequisite from §2.2: `gate.ci-lane-parity` is only registered when `HL_HAVE_GUEST_CC` is true
(`LaneParity.cmake:10-12`, `CMakeLists.txt:246`). **Declaring the token without solving that means the parity
gate does not exist on the host it was declared for.** Either the Windows configure must satisfy
`HL_HAVE_GUEST_CC` (which, given §5, means "the corpus was supplied rather than compiled" — a build-system
decision), or `LaneParity.cmake` must be registered on a weaker condition. The test-side requirement is
simply: *the gate must be registered and green on the Windows runner in the same change that declares the
token.* How is the build agent's call.

### 3.2 `HL_CI_HOST_CPU_ONLY` — what it is and is not for

`HL_CI_HOST_CPU_ONLY` (`CiLanes.cmake:76-80`) takes `<host-token>:<lane>` and, per
`check_lane_parity.sh:59-85`, drops a label that is *named for some host but not this one*. Its purpose is
stated at `CiLanes.cmake:63-64`: lanes applying to only **some of a host OS's CPUs**.

It is therefore **not** the escape hatch for "this lane cannot run on Windows". Cross-OS asymmetry is already
expressed by the per-OS lists: a lane simply absent from `HL_CI_*_WINDOWS` is not checked on Windows, and
that is correct and sufficient. `HL_CI_HOST_CPU_ONLY` becomes relevant only when a **second Windows host CPU**
(Windows-aarch64) exists and a lane is non-empty on only one of them — for instance, if a Windows-x86_64
runner is the only place a cross-built Windows-aarch64 engine can be compiled-and-not-run, the entry would be
`Windows-x86_64:<lane>`, exactly mirroring `Linux-x86_64:emulated-aarch64`.

Using it to hide a lane that simply is not implemented yet would be a misuse: the entry would read as
"this CPU cannot", when the truth is "no Windows CPU can yet".

### 3.3 Passing `gate.ci-lane-parity` honestly

Parity counts tests; it cannot compare them (`DOCS.md` 7.5.1 says so, and lists the two known asymmetries
that are consequently written down rather than detected). The Windows lists must therefore be conservative:
declare a lane only when it is non-empty *and* green, and write down any lane whose **contents** differ from
the same label on another host. On Windows the differing-contents cases are predictable in advance and should
be recorded at the declaration site, not discovered later:

- `unit` will be smaller than on Linux by the Windows exclusion list plus `unit.linux` and
  `unit.native-capacity`, and larger than on Darwin only where the Windows backend implements something the
  macOS one does not. A green `unit` on Windows is not the same lane as a green `unit` on Linux.
- `package` will lack the activation and embedded legs until `HL_HAVE_ACTIVATION` is true there
  (`Phase4Install.cmake:245`), so a green `package` on Windows proves the plain consumer link only.

---

## 4. `tools/check_ci_workflows.sh`: I19/I20 and the two-OS assumption

### 4.1 What breaks, precisely

The guard has four places that assume exactly two operating systems, each of which is one Linux list against
one Darwin list.

| Site | Lines | Assumption | Effect of adding a third OS |
|---|---|---|---|
| I13/I14 call sites | 251-252 | `mac.yml`↔`HL_CI_SHARDED_DARWIN`, `linux.yml`↔`HL_CI_SHARDED_LINUX`, hardcoded | `windows-x86_64.yml` is checked by **neither**. A compat lane named there is unguarded. |
| I19 pairwise diff | 295-310 | Two loops, `$darwin_lanes` vs `$linux_lanes` | Correct-but-vacuous while Windows is not a compat host; wrong the moment it is. Parity becomes an all-pairs relation. |
| I19 `sole_host_for` | 281-294 | Exactly one compat host per OS, because `HL_CI_SHARDED_*` is keyed by OS | A second Windows host CPU hits the identical failure the comment at `CiLanes.cmake:46-49` describes for Linux. |
| I19 stale-exemption walk | 322-331 | `case "${host%%-*}" in Linux) own=…; other=…; Darwin) …; *) VIOLATION` | A `Windows-*:<lane>` entry in `HL_CI_SHARDED_HOST_ONLY` is rejected as "a host OS with no `HL_CI_SHARDED_*` list", and `own`/`other` are meaningless with three lists. |
| I20 | 346-348 | `if ! has Linux-x86_64 "$compat_hosts"; then check_shards linux-x86_64.yml "" I20` — the token is a **literal** | `windows-x86_64.yml` gets no I20 check at all. This is the exact hole `CiLanes.cmake:53-55` warns about, arrived at from the other direction: not "declaring the token turns I20 off", but "a token I20 never knew about was never on". |

Two more, less obvious:

- **I17/I5 will not fire on the Windows workflow.** Both key on `step_runs[i] ~ /(nix build|nix develop|cargo )/`
  (lines 129-131, 143-146). A Windows workflow has no nix (§5), so its `ctest`/`cmake` steps are exempt from
  the "must emit its own `::error`" and "must have a step timeout" requirements. That is a real weakening of
  the invariant set for the newest and least-proven lane. It should be closed, but closing it changes the
  regex for the existing three workflows too, so it is its own change with its own measurement — not
  something to fold into the Windows one.
- **I10/I10b/I11 are per-file literals** (lines 190-204) covering `linux.yml` and `mac.yml` only.
  `linux-x86_64.yml` already has an unguarded concurrency block; `windows-x86_64.yml` would be the second.
  Worth noting as an existing gap rather than a Windows-specific one.

I3/I4/I7/I9/I15/I16/I18 all iterate `"$wfdir"/*.yml` and apply to a new file automatically. That is good: the
new workflow inherits the timeout arithmetic, the `--no-tests=error` requirement and the Node-generation
action pin from day one, with no edit.

### 4.2 The required order

Mirroring the four-step sequence at `CiLanes.cmake:46-57`, and for the same reason — the last step is the one
that turns a guard off:

1. **Replace the two hardcoded I13/I14 call sites with a host-token → (workflow file, sharded list) map.**
   Derive the map from `HL_CI_HOSTS` rather than restating it; a token with no workflow file, or a workflow
   file with no token, should itself be a violation. This step alone is a pure refactor with no behaviour
   change on the current three hosts, so it can land and be verified first.
2. **Generalise I20 to iterate.** `for host in $hosts; do has "$host" "$compat_hosts" && continue;
   check_shards "$(workflow_for "$host")" "" I20; done`. After this, any future host token gets the
   shards-nothing guarantee automatically, including `Linux-x86_64` exactly as today.
3. **Generalise I19 to all-pairs over `HL_CI_COMPAT_HOSTS`**, keyed on the token rather than
   `${host%%-*}`, and make the stale-exemption walk look up the exempted host's own list from the same map.
   Deferrable until Windows becomes a compat host — but note that step 1's map is what makes it possible, so
   doing 1 and 3 together is cheaper than doing 3 later.
4. **Teach `tools/check_lane_parity.sh` a `Windows)` arm** (lines 39-53) selecting
   `HL_CI_SHARDED_WINDOWS` / `HL_CI_DIRECT_WINDOWS` / `HL_CI_REGISTRY_WINDOWS`.
5. **Only then**, in one change: add `Windows-x86_64` to `HL_CI_HOSTS`, add the three lane lists (at minimum
   `HL_CI_DIRECT_WINDOWS = unit`), and add `.github/workflows/windows-x86_64.yml`. Steps 2 and 4 are what
   make this change guarded rather than merely declared.
6. Much later, and separately: `HL_CI_COMPAT_HOSTS`. `DOCS.md` 11 step 6 is explicit that this is also the
   point at which the host becomes a release gate (`publish.yml`, P1) — and `publish.yml`'s
   `needs: [linux, mac]` is asserted literally at `check_ci_workflows.sh:377-383`, so that is a deliberate,
   separate decision, not a consequence.

---

## 5. The `excluded-windows` disposition

### 5.1 The bug that exists today

`tools/matrix_runner.c:375-386`:

```c
static int engine_is_macho(const char *engine_path) {
    …
    /* ELF -> Linux engine; anything else (Mach-O 0xFEEDFACF / fat 0xCAFEBABE) -> treat as macOS. */
    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return 0;
    return 1;
}
```

The default is `return 1`. A PE binary begins `4D 5A` (`MZ`), so **a Windows engine is classified as macOS
and silently inherits every `excluded-macos` case** — 58 rows across the 16 top-level manifests, 60 counting
the nested `core/`, `isa/`, `abi/corpus` and `soak` manifests (count them from field 12; do not quote this
number). Those are Darwin-specific gaps: no OFD locks, no `F_SETPIPE_SZ`, no child-subreaper, no netns
bridging. Skipping them on Windows would hide 60 cases behind a reason that does not apply, on the first day
the lane exists. Note also that the sniff reads `argv[2]` — the *aarch64* engine — so both engines are
assumed to be the same format.

The same logic is duplicated at `tools/linux_matrix.c:352-356` and must move with it.

### 5.2 Proposed detection

Replace the boolean with a tri-state, and make the unknown case **fatal rather than defaulted**:

- `7F 45 4C 46` → ELF (Linux engine)
- `CF FA ED FE` / `CE FA ED FE` / `CA FE BA BE` (and byte-swapped) → Mach-O (macOS engine)
- `4D 5A` → PE (Windows engine)
- anything else → refuse to run, naming the path and the four bytes.

`load_manifest()` (`matrix_runner.c:388-389`) takes `int host_macho`; it becomes an enum. Then
`excluded-macos` drops only for Mach-O, `excluded-windows` drops only for PE, and every other `excluded-*`
prefix drops everywhere, as now.

The defaulting change is the important half. The current `return 1` is precisely the shape of failure this
project keeps writing down as unacceptable — a silent misclassification that presents as a green lane.

### 5.3 A format decision Windows forces and macOS did not

Column 12 holds **one** token (`matrix_runner.c:432-441` does `strcmp` against a single value, and
`strncmp(fields[11], "excluded-", 9)` for the rest). With two non-Linux engines there will be cases that must
drop on *both* — and there is no way to spell that. `excluded-known-bug` is not the answer: it drops on the
Linux engine too, which loses coverage the macOS mechanism was built specifically to preserve
(`src/host/macos/README.md`, "no Linux coverage is lost").

Recommendation: allow a comma-separated set in column 12 (`excluded-macos,excluded-windows`), parsed in both
runners, with a per-engine membership test replacing the two `strcmp`s. This is a small parser change and a
manifest-format widening; it should land *with* the PE detection, before the first `excluded-windows` row
exists, so no manifest is ever written against the narrower format and then rewritten.

### 5.4 Policy: what may be excluded

The requirement is that all tests pass. An exclusion is therefore an assertion about the *host kernel*, not a
place to park work. Five rules, each of which should be checkable by a reviewer without running anything:

1. **An `excluded-windows` row names a Win32/NT capability the case asserts and that the host cannot provide,
   and cites it.** Field 13 carries the reason, exactly as the macOS rows do. "Not implemented yet" is never a
   reason; that is a milestone, and it belongs in this document's staging table, not in a manifest.

2. **The bar is higher on Windows than it was on macOS, not lower.** The `excluded-macos` set is what it is
   because the macOS backend *passes some POSIX primitives through* to a BSD kernel whose semantics differ —
   pipes with no `F_SETPIPE_SZ`, `MSG_PEEK|MSG_TRUNC` reporting the copied length, no OFD locks. On Windows
   there is almost nothing to pass through: `src/host/windows/README.md` already states that "guest fork,
   epoll, and signal semantics remain owned by the Linux ABI rather than a host passthrough". Where the
   engine emulates, a divergence is an engine defect. The set of things that are genuinely NT-shaped is small
   — and it is not obviously a superset of the Darwin set; NTFS, for instance, records a creation time, so
   `statx` btime may well *pass* where ext4 needs a workaround.

3. **Timing is never a reason.** A case that is slow because the Windows backend interprets is handled by
   `HL_MATRIX_TIMEOUT_SCALE` / `HL_MATRIX_CASE_TIMEOUT_MS`, which already exist for exactly this
   (`Phase3Compat.cmake:418-431`, `docs/ci-green.md`, "Why the timeout is scaled rather than raised for
   everyone"). `Phase3Compat.cmake:418` keys the default on `HL_HOST_ARCH`; if Windows-x86_64 needs a
   different factor from Linux-x86_64 the key becomes the (OS, CPU) pair, not a new exclusion. Note that
   `excluded-macos` does contain one timing row (`reallocchurn`, "soak timeout") — that is a precedent to
   *avoid* copying, not to follow.

4. **Environment is never a reason.** The two preconditions `docs/ci-green.md` documents — the scheduling
   nice level and the filesystem under the guest's `/tmp` — are properties of the invoking environment.
   `HL_MATRIX_SCRATCH_DIR` is set per suite in the workflow (`linux.yml`, and `Phase3Compat.cmake:455-460`
   explains why CMake deliberately does not set it). Whatever the Windows analogue of "tmpfs-backed scratch"
   turns out to be, it is a workflow line, not a manifest edit.

5. **The count is gated.** `load_manifest` already accumulates an `excluded` count and the runner prints it.
   Pin the expected `excluded-windows` count per suite so that adding one is a visible diff that a reviewer
   has to approve — the same discipline the archive-stamp gate applies to the crate archives. Without this,
   an exclusion is the cheapest way to turn a red lane green, and cheap is exactly what it must not be.

A useful cross-check exists and should be mandatory before granting an exclusion: `docs/ci-green.md`
("Host-environment preconditions") notes that on an x86-64 host you can run the case's own x86-64 fixture
binary natively and diff it against the golden. A Windows runner cannot do that — which is precisely why the
Linux corpus job of §6 should be asked to produce the native-oracle verdict alongside the binaries, so a
Windows failure can be attributed without a second machine.

---

## 6. Guest fixtures on a Windows runner

### 6.1 What a compat lane actually needs at execution time

From `Phase3Compat.cmake:493-499`, one suite needs exactly:

- `build/compat/<suite>/aarch64/*` and `build/compat/<suite>/x86_64/*` — static (or `-static-pie`/`nonpie`/
  `freestanding`, per `GuestFixtures.cmake:125-182`) **Linux ELF** binaries for both guest ISAs;
- the two engines named by `HL_ENGINE_AARCH64` / `HL_ENGINE_X86_64`, built on the Windows host;
- `tests/compat/<suite>/manifest.tsv` and `expected/*.out`, which are already in the repo;
- a writable scratch base (`matrix_runner.c:815-824`), and `matrix_runner.c:1217` rejects a binary root under
  `/tmp/`.

Note `Phase3Compat.cmake:403-410`: `HL_MATRIX_ENGINE_DIR` is `build/production` on Darwin and
`build/linux-production` **on everything else**, keyed on `CMAKE_HOST_SYSTEM_NAME`. A Windows host takes the
else branch — which is arguably the right answer (the `linux` in `hl-engine-linux-<arch>` names the *guest*
ABI), but it is a fall-through, not a decision, and should be made explicit. Related: `CMakeLists.txt:45-49`
has the same shape and would file a Windows package artifact under `package/linux-x86_64` — the same class of
mistake `docs/amd64-host.md` §8.1 records for `package/linux-aarch64`.

The binaries themselves are host-CPU-neutral and host-OS-neutral: they are Linux ELF, cross-built, and
nothing about producing them depends on the machine that runs them.

### 6.2 The constraint

There is no nix on a Windows runner (`flake.nix:11` lists three systems, and `DOCS.md:481` says a Windows
entry follows `src/host/windows/` having code). There is also no Linux container: GitHub-hosted Windows
images run Windows containers only, and the Linux-container path needs WSL2, which those images do not
provide. So the corpus cannot be built on the Windows runner and cannot be built beside it in a container.

### 6.3 Options, and the recommendation

| Option | Verdict |
|---|---|
| **(A) CI artifact from a Linux job in the same run** | **Recommended.** A `guest-corpus` job on `ubuntu-24.04` with nix, building the per-suite fixture targets (`compat-<label>-fixtures`, `Phase3Compat.cmake:491`) and uploading them; the Windows job `needs:` it and downloads into its build tree. |
| (B) Committed binaries | Rejected. Order of 1400 cases × 2 ISAs of static-glibc binaries; the repo already refuses a third 24 MB crate archive against a 10 MB budget (`DOCS.md` 7.7). And they would need regenerating on every fixture edit, with the same staleness failure mode 0.1.17/0.1.18/0.1.26 shipped. |
| (C) A Windows-hosted Linux cross toolchain (`zig cc -target …-linux-gnu -static`, a Windows-hosted `*-linux-musl` GCC) | Not the gate. The goldens were captured against the corpus's exact static-glibc toolchain, and `compat-libc` exists specifically to assert glibc behaviour; a different libc changes observable output. Keep as an unsupported local-developer convenience if it works at all — unverified. |
| (D) Self-hosted Windows runner with WSL2 | The escape hatch if (A)'s artifact size proves fatal. Not for hosted runners. |

### 6.4 Test-execution consequences of (A)

These are the parts that are the test side's problem, not the build system's:

1. **The configure must accept a supplied corpus.** Today `HL_HAVE_GUEST_CC` is false without the cross
   compilers, and `CMakeLists.txt:218` then skips `Phase3Compat.cmake` entirely — so the compat tests are not
   registered, and neither is `gate.ci-lane-parity`. The requirement on the build side is a mode in which the
   corpus is *supplied* rather than *compiled*, and the compat labels and the parity gate register anyway.
   Without that, artifact download changes nothing.

2. **`compat.filesystem-fixtures-present` will fail for the wrong reason.** `Phase3Compat.cmake:568-573`
   runs `cmake/AssertExecutables.cmake`, which does `execute_process(COMMAND test -x "${f}")` on four
   fixtures. On Windows there is no `test` on `PATH` outside a shell, no execute bit, and the artifact
   round-trip does not preserve modes anyway. The check must become "exists, non-empty, and begins
   `7F 45 4C 46`" on non-Unix hosts — which is a *stronger* check, since it is what the engine actually
   requires.

3. **`exec_symlink_entry` is a symlink.** `Phase3Compat.cmake:162-171` creates it with
   `cmake -E create_symlink`. Symlink creation on Windows needs Developer Mode or elevation, and a zip
   artifact will not carry it. The case exists to prove the engine follows a symlinked entry program exactly
   as `execve` does; on Windows that has to be staged some other way, or `compat-process` carries a
   documented, narrow `excluded-windows` for it. This is a candidate that satisfies rule 5.1 honestly: it is
   a host-filesystem capability, not an engine gap.

4. **Shard the artifact.** Downloading the whole corpus into every compat shard pays the transfer N times.
   Producing one artifact per compat bucket, named the same way the matrix names its buckets, keeps each
   Windows shard's download proportional to its work. Sizes are unmeasured — see §8.

5. **Provenance.** The corpus must come from the same commit as the engine. Upload/download within one
   workflow run guarantees that; a cross-run artifact download does not, and would reintroduce the stale-
   artifact class of failure.

---

## 7. `.github/workflows/windows-x86_64.yml`

Modelled on `linux-x86_64.yml`, which is the closest existing file: one host token, one workflow, a lane set
that starts small and grows, and a comment block that says what is absent and why.

### 7.1 Shape

```yaml
name: Windows x86-64

# The x86_64 Windows HOST lane. One file per host token (HL_CI_HOSTS), as I13/I14
# require. Scope: configure and build with mingw-w64 clang, run `unit`, install and
# assert the SDK artifacts. NO guest lane: no compat, production, checkpoint, perf or
# lifecycle shard, because guests do not execute under a PE engine yet. Do not add a
# compat shard here before Windows-x86_64 is in HL_CI_COMPAT_HOSTS -- the generalised
# I20 turns that into a hard failure, which is the point.

on:
  pull_request: {}
  push:
    branches: [main]
  workflow_call: {}

permissions:
  contents: read

concurrency:
  group: windows-x86-64-${{ github.workflow }}-${{ github.ref == 'refs/heads/main' && github.sha || github.ref }}
  cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}

defaults:
  run:
    shell: bash          # Git Bash, so tools/*.sh run unmodified

jobs:
  windows-x86-64:
    runs-on: windows-2025
    timeout-minutes: 60          # I4
    steps:
      - uses: actions/checkout@v5           # I9: v5, not v4
      - name: Runner environment
        timeout-minutes: 5
        run: tools/ci_env.sh windows-2025
      - name: Install the toolchain          # pinned llvm-mingw tarball + sha256 check
        timeout-minutes: 15
      - name: Configure and build (CMake)
        timeout-minutes: 30
      - name: Test C unit suite
        timeout-minutes: 20
        run: ctest --test-dir build-ci -L unit --no-tests=error --output-on-failure   # I15
      - name: Install and assert the SDK artifacts
        timeout-minutes: 10
```

### 7.2 Choices and their reasons

- **Runner image.** `windows-2025` (x86-64). `windows-11-arm` is the later Windows-aarch64 token's runner and
  is out of scope. Both hosted Windows images ship Git Bash and an MSYS2 tree; the preinstalled LLVM version
  should be checked rather than assumed (§8).
- **Toolchain: a pinned llvm-mingw release, verified by SHA-256, not `pacman -S`.** MSYS2's repository is
  rolling; a CI lane whose compiler changes underneath it produces failures that are not about the change
  under test. This mirrors the project's existing habit of pinning (`flake.lock`, the crate archive stamp).
- **`shell: bash` everywhere.** This is what lets `tools/check_ci_workflows.sh`, `tools/check_lane_parity.sh`
  and the `HL_BASH_EXECUTABLE`-driven CTest cases run without a rewrite. Set `MSYS2_ARG_CONV_EXCL='*'` on
  steps that pass POSIX-looking arguments to native `.exe`s, or MSYS will path-translate them.
- **No nix, no `.#checks.*` steps.** `linux-x86_64.yml`'s first two steps have no Windows analogue and should
  not be faked.
- **`tools/ci_run.sh` is optional here and probably wrong at first.** It works under Git Bash, but its bound
  loop `kill -TERM`s a *bash job*; a hung native Win32 grandchild is not in a POSIX process group and will
  survive. Until that is addressed (a `taskkill /T /F /PID` arm, or a Win32 Job Object in the runner itself),
  a hung step is bounded only by `timeout-minutes`, and the step will be killed by the runner with **no
  `::error` annotation** — which is exactly the failure mode `ci_run.sh` exists to prevent. Either fix it or
  accept, knowingly, that the Windows lane's first failures will be less diagnosable than the others'.
- **`tools/ci_env.sh` needs a Windows arm.** It reads `/proc/meminfo`, falls back to `sysctl` (also absent),
  and reads `/proc/sys/*` and `/sys/fs/cgroup/*`. On Windows it degrades to `memory: ? bytes` and prints
  nothing useful — and this is the file whose whole purpose is making a runner-only difference observable.
  `tools/` is shared; coordinate before editing.
- **Invariants the file must satisfy on day one**, because they iterate `*.yml`: I1 (no `continue-on-error`),
  I2 (no `--skip`), I3 (a parseable `jobs:` block), I4 (job `timeout-minutes`), I9 (action generations),
  I15 (`--no-tests=error` on every `ctest -L`), I16/I18 (if any retry loop or deadline arithmetic appears).
  I5 and I17 will **not** fire, per §4.1.

### 7.3 Phased lane additions

| Phase | Steps added | Lane |
|---|---|---|
| 1 | configure, build, `ctest -L unit`, install assertions | `unit` |
| 2 | `ctest -L package` | `package` |
| 3 | (nothing gated) a hand-run `production.smoke-<arch>`, reported in a doc, not in CI | — |
| 4 | a `guest-corpus` job on `ubuntu-24.04` + a `windows-compat` matrix job modelled on `mac.yml:234-380` — per-suite `ctest` calls, per-suite retry, a step deadline, `::error` on failure | the compat subset measured green on **both** guest ISAs |
| 5 | `checkpoint`, `nested-engine`, `perf-linux` (record-only) | as they go green |

The phase-4 job must land in the *same* change as `Windows-x86_64` in `HL_CI_COMPAT_HOSTS`, for the reason
`CiLanes.cmake:53-55` gives: declaring the token switches I20 off, and a workflow that shards nothing while
exempt from I20 has no structural guard at all.

---

## 8. Staging

`DOCS.md` 11 ("Add a host OS", "Add a host CPU" step 6) draws the line this plan is organised around: until
guests execute, **a host proves the build, not the product**. Milestones M0-M2 below are all on the "build"
side of that line, and none of them should be described as Windows support.

| Milestone | Green lanes | Gate | Prerequisites |
|---|---|---|---|
| **M0 — compiles** | none | none | mingw-w64 clang toolchain; `src/host/windows/` implements `hl_host_services`. No CiLanes edit, no workflow, nothing declared. |
| **M1 — `unit`** | `unit` | `Windows-x86_64` in `HL_CI_HOSTS`; `HL_CI_DIRECT_WINDOWS = unit`; `windows-x86_64.yml` | §2.4's three blockers; §4's steps 1, 2, 4, 5; `gate.ci-lane-parity` registered and green on the runner (§3.1). This is the first point at which anything is declared. |
| **M2 — `package`** | `unit`, `package` | `package` added to `HL_CI_DIRECT_WINDOWS` | `PackageTest.cmake`'s consumer link works with the mingw driver; `HL_PACKAGE_HOST` names a Windows provider; `.exe` suffixing in the install assertions. `gate.archive-closure` is a bonus, not required. |
| **M3 — first guest** | none new | none — this is a hand-run measurement, recorded in a doc | `matrix_runner.c` ported (§9 risk 1); `Phase3Gates.cmake:143` admits a Windows subset without tripping the `FATAL_ERROR` at line 943; the corpus pipeline of §6. The milestone is `production.smoke-<arch>` on both guest ISAs, exactly as `docs/amd64-host.md` §7 uses it for Linux-x86_64. |
| **M4 — compat, suite by suite** | the measured-green subset of the 24 | `Windows-x86_64` in `HL_CI_COMPAT_HOSTS` + `HL_CI_SHARDED_WINDOWS` + a sharded matrix job, all in one change | A full sweep of all 24 manifests on both guest ISAs first, as `docs/amd64-host.md` §3.10 did. A suite green on one guest ISA and red on the other is a **red lane**, because `Phase3Compat.cmake` gives each label one CTest case covering both. §4 step 3 (all-pairs I19) must land before this. |
| **M5 — the rest** | `checkpoint`, `checkpoint-io`, `lifecycle`, `nested-engine`, `dynamic-e2e`, `perf-linux` (record-only) | added to `HL_CI_DIRECT_WINDOWS` / `HL_CI_REGISTRY_WINDOWS` as each goes green | Per-lane. `perf-linux` needs the `/tmp` and `UNIX_COMMAND` fixes of §2.3 before it can even be measured. |
| **M6 — release gate** | — | `publish.yml` `needs:` grows a third host; P1 (`check_ci_workflows.sh:377-383`) updated | Only after M4. `DOCS.md` 11 step 6 makes this an explicit, separate decision. |

Lanes that should be registered as **registry-only on Windows before they are green**, so a label rename
cannot silently empty a future step: `production`, `production-config`, `lifecycle`, `checkpoint`,
`checkpoint-io`, `embedding`. That is what `HL_CI_REGISTRY_*` is for (`CiLanes.cmake:185-197`) — but note the
trap: a registry lane must still be **non-empty**, or `gate.ci-lane-parity` fails. So they can only be
registered once `Phase3Gates.cmake` registers them on Windows, i.e. at M3, not before.

Lanes that should be recorded as **permanently absent on Windows**, with the reason written at the
declaration site: `compat-native`, `perf-native`, `e2e-oracle` (native Linux oracle impossible),
`emulated-aarch64*`, `ckpt-cross` (qemu-user), `isa-fuzz` (no differential oracle; shebang execution),
`macos`, `e2e-mac`, `compat-direct`, `perf-macos`, `package-activation`, `package-embedded` (Darwin).

---

## 9. Risks

1. **`tools/matrix_runner.c` is a POSIX process supervisor, and it is the gate on 22 of the 24 blocked
   lanes.** `fork()` (line 848), `setpgid` (855, 866), `waitpid` (309, 549, 556, 927), `pipe()` (803),
   `poll()` (913), `mkdtemp` (825, 1014), and `/proc/<pid>/stat` for the CPU-progress walk (140) plus
   `/proc/self/fd` and `/proc/self/task` for the resource baseline (302). None of that has a Win32 analogue
   that is a rewrite of one function; it is CreateProcess + a Job Object + named pipes with overlapped I/O +
   `GetProcessTimes`. Worse, the stall detector's stated contract is that *"anything the host will not answer
   reports unknown, and unknown counts as progress"* (`docs/ci-green.md`) — so an unported detector does not
   fail loudly on Windows, it goes **inert**, restoring the five-hour-hang hole that section exists to
   describe. The same applies to `tools/linux_matrix.c` and the four other runners that read
   `HL_MATRIX_TIMEOUT_SCALE`.

2. **Corpus acquisition is a hard dependency with unmeasured cost.** Every compat lane, and therefore the
   whole of M4, sits behind an artifact of order 1400 cases × 2 ISAs of static binaries moving between two
   jobs on every run. Nobody has measured its size or transfer time (§10). If it is prohibitive, the fallback
   is a self-hosted Windows runner with WSL2 — a materially different CI story that should be discovered now,
   not at M4.

3. **The token can be declared while nothing checks it.** Two independent mechanisms produce this: I20's
   hardcoded `Linux-x86_64` literal (`check_ci_workflows.sh:346`), which means a Windows workflow is not
   checked for shards-nothing; and `gate.ci-lane-parity` being registered only under `HL_HAVE_GUEST_CC`
   (`LaneParity.cmake:10-12`), which means it does not exist on a Windows runner without a corpus. Either one
   alone turns "declared" into "unguarded". Both must be closed **before** step 5 of §4, which is why the
   order there is not cosmetic.

Secondary, but worth naming: `ci_run.sh`'s process-group kill not reaching native grandchildren (a hung
Windows step will be runner-killed with no `::error`); I5/I17 not applying to a nix-free workflow; and the
`excluded-windows` count being the cheapest available way to turn a red lane green if §5.4 rule 5 is not
implemented.

---

## 10. What was not verified

Stated explicitly, because a plan that does not distinguish read-from-the-tree from assumed is not usable.

- **Runner image contents.** Whether `windows-2025` preinstalls a clang capable of building this tree, and
  whether Git Bash on the runner ships `awk` (the guard scripts need it: `check_ci_workflows.sh:13`,
  `check_lane_parity.sh:25-29`). The local shell used while writing this is Cygwin bash 5.3 with GNU awk 5.4,
  which is **not** the runner's shell. Verify before relying on it; MSYS2 at `C:\msys64` is the fallback.
- **Corpus size and artifact transfer time.** No build tree was available. The "order 1400 cases × 2 ISAs"
  figure is a case count read from the manifests (1344 `active` + 60 `excluded-macos` + 14
  `excluded-known-bug` across all manifests), not a byte measurement.
- **Whether `PackageTest.cmake` passes under mingw.** Its link line (`112-115`) is GCC syntax that clang
  accepts, and the Darwin-only branches are correctly skipped, but the `.exe` suffix, `IS_SYMLINK` assertions
  (195-199) and the uninstall manifest walk were not exercised.
- **Which cases genuinely need `excluded-windows`.** Nothing here should be read as a pre-approval. The
  symlinked entry program (§6.4.3) is the one candidate with a clean argument; `memfd-seals` and the statx
  btime cases are *unknown* — NTFS records a creation time, which may make btime pass, and what "tmpfs" means
  under an engine-emulated memfd on Windows has not been reasoned through, let alone measured.
- **Whether `nested-engine` is reachable on Windows.** `tools/nested_engine_gate.sh` is plausible under Git
  Bash but tests the executable bit and shells `diff`; untried.
- **The `HL_MATRIX_TIMEOUT_SCALE` factor for a Windows host.** `Phase3Compat.cmake:418` keys the default on
  `HL_HOST_ARCH` alone. Whether Windows-x86_64 wants the same 30 as Linux-x86_64 is a measurement nobody has
  taken, and the file's own comment is clear that 30 was "a predicted middle, not a measurement".
- **Whether the `Phase3Compat.cmake:563-565` label overwrite matters.** Those two lines replace
  `compat;compat-core-workload-extended` with `compat-extended`, so `ctest -L compat-core-workload-extended`
  selects nothing. It is harmless for parity today (that label is not declared) and is noted only so it is not
  rediscovered as a Windows problem.
