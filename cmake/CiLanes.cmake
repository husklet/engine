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
# Every list below is keyed by host OS, and for the whole life of this file that
# was indistinguishable from "keyed by host", because `Linux` could only ever
# mean `Linux-aarch64`. It cannot any more: an x86_64 Linux host runs the same
# host OS and a different host CPU, and it does not run the same lanes.
#
# So name the pair. A host token is `<CMAKE_SYSTEM_NAME>-<HL_HOST_ARCH>` -- the
# two names CMakeLists.txt already derives, so nothing downstream re-derives a
# host string of its own. Adding Windows/amd64 later is one line here plus one
# workflow file, not a new `case` arm in two shell scripts.
#
#   token           | workflow                        | what it runs
#   ----------------+---------------------------------+---------------------
#   Linux-aarch64   | .github/workflows/linux.yml     | everything
#   Linux-x86_64    | .github/workflows/linux-x86_64.yml | host build + unit
#   Darwin-aarch64  | .github/workflows/mac.yml       | everything mac
set(HL_CI_HOSTS
  Linux-aarch64
  Linux-x86_64
  Darwin-aarch64
)

# Host tokens whose workflow shards the compat matrix, i.e. the hosts that run
# guests. This is a capability statement, not a scheduling one: `Linux-x86_64`
# is absent because the engine cannot yet EXECUTE guests on an x86_64 host --
# the host code generator / interpreter backends are unwritten -- so a compat
# shard there would fail every case rather than measure anything.
#
# Consumed by tools/check_ci_workflows.sh: I19 requires cross-host parity only
# between the hosts named here, and I20 requires a host NOT named here to shard
# nothing at all. Adding a compat shard to the x86_64 workflow without also
# adding its token here is therefore a red build, not a silently green matrix.
set(HL_CI_COMPAT_HOSTS
  Linux-aarch64
  Darwin-aarch64
)

# Lanes that apply to only SOME of a host OS's CPUs, as `<host-token>:<lane>`.
# The lists further down are per host OS; this is the escape hatch for a lane
# whose registration is guarded on the host CPU, so that
# gate.ci-lane-parity checks it only where it can be non-empty.
#
# Empty today, deliberately. The one lane that looked host-CPU-only,
# `perf-native`, is not: it measures the host running its OWN ISA's fixtures
# directly as the baseline `perf-linux` is read against, which is meaningful on
# every host CPU, so cmake/Phase3Gates.cmake registers a
# `perf.native-*-<host cpu>` family instead of an aarch64-only one.
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
# `<host-token>:<lane>` (see HL_CI_HOSTS), naming the host that DOES shard it.
# The token is the (OS, CPU) pair, not the bare OS: a bare `Linux` cannot say
# which Linux host it means now that there are two. I19 also rejects a stale
# entry: the lane must be present in that host OS's list above and absent from
# the other's. Empty today -- every sharded lane runs on both compat hosts.
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
#
# Two of these are NON-EMPTY on both Linux host CPUs but do not carry the same
# coverage on both, which gate.ci-lane-parity cannot see -- it counts tests, it
# does not compare them. Read a green run accordingly:
#
#   isa-fuzz    Three cases on an aarch64 host, ONE on x86_64. The aarch64
#               differential oracle IS the host CPU executing the same binary,
#               so tests/fuzz/isa/aarch64/run.sh refuses a non-ARM host and
#               cmake/Phase3Gates.cmake registers isa-fuzz.aarch64-regress{,-pie}
#               only there. On an x86_64 host the lane is
#               isa-fuzz.x86_64-regress alone: HALF the differential ISA fuzz
#               coverage is host-conditional, and green does not mean full.
#   perf-native The host's own ISA fixtures run natively, so which twelve
#               measurements the lane contains depends on the host CPU. It is a
#               baseline, not a pass/fail gate (no thresholds are applied).
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
