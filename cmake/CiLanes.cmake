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
#
# Adding a suite therefore takes one edit here plus one shard entry; forgetting
# either turns a build red rather than silently dropping coverage.

# --- sharded compat lanes: the workflow matrices must cover these exactly ----
set(HL_CI_SHARDED_LINUX
  compat-abi
  compat-completeness
  compat-filesystem
  compat-ipc
  compat-isolation
  compat-libc
  compat-memory
  compat-network
  compat-posix
  compat-process
  compat-procfs
  compat-signals
  compat-syscall
  compat-syscall-edges
  compat-threads
  compat-time
)

# The mac additionally owns the ISA, core and corpus suites: they need the
# macOS-built production engines, so a Linux runner has nothing to run them on.
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
