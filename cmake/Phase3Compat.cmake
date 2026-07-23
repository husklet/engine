# ---------------------------------------------------------------------------
# Phase 3b — the compatibility matrix.
#
#   * every guest fixture the Makefile cross-builds (~40 pattern-rule pairs),
#     expressed through the single hl_guest_binary/hl_guest_suite helper;
#   * the host-side runner tools;
#   * one CTest per compat suite, invoking tools/matrix-runner with its
#     positional signature
#         matrix-runner BRIDGE A64_ENGINE A64_BINDIR X64_ENGINE X64_BINDIR SUITE
#     and the suite's manifest.tsv (the runner loads <SUITE>/manifest.tsv).
#
# Which engines: on a Linux host the Makefile drives the compat suites through
# build/linux-production/hl-engine-linux-{aarch64,x86_64} with BRIDGE=`env`
# (target test-linux-production-typed). The mac lane uses build/production/*
# with BRIDGE=`mac`. Both are expressed; the host guard picks one, matching the
# Makefile's HOST conditional.
#
# ORDER MATTERS in this file: special cases are declared before the generic
# suite sweeps, because hl_guest_binary() gives the first declaration of an
# output path precedence — the CMake equivalent of make preferring an explicit
# rule over a pattern rule.
# ---------------------------------------------------------------------------

set(HL_COMPAT ${CMAKE_BINARY_DIR}/compat)
set(HL_TESTS  ${CMAKE_SOURCE_DIR}/tests)

# Recurring flag/lib bundles, named once (the Makefile repeats these literals
# in every one of the ~80 recipes).
set(_gnu   -O2 -std=gnu11)
set(_o2    -O2)
set(_pt    -pthread)
set(_ptm   -pthread -lm)
set(_ptrt  -pthread -lrt)
# "purpose PIE": suites whose dlopen/TLS cases need a PIE guest plus libdl.
set(_purpose_libs -pthread -lm -ldl)

# ===========================================================================
# 1. abi
# ===========================================================================
# lse_atomics needs the LSE atomics ISA extension explicitly, and outline
# atomics disabled so the LSE instructions actually reach the translator.
hl_guest_binary(aarch64 ${HL_COMPAT}/abi/aarch64/lse_atomics
  ${HL_TESTS}/compat/abi/lse_atomics.c LINKAGE static-pie
  FLAGS ${_o2} -pthread -march=armv8.2-a+lse -mno-outline-atomics LIBS -lm)
hl_guest_named(${HL_COMPAT}/abi tlsmodels_main ${HL_TESTS}/compat/abi/tlsmodels_main.c
  LINKAGE nonpie FLAGS ${_o2} LIBS ${_purpose_libs})
# cpuid_features / rflags_id are x86-only (they include <cpuid.h>), so the
# aarch64 lane filters them out — see ABI_CASE_AARCH64_NAMES.
foreach(_c atomics_builtins tls tlsmodels)
  hl_guest_named(${HL_COMPAT}/abi ${_c} ${HL_TESTS}/compat/abi/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
foreach(_c cpuid_features rflags_id)
  hl_guest_binary(x86_64 ${HL_COMPAT}/abi/x86_64/${_c} ${HL_TESTS}/compat/abi/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
hl_guest_suite(SRC_DIR tests/compat/abi OUT_DIR ${HL_COMPAT}/abi
  LINKAGE static FLAGS ${_o2} LIBS -lm
  EXCLUDE_aarch64 cpuid_features rflags_id
  EXCLUDE_x86_64  lse_atomics)

hl_guest_suite(SRC_DIR tests/compat/abi/corpus OUT_DIR ${HL_COMPAT}/abi-corpus
  LINKAGE static FLAGS ${_gnu} ${_pt} LIBS -lm)

# ===========================================================================
# 2. libc / completeness / posix / syscall / network / procfs / memory
# ===========================================================================
hl_guest_suite(SRC_DIR tests/compat/libc OUT_DIR ${HL_COMPAT}/libc
  LINKAGE static FLAGS ${_gnu} LIBS -lm)

# completeness has PER-ARCH source directories plus a shared syscall/ dir, and
# keeps the sub-path in the output name (Makefile 271-277).
foreach(_arch aarch64 x86_64)
  foreach(_sub ${_arch} syscall)
    file(GLOB _srcs CONFIGURE_DEPENDS ${HL_TESTS}/compat/completeness/${_sub}/*.c)
    foreach(_s ${_srcs})
      get_filename_component(_n ${_s} NAME_WE)
      hl_guest_binary(${_arch} ${HL_COMPAT}/completeness/${_arch}/${_sub}/${_n} ${_s}
        LINKAGE static FLAGS ${_gnu} -I${HL_TESTS}/compat/completeness)
    endforeach()
  endforeach()
endforeach()

hl_guest_suite(SRC_DIR tests/compat/posix OUT_DIR ${HL_COMPAT}/posix
  LINKAGE static FLAGS ${_gnu} LIBS -pthread -lutil)
hl_guest_suite(SRC_DIR tests/compat/syscall OUT_DIR ${HL_COMPAT}/syscall
  LINKAGE static FLAGS ${_gnu} LIBS ${_pt})
hl_guest_suite(SRC_DIR tests/compat/syscall_edges OUT_DIR ${HL_COMPAT}/syscall_edges
  LINKAGE static FLAGS ${_gnu} LIBS ${_ptrt})

foreach(_c ltp_neterr net_nonblock net_sendmsg net_sockopt net_tcp net_udp net_unix sentry_net)
  hl_guest_named(${HL_COMPAT}/network ${_c} ${HL_TESTS}/compat/network/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
hl_guest_suite(SRC_DIR tests/compat/network OUT_DIR ${HL_COMPAT}/network
  LINKAGE static FLAGS ${_gnu} LIBS ${_pt})

hl_guest_suite(SRC_DIR tests/compat/procfs OUT_DIR ${HL_COMPAT}/procfs
  LINKAGE static FLAGS ${_gnu} -I${HL_TESTS}/compat/procfs LIBS ${_pt})
hl_guest_suite(SRC_DIR tests/compat/memory OUT_DIR ${HL_COMPAT}/memory
  LINKAGE static FLAGS ${_gnu} -I${HL_TESTS}/compat/memory LIBS ${_pt})

# ===========================================================================
# 3. signals
# ===========================================================================
# Folded-fault register reconstruction is a guest_base (non-PIE ET_EXEC) path;
# the generic -static-pie recipe would make it pass trivially (Makefile 985).
hl_guest_named(${HL_COMPAT}/signals synchronous_fault_registers
  ${HL_TESTS}/compat/signals/synchronous_fault_registers.c
  LINKAGE nonpie FLAGS ${_gnu} LIBS ${_pt})
hl_guest_suite(SRC_DIR tests/compat/signals OUT_DIR ${HL_COMPAT}/signals
  LINKAGE static-pie FLAGS ${_gnu} LIBS ${_pt}
  EXCLUDE_x86_64 sigurg_preempt sigurg_go_preempt)

# ===========================================================================
# 4. filesystem  (three linkages + a flattened corpus dir)
# ===========================================================================
foreach(_c dup2redir fcntlflags ltp_aterr ltp_dupfcntl ltp_linkstat
           missing_root_stat mkfifo scratch_exec sentry_fs)
  hl_guest_named(${HL_COMPAT}/filesystem ${_c} ${HL_TESTS}/compat/filesystem/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
foreach(_sub dentry pcachex)
  hl_guest_suite(SRC_DIR tests/compat/filesystem OUT_DIR ${HL_COMPAT}/filesystem
    SOURCE_DIRS tests/compat/filesystem/${_sub} RELATIVE_NAMES
    LINKAGE static-pie FLAGS ${_gnu} LIBS ${_pt})
endforeach()
# corpus/ FLATTENS into the suite directory (Makefile 999-1005).
hl_guest_suite(SRC_DIR tests/compat/filesystem/corpus OUT_DIR ${HL_COMPAT}/filesystem
  LINKAGE static FLAGS ${_gnu} LIBS ${_ptrt})
hl_guest_suite(SRC_DIR tests/compat/filesystem OUT_DIR ${HL_COMPAT}/filesystem
  LINKAGE static FLAGS ${_gnu} LIBS ${_ptrt})

# ===========================================================================
# 5. process
# ===========================================================================
hl_guest_named(${HL_COMPAT}/process nonpie_ptrargs
  ${HL_TESTS}/compat/process/nonpie_ptrargs.c LINKAGE nonpie FLAGS ${_gnu} LIBS ${_pt})
hl_guest_named(${HL_COMPAT}/process nonpie_dladdr
  ${HL_TESTS}/compat/process/nonpie_dladdr.c LINKAGE dynamic FLAGS ${_o2})
foreach(_c execfault forkserver_probe forkstorm forkwait ltp_checkpoint ltp_procmisc
           pipeproc procreap sentry_exec_proc sentry_fork sysinfo thrfork waitcore)
  hl_guest_named(${HL_COMPAT}/process ${_c} ${HL_TESTS}/compat/process/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
hl_guest_suite(SRC_DIR tests/compat/process OUT_DIR ${HL_COMPAT}/process
  SOURCE_DIRS tests/compat/process/procexe RELATIVE_NAMES
  LINKAGE static-pie FLAGS ${_gnu} LIBS ${_pt})
hl_guest_suite(SRC_DIR tests/compat/process OUT_DIR ${HL_COMPAT}/process
  LINKAGE static FLAGS ${_gnu} LIBS ${_pt})

# ===========================================================================
# 6. time / isolation / ipc / threads
# ===========================================================================
hl_guest_suite(SRC_DIR tests/compat/time OUT_DIR ${HL_COMPAT}/time
  LINKAGE static FLAGS ${_gnu} LIBS ${_ptrt})
hl_guest_suite(SRC_DIR tests/compat/isolation OUT_DIR ${HL_COMPAT}/isolation
  LINKAGE static-pie FLAGS ${_gnu} LIBS ${_pt})

# neonshm is aarch64-only (NEON shared-memory ordering); see IPC_CASE_BINS.
foreach(_c msg sem shm shmposix sysvshm)
  hl_guest_named(${HL_COMPAT}/ipc ${_c} ${HL_TESTS}/compat/ipc/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
hl_guest_binary(aarch64 ${HL_COMPAT}/ipc/aarch64/neonshm ${HL_TESTS}/compat/ipc/neonshm.c
  LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
hl_guest_suite(SRC_DIR tests/compat/ipc OUT_DIR ${HL_COMPAT}/ipc
  LINKAGE static FLAGS ${_gnu} ${_pt} LIBS -lrt
  EXCLUDE_aarch64 ipc_tso_unaligned ipc_tso_simd_mp
  EXCLUDE_x86_64  ipc_mq_notify neonshm)

foreach(_c threads_basic threads_many threads_mutex_queue atomics_outline_pie)
  hl_guest_named(${HL_COMPAT}/threads ${_c} ${HL_TESTS}/compat/threads/${_c}.c
    LINKAGE static-pie FLAGS ${_o2} LIBS ${_purpose_libs})
endforeach()
hl_guest_named(${HL_COMPAT}/threads threads_nopie_tls
  ${HL_TESTS}/compat/threads/threads_nopie_tls.c LINKAGE nonpie FLAGS ${_o2} LIBS ${_ptm})
# threads_mutex_nopie is a second, non-PIE build of threads_mutex_queue.c.
hl_guest_named(${HL_COMPAT}/threads threads_mutex_nopie
  ${HL_TESTS}/compat/threads/threads_mutex_queue.c LINKAGE nonpie FLAGS ${_o2} LIBS ${_ptm})
hl_guest_suite(SRC_DIR tests/compat/threads OUT_DIR ${HL_COMPAT}/threads
  LINKAGE static FLAGS ${_gnu} ${_pt} LIBS -lm)

# ===========================================================================
# 7. core/{abi,workload,syscall,regress}
# ===========================================================================
set(CORE_ABI_BOTH hello math strings bitops varargs longjmp recursion fnptr
  jumptable ibtc_dispatch floatmath heap qsort files statfile pipe mmapanon
  munmap_partial regex globmatch strtod timefmt environ atexit sigaction2
  sigjmp sortbig)
foreach(_c ${CORE_ABI_BOTH} stolen_regs)
  hl_guest_binary(aarch64 ${HL_COMPAT}/core/abi/aarch64/${_c}
    ${HL_TESTS}/compat/core/abi/${_c}.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
endforeach()
foreach(_c ${CORE_ABI_BOTH} moffs fpedge fpdnan repmovsdf x87m80 shldflags btwidth)
  hl_guest_binary(x86_64 ${HL_COMPAT}/core/abi/x86_64/${_c}
    ${HL_TESTS}/compat/core/abi/${_c}.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
endforeach()

set(CORE_WORKLOAD_BOTH busyloop bigmem bigarr soak_codecache soak_indirect
  soak_threadchurn soak_forkchurn soak_allocchurn smc_mprotect stw_futex_quiesce)
# ibtc_dispatch's workload build comes from the core/abi source (Makefile 1099).
hl_guest_named(${HL_COMPAT}/core/workload ibtc_dispatch
  ${HL_TESTS}/compat/core/abi/ibtc_dispatch.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
# dbserver/sqlite link the per-arch static sqlite nix puts on *_STATIC_CC.
foreach(_c dbserver sqlite)
  hl_guest_binary(aarch64 ${HL_COMPAT}/core/workload/aarch64/${_c}
    ${HL_TESTS}/compat/core/workload/${_c}.c LINKAGE static-pie FLAGS ${_o2}
    LIBS -pthread -lsqlite3 -lm -ldl)
endforeach()
foreach(_c ${CORE_WORKLOAD_BOTH} luajit_trace soak_smc smc_threads smc_selfflush)
  if(EXISTS ${HL_TESTS}/compat/core/workload/${_c}.c)
    hl_guest_binary(aarch64 ${HL_COMPAT}/core/workload/aarch64/${_c}
      ${HL_TESTS}/compat/core/workload/${_c}.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
  endif()
endforeach()
foreach(_c ${CORE_WORKLOAD_BOTH} smc_remap_reuse smc_mremap smc_table_overflow)
  if(EXISTS ${HL_TESTS}/compat/core/workload/${_c}.c)
    hl_guest_binary(x86_64 ${HL_COMPAT}/core/workload/x86_64/${_c}
      ${HL_TESTS}/compat/core/workload/${_c}.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
  endif()
endforeach()

hl_guest_suite(SRC_DIR tests/compat/core/syscall OUT_DIR ${HL_COMPAT}/core/syscall
  LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})

# core/regress: a fixed name list per arch, three of which are non-PIE, and one
# committed Go-built binary that is copied rather than compiled.
foreach(_c nonpie_ldapr nonpie_pairatomics)
  hl_guest_binary(aarch64 ${HL_COMPAT}/core/regress/aarch64/${_c}
    ${HL_TESTS}/compat/core/regress/${_c}.c LINKAGE nonpie FLAGS ${_o2} LIBS ${_ptm})
endforeach()
foreach(_c nonpie_vec repcmps_nopie nonpie_v8blob)
  hl_guest_binary(x86_64 ${HL_COMPAT}/core/regress/x86_64/${_c}
    ${HL_TESTS}/compat/core/regress/${_c}.c LINKAGE nonpie FLAGS ${_o2} LIBS ${_ptm})
endforeach()
hl_guest_binary(aarch64 ${HL_COMPAT}/core/regress/aarch64/go_cgo_stackgrow_arm
  ${HL_TESTS}/compat/core/regress/go_cgo_stackgrow_arm LINKAGE copy)
set(CORE_REGRESS_BOTH lseek_read offset_track sha512_kat ccmp_test
  getaffinity_tid stackoverflow stackoverflow_catch)
foreach(_c ${CORE_REGRESS_BOTH} ldrsw_literal)
  hl_guest_binary(aarch64 ${HL_COMPAT}/core/regress/aarch64/${_c}
    ${HL_TESTS}/compat/core/regress/${_c}.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
endforeach()
foreach(_c ${CORE_REGRESS_BOTH})
  hl_guest_binary(x86_64 ${HL_COMPAT}/core/regress/x86_64/${_c}
    ${HL_TESTS}/compat/core/regress/${_c}.c LINKAGE static-pie FLAGS ${_o2} LIBS ${_ptm})
endforeach()

# ===========================================================================
# 8. isa/x86_64 — committed binary inputs, copied (never rebuilt)
# ===========================================================================
# isa_regress is the one SOURCE-built case in this suite: it pins the
# instruction lowerings the differential ISA fuzzer found diverging from
# qemu-x86_64, and must be non-PIE so the folded guest_base paths stay live.
hl_guest_binary(x86_64 ${HL_COMPAT}/isa/x86_64/isa_regress
  ${HL_TESTS}/compat/isa/x86_64/isa_regress.c LINKAGE nonpie FLAGS ${_gnu})
foreach(_c ctest_x64 g_x64 go_goro_x86 go_heapgc_x86 gw hello_x86 hx)
  hl_guest_binary(x86_64 ${HL_COMPAT}/isa/x86_64/${_c}
    ${HL_TESTS}/compat/isa/x86_64/${_c} LINKAGE copy)
endforeach()

# ===========================================================================
# 9. soak
# ===========================================================================
hl_guest_suite(SRC_DIR tests/soak OUT_DIR ${CMAKE_BINARY_DIR}/soak
  LINKAGE static-pie FLAGS ${_gnu} LIBS ${_ptm})

hl_guest_finalize(guest-fixtures)

# ===========================================================================
# 10. host-side runner tools
# ===========================================================================
# These are plain host C programs built with CFLAGS+WARNINGS (no CPPFLAGS
# unless the tool includes hl/ headers). One helper, no repetition.
function(hl_tool name source)
  cmake_parse_arguments(T "HL_HEADERS" "SUBDIR" "FLAGS;LINK" ${ARGN})
  if(NOT T_SUBDIR)
    set(T_SUBDIR tools)
  endif()
  add_executable(${name} ${source})
  target_compile_options(${name} PRIVATE
    -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes ${T_FLAGS})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_options(${name} PRIVATE ${T_LINK})
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${T_SUBDIR})
endfunction()

hl_tool(compat-runner          tools/compat_runner.c)
hl_tool(e2e-runner             tools/e2e_runner.c)
hl_tool(bridge-runner          tools/bridge_runner.c)
hl_tool(config-e2e-runner      tools/config_e2e_runner.c)
hl_tool(rootfs-e2e-runner      tools/rootfs_e2e_runner.c)
hl_tool(perf-runner            tools/perf_runner.c)
hl_tool(remote-supervisor      tools/remote_supervisor.c)
# matrix-runner re-execs a supervisor it looks up NEXT TO the engine binary, so
# a second copy must land in the production directory (Makefile 2174).
hl_tool(linux-production-remote-supervisor tools/remote_supervisor.c
        SUBDIR linux-production LINK -lc)
set_target_properties(linux-production-remote-supervisor PROPERTIES
  OUTPUT_NAME hl-remote-supervisor)
hl_tool(checkpoint-tree-runner tests/integration/checkpoint_tree_runner.c)
hl_tool(bridge-jobserver-test  tests/integration/bridge_jobserver.c)
hl_tool(forkserver-runner      tests/compat/process/integration/forkserver_runner.c)
hl_tool(deny-icmp              tests/integration/deny_icmp.c SUBDIR tests)
hl_tool(tests-remote-supervisor tests/integration/remote_supervisor.c SUBDIR tests)
# -Werror + no -Wconversion, matching their Makefile recipes.
hl_tool(linux-production-smoke tools/linux_production_smoke.c)
hl_tool(linux-matrix           tools/linux_matrix.c)
# bench-runner does many benign width conversions; -Wconversion is dropped.
add_executable(bench-runner tools/bench_runner.c)
target_compile_options(bench-runner PRIVATE -O2 -g -std=gnu11 -Wall -Wextra)
set_target_properties(bench-runner PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# matrix-runner needs the host loader/libc paths baked in (Makefile 2166).
hl_tool(matrix-runner tools/matrix_runner.c LINK -lc
  FLAGS -DHL_ENABLE_LOGGING=${HL_DEBUG_LOGGING}
        -DAARCH64_DYNAMIC_LOADER="${HL_AARCH64_DYNAMIC_LOADER}"
        -DAARCH64_DYNAMIC_LIBC="${HL_AARCH64_DYNAMIC_LIBC}"
        -DX86_64_DYNAMIC_LOADER="${HL_X86_64_DYNAMIC_LOADER}"
        -DX86_64_DYNAMIC_LIBC="${HL_X86_64_DYNAMIC_LIBC}")

# Native (host-arch) smoke fixtures used by `make compat-native`.
set(_native_smoke atomics clockelapsed epoll epoll_edge eventfd eventfd_sema
  forkwait mmapanon mmapshared seccomp statx_agree sysv_ipc timerfd)
set(_native_smoke_bins "")
foreach(_c ${_native_smoke})
  add_executable(fixture-${_c} tests/compat/fixtures/${_c}.c)
  target_compile_options(fixture-${_c} PRIVATE -O2 -g -std=gnu11 -Wall -Wextra)
  target_link_options(fixture-${_c} PRIVATE -pthread)
  set_target_properties(fixture-${_c} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures OUTPUT_NAME ${_c})
  list(APPEND _native_smoke_bins $<TARGET_FILE:fixture-${_c}>)
endforeach()
add_test(NAME compat-native COMMAND compat-runner ${_native_smoke_bins})
set_tests_properties(compat-native PROPERTIES LABELS "compat-native" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ===========================================================================
# 11. the compat suites as CTest cases
# ===========================================================================
# Linux host: BRIDGE=env + the native linux-production engines.
# macOS host: BRIDGE=mac + the build/production engines (Phase 4).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(HL_MATRIX_BRIDGE mac)
  set(HL_MATRIX_ENGINE_DIR ${CMAKE_BINARY_DIR}/production)
else()
  set(HL_MATRIX_BRIDGE env)
  set(HL_MATRIX_ENGINE_DIR ${CMAKE_BINARY_DIR}/linux-production)
endif()
set(HL_ENGINE_AARCH64 ${HL_MATRIX_ENGINE_DIR}/hl-engine-linux-aarch64)
set(HL_ENGINE_X86_64  ${HL_MATRIX_ENGINE_DIR}/hl-engine-linux-x86_64)

# hl_compat_suite(<label> <bin-subdir> <suite-source-dir> [SERIAL] [LOCKS ...])
#   Adds `compat.<label>` running the whole suite through matrix-runner, with
#   LABELS <label> so `ctest -L compat-ipc` selects exactly that suite.
function(hl_compat_suite label bindir suitedir)
  cmake_parse_arguments(C "SERIAL" "" "LOCKS;ARGS" ${ARGN})
  add_test(NAME compat.${label}
    COMMAND $<TARGET_FILE:matrix-runner> ${HL_MATRIX_BRIDGE}
            ${HL_ENGINE_AARCH64} ${bindir}/aarch64
            ${HL_ENGINE_X86_64}  ${bindir}/x86_64
            ${CMAKE_SOURCE_DIR}/${suitedir} ${C_ARGS})
  set_tests_properties(compat.${label} PROPERTIES
    LABELS "compat;compat-${label}" TIMEOUT 3600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  if(C_SERIAL)
    set_tests_properties(compat.${label} PROPERTIES RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
  # Serialisation. The Makefile runs the whole compat matrix inside ONE
  # sequential recipe (test-linux-production-typed / e2e-compat), and that is
  # not incidental: the guests bind ports, create System V keys, spawn process
  # groups and share /tmp scratch, so two suites running at once corrupt each
  # other's results (observed: syscall goes from 1 to 5 failures under -j4).
  # `hl-guest` is therefore held by every suite, which keeps ctest -j safe while
  # still letting the compat matrix overlap the unit lane and letting
  # `ctest -L compat-ipc` select one suite. Extra LOCKS narrow it further.
  set_tests_properties(compat.${label} PROPERTIES
    RESOURCE_LOCK "hl-guest;${C_LOCKS}" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

hl_compat_suite(abi          ${HL_COMPAT}/abi          tests/compat/abi)
hl_compat_suite(abi-corpus   ${HL_COMPAT}/abi-corpus   tests/compat/abi/corpus)
hl_compat_suite(libc         ${HL_COMPAT}/libc         tests/compat/libc)
hl_compat_suite(completeness ${HL_COMPAT}/completeness tests/compat/completeness)
hl_compat_suite(posix        ${HL_COMPAT}/posix        tests/compat/posix)
hl_compat_suite(syscall      ${HL_COMPAT}/syscall      tests/compat/syscall)
hl_compat_suite(syscall-edges ${HL_COMPAT}/syscall_edges tests/compat/syscall_edges)
hl_compat_suite(memory       ${HL_COMPAT}/memory       tests/compat/memory)
hl_compat_suite(time         ${HL_COMPAT}/time         tests/compat/time)
hl_compat_suite(core-abi     ${HL_COMPAT}/core/abi     tests/compat/core/abi)
hl_compat_suite(core-syscall ${HL_COMPAT}/core/syscall tests/compat/core/syscall)
hl_compat_suite(core-regress ${HL_COMPAT}/core/regress tests/compat/core/regress)
hl_compat_suite(core-workload ${HL_COMPAT}/core/workload tests/compat/core/workload)
hl_compat_suite(isa-x86-64   ${HL_COMPAT}/isa          tests/compat/isa/x86_64)

# --- suites with real, non-obvious concurrency constraints ------------------
# The Makefile expresses these as prose + .NOTPARALLEL; CTest can express them
# precisely, so `ctest -j` stays safe instead of merely serialising everything.
#
#  * network / procfs / isolation bind ports and inspect a shared network
#    namespace view -> they must not overlap each other.
#  * ipc / process / filesystem / signals / threads use System V keys, process
#    groups, and shared /tmp scratch; they take a scratch lock.
hl_compat_suite(network    ${HL_COMPAT}/network    tests/compat/network    LOCKS hl-net)
hl_compat_suite(procfs     ${HL_COMPAT}/procfs     tests/compat/procfs     LOCKS hl-net)
hl_compat_suite(isolation  ${HL_COMPAT}/isolation  tests/compat/isolation  LOCKS hl-net)
hl_compat_suite(ipc        ${HL_COMPAT}/ipc        tests/compat/ipc        LOCKS hl-ipc)
hl_compat_suite(process    ${HL_COMPAT}/process    tests/compat/process    LOCKS hl-scratch)
hl_compat_suite(filesystem ${HL_COMPAT}/filesystem tests/compat/filesystem LOCKS hl-scratch)
hl_compat_suite(signals    ${HL_COMPAT}/signals    tests/compat/signals    LOCKS hl-scratch)
hl_compat_suite(threads    ${HL_COMPAT}/threads    tests/compat/threads)
# soak is long and CPU-hungry: give it the machine.
hl_compat_suite(soak       ${CMAKE_BINARY_DIR}/soak tests/soak SERIAL)

# compat-filesystem asserts four fixtures exist before running (Makefile 2286).
add_test(NAME compat.filesystem-fixtures-present
  COMMAND ${CMAKE_COMMAND}
    -DFILES=${HL_COMPAT}/filesystem/aarch64/child_create_reopen$<SEMICOLON>${HL_COMPAT}/filesystem/x86_64/child_create_reopen$<SEMICOLON>${HL_COMPAT}/filesystem/aarch64/sibling_generation_stress$<SEMICOLON>${HL_COMPAT}/filesystem/x86_64/sibling_generation_stress
    -P ${CMAKE_SOURCE_DIR}/cmake/AssertExecutables.cmake)
set_tests_properties(compat.filesystem-fixtures-present PROPERTIES
  LABELS "compat;compat-filesystem" FIXTURES_SETUP hl-fs-fixtures WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(compat.filesystem PROPERTIES FIXTURES_REQUIRED hl-fs-fixtures WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# compat-process also runs the forkserver integration probe per arch — but ONLY
# in the mac gate lane. The Linux lane (test-linux-production-typed) drives the
# process suite through matrix-runner alone and never invokes forkserver-runner,
# so registering it on Linux would add a test `make` does not run and break the
# pass/fail parity claim.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
foreach(_arch aarch64 x86_64)
  if(_arch STREQUAL aarch64)
    set(_eng ${HL_ENGINE_AARCH64})
  else()
    set(_eng ${HL_ENGINE_X86_64})
  endif()
  add_test(NAME compat.process-forkserver-${_arch}
    COMMAND $<TARGET_FILE:forkserver-runner> ${HL_MATRIX_BRIDGE} ${_eng}
            ${HL_COMPAT}/process/${_arch}/forkserver_probe
            ${HL_TESTS}/compat/process/expected/forkserver_integration.out)
  set_tests_properties(compat.process-forkserver-${_arch} PROPERTIES
    LABELS "compat;compat-process" RESOURCE_LOCK "hl-guest;hl-scratch" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
endif()

# compat-network additionally runs the suite under an ICMP-denied bridge.
add_test(NAME compat.network-icmp-bridge
  COMMAND $<TARGET_FILE:deny-icmp> $<TARGET_FILE:matrix-runner> ${HL_MATRIX_BRIDGE}
          ${HL_ENGINE_AARCH64} ${HL_COMPAT}/network/aarch64
          ${HL_ENGINE_X86_64}  ${HL_COMPAT}/network/x86_64
          ${HL_TESTS}/compat/network icmp-bridge)
set_tests_properties(compat.network-icmp-bridge PROPERTIES
  LABELS "compat;compat-network" RESOURCE_LOCK "hl-guest;hl-net" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
