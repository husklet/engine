# ---------------------------------------------------------------------------
# Phase 3c — e2e / lifecycle / checkpoint gates, the linux-production matrix,
# and the perf + bench fixtures.
#
# Everything here is Linux-host; the mac gates (bridge-runner driven, four
# signed launches per process) live in Phase4Mac.cmake behind the host guard.
# ---------------------------------------------------------------------------

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

# combined-bench: ONE source for both arches. The sqlite phase links the static
# libsqlite3 nix supplies for BOTH arches through *_LINUX_STATIC_CC (see
# linuxArmSqlite / linuxX86Sqlite in flake.nix), so it is on by default and can
# be switched off per arch, matching COMBINED_BENCH_SQLITE_<arch>.
foreach(_arch aarch64 x86_64)
  option(HL_COMBINED_BENCH_SQLITE_${_arch} "link static sqlite into combined-bench (${_arch})" ON)
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

# test-linux-production-{aarch64-,}full: whole-suite sweeps via linux-matrix.
set(_full_suites
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
foreach(_arch aarch64 x86_64)
  foreach(_pair ${_full_suites})
    string(REPLACE ":" ";" _p ${_pair})
    list(GET _p 0 _dir)
    list(GET _p 1 _srcdir)
    string(REPLACE "/" "-" _label ${_dir})
    set(_bindir ${HL_COMPAT}/${_dir}/${_arch})
    if(_dir STREQUAL "abi-corpus")
      set(_bindir ${HL_COMPAT}/abi-corpus/${_arch})
    endif()
    add_test(NAME production.full-${_arch}.${_label}
      COMMAND $<TARGET_FILE:linux-matrix> --suite
              ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
              ${_bindir} ${CMAKE_SOURCE_DIR}/${_srcdir})
    set_tests_properties(production.full-${_arch}.${_label} PROPERTIES
      LABELS "production;production-full;production-full-${_arch}"
      RESOURCE_LOCK hl-guest TIMEOUT 3600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
  add_test(NAME production.full-${_arch}.soak
    COMMAND $<TARGET_FILE:linux-matrix> --suite
            ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
            ${CMAKE_BINARY_DIR}/soak/${_arch} ${CMAKE_SOURCE_DIR}/tests/soak)
  set_tests_properties(production.full-${_arch}.soak PROPERTIES
    LABELS "production;production-full" RESOURCE_LOCK hl-guest RUN_SERIAL TRUE TIMEOUT 3600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
# The x86_64 lane also sweeps the committed ISA corpus.
add_test(NAME production.full-x86_64.isa
  COMMAND $<TARGET_FILE:linux-matrix> --suite
          ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-x86_64
          ${HL_COMPAT}/isa/x86_64 ${CMAKE_SOURCE_DIR}/tests/compat/isa/x86_64)
set_tests_properties(production.full-x86_64.isa PROPERTIES LABELS "production;production-full" RESOURCE_LOCK hl-guest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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

  # The 16 IO-recovery scenarios all share one fixture binary.
  foreach(_s io-replace io-recreate io-directory io-duplicate io-device
             io-type-change io-permission io-missing-root io-append io-shortened
             io-repeat io-directory-change io-missing-child-strict io-fifo-refusal
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
  COMMAND ${CMAKE_BINARY_DIR}/package/linux-aarch64/dual-backend-link-test
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

# hl_perf_linux(<case> <arch> <warmups> <samples> <payload> <expect-status>)
function(hl_perf_linux case arch warmups samples payload expect)
  list(GET PERF_LIMIT_${case} 0 _cold)
  list(GET PERF_LIMIT_${case} 1 _p99)
  add_test(NAME perf.linux-${case}-${arch}
    COMMAND $<TARGET_FILE:perf-runner> --label linux-${case}-${arch}
            --warmups ${warmups} --samples ${samples} --expect ${expect}
            --max-cold-us ${_cold} --max-p99-us ${_p99} --
            ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${arch} ${payload})
  # Timing measurements are meaningless when other tests contend for the CPU.
  set_tests_properties(perf.linux-${case}-${arch} PROPERTIES
    LABELS "perf;perf-linux" RUN_SERIAL TRUE TIMEOUT 1800 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
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
  add_test(NAME perf.linux-resource-${_arch}
    COMMAND ${CMAKE_BINARY_DIR}/linux-production/hl-engine-linux-${_arch}
            ${HL_PERF}/resource-${_arch})
  set_tests_properties(perf.linux-resource-${_arch} PROPERTIES
    LABELS "perf;perf-linux" RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# perf-native-aarch64: only meaningful when the host can run the AArch64 Linux
# fixtures directly (Makefile guards this with uname -s/-m and exits 2).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  function(hl_perf_native case warmups samples payload expect)
    add_test(NAME perf.native-${case}-aarch64
      COMMAND $<TARGET_FILE:perf-runner> --label native-${case}-aarch64
              --warmups ${warmups} --samples ${samples} --expect ${expect} -- ${payload})
    set_tests_properties(perf.native-${case}-aarch64 PROPERTIES
      LABELS "perf;perf-native" RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endfunction()
  hl_perf_native(startup ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES} ${HL_E2E}/guest-exit-aarch64 42)
  hl_perf_native(compute ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
                 ${HL_COMPAT}/core/workload/aarch64/busyloop 0)
  hl_perf_native(syscall-startup ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
                 ${HL_COMPAT}/syscall/aarch64/gettid 0)
  hl_perf_native(syscall-1m ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
                 ${HL_PERF}/syscall-aarch64 0)
  hl_perf_native(fork-stress 1 ${HL_PERF_HEAVY_SAMPLES}
                 ${HL_COMPAT}/process/aarch64/forkstorm 0)
  foreach(_op mmap file pipe event ipc-latency ipc-throughput)
    hl_perf_native(${_op} ${HL_PERF_WARMUPS} ${HL_PERF_OP_SAMPLES} ${HL_PERF}/${_op}-aarch64 0)
  endforeach()
endif()

# `make bench` has no pass/fail semantics (it prints a comparison table), so it
# stays a build target rather than a CTest case.
add_custom_target(bench
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bench
  COMMAND $<TARGET_FILE:bench-runner> report --baseline hl-engine
  DEPENDS bench-runner
  COMMENT "fair combined self-timing bench (see tools/bench/README.md)")


endif() # native Linux lane
