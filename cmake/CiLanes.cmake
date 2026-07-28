# Single source of truth for the CI test lanes.
#
# Consumed by two guards, both of which parse this file TEXTUALLY, so keep the
# format literal: `set(<NAME>` on its own line, one lane per line, `)` alone.
#
#   tools/check_lane_parity.sh   -> gate.ci-lane-parity. Every lane below must
#       select at least one test on the host it applies to. `ctest -L <miss>`
#       exits 0, so nothing else catches a renamed or deleted label.
#   tools/check_ci_workflows.sh  -> I13/I14. Every SHARDED lane must be named by
#       exactly one workflow shard, and no shard may name a lane absent here.
#   tools/check_ci_workflows.sh  -> I19. Every SHARDED lane must appear in the
#       list of every host in HL_CI_COMPAT_HOSTS unless HL_CI_SHARDED_HOST_ONLY
#       declares the asymmetry.
#   tools/check_ci_workflows.sh  -> I20. A host NOT in HL_CI_COMPAT_HOSTS must
#       shard nothing, so its workflow may name no lane at all.
#
# Adding a suite therefore takes one edit here plus one shard entry; forgetting
# either turns a build red rather than silently dropping coverage.

# --- the host axis: a CI host is an (OS, CPU) PAIR --------------------------
# The lists below are keyed by host OS, which stopped identifying a host once
# `Linux` could mean two host CPUs running different lanes. A token is
# `<CMAKE_SYSTEM_NAME>-<HL_HOST_ARCH>`, one workflow file each.
set(HL_CI_HOSTS
  Linux-aarch64
  Linux-x86_64
  Darwin-aarch64
  Windows-x86_64
)

# Windows-x86_64 IS now declared, and this records what made it declarable,
# because the previous state of this comment was a list of reasons not to.
#
# The blocker was never the wiring -- tools/check_lane_parity.sh already had a
# Windows arm selecting HL_CI_{SHARDED,DIRECT,REGISTRY}_WINDOWS,
# cmake/LaneParity.cmake already keyed gate.ci-lane-parity on this list rather
# than on guest cross compilers a Windows host can never have,
# tools/check_ci_workflows.sh's I20 already iterated HL_CI_HOSTS through a
# token -> workflow map, and its I21 already failed a token declared here whose
# three lane lists are empty. The blocker was that `ctest -L unit` was RED
# there: cmake/RustLint.cmake registers rust.fmt and rust.clippy with
# LABELS "unit;rust" wherever cargo is on PATH -- it is on a hosted Windows
# runner -- and rust.clippy failed because pkgs/rust/build.rs refused the host
# triple, a build-script panic rather than a lint verdict.
#
# That is fixed. build.rs now accepts x86_64-pc-windows-msvc and downgrades the
# missing prebuilt archive to a warning, so a check and a lint succeed while
# only a LINK against the crate still fails. Measured on a Windows host,
# `ctest -L unit` selects five cases and all five pass:
# unit.ci-workflow-invariants, unit.publish-gating, gate.windows-imports,
# rust.fmt and rust.clippy. Declaring the token adds gate.ci-lane-parity to that
# same label, which is what makes the declaration self-checking.
#
# The lists below are therefore `unit` and nothing else, and the two empties are
# deliberate rather than unfinished:
#   HL_CI_SHARDED_WINDOWS  -- a Windows host shards no compat lane. The guest
#       corpus is not consumed there (cmake/Phase3Compat.cmake and
#       cmake/Phase3Gates.cmake return at their own Windows guard) and there is
#       no Windows production engine to run a fixture against. Adding an entry
#       here without also adding the token to HL_CI_COMPAT_HOSTS is a violation
#       I20 catches; doing both needs a measured green matrix first.
#   HL_CI_REGISTRY_WINDOWS -- registry-only lanes reserve a LABEL against being
#       renamed. Every label worth reserving on this host is guest-backed and
#       does not register here, so reserving one would assert a selection that
#       cannot be non-empty and gate.ci-lane-parity would fail on it.
#
# `package` is NOT declared, and that is the honest part of this change:
# package.consumer-link registers on a Windows host and FAILS. It is labelled
# `package`, never `unit`, so it is outside the declared lane -- but do not
# promote it. The install leg passes now that hl-engine-runner links; the
# consumer leg does not, because tests/integration/package.c has no Windows arm
# and stops at `#error "the package integration test needs a supported host
# provider"`. That is a real gap, not a toolchain accident, and the lane stays
# undeclared until it closes.

# Host tokens whose workflow shards the compat matrix. I19 requires cross-host
# parity only between these; I20 requires a host NOT named here to shard nothing.
#
# `Linux-x86_64` is absent because the matrix is not fully green there, not
# because the engine cannot execute guests -- it can. The unit of gating is the
# LABEL, and cmake/Phase3Compat.cmake gives each label ONE CTest case covering
# both guest ISAs, so only suites green on BOTH are gateable.
#
# The gateable set is much larger than this comment used to claim (compat-ipc,
# compat-syscall-edges, compat-time). The second corpus sweep -- pinned binaries,
# 3013 (case, guest-ISA) runs, docs/amd64-host.md 3.10 -- scores 99.34%
# with 20 of the 24 suites fully green on BOTH guest ISAs; the residue is
# compat-completeness, compat-core-regress, compat-process and compat-procfs, and
# two of those four fail partly on the nice-level precondition that CI does not
# have. Re-measure before quoting; it is a snapshot, not a gate.
#
# It is still not a one-line edit, and the order matters:
#   1. Split HL_CI_SHARDED_LINUX per host CPU. I19 refuses a second compat host on
#      an OS whose sharded list cannot say which of the two it describes.
#   2. Rework the guards that key off OS. check_ci_workflows.sh's I19 compares one
#      Linux list against one Darwin list; with three compat hosts parity is an
#      all-pairs relation, and HL_CI_SHARDED_HOST_ONLY grows an entry per lane a
#      host cannot run. check_lane_parity.sh's `case "$os"` needs the same keying.
#   3. Only then declare the token, and in the SAME change give linux-x86_64.yml a
#      sharded matrix job: declaring it turns I20 off, and a workflow that shards
#      nothing while exempt from I20 has no structural guard at all.
#   4. Gate lane by lane against a fresh measurement. A suite green on one guest
#      ISA and red on the other is a red lane, not half a green one.
set(HL_CI_COMPAT_HOSTS
  Linux-aarch64
  Darwin-aarch64
)

# Lanes applying to only SOME of a host OS's CPUs, as `<host-token>:<lane>`, so
# gate.ci-lane-parity checks a CPU-guarded lane only where it can be non-empty.
#
# emulated-aarch64 runs the compat suites against the cross-built aarch64-host
# engines under qemu-user (cmake/Phase3Gates.cmake section 9b), and
# emulated-aarch64-gated is the green subset of it that linux-x86_64.yml runs.
# Both belong to the x86_64 host alone: that is where those engines are built and
# never run. On an aarch64 host the same suites run natively, so registering them
# there would mean emulating a host to test the host doing the emulating.
#
# ckpt-cross belongs to the same host for the same reason: it restores an image
# across the two host backends, and one of them is the cross-built aarch64 host
# that only this host builds without running.
set(HL_CI_HOST_CPU_ONLY
  Linux-x86_64:emulated-aarch64
  Linux-x86_64:emulated-aarch64-gated
  Linux-x86_64:ckpt-cross
)

# --- sharded compat lanes: the workflow matrices must cover these exactly ----
set(HL_CI_SHARDED_LINUX
  compat-abi
  compat-abi-corpus
  compat-completeness
  compat-core-abi
  compat-core-regress
  compat-core-syscall
  compat-core-workload
  compat-filesystem
  compat-ipc
  compat-isa-aarch64
  compat-isa-x86-64
  compat-isolation
  compat-libc
  compat-memory
  compat-network
  compat-posix
  compat-process
  compat-procfs
  compat-signals
  compat-soak
  compat-syscall
  compat-syscall-edges
  compat-threads
  compat-time
)

# Identical set on the mac. The ISA, core, corpus and soak suites were once
# claimed to need "the macOS-built production engines"; that was never true --
# cmake/Phase3Compat.cmake points the runner at build/linux-production on a
# Linux host and build/production on Darwin, and the guest corpus is
# cross-compiled either way. The claim just hid an 8-lane, ~270-case gap.
set(HL_CI_SHARDED_DARWIN
  compat-abi
  compat-abi-corpus
  compat-completeness
  compat-core-abi
  compat-core-regress
  compat-core-syscall
  compat-core-workload
  compat-filesystem
  compat-ipc
  compat-isa-aarch64
  compat-isa-x86-64
  compat-isolation
  compat-libc
  compat-memory
  compat-network
  compat-posix
  compat-process
  compat-procfs
  compat-signals
  compat-soak
  compat-syscall
  compat-syscall-edges
  compat-threads
  compat-time
)

# Empty, and it must stay empty while Windows-x86_64 is absent from
# HL_CI_COMPAT_HOSTS: I20 requires a non-compat host's workflow to name no lane,
# so an entry here with no matching shard in windows-x86_64.yml fails I20, and a
# shard added without the HL_CI_COMPAT_HOSTS entry fails it the other way. The
# block exists rather than being omitted because tools/check_lane_parity.sh's
# Windows arm reads it by name, and a missing block and an empty one are the
# same thing to that parser -- but not to a reader deciding whether the omission
# was a decision.
set(HL_CI_SHARDED_WINDOWS
)

# --- declared single-host sharded lanes -------------------------------------
# I19 requires every sharded lane to run on BOTH hosts, because that is the
# only property neither I13 nor I14 can see: each of those compares ONE host's
# declared list against ONE workflow, so a lane simply omitted from
# HL_CI_SHARDED_LINUX satisfied both while running nowhere on Linux.
#
# A lane that genuinely cannot run on one host is declared here as
# `<host-token>:<lane>` (see HL_CI_HOSTS), naming the host that DOES shard it --
# the (OS, CPU) pair, since a bare `Linux` cannot say which Linux host it means.
# I19 also rejects a stale entry. Empty today.
set(HL_CI_SHARDED_HOST_ONLY
)

# --- lanes each host's main job runs directly (not sharded) -----------------
#
# nested-engine is the engine-in-engine gate (cmake/Phase3Gates.cmake section 9).
# It is non-empty on both Linux host CPUs without a cross build tree -- the two
# host-ISA cells always run -- so it needs no HL_CI_HOST_CPU_ONLY entry. The three
# foreign-ISA cells SKIP (SKIP_RETURN_CODE 77) where that tree is absent, which on
# Linux-x86_64 it is not: the workflow builds build-arm-check one step earlier.
#
# Only linux-x86_64.yml runs it today. That is the host where all five cells run
# and where every cell has been executed by hand; the aarch64 host's own two cells
# have not, and adding an unrun gate to the lane that SHIPS is a decision for
# someone with the hardware, not an assumption. No invariant is involved -- I13/I14
# see sharded lanes only -- so this list, not a guard, is where it is recorded.
#
# emulated-aarch64-gated is the same shape: linux-x86_64.yml alone, because it is
# the only host that HAS a cross-built aarch64 host arm to emulate. It is EMULATION
# and CI says so -- docs/amd64-host.md is the category list, and weak memory
# ordering is not on the vouched-for side of it.
set(HL_CI_DIRECT_LINUX
  unit
  nested-engine
  emulated-aarch64-gated
)
set(HL_CI_DIRECT_DARWIN
  unit
  package
  macos
  e2e-mac
)

# The whole of this host's declared surface: one lane, measured green on a
# Windows host before it was written here.
#
# On this host `unit` is a SMALLER label than on Linux or Darwin -- no host unit
# binary registers yet (cmake/Phase3Units.cmake returns at its Windows guard,
# pending libhl-host-windows plus a _HL_WINDOWS_EXCLUDED_UNITS list), so what it
# selects is the shell-backed CI metadata gates, the PE import gate, the two
# cargo cases and gate.ci-lane-parity itself. That is not the same coverage the
# name carries elsewhere, and .github/workflows/windows-x86_64.yml says so in
# its scope block rather than letting a shared label imply parity.
#
# `package` is deliberately not here; see the note under HL_CI_HOSTS.
set(HL_CI_DIRECT_WINDOWS
  unit
)

# --- registry-only lanes ----------------------------------------------------
# No workflow runs these today. They are still guarded: a label rename here is
# exactly the kind of edit that would silently empty a future workflow step.
#
# isa-fuzz and perf-native are non-empty on both Linux host CPUs without carrying
# the same coverage, which gate.ci-lane-parity cannot see -- it counts tests, it
# does not compare them.
#
# emulated-aarch64 is the FULL set of emulated cells; its green subset moved to
# HL_CI_DIRECT_LINUX as emulated-aarch64-gated. What stays here is the ungated
# cell, emulated.completeness, red on genuine aarch64-host x87 defects nothing
# else on this host can see -- cmake/Phase3Gates.cmake section 9b names them. The
# label is declared so it cannot rot while that is true.
set(HL_CI_REGISTRY_LINUX
  checkpoint
  checkpoint-io
  ckpt-cross
  compat-extended
  compat-native
  dynamic-e2e
  e2e-oracle
  embedding
  emulated-aarch64
  integration
  isa-fuzz
  lifecycle
  package
  perf-linux
  perf-native
  production
  production-config
  production-full-aarch64
  production-full-x86_64
)
set(HL_CI_REGISTRY_DARWIN
  compat-direct
  compat-extended
  embedding
  package-activation
  package-embedded
  perf-macos
)

# Empty on purpose. A registry-only lane is a RESERVATION -- it says "this label
# must keep selecting something here even though no workflow runs it" -- and
# gate.ci-lane-parity enforces exactly that. Every label that would be worth
# reserving on this host (checkpoint, compat-*, production, perf-*, lifecycle)
# is guest-backed and registers no case here at all, so reserving one would make
# gate.ci-lane-parity fail rather than protect anything. Add an entry only once
# the suite behind it registers on a Windows host.
set(HL_CI_REGISTRY_WINDOWS
)
