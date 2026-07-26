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
#   tools/check_ci_workflows.sh  -> I19. Every SHARDED lane must appear in BOTH
#       host lists below unless HL_CI_SHARDED_HOST_ONLY declares the asymmetry.
#
# Adding a suite therefore takes one edit here plus one shard entry; forgetting
# either turns a build red rather than silently dropping coverage.

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
# `<Linux|Darwin>:<lane>`, naming the host that DOES shard it. I19 also
# rejects a stale entry: the lane must be present in that host's list above
# and absent from the other's. Empty today -- every sharded lane runs on both.
set(HL_CI_SHARDED_HOST_ONLY
)

# --- lanes each host's main job runs directly (not sharded) -----------------
set(HL_CI_DIRECT_LINUX
  unit
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
