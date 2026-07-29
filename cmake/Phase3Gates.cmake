# ---------------------------------------------------------------------------
# Phase 3c — e2e / lifecycle / checkpoint gates, the linux-production matrix,
# and the perf + bench fixtures.
#
# Everything here is Linux-host; the mac gates (bridge-runner driven, four
# signed launches per process) live in Phase4Mac.cmake behind the host guard.
# ---------------------------------------------------------------------------

# --- host guard -------------------------------------------------------------
# The native-Linux lane below (section 2b onward) is already behind
# `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`, and the FATAL_ERROR sweep at the end
# of this file -- six test-name patterns, each of which MUST match at least one
# registered test or configure dies -- sits inside it. That guard is load-bearing
# and must not be relaxed to admit a host that does not register those six lanes.
# This early return is the safe way to add a host: sections 1-2 (the guest
# fixtures) are host-agnostic only in the sense that they drive Linux cross
# compilers, which a Windows host does not have, so there is nothing above the
# guard for it to do either.
#
# Purely additive: false on Linux and Darwin, where the file is unchanged.
#
# ---------------------------------------------------------------------------
# THE WINDOWS ARM. What is registered here and what is not:
#
#   REGISTERED (behind HL_WINDOWS_GUEST_LANES; see cmake/Phase3Compat.cmake for
#   why that switch exists and what has to be measured before it is turned on):
#   the production.full-x86_64.* sweeps. These are the right shape for a host
#   with ONE guest engine. linux-matrix selects on the manifest's own ISA column
#   and runs each case as a bare `<engine> <guest>` launch, so it needs neither
#   an aarch64 engine nor a supervisor nor a typed launch config -- and it
#   reports what it skipped, per suite, in its own summary line. The x86_64 legs
#   of all 22 compat suites plus soak are therefore reachable here even though
#   only one compat suite can register through matrix-runner.
#
#   NOT REGISTERED, each for its own reason and none of them "we did not get to
#   it":
#     * every e2e, lifecycle, dynamic-e2e, nested, pty, stdio, dir and rootfs
#       gate -- their runners (tools/e2e_runner.c, tools/bridge_runner.c,
#       tools/config_e2e_runner.c, ...) are POSIX process supervisors that have
#       not been ported, and cmake/Phase3Compat.cmake does not build them here.
#     * every checkpoint gate -- checkpoint-tree-runner needs Linux-only
#       primitives, and checkpoint/restore has no Windows arm at all.
#     * the perf and bench lanes -- perf-runner is POSIX, and a timing lane on a
#       host whose engine cannot yet start a guest would measure nothing.
#     * production.smoke-* -- tools/linux_production_smoke.c is POSIX.
#     * production.matrix -- its 24 by-name triples include e2e-built fixtures
#       that sections 1-2 below declare, and those sections are not reached on
#       this host.
#     * the whole aarch64 half of every sweep above. There is no aarch64 Windows
#       engine (cmake/Phase2Production.cmake declares one target), so a
#       production.full-aarch64.* lane would be a name with nothing behind it.
#
# Sections 1-2 (the e2e/perf/checkpoint guest fixtures) are still not reached:
# nothing registered above consumes them, and declaring ~600 more staging rules
# for a lane that does not exist is cost without a claim.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(NOT HL_WINDOWS_GUEST_LANES)
    message(STATUS
      "Windows host -- the production sweeps are wired but not registered "
      "(-DHL_WINDOWS_GUEST_LANES=ON); the e2e/checkpoint/perf gates need host "
      "runners that are still POSIX-only.")
    return()
  endif()
  # The same suite list the Linux lane sweeps, x86_64 only.
  set(_win_suites
    abi:tests/compat/abi  abi-corpus:tests/compat/abi/corpus  libc:tests/compat/libc
    completeness:tests/compat/completeness  posix:tests/compat/posix
    syscall:tests/compat/syscall  network:tests/compat/network
    procfs:tests/compat/procfs  memory:tests/compat/memory
    filesystem:tests/compat/filesystem  signals:tests/compat/signals
    process:tests/compat/process  time:tests/compat/time
    core/abi:tests/compat/core/abi  core/workload:tests/compat/core/workload
    core/syscall:tests/compat/core/syscall  core/regress:tests/compat/core/regress
    ipc:tests/compat/ipc  threads:tests/compat/threads
    isolation:tests/compat/isolation  syscall_edges:tests/compat/syscall_edges)
  foreach(_pair ${_win_suites})
    string(REPLACE ":" ";" _p ${_pair})
    list(GET _p 0 _dir)
    list(GET _p 1 _srcdir)
    string(REPLACE "/" "-" _label ${_dir})
    add_test(NAME production.full-x86_64.${_label}
      COMMAND $<TARGET_FILE:linux-matrix> --suite ${HL_ENGINE_X86_64}
              ${HL_COMPAT}/${_dir}/x86_64 ${CMAKE_SOURCE_DIR}/${_srcdir})
    set_tests_properties(production.full-x86_64.${_label} PROPERTIES
      LABELS "production;production-full;production-full-x86_64"
      RESOURCE_LOCK hl-guest TIMEOUT 3600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    hl_matrix_timeout_scale(production.full-x86_64.${_label})
  endforeach()
  add_test(NAME production.full-x86_64.isa
    COMMAND $<TARGET_FILE:linux-matrix> --suite ${HL_ENGINE_X86_64}
            ${HL_COMPAT}/isa/x86_64 ${CMAKE_SOURCE_DIR}/tests/compat/isa/x86_64)
  set_tests_properties(production.full-x86_64.isa PROPERTIES
    LABELS "production;production-full;production-full-x86_64"
    RESOURCE_LOCK hl-guest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  hl_matrix_timeout_scale(production.full-x86_64.isa)
  add_test(NAME production.full-x86_64.soak
    COMMAND $<TARGET_FILE:linux-matrix> --suite ${HL_ENGINE_X86_64}
            ${CMAKE_BINARY_DIR}/soak/x86_64 ${CMAKE_SOURCE_DIR}/tests/soak)
  set_tests_properties(production.full-x86_64.soak PROPERTIES
    LABELS "production;production-full;production-full-x86_64"
    RESOURCE_LOCK hl-guest RUN_SERIAL TRUE TIMEOUT 3600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  hl_matrix_timeout_scale(production.full-x86_64.soak)
  return()
endif()

set(HL_E2E  ${CMAKE_BINARY_DIR}/e2e)
set(HL_PERF ${CMAKE_BINARY_DIR}/perf)
set(_o2 -O2)

# ===========================================================================
# 1. e2e guest programs
# ===========================================================================
# Freestanding guests: no libc at all, entry forced to _start.
hl_guest_pair(${HL_E2E} guest-exit    ${HL_TESTS}/e2e/guest_exit.c  LINKAGE freestanding)
hl_guest_pair(${HL_E2E} guest-exit70  ${HL_TESTS}/e2e/guest_exit.c  LINKAGE freestanding
               FLAGS -DHL_GUEST_EXIT_STATUS=70)
hl_guest_pair(${HL_E2E} guest-exit139 ${HL_TESTS}/e2e/guest_exit.c  LINKAGE freestanding
               FLAGS -DHL_GUEST_EXIT_STATUS=139)
hl_guest_pair(${HL_E2E} guest-fault   ${HL_TESTS}/e2e/guest_fault.c LINKAGE freestanding)
hl_guest_pair(${HL_E2E} guest-spin    ${HL_TESTS}/e2e/guest_spin.c  LINKAGE freestanding FLAGS -O0)
hl_guest_binary(aarch64 ${HL_E2E}/guest-output-aarch64 ${HL_TESTS}/e2e/guest_output.c
                LINKAGE freestanding)

# Ordinary static guests.
hl_guest_pair(${HL_E2E} guest-domain    ${HL_TESTS}/e2e/guest_domain.c    FLAGS ${_o2})
hl_guest_pair(${HL_E2E} clock-injected  ${HL_TESTS}/e2e/clock_injected.c  FLAGS ${_o2})
hl_guest_pair(${HL_E2E} fd-binding      ${HL_TESTS}/e2e/fd_binding.c      FLAGS ${_o2})
hl_guest_pair(${HL_E2E} stdio-binding   ${HL_TESTS}/e2e/stdio_binding.c   FLAGS ${_o2})
hl_guest_pair(${HL_E2E} dir-binding     ${HL_TESTS}/e2e/dir_binding.c     FLAGS ${_o2})
hl_guest_binary(aarch64 ${HL_E2E}/guest-descendant-aarch64
                ${HL_TESTS}/e2e/guest_descendant.c FLAGS ${_o2})
hl_guest_binary(aarch64 ${HL_E2E}/guest-external-term-aarch64
                ${HL_TESTS}/e2e/guest_external_term.c FLAGS ${_o2})
hl_guest_binary(aarch64 ${HL_E2E}/pty-binding-aarch64
                ${HL_TESTS}/e2e/pty_binding.c FLAGS ${_o2})

# Dynamically linked guests, run inside a synthesised rootfs.
foreach(_arch aarch64 x86_64)
  hl_guest_binary(${_arch} ${HL_E2E}/dynamic-${_arch} ${HL_TESTS}/e2e/dynamic_guest.c
    LINKAGE raw-dyn FLAGS -O2 -fPIE -pie
      "-Wl,--dynamic-linker,${HL_GUEST_LOADER_${_arch}}" -Wl,-rpath,/lib)
endforeach()

# Checkpoint fixtures: -O0 so the compiler does not hoist state the checkpoint
# is meant to observe. `threads` additionally needs -pthread.
set(HL_CHECKPOINT_CASES
  tree:checkpoint_tree            signal:checkpoint_signal_state
  pipe:checkpoint_pipe            deleted:checkpoint_deleted
  threads:checkpoint_threads      memfd:checkpoint_memfd
  cycle:checkpoint_cycle          handler:checkpoint_handler
  eventfd:checkpoint_eventfd      timerfd:checkpoint_timerfd
  inotify:checkpoint_inotify      epoll:checkpoint_epoll
  socketpair:checkpoint_socketpair socket-state:checkpoint_socket_state
  connected-socket:checkpoint_connected_socket
  connecting-refusal:checkpoint_connecting_refusal
  connecting-fallback:checkpoint_connecting_fallback
  missing-external:checkpoint_missing_external
  modified-external:checkpoint_modified_external
  io-recovery:checkpoint_io_recovery)
foreach(_pair ${HL_CHECKPOINT_CASES})
  string(REPLACE ":" ";" _p ${_pair})
  list(GET _p 0 _name)
  list(GET _p 1 _src)
  set(_extra "")
  if(_name STREQUAL threads)
    set(_extra -pthread)
  endif()
  hl_guest_pair(${HL_E2E} checkpoint-${_name} ${HL_TESTS}/e2e/${_src}.c
    FLAGS -O0 ${_extra})
endforeach()

# The E2E_CASES fixtures are shared with the native-oracle smoke set.
set(HL_E2E_CASES atomics clockelapsed epoll epoll_dup_lifetime epoll_edge
  epoll_et epoll_fork_inherit epoll_highfd epoll_mod epoll_oneshot
  epoll_reblock_inf eventfd eventfd_nonblock eventfd_sema fd_reuse_guard
  forkwait futex futex_shared_key futex_xproc inotify inotify_moves mmapanon
  mmapshared procreap seccomp signalfd signalfd_multi signalfd_rt signals
  stat_layout statx_agree sysv_ipc timerfd timerfd_interval)
foreach(_c ${HL_E2E_CASES})
  foreach(_arch aarch64 x86_64)
    hl_guest_binary(${_arch} ${HL_E2E}/${_c}-${_arch}
      ${HL_TESTS}/compat/fixtures/${_c}.c FLAGS ${_o2} LIBS -pthread)
  endforeach()
endforeach()

# ===========================================================================
# 2. perf fixtures
# ===========================================================================
hl_guest_pair(${HL_PERF} syscall   ${HL_TESTS}/perf/syscall.c   LINKAGE static-pie FLAGS ${_o2})
hl_guest_pair(${HL_PERF} translate ${HL_TESTS}/perf/translate.c LINKAGE static-pie FLAGS -O2 -std=c11)
hl_guest_pair(${HL_PERF} resource  ${HL_TESTS}/perf/resource.c  LINKAGE static-pie
               FLAGS -O2 -std=gnu11 -pthread)

# combined-bench: ONE source for both arches. Its sqlite phase links the static
# libsqlite3 that *_LINUX_STATIC_CC carries, and that is NOT supplied for every
# arch by default -- flake.nix gates it on the ISA's testNeedsSqlite, because the
# x86_64 static sqlite comes from a musl stdenv and implies a from-source x86_64
# musl cross gcc that nothing except this benchmark wants. So the default here
# mirrors the Makefile's COMBINED_BENCH_SQLITE_<arch>: ON for aarch64 (whose
# sqlite the dbserver/sqlite compat workloads need anyway), OFF for x86_64.
# `nix develop .#bench` supplies the x86_64 flags; configure with
# -DHL_COMBINED_BENCH_SQLITE_x86_64=ON there to get the phase back.
foreach(_arch aarch64 x86_64)
  if(_arch STREQUAL "aarch64")
    set(_sq_default ON)
  else()
    set(_sq_default OFF)
  endif()
  option(HL_COMBINED_BENCH_SQLITE_${_arch} "link static sqlite into combined-bench (${_arch})" ${_sq_default})
  if(HL_COMBINED_BENCH_SQLITE_${_arch})
    set(_sq_cpp -DHL_BENCH_SQLITE)
    set(_sq_lib -lsqlite3 -lm)
  else()
    set(_sq_cpp "")
    set(_sq_lib "")
  endif()
  hl_guest_binary(${_arch} ${HL_PERF}/combined-bench-${_arch} ${HL_TESTS}/perf/combined_bench.c
    LINKAGE static-pie FLAGS -O2 -std=gnu11 ${_sq_cpp} LIBS ${_sq_lib})
endforeach()

# The six OS-operation micro-benchmarks are one source selected by -DHL_PERF_OP.
set(HL_PERF_OPS mmap:1 file:2 pipe:3 event:4 ipc-latency:5 ipc-throughput:6)
foreach(_pair ${HL_PERF_OPS})
  string(REPLACE ":" ";" _p ${_pair})
  list(GET _p 0 _op)
  list(GET _p 1 _id)
  hl_guest_pair(${HL_PERF} ${_op} ${HL_TESTS}/perf/ops.c LINKAGE static-pie
    FLAGS -O2 -std=gnu11 -DHL_PERF_OP=${_id})
endforeach()

hl_guest_finalize(guest-fixtures-gates)

# ---------------------------------------------------------------------------
# Everything from here on is the NATIVE LINUX lane: it consumes the
# linux-production engines and the prod_* objects that Phase2Production.cmake
# only defines on a Linux host, so it must be guarded or a Darwin configure
# fails on missing targets. Sections 1-2 above build guest fixtures and are
# host-agnostic. The macOS equivalents live in Phase4Mac.cmake.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")

# ===========================================================================
# 2b. native-oracle fixtures for the e2e-compat differential cases
# ===========================================================================
# The cross-built guest runs under the engine and its output is diffed against
# the SAME source built and run natively on the HOST (build/fixtures/<case>).
# Host-native Linux syscall surface, so Linux-only. Phase3Compat already builds
# the 13 `compat-native` smoke fixtures; E2E_CASES is a superset.
foreach(_c ${HL_E2E_CASES})
  if(NOT TARGET fixture-${_c})
    add_executable(fixture-${_c} ${HL_TESTS}/compat/fixtures/${_c}.c)
    target_compile_options(fixture-${_c} PRIVATE -O2 -g -std=gnu11 -Wall -Wextra)
    target_link_options(fixture-${_c} PRIVATE -pthread)
    set_target_properties(fixture-${_c} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures OUTPUT_NAME ${_c})
  endif()
endforeach()

# ===========================================================================
# 3. lifecycle runners (native Linux)
# ===========================================================================
# Same object shapes as the production engine, plus the runner TU. hl_object()
# comes from Phase2Production.cmake and applies the minimal production flag set.
foreach(_arch aarch64 x86_64)
  string(TOUPPER ${_arch} _ISA)
  hl_object(life_runner_${_arch} tools/lifecycle_e2e_runner.c
    FLAGS -D_GNU_SOURCE -DHL_TEST_HOST_LINUX=1 -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_${_ISA} -O2)
  hl_object(life_target_${_arch} src/core/target/${_arch}.c
    FLAGS -D_GNU_SOURCE -DHL_ENGINE_NO_MAIN=1 -O2 UNITY)
  add_executable(lifecycle-linux-${_arch}
    $<TARGET_OBJECTS:life_runner_${_arch}>
    $<TARGET_OBJECTS:life_target_${_arch}>
    $<TARGET_OBJECTS:prod_life_${_arch}>)
  target_link_libraries(lifecycle-linux-${_arch} PRIVATE
    hl-engine hl-translator hl-linux-abi hl-host-linux -pthread -lm -ldl -latomic)
  set_target_properties(lifecycle-linux-${_arch} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
  hl_codesign(lifecycle-linux-${_arch})
endforeach()

# test-linux-lifecycle, one CTest per assertion instead of one opaque target,
# so a failure names the exact (scenario, arch) cell.
function(hl_lifecycle_case name arch guest)
  cmake_parse_arguments(L "" "" "OPTS" ${ARGN})
  add_test(NAME lifecycle.${name}
    COMMAND $<TARGET_FILE:lifecycle-linux-${arch}> ${L_OPTS} ${HL_E2E}/${guest})
  set_tests_properties(lifecycle.${name} PROPERTIES
    LABELS "lifecycle" RESOURCE_LOCK hl-guest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

foreach(_arch aarch64 x86_64)
  # Bare run: the runner exits with the guest's status (42).
  add_test(NAME lifecycle.exit-${_arch}
    COMMAND ${CMAKE_COMMAND} -DEXPECT=42
      -DCMD=$<TARGET_FILE:lifecycle-linux-${_arch}>
      -DARG1=${HL_E2E}/guest-exit-${_arch}
      -P ${CMAKE_SOURCE_DIR}/cmake/ExpectExit.cmake)
  hl_lifecycle_case(exit139-${_arch} ${_arch} guest-exit139-${_arch} OPTS --expect-exit 139)
  hl_lifecycle_case(fault-${_arch}   ${_arch} guest-fault-${_arch}   OPTS --expect-signal 11)
  hl_lifecycle_case(clock-${_arch}   ${_arch} clock-injected-${_arch} OPTS --clock-spy)
  hl_lifecycle_case(force-${_arch}   ${_arch} guest-spin-${_arch}    OPTS --force-stop)
  set_tests_properties(lifecycle.exit-${_arch} PROPERTIES LABELS "lifecycle" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# ===========================================================================
# 4. linux-production smoke / matrix / config
# ===========================================================================
foreach(_arch aarch64 x86_64)
  add_test(NAME production.smoke-${_arch}
    COMMAND $<TARGET_FILE:linux-production-smoke>
            ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
            ${HL_COMPAT}/core/abi/${_arch}/hello)
  set_tests_properties(production.smoke-${_arch} PROPERTIES LABELS "production" RESOURCE_LOCK hl-guest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# test-linux-production-matrix: 24 (binary, expected, status) triples driven by
# tools/linux-matrix in one process.
set(_matrix_cases
  "core/abi/x86_64/files|tests/compat/core/abi/expected/files.out"
  "core/abi/x86_64/statfile|tests/compat/core/abi/expected/statfile.out"
  "core/abi/x86_64/mmapanon|tests/compat/core/abi/expected/mmapanon.out"
  "core/abi/x86_64/pipe|tests/compat/core/abi/expected/pipe.out"
  "posix/x86_64/pollpipe|tests/compat/posix/expected/pollpipe.out"
  "posix/x86_64/waitstatus|tests/compat/posix/expected/waitstatus.out"
  "signals/x86_64/signals_core|tests/compat/signals/expected/signals_core.out"
  "memory/x86_64/mprotect_enforce|tests/compat/memory/expected/mprotect_enforce.out"
  "posix/x86_64/mmapfile|tests/compat/posix/expected/mmapfile.out"
  "core/syscall/x86_64/epoll|tests/compat/core/syscall/expected/epoll.out"
  "core/syscall/x86_64/epoll_edge|tests/compat/core/syscall/expected/epoll_edge.out"
  "core/syscall/x86_64/epoll_dup_lifetime|tests/compat/core/syscall/expected/epoll_dup_lifetime.out"
  "core/syscall/x86_64/epoll_fork_inherit|tests/compat/core/syscall/expected/epoll_fork_inherit.out"
  "syscall/x86_64/epoll_pwait|tests/compat/syscall/expected/epoll_pwait.out"
  "core/syscall/x86_64/eventfd|tests/compat/core/syscall/expected/eventfd.out"
  "core/syscall/x86_64/eventfd_sema|tests/compat/core/syscall/expected/eventfd_sema.out"
  "procfs/x86_64/peerfd|tests/compat/procfs/expected/shared/peerfd.out")
set(_matrix_args "")
foreach(_c ${_matrix_cases})
  string(REPLACE "|" ";" _p ${_c})
  list(GET _p 0 _bin)
  list(GET _p 1 _exp)
  list(APPEND _matrix_args ${HL_COMPAT}/${_bin} ${CMAKE_SOURCE_DIR}/${_exp} 0)
endforeach()
# The e2e-built cases in the same list live under build/e2e, not build/compat.
foreach(_c epoll_mod epoll_et epoll_reblock_inf eventfd_nonblock timerfd
           timerfd_interval epoll_oneshot)
  if(_c STREQUAL timerfd)
    set(_expdir tests/compat/core/syscall/expected)
  else()
    set(_expdir tests/compat/syscall/expected)
  endif()
  list(APPEND _matrix_args ${HL_E2E}/${_c}-x86_64 ${CMAKE_SOURCE_DIR}/${_expdir}/${_c}.out 0)
endforeach()
add_test(NAME production.matrix
  COMMAND $<TARGET_FILE:linux-matrix>
          ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64 ${_matrix_args})
set_tests_properties(production.matrix PROPERTIES LABELS "production" RESOURCE_LOCK hl-guest TIMEOUT 1800 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
hl_matrix_timeout_scale(production.matrix)

# test-linux-production-config
add_test(NAME production.config-env
  COMMAND $<TARGET_FILE:config-e2e-runner> env
          ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64
          ${HL_E2E}/guest-exit-x86_64 42 32)
add_test(NAME production.config-supervisor
  COMMAND $<TARGET_FILE:config-e2e-runner> $<TARGET_FILE:remote-supervisor>
          ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64
          ${HL_E2E}/guest-exit-x86_64 42 32)
add_test(NAME production.config-exit70
  COMMAND $<TARGET_FILE:config-e2e-runner> env
          ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64
          ${HL_E2E}/guest-exit70-x86_64 70)
set_tests_properties(production.config-env production.config-supervisor
  production.config-exit70 PROPERTIES LABELS "production;production-config" RESOURCE_LOCK hl-guest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ===========================================================================
# 4b. native-oracle differential e2e (Makefile run-e2e-compat-<case>)
# ===========================================================================
# The Makefile drives these through the build/production engines, i.e. the
# macOS-built binaries reached over the `mac` remote transport, and gates the
# whole family on HOST=linux because the ORACLE (build/fixtures/<case>) has to
# be a natively-runnable Linux binary.
#
# DELIBERATE DIFFERENCE, stated rather than hidden: CMake models no remote
# execution (see the header of Phase4Mac.cmake), so the engine here is the
# LOCAL linux-production engine of the matching guest ISA. The case set, the
# guest binaries, the oracle fixtures and the comparison are identical; only
# which build of the engine runs them differs. Every case the Makefile runs is
# run here (34 cases x 2 arches = 68).
foreach(_c ${HL_E2E_CASES})
  foreach(_arch aarch64 x86_64)
    add_test(NAME e2e-oracle.${_c}-${_arch}
      COMMAND $<TARGET_FILE:e2e-runner> env
              ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
              ${HL_E2E}/${_c}-${_arch} 0 ${CMAKE_BINARY_DIR}/fixtures/${_c})
    set_tests_properties(e2e-oracle.${_c}-${_arch} PROPERTIES
      LABELS "e2e-oracle" RESOURCE_LOCK "hl-guest;hl-scratch" TIMEOUT 900
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
endforeach()

add_test(NAME remote-supervisor COMMAND $<TARGET_FILE:tests-remote-supervisor>
         $<TARGET_FILE:remote-supervisor>)
set_tests_properties(remote-supervisor PROPERTIES LABELS "integration" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ===========================================================================
# 5. checkpoint gates
# ===========================================================================
# checkpoint-tree-runner takes (engine, guest [, scenario]). Each scenario is a
# separate CTest so a failure names itself. They all write checkpoint images
# into a shared scratch area, so they take one lock.
set(HL_CHECKPOINT_SCENARIOS
  "tree|"                 "signal|signal-state"     "pipe|pipe"
  "deleted|deleted"       "threads|threads"         "memfd|memfd"
  "cycle|cycle"           "handler|handler"
  "eventfd|eventfd"       "timerfd|timerfd"         "inotify|inotify"
  "epoll|epoll-checkpoint" "socketpair|socketpair"  "socket-state|socket-state"
  "connected-socket|connected-socket"
  "connecting-refusal|connecting-refusal")
foreach(_arch aarch64 x86_64)
  foreach(_pair ${HL_CHECKPOINT_SCENARIOS})
    string(REPLACE "|" ";" _p "${_pair}")
    list(GET _p 0 _fixture)
    set(_scenario "")
    list(LENGTH _p _n)
    if(_n GREATER 1)
      list(GET _p 1 _scenario)
    endif()
    add_test(NAME checkpoint.${_arch}.${_fixture}
      COMMAND $<TARGET_FILE:checkpoint-tree-runner>
              ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
              ${HL_E2E}/checkpoint-${_fixture}-${_arch} ${_scenario})
    set_tests_properties(checkpoint.${_arch}.${_fixture} PROPERTIES
      LABELS "checkpoint;checkpoint-${_arch}" RESOURCE_LOCK hl-checkpoint TIMEOUT 900 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()

  # Corruption + recovery scenarios reuse the tree fixture.
  foreach(_s corrupt-magic corrupt-truncated corrupt-content corrupt-missing corrupt-extra)
    add_test(NAME checkpoint.${_arch}.${_s}
      COMMAND $<TARGET_FILE:checkpoint-tree-runner>
              ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
              ${HL_E2E}/checkpoint-tree-${_arch} ${_s})
    set_tests_properties(checkpoint.${_arch}.${_s} PROPERTIES
      LABELS "checkpoint;checkpoint-${_arch}" RESOURCE_LOCK hl-checkpoint WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
  foreach(_s missing-external modified-external connecting-fallback)
    add_test(NAME checkpoint.${_arch}.${_s}
      COMMAND $<TARGET_FILE:checkpoint-tree-runner>
              ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
              ${HL_E2E}/checkpoint-${_s}-${_arch} ${_s})
    set_tests_properties(checkpoint.${_arch}.${_s} PROPERTIES
      LABELS "checkpoint;checkpoint-${_arch}" RESOURCE_LOCK hl-checkpoint WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()

  # The 17 IO-recovery scenarios all share one fixture binary.
  foreach(_s io-replace io-recreate io-directory io-duplicate io-device
             io-type-change io-permission io-missing-root io-append io-shortened
             io-repeat io-directory-change io-missing-child-strict io-missing-child-default io-fifo-refusal
             io-queued-device io-queued-missing)
    add_test(NAME checkpoint.${_arch}.${_s}
      COMMAND $<TARGET_FILE:checkpoint-tree-runner>
              ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
              ${HL_E2E}/checkpoint-io-recovery-${_arch} ${_s})
    set_tests_properties(checkpoint.${_arch}.${_s} PROPERTIES
      LABELS "checkpoint;checkpoint-io;checkpoint-${_arch}" RESOURCE_LOCK hl-checkpoint WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
endforeach()

# ===========================================================================
# 6. dual-backend + dynamic rootfs
# ===========================================================================
add_test(NAME dual-backend.link
  COMMAND ${HL_PACKAGE_ARCH_DIR}/dual-backend-link-test
          ${HL_E2E}/guest-exit-aarch64 ${HL_E2E}/guest-exit-x86_64
          ${HL_E2E}/guest-exit70-aarch64 ${HL_E2E}/guest-exit70-x86_64
          ${HL_E2E}/guest-spin-aarch64 ${HL_E2E}/guest-output-aarch64)
set_tests_properties(dual-backend.link PROPERTIES LABELS "embedding" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

foreach(_arch aarch64 x86_64)
  if(_arch STREQUAL aarch64)
    set(_loader ${HL_AARCH64_DYNAMIC_LOADER})
    set(_libc   ${HL_AARCH64_DYNAMIC_LIBC})
  else()
    set(_loader ${HL_X86_64_DYNAMIC_LOADER})
    set(_libc   ${HL_X86_64_DYNAMIC_LIBC})
  endif()
  add_test(NAME dynamic-e2e.${_arch}
    COMMAND $<TARGET_FILE:rootfs-e2e-runner> env
            ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
            ${CMAKE_BINARY_DIR}/rootfs/${_arch} ${HL_E2E}/dynamic-${_arch}
            ${_loader} ${_libc} ${HL_GUEST_LOADER_${_arch}}
            "dynamic-ok ctor=17 tls=23 file=rootfs-data path=1 arg=probe")
  set_tests_properties(dynamic-e2e.${_arch} PROPERTIES
    LABELS "dynamic-e2e" RESOURCE_LOCK hl-scratch WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# ===========================================================================
# 7. perf (opt-in: these are measurements, not pass/fail unit work)
# ===========================================================================
# Thresholds mirror the Makefile PERF_LIMIT_<case> pairs (max-cold-us,
# max-p99-us); perf-runner fails the case when either is exceeded.
set(PERF_LIMIT_startup         15000 10000)
set(PERF_LIMIT_compute         750000 650000)
set(PERF_LIMIT_syscall-startup 30000 25000)
set(PERF_LIMIT_syscall-1m      500000 400000)
set(PERF_LIMIT_fork-stress     9000000 8000000)
set(PERF_LIMIT_mmap            150000 120000)
set(PERF_LIMIT_file            75000 60000)
set(PERF_LIMIT_pipe            250000 200000)
set(PERF_LIMIT_event           250000 200000)
set(PERF_LIMIT_ipc-latency     150000 120000)
set(PERF_LIMIT_ipc-throughput  75000 60000)
set(PERF_LIMIT_translation     40000 30000)
set(PERF_LIMIT_warm-cache      100000 80000)
set(HL_PERF_WARMUPS 3     CACHE STRING "perf warmup iterations")
set(HL_PERF_SAMPLES 25    CACHE STRING "perf samples")
set(HL_PERF_HEAVY_SAMPLES 7 CACHE STRING "perf samples for heavy cases")
set(HL_PERF_OP_SAMPLES 7  CACHE STRING "perf samples for OS-op cases")

# --- whether the thresholds above are ENFORCED, as a function of the host CPU -
# Every PERF_LIMIT_ pair above describes a JIT host, which an x86_64 host is not; OFF records
# the measurement without a verdict. Record-only cases are RENAMED so a green line cannot read
# as a threshold met; --label is not.
#
# The reason is NOT that the thresholds are all unreachable here. Five of the
# thirteen x86_64 cases pass the JIT thresholds unchanged and four more are within
# 3x. What rules the five out is MARGIN against
# this host's measured load spread of ~1.9x (compute: 348s/run loaded vs 184s median):
# translation clears p99 by 5.6%, fork-stress by 9.5%, startup by 39%, ipc-throughput by 38% --
# enforcing them buys flaky red, not signal. warm-cache is the only one with real margin and it
# measures a tautology here: warm and cold agree to 0.2% because an interpreter emits no host
# code for the persistent cache to hold. So: record all thirteen, and revisit per case when
# Stage 2 moves the numbers rather than when someone rereads this comment.
if(HL_HOST_ARCH STREQUAL "x86_64")
  set(_hl_perf_enforce_default OFF)
else()
  set(_hl_perf_enforce_default ON)
endif()
option(HL_PERF_ENFORCE
  "enforce the tracked cold/p99 perf thresholds (OFF records the measurement without a verdict)"
  ${_hl_perf_enforce_default})

# --- per-(case, arch) CTest budgets where the shared 1800s is not one ---------
# perf-runner does 1 cold + warmups + samples RUNS, so the budget is runs x per-run cost, not a
# per-run number. compute is 11 x 184.0s = 2024s on this host and could never have finished
# inside its own TIMEOUT 1800 -- it would have failed as a timeout and read as a performance
# regression rather than as a budget nobody checked against an interpreter. Derived per leg as
# runs x measured median x 1.9 (this host's measured load spread) x 1.25 headroom:
#   x86_64  11 x 184.0s x 1.9 x 1.25 = 4807 -> 4800
#   aarch64 11 x 135.6s x 1.9 x 1.25 = 3542 -> 3600   (1492s unloaded: 17% margin, i.e. flaky)
# On a JIT host the case is ~1s and 1800s stays the tighter hang bound.
if(HL_HOST_ARCH STREQUAL "x86_64")
  set(PERF_TIMEOUT_compute_x86_64  4800)
  set(PERF_TIMEOUT_compute_aarch64 3600)
endif()

# hl_perf_linux(<case> <arch> <warmups> <samples> <payload> <expect-status>)
function(hl_perf_linux case arch warmups samples payload expect)
  list(GET PERF_LIMIT_${case} 0 _cold)
  list(GET PERF_LIMIT_${case} 1 _p99)
  set(_guest /tmp/hl-perf-linux-${case}-${arch})
  set(_name perf.linux-${case}-${arch})
  # A STRING, not a list: cmake/RunSequence.cmake splits the -DCMD1= line with
  # separate_arguments, and a CMake list would arrive there as semicolons.
  set(_limits "--max-cold-us ${_cold} --max-p99-us ${_p99}")
  set(_record "")
  if(NOT HL_PERF_ENFORCE)
    set(_name ${_name}.record-only)
    set(_limits "")
    # LAST, so the enforced path's command list is unchanged.
    set(_record "-DCMD2=${CMAKE_COMMAND} -E echo RECORD-ONLY ${_name}: measured but NOT gated. Its tracked thresholds (cold ${_cold}us p99 ${_p99}us) describe a JIT host and were not applied here.")
  endif()
  add_test(NAME ${_name}
    COMMAND ${CMAKE_COMMAND}
      "-DCMD0=${CMAKE_COMMAND} -E copy_if_different ${payload} ${_guest}"
      "-DCMD1=$<TARGET_FILE:perf-runner> --label linux-${case}-${arch} --warmups ${warmups} --samples ${samples} --expect ${expect} ${_limits} -- ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${arch} ${_guest}"
      ${_record}
      -P ${CMAKE_SOURCE_DIR}/cmake/RunSequence.cmake)
  set(_budget 1800)
  if(DEFINED PERF_TIMEOUT_${case}_${arch})
    set(_budget ${PERF_TIMEOUT_${case}_${arch}})
  endif()
  # Timing measurements are meaningless when other tests contend for the CPU.
  set_tests_properties(${_name} PROPERTIES
    LABELS "perf;perf-linux" RUN_SERIAL TRUE TIMEOUT ${_budget} WORKING_DIRECTORY /tmp)
endfunction()

foreach(_arch aarch64 x86_64)
  hl_perf_linux(startup ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
                ${HL_E2E}/guest-exit-${_arch} 42)
  hl_perf_linux(compute ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
                ${HL_COMPAT}/core/workload/${_arch}/busyloop 0)
  hl_perf_linux(syscall-startup ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
                ${HL_COMPAT}/syscall/${_arch}/gettid 0)
  hl_perf_linux(syscall-1m ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
                ${HL_PERF}/syscall-${_arch} 0)
  hl_perf_linux(fork-stress ${_arch} 1 ${HL_PERF_HEAVY_SAMPLES}
                ${HL_COMPAT}/process/${_arch}/forkstorm 0)
  hl_perf_linux(translation ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
                ${HL_PERF}/translate-${_arch} 0)
  foreach(_op mmap file pipe event ipc-latency ipc-throughput)
    hl_perf_linux(${_op} ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_OP_SAMPLES}
                  ${HL_PERF}/${_op}-${_arch} 0)
  endforeach()
  # warm-cache (Makefile HL_PERF_CACHE_LINUX): the translation payload run
  # through config-e2e-runner with a PERSISTENT code-cache directory, so the
  # measurement is the warm path. The cache dir must be empty at the start of
  # the case, exactly like the Makefile's `$(RM) -r` prologue.
  list(GET PERF_LIMIT_warm-cache 0 _wc_cold)
  list(GET PERF_LIMIT_warm-cache 1 _wc_p99)
  # Same record-only decision as hl_perf_linux, which this case does not use.
  set(_wc_name perf.linux-warm-cache-${_arch})
  set(_wc_limits "--max-cold-us ${_wc_cold} --max-p99-us ${_wc_p99}")
  set(_wc_record "")
  if(NOT HL_PERF_ENFORCE)
    set(_wc_name ${_wc_name}.record-only)
    set(_wc_limits "")
    set(_wc_record "-DCMD3=${CMAKE_COMMAND} -E echo RECORD-ONLY ${_wc_name}: measured but NOT gated. Its tracked thresholds (cold ${_wc_cold}us p99 ${_wc_p99}us) describe a JIT host and were not applied here.")
  endif()
  add_test(NAME ${_wc_name}
    COMMAND ${CMAKE_COMMAND}
      "-DCMD0=${CMAKE_COMMAND} -E copy_if_different ${HL_PERF}/translate-${_arch} /tmp/hl-perf-linux-warm-cache-${_arch}"
      "-DCMD1=${CMAKE_COMMAND} -E rm -rf /tmp/hl-perf-cache-warm-linux-${_arch}"
      "-DCMD2=$<TARGET_FILE:perf-runner> --label linux-warm-cache-${_arch} --warmups ${HL_PERF_WARMUPS} --samples ${HL_PERF_SAMPLES} ${_wc_limits} -- $<TARGET_FILE:config-e2e-runner> env ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch} /tmp/hl-perf-linux-warm-cache-${_arch} 0 1 /tmp/hl-perf-cache-warm-linux-${_arch}"
      ${_wc_record}
      -P ${CMAKE_SOURCE_DIR}/cmake/RunSequence.cmake)
  set_tests_properties(${_wc_name} PROPERTIES
    LABELS "perf;perf-linux" RUN_SERIAL TRUE TIMEOUT 1800
    WORKING_DIRECTORY /tmp)

  # NOT record-only anywhere: tests/perf/resource.c asserts leak bounds, not time.
  add_test(NAME perf.linux-resource-${_arch}
    COMMAND ${CMAKE_COMMAND}
      "-DCMD0=${CMAKE_COMMAND} -E copy_if_different ${HL_PERF}/resource-${_arch} /tmp/hl-perf-linux-resource-${_arch}"
      "-DCMD1=${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch} /tmp/hl-perf-linux-resource-${_arch}"
      -P ${CMAKE_SOURCE_DIR}/cmake/RunSequence.cmake)
  set_tests_properties(perf.linux-resource-${_arch} PROPERTIES
    LABELS "perf;perf-linux" RUN_SERIAL TRUE WORKING_DIRECTORY /tmp)
endforeach()

# perf-native-<host cpu>: the NATIVE baseline the perf-linux numbers are read
# against -- the same payloads run directly by the host, no thresholds. It needs
# the fixture's ISA to BE the host CPU, so the family follows HL_HOST_ARCH and is
# non-empty on both. CMAKE_HOST_SYSTEM_* is checked too, because HL_HOST_ARCH
# names the TARGET host, which a cross configure can set to an unrunnable CPU.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
   ((HL_HOST_ARCH STREQUAL "aarch64" AND
     CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$") OR
    (HL_HOST_ARCH STREQUAL "x86_64" AND
     CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")))
  set(_native ${HL_HOST_ARCH})
  function(hl_perf_native case warmups samples payload expect)
    add_test(NAME perf.native-${case}-${_native}
      COMMAND $<TARGET_FILE:perf-runner> --label native-${case}-${_native}
              --warmups ${warmups} --samples ${samples} --expect ${expect} -- ${payload})
    set_tests_properties(perf.native-${case}-${_native} PROPERTIES
      LABELS "perf;perf-native" RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endfunction()
  hl_perf_native(startup ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES} ${HL_E2E}/guest-exit-${_native} 42)
  hl_perf_native(compute ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
                 ${HL_COMPAT}/core/workload/${_native}/busyloop 0)
  hl_perf_native(syscall-startup ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
                 ${HL_COMPAT}/syscall/${_native}/gettid 0)
  hl_perf_native(syscall-1m ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
                 ${HL_PERF}/syscall-${_native} 0)
  hl_perf_native(fork-stress 1 ${HL_PERF_HEAVY_SAMPLES}
                 ${HL_COMPAT}/process/${_native}/forkstorm 0)
  add_test(NAME perf.native-combined-options-${_native}
    COMMAND ${HL_PERF}/combined-bench-${_native}
            --divisor 100000 --phase syscall)
  set_tests_properties(perf.native-combined-options-${_native} PROPERTIES
    LABELS "perf;perf-native" RUN_SERIAL TRUE
    PASS_REGULAR_EXPRESSION "PHASE syscall us=[0-9]+ ok=[0-9]+"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  foreach(_op mmap file pipe event ipc-latency ipc-throughput)
    hl_perf_native(${_op} ${HL_PERF_WARMUPS} ${HL_PERF_OP_SAMPLES} ${HL_PERF}/${_op}-${_native} 0)
  endforeach()
endif()

# ===========================================================================
# 8. differential ISA fuzzing (Makefile isa-fuzz* targets)
# ===========================================================================
# Only the DETERMINISTIC regress seed sets become CTest cases: `isa-fuzz` /
# `isa-fuzz-arm` are open-ended campaigns over fresh random seeds, which is a
# search, not a test, and has no place in a pass/fail suite. Drive a campaign
# straight from the shell script (tests/fuzz/isa/<arch>/run.sh --seeds N).
# Both are opt-in via the `isa-fuzz` label; run.sh consumes the engine from
# build/linux-production, which is where this configure puts it.
set(HL_ISA_FUZZ_REGRESS_SEEDS
  "1 17 33 50 66 83 99 116 133 149 166 185 201 218 236 253 269 285 302 320 337 354 372 388 587"
  CACHE STRING "x86_64 differential ISA fuzz regression seeds")
set(HL_ISA_FUZZ_ARM_REGRESS_SEEDS
  "1 2 9 10 22 26 37 44 55 70 93 99 118 150 153 173 184 202 215 226 244 275 283 300"
  CACHE STRING "aarch64 differential ISA fuzz regression seeds")
set(HL_ISA_FUZZ_ARM_ARGS "+i8mm +bf16 +dczva +fpcr"
  CACHE STRING "aarch64 ISA fuzz generator feature args")
set(HL_ISA_FUZZ_SEEDS 200
  CACHE STRING "fresh-seed count for the opt-in x86_64 fuzz campaign")
set(HL_ISA_FUZZ_ARM_SEEDS 200
  CACHE STRING "fresh-seed count for the opt-in aarch64 fuzz campaign")

# Preserve the open-ended developer entry points as build targets.  They are
# intentionally not CTest cases: a fresh-seed search is useful work, but not a
# deterministic release verdict.
add_custom_target(isa-fuzz
  COMMAND ${HL_BASH_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tests/fuzz/isa/x86_64/run.sh
          --engine ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64
          --out ${CMAKE_BINARY_DIR}/isafuzz
          --seeds ${HL_ISA_FUZZ_SEEDS} --ignore-mxcsr
  DEPENDS hl-engine-linux-x86_64
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL)

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  add_custom_target(isa-fuzz-arm
    COMMAND ${HL_BASH_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/fuzz/isa/aarch64/run.sh
            --engine ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-aarch64
            --out ${CMAKE_BINARY_DIR}/isafuzz-arm
            --seeds ${HL_ISA_FUZZ_ARM_SEEDS}
            --gen-args "${HL_ISA_FUZZ_ARM_ARGS}"
    DEPENDS hl-engine-linux-aarch64
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
endif()

# MXCSR is excluded from the x86_64 comparison: the engine mirrors the host
# FPSR rather than modelling x86's sticky exception-status bits (Makefile 2345).
add_test(NAME isa-fuzz.x86_64-regress
  COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/isa/x86_64/run.sh
          --engine ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64
          --out ${CMAKE_BINARY_DIR}/isafuzz
          --list "${HL_ISA_FUZZ_REGRESS_SEEDS}" --ignore-mxcsr)

# The aarch64 oracle is the ARM64 HOST running the same binary, so run.sh
# refuses to run anywhere else; register it only where it can pass. So on x86_64
# `isa-fuzz` is one of these three cases alone: non-empty, not complete.
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  add_test(NAME isa-fuzz.aarch64-regress
    COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/isa/aarch64/run.sh
            --engine ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-aarch64
            --out ${CMAKE_BINARY_DIR}/isafuzz-arm
            --list "${HL_ISA_FUZZ_ARM_REGRESS_SEEDS}" --gen-args "${HL_ISA_FUZZ_ARM_ARGS}")
  # The Makefile runs the ARM lane TWICE: once default, once --pie into a
  # separate output dir. Both legs are the gate, so both are registered.
  add_test(NAME isa-fuzz.aarch64-regress-pie
    COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/isa/aarch64/run.sh
            --engine ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-aarch64
            --list "${HL_ISA_FUZZ_ARM_REGRESS_SEEDS}" --pie
            --gen-args "${HL_ISA_FUZZ_ARM_ARGS}"
            --out ${CMAKE_BINARY_DIR}/isafuzz-arm-pie)
endif()
get_property(_gate_tests DIRECTORY PROPERTY TESTS)
foreach(_t ${_gate_tests})
  if(_t MATCHES "^isa-fuzz\\.")
    set_tests_properties(${_t} PROPERTIES
      LABELS "isa-fuzz" RUN_SERIAL TRUE TIMEOUT 3600
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endforeach()

# The bench has no pass/fail semantics (it prints a comparison table), so it
# stays a build target rather than a CTest case.
add_custom_target(bench
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bench
  COMMAND $<TARGET_FILE:bench-runner> report --baseline hl-engine
  DEPENDS bench-runner
  COMMENT "fair combined self-timing bench (see tools/bench/README.md)")

# ===========================================================================
# 9. engine in engine
# ===========================================================================
# `<engine> <engine> [<engine>] <guest>`: the outer engine's guest is ANOTHER engine, which
# runs a guest of its own. This is the branch's acceptance criterion ("engine in engine, like
# docker in docker") and it was verified only by hand, i.e. not verified.
#
# It is the only test in the tree that runs one host backend as a GUEST of the other. On an
# x86_64 host the outer engine is the INTERPRETER and the cross-built inner engine is the
# ARM64 JIT, so the interpreter executes the JIT's own emitted-code arena, its W^X flips and
# its signal handling -- paths nothing else here reaches, because on this host they are never
# compiled in. It is also the only place the inner engine allocates guest descriptors and W^X
# mappings INSIDE another engine's guest: the constraint that forced
# hl_host_process_fd_private_floor() (src/host/private.c) to anchor the private band on the
# reserve rather than on HL_HOST_GUEST_DESCRIPTOR_MINIMUM, since the inner guest sees
# RLIMIT_NOFILE 20480. Verified: an inner engine built with that fix reverted exits 70
# (HL_STATUS_RESOURCE_LIMIT) in every cell, while the SAME binary run at one level passes.
#
# Case names are the guest-ISA chain, OUTERMOST FIRST; the last element is also the ISA of the
# `hello` fixture at the bottom. An engine binary's ELF arch is the host it was built for, so
# the outer engine's guest ISA must equal the inner engine's ELF arch -- which is what decides
# where each inner engine can come from:
#   ${HL_HOST_ARCH} inner  -> this build tree, always present  (2 cells, never skip)
#   foreign inner          -> HL_NESTED_FOREIGN_TREE           (3 cells, skip if absent)
if(HL_HOST_ARCH STREQUAL "aarch64")
  set(_nest_foreign x86_64)
else()
  set(_nest_foreign aarch64)
endif()
set(_nest_short arm)
if(_nest_foreign STREQUAL "x86_64")
  set(_nest_short x86)
endif()
set(HL_NESTED_FOREIGN_TREE ${CMAKE_SOURCE_DIR}/build-${_nest_short}-check CACHE PATH
    "cross build tree supplying the ${_nest_foreign}-host engines the nested gate runs as guests")

# NOT a declared dependency of the default build, deliberately. It is a second toolchain and a
# second configure; linux-x86_64.yml already budgets 25 min for exactly this tree, and
# GuestFixtures.cmake already refuses to make a cross compiler mandatory (a plain `nix build`
# sandbox exports none, and CTest does not build test dependencies, so "declared" would have to
# mean ALL -- taxing every build). So it stays ONE named command, which the skip path prints.
# Run the lane with `-V`: CTest prints nothing at all for a SKIPPED test, so without it the
# message naming the missing tree and that command never reaches anyone.
set(_nest_cross_dir ${HL_NESTED_FOREIGN_TREE}/linux-production)
set(_nest_fix "cmake --build ${CMAKE_BINARY_DIR} --target nested-foreign-engines")
add_custom_target(nested-foreign-engines
  COMMAND ${CMAKE_COMMAND} -G Ninja -S ${CMAKE_SOURCE_DIR} -B ${HL_NESTED_FOREIGN_TREE}
          -DHL_BUILD_TESTS=OFF
          -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/toolchains/${_nest_foreign}-linux.cmake
  COMMAND ${CMAKE_COMMAND} --build ${HL_NESTED_FOREIGN_TREE}
          --target hl-engine-linux-aarch64 hl-engine-linux-x86_64
  COMMENT "cross-building the ${_nest_foreign}-host engines the nested gate runs as guests"
  USES_TERMINAL)

# hl_nested_case(<name> CROSS <dir|-> BUDGET <s> CHAIN <link>...)
#   BUDGET is the JIT-host budget; section 10 multiplies it by HL_MATRIX_TIMEOUT_SCALE.
set(_nest_hello_out ${CMAKE_SOURCE_DIR}/tests/compat/core/abi/expected/hello.out)
function(hl_nested_case name)
  cmake_parse_arguments(N "" "CROSS;BUDGET" "CHAIN" ${ARGN})
  add_test(NAME nested.${name}
    COMMAND ${HL_BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/nested_engine_gate.sh
            ${N_CROSS} "${_nest_fix}" ${_nest_hello_out} 42 -- ${N_CHAIN})
  set_tests_properties(nested.${name} PROPERTIES
    LABELS "nested-engine" SKIP_RETURN_CODE 77 RESOURCE_LOCK hl-guest
    TIMEOUT ${N_BUDGET} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# Budgets, derived not picked. Measured on this x86_64 host at HL_MATRIX_TIMEOUT_SCALE=30:
# 4.1-5.2s for the two-engine cells, 107s for the three-engine one -- the cost is one whole
# ENGINE start-up interpreted by another engine, not the 3-byte guest at the bottom. The
# numbers below are the JIT-host budgets that section 10 scales, giving 900s (~170x measured)
# and 3600s (~34x measured) here.
set(_nest_budget2 30)
set(_nest_budget3 120)
set(_nest_native ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux)
set(_nest_cross  ${_nest_cross_dir}/hl-engine-linux)
set(_nest_hello  ${HL_COMPAT}/core/abi)

# The two host-ISA cells: inner engine comes from this tree, so they can never skip and they
# keep the lane non-empty on every host (gate.ci-lane-parity). On an aarch64 host the second
# of them IS the acceptance criterion, natively.
hl_nested_case(${HL_HOST_ARCH}-${HL_HOST_ARCH} CROSS - BUDGET ${_nest_budget2}
  CHAIN ${_nest_native}-${HL_HOST_ARCH} ${_nest_native}-${HL_HOST_ARCH}
        ${_nest_hello}/${HL_HOST_ARCH}/hello)
hl_nested_case(${HL_HOST_ARCH}-${_nest_foreign} CROSS - BUDGET ${_nest_budget2}
  CHAIN ${_nest_native}-${HL_HOST_ARCH} ${_nest_native}-${_nest_foreign}
        ${_nest_hello}/${_nest_foreign}/hello)

# The three foreign-ISA cells. On an x86_64 host `nested.aarch64-x86_64` is the hand-verified
# claim; the three-engine cell is the INVERSE gate, in the sense linux-x86_64.yml already uses
# -- an aarch64 host running engine-in-engine, reproduced here with no aarch64 hardware.
hl_nested_case(${_nest_foreign}-${_nest_foreign} CROSS ${_nest_cross_dir} BUDGET ${_nest_budget2}
  CHAIN ${_nest_native}-${_nest_foreign} ${_nest_cross}-${_nest_foreign}
        ${_nest_hello}/${_nest_foreign}/hello)
hl_nested_case(${_nest_foreign}-${HL_HOST_ARCH} CROSS ${_nest_cross_dir} BUDGET ${_nest_budget2}
  CHAIN ${_nest_native}-${_nest_foreign} ${_nest_cross}-${HL_HOST_ARCH}
        ${_nest_hello}/${HL_HOST_ARCH}/hello)
hl_nested_case(${_nest_foreign}-${_nest_foreign}-${HL_HOST_ARCH}
  CROSS ${_nest_cross_dir} BUDGET ${_nest_budget3}
  CHAIN ${_nest_native}-${_nest_foreign} ${_nest_cross}-${_nest_foreign}
        ${_nest_cross}-${HL_HOST_ARCH} ${_nest_hello}/${HL_HOST_ARCH}/hello)

if(HL_HOST_ARCH STREQUAL "x86_64")
  hl_nested_case(aarch64-x86_64-aarch64
    CROSS ${_nest_cross_dir} BUDGET ${_nest_budget3}
    CHAIN ${_nest_native}-aarch64 ${_nest_cross}-x86_64
          ${_nest_native}-aarch64 ${_nest_hello}/aarch64/hello)
else()
  hl_nested_case(aarch64-x86_64-aarch64
    CROSS ${_nest_cross_dir} BUDGET ${_nest_budget3}
    CHAIN ${_nest_native}-aarch64 ${_nest_native}-x86_64
          ${_nest_cross}-aarch64 ${_nest_hello}/aarch64/hello)
endif()

# ===========================================================================
# 9b. the compat corpus against an EMULATED aarch64 host (qemu-user)
# ===========================================================================
# The aarch64 host arm is compiled on this x86_64 host by the cross tree above and
# then never executed, so every JIT change ships unrun -- which is how a 107-line
# NaN-selection defect and four unimplemented-encoding defects reached HEAD. These
# cells run the SAME compat suites against the cross tree's engines under
# qemu-aarch64, on the host that builds them.
#
# EMULATION, NOT HARDWARE, and the name says so. The fidelity probes cover
# instruction semantics, the dual-alias W^X
# arena, signal delivery and uc_mcontext are vouched for; weak memory ordering and
# timing are NOT, because qemu-user inherits the x86 host's stronger model. A green
# cell here is evidence, not proof, and never a substitute for an aarch64 runner.
#
# x86_64 Linux hosts only: on an aarch64 host these suites run natively and better,
# so cmake/CiLanes.cmake reserves the lane with HL_CI_HOST_CPU_ONLY.
#
# TWO labels. `emulated-aarch64` is every cell and is registry-only; the GATED
# subset also carries `emulated-aarch64-gated`, which linux-x86_64.yml runs. A red
# cell stays in the registry so the label cannot rot, and out of CI so the gate
# cannot be weakened to admit it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND HL_HOST_ARCH STREQUAL "x86_64")
  set(_emu_cross ${HL_NESTED_FOREIGN_TREE}/linux-production)
  set(_emu_shim ${CMAKE_BINARY_DIR}/emulated-aarch64)
  # hl_emulated_case(<label> <bin-subdir> <suite-source-dir> [GATED])
  function(hl_emulated_case label bindir suitedir)
    set(_emu_labels "emulated-aarch64")
    if("GATED" IN_LIST ARGN)
      list(APPEND _emu_labels "emulated-aarch64-gated")
    endif()
    add_test(NAME emulated.${label}
      COMMAND ${HL_BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/emulated_aarch64_gate.sh
              ${_emu_cross} ${_emu_shim}/${label} $<TARGET_FILE:matrix-runner>
              ${bindir} ${CMAKE_SOURCE_DIR}/${suitedir})
    # Same RESOURCE_LOCK as the compat suites: these ARE those suites, and the
    # guests still bind ports and share /tmp scratch.
    set_tests_properties(emulated.${label} PROPERTIES
      LABELS "${_emu_labels}" SKIP_RETURN_CODE 77 RESOURCE_LOCK hl-guest
      TIMEOUT 3600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endfunction()

  # isa-x86-64 is the ISA regression corpus that exposed the NaN defect; abi is the
  # green control -- without one, a total breakage and a correctly-reported defect
  # look alike. Both measured green under qemu-user 10.2.2 and 11.0.2.
  hl_emulated_case(isa-x86-64   ${HL_COMPAT}/isa          tests/compat/isa/x86_64 GATED)
  hl_emulated_case(abi          ${HL_COMPAT}/abi          tests/compat/abi        GATED)

  # completeness is NOT gated: it is RED on real aarch64-host defects, which is the
  # lane working, not the lane failing. Measured 2026-07-27 against build-arm-check,
  # identically under both qemu versions, all three passing on the native x86_64
  # host: x87-stack-faults dies on `UNIMPL 1B opcode 0xdd` (FRSTOR, DD /4, not
  # lowered), x87-fprem-loop and x87-precision-rounding diverge on FPREM and
  # precision-control results. All three fixtures arrived with 5d28c1ca, which fixed
  # only the x86-host arm. Add GATED when they pass, and not before.
  # completeness/priority also fails on nice level alone.
  hl_emulated_case(completeness ${HL_COMPAT}/completeness tests/compat/completeness)

  # ---- 9c. CROSS-BACKEND checkpoint restore (label: ckpt-cross) -------------
  # struct cpu IS the checkpoint format (sizeof is written into the image and validated on
  # restore) and the stated reason is that the interpreter and the JIT must be able to read
  # each other's guest state. Nothing tested it. These cells do: the image is written by one
  # backend and read by the other, both directions, both guest ISAs.
  #
  # It shares 9b's emulation boundary, but the property under test does not depend on
  # emulation fidelity -- it is whether one backend's image is the other's image.
  #
  # ONE cell is deliberately absent: x86_64 / interp-to-jit / threads. It fails, and not on
  # cpu-state interchange -- the image pins the guest VAs the CAPTURING host's allocator
  # chose, and under qemu one of them (a MAP_SHARED guest region) names a range qemu-user
  # reserves and does not expose, so restore's MAP_FIXED takes it and the next access is
  # SEGV_ACCERR. Whether a real aarch64 host collides there is unknown, so the cell is
  # unproven in both directions rather than a defect to gate on. The underlying engine
  # gap is that restore MAP_FIXEDs saved guest VAs unconditionally.
  function(hl_checkpoint_cross isa direction fixture scenario)
    add_test(NAME checkpoint-cross.${isa}.${direction}-${fixture}
      COMMAND ${HL_BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/checkpoint_cross_gate.sh
              ${_emu_cross} ${CMAKE_BINARY_DIR}/checkpoint-cross
              $<TARGET_FILE:checkpoint-tree-runner> ${HL_E2E} ${isa} ${direction} ${fixture}
              ${scenario})
    set_tests_properties(checkpoint-cross.${isa}.${direction}-${fixture} PROPERTIES
      # LABEL is "ckpt-cross", not "checkpoint-cross": ctest -L takes a REGEX, so any label containing
      # "checkpoint" would drag these eleven skip-gated cells into `-L checkpoint` and `-L checkpoint-io`.
      LABELS "ckpt-cross" SKIP_RETURN_CODE 77 RESOURCE_LOCK hl-checkpoint
      TIMEOUT 1800 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endfunction()
  foreach(_dir interp-to-jit jit-to-interp)
    foreach(_isa aarch64 x86_64)
      hl_checkpoint_cross(${_isa} ${_dir} tree  "")
      hl_checkpoint_cross(${_isa} ${_dir} cycle cycle)
      if(NOT (_isa STREQUAL x86_64 AND _dir STREQUAL interp-to-jit))
        hl_checkpoint_cross(${_isa} ${_dir} threads threads)
      endif()
    endforeach()
  endforeach()
endif()

# ===========================================================================
# 10. the host-backend timeout scale, for the lanes not driven by matrix-runner
# ===========================================================================
# hl_matrix_timeout_scale() (cmake/Phase3Compat.cmake) covers the compat suites;
# the four runners registered ABOVE carry per-case budgets of their own (30s,
# 15s for checkpoint-tree-runner) and read the SAME
# variable. Selected from the directory's TESTS property, not by repeating the
# case lists, so a case added to a lane cannot become the one left unscaled. Each
# pattern is asserted non-empty so a rename cannot empty the sweep.
set(_hl_scaled_patterns
  "^e2e-oracle\\."
  "^production\\.config-"
  "^perf\\.linux-warm-cache-"
  "^dynamic-e2e\\."
  "^nested\\."
  "^checkpoint\\.")
get_property(_hl_lane_tests DIRECTORY PROPERTY TESTS)
foreach(_pattern IN LISTS _hl_scaled_patterns)
  set(_hl_matched "")
  foreach(_t IN LISTS _hl_lane_tests)
    if(_t MATCHES "${_pattern}")
      list(APPEND _hl_matched ${_t})
    endif()
  endforeach()
  if(NOT _hl_matched)
    message(FATAL_ERROR
      "HL_MATRIX_TIMEOUT_SCALE: no registered test matches '${_pattern}'. The lane was renamed or moved; "
      "fix the pattern rather than leaving its per-case timeout unscaled on an interpreting host.")
  endif()
  hl_matrix_timeout_scale(${_hl_matched})
endforeach()

endif() # native Linux lane
