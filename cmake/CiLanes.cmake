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
)

# Host tokens whose workflow shards the compat matrix. I19 requires cross-host
# parity only between these; I20 requires a host NOT named here to shard nothing.
#
# `Linux-x86_64` is absent because the matrix is not fully green there, not
# because the engine cannot execute guests -- it can. The unit of gating is the
# LABEL, and cmake/Phase3Compat.cmake gives each label ONE CTest case covering
# both guest ISAs, so only suites green on BOTH are gateable: today compat-ipc,
# compat-syscall-edges and compat-time. Adding them needs HL_CI_SHARDED_LINUX
# split per host CPU FIRST -- I19 refuses a second compat host on an OS whose
# sharded list cannot say which of the two it describes, and declaring the token
# before the split turns I20 off, leaving that workflow with no structural guard.
set(HL_CI_COMPAT_HOSTS
  Linux-aarch64
  Darwin-aarch64
)

# Lanes applying to only SOME of a host OS's CPUs, as `<host-token>:<lane>`, so
# gate.ci-lane-parity checks a CPU-guarded lane only where it can be non-empty.
set(HL_CI_HOST_CPU_ONLY
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
set(HL_CI_DIRECT_LINUX
  unit
  nested-engine
)
set(HL_CI_DIRECT_DARWIN
  unit
  package
  macos
  e2e-mac
)

# --- registry-only lanes ----------------------------------------------------
# No workflow runs these today. They are still guarded: a label rename here is
# exactly the kind of edit that would silently empty a future workflow step.
#
# isa-fuzz and perf-native are non-empty on both Linux host CPUs without carrying
# the same coverage, which gate.ci-lane-parity cannot see -- it counts tests, it
# does not compare them.
set(HL_CI_REGISTRY_LINUX
  checkpoint
  checkpoint-io
  compat-extended
  compat-native
  dynamic-e2e
  e2e-oracle
  embedding
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
