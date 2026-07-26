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
hl_guest_named(${HL_COMPAT}/memory elf_rodata_write
  ${HL_TESTS}/compat/memory/elf_rodata_write.c
  LINKAGE static FLAGS ${_gnu} -Wl,-T,${HL_TESTS}/compat/memory/elf_rodata_write.ld LIBS ${_pt})
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
# A RELATIVE symlink to the exec_symlink guest, registered as its own manifest case
# (exec-symlink-entry). The engine's launch path must follow it exactly as execve does; opening the entry
# program O_NOFOLLOW failed every symlinked program with ELOOP. Same binary, so the golden is shared.
foreach(_arch ${HL_GUEST_ARCHES})
  add_custom_command(OUTPUT ${HL_COMPAT}/process/${_arch}/exec_symlink_entry
    COMMAND ${CMAKE_COMMAND} -E create_symlink exec_symlink
            ${HL_COMPAT}/process/${_arch}/exec_symlink_entry
    DEPENDS ${HL_COMPAT}/process/${_arch}/exec_symlink
    COMMENT "guest[${_arch}] symlink exec_symlink_entry"
    VERBATIM)
  set_property(GLOBAL APPEND PROPERTY HL_GUEST_ALL_OUTPUTS
    ${HL_COMPAT}/process/${_arch}/exec_symlink_entry)
endforeach()

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
# smc_precise was dropped from this list while the case was `excluded-macos`; the
# manifest re-activated it but the fixture was never registered, so on Linux the
# runner launched a path that does not exist -> `execution failed status=6`
# (HL_STATUS_NOT_FOUND from opening argv[0]), not the engine abort it was blamed on.
foreach(_c ${CORE_WORKLOAD_BOTH} luajit_trace soak_smc smc_threads smc_selfflush smc_precise)
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

# 8b. isa/aarch64 — the AArch64 counterpart of isa_regress, pinning the
# lowerings the differential aarch64 fuzzer found diverging from a NATIVE run
# of the same binary. Non-PIE on purpose (Makefile 1084): g_nonpie_lo must be
# armed for the LSE-upgrade and bias-fold paths.
hl_guest_binary(aarch64 ${HL_COMPAT}/isa/aarch64/isa_regress
  ${HL_TESTS}/compat/isa/aarch64/isa_regress.c LINKAGE nonpie FLAGS ${_gnu})

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
    -Wstrict-prototypes -Wmissing-prototypes
    -Werror=implicit-function-declaration -Werror=implicit-int ${T_FLAGS})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  # Tests run from CMAKE_SOURCE_DIR, so a runner that needs host scratch space
  # cannot guess where the build tree is: the directory is only named `build`
  # by convention, and deriving <cwd>/build silently misses every other name.
  target_compile_definitions(${name} PRIVATE HL_BUILD_DIR="${CMAKE_BINARY_DIR}")
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
hl_tool(bridge-jobserver-test  tests/integration/bridge_jobserver.c)
hl_tool(forkserver-runner      tests/compat/process/integration/forkserver_runner.c)
hl_tool(tests-remote-supervisor tests/integration/remote_supervisor.c SUBDIR tests)
# Linux-host-only sources: checkpoint_tree_runner.c needs mkdtemp under the
# strict c11 dialect, deny_icmp.c needs <linux/filter.h>. Their consumers (the
# checkpoint gates, compat.network-icmp-bridge) are Linux-only too.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  hl_tool(checkpoint-tree-runner tests/integration/checkpoint_tree_runner.c)
  hl_tool(deny-icmp              tests/integration/deny_icmp.c SUBDIR tests)
endif()
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

# Native (host-arch) smoke fixtures. Linux-only: they are the Linux syscall
# surface (epoll/eventfd/timerfd/seccomp/statx) compiled for the HOST, so a
# Darwin host cannot build them and never ran this lane.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
endif()

# ===========================================================================
# 11. the compat suites as CTest cases
# ===========================================================================
# The bridge is always `env`: the runner launches the engine on the host it is
# running on. `mac` is OrbStack's Linux->macOS FORWARDER and is meaningful only
# when a Linux host drives the mac, which is what tools/run_remote_macos_ctest.sh
# does -- and that configures CMake ON the mac, so it is `env` there too.
# Makefile: MAC ?= $(if $(filter Darwin,$(shell uname -s)),env,mac).
# Only the engine directory differs: Phase 4 builds build/production on Darwin,
# Phase 2 builds build/linux-production on Linux.
set(HL_MATRIX_BRIDGE env)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(HL_MATRIX_ENGINE_DIR ${CMAKE_BINARY_DIR}/production)
else()
  set(HL_MATRIX_ENGINE_DIR ${CMAKE_BINARY_DIR}/linux-production)
endif()
set(HL_ENGINE_AARCH64 ${HL_MATRIX_ENGINE_DIR}/hl-engine-linux-aarch64)
set(HL_ENGINE_X86_64  ${HL_MATRIX_ENGINE_DIR}/hl-engine-linux-x86_64)

# --- the per-case timeout budget, as a function of the HOST CPU --------------
# tools/matrix_runner.c and tools/linux_matrix.c kill a case that outruns a
# per-case budget (120s and 20s) and report a hang. Both budgets were calibrated
# on a JIT host, which was every host there was: on an ARM64 host these cases are
# milliseconds of guest work, so "did not finish" could only mean hung.
#
# An x86_64 host has no JIT. Neither guest frontend has an amd64 back end -- both
# emit ARM64 -- so its backend decodes and executes, at roughly 10-50x the cost
# (docs/amd64-host.md section 3). Leaving the budgets alone there would turn
# slow-but-correct into a killed process reported as a hang: the worst available
# diagnostic, because it is both false and identical to the true one.
#
# So the scale is declared HERE, beside the test registration, and passed to the
# runners in the environment. It is not detected inside them -- see the long note
# above case_timeout_ms() in tools/matrix_runner.c for why a harness must not
# infer its own budget from the backend it is testing.
#
# HL_HOST_ARCH is the axis, taken from CMakeLists.txt, which derives it once and
# normalises arm64/aarch64 and amd64/x86_64. CMAKE_SYSTEM_PROCESSOR is not
# re-tested here: two spellings of one host CPU are exactly how a guard like this
# comes to apply on one machine and not its twin.
#
# 30 is a placeholder with a reason, not a measurement: the interpreters are
# being written, nothing has been measured, and 30 is the middle of the range
# docs/amd64-host.md predicts. Raise or lower it with -D once there are numbers;
# what must not happen is that it silently stays at 1.
if(HL_HOST_ARCH STREQUAL "x86_64")
  set(_hl_timeout_scale_default 30)
else()
  set(_hl_timeout_scale_default 1)
endif()
set(HL_MATRIX_TIMEOUT_SCALE ${_hl_timeout_scale_default} CACHE STRING
    "multiply every per-case guest timeout by this factor (>1 where the host backend interprets)")
# The runners reject a malformed scale; rejecting it at configure time as well
# means a typo cannot survive as far as a test verdict.
if(NOT HL_MATRIX_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$" OR HL_MATRIX_TIMEOUT_SCALE GREATER 100)
  message(FATAL_ERROR
    "HL_MATRIX_TIMEOUT_SCALE='${HL_MATRIX_TIMEOUT_SCALE}' must be an integer factor in [1, 100]. "
    "It multiplies the per-case guest timeout; a run that needs more than 100x wants a different budget "
    "(HL_MATRIX_CASE_TIMEOUT_MS), not a larger factor.")
endif()

# hl_matrix_timeout_scale(<test> ...)
#   Scales the CTest budget of tests driven by matrix-runner / linux-matrix and
#   hands them the factor. Both layers have to move together: a per-case budget
#   of 20s x 30 buys nothing if CTest kills the whole suite at its own TIMEOUT
#   first, and that failure is worse than the one being fixed -- CTest reports
#   the suite, so no case is named at all.
#
#   At scale 1 this function writes NOTHING. That is the point: the aarch64
#   lanes' generated CTestTestfile.cmake is byte-identical to what it was before
#   this existed, so "the default changes nothing" is a property of the build
#   rather than a claim about it. It also means the multiplier applies to
#   whatever budget the caller already set, including CTest's own 1500s default
#   for a test that set none.
function(hl_matrix_timeout_scale)
  if(HL_MATRIX_TIMEOUT_SCALE EQUAL 1)
    return()
  endif()
  foreach(_test IN LISTS ARGN)
    get_test_property(${_test} TIMEOUT _hl_current_timeout)
    if(NOT _hl_current_timeout)
      set(_hl_current_timeout 1500)
    endif()
    math(EXPR _hl_scaled_timeout "${_hl_current_timeout} * ${HL_MATRIX_TIMEOUT_SCALE}")
    set_tests_properties(${_test} PROPERTIES
      TIMEOUT ${_hl_scaled_timeout}
      ENVIRONMENT HL_MATRIX_TIMEOUT_SCALE=${HL_MATRIX_TIMEOUT_SCALE})
  endforeach()
endfunction()

# hl_compat_suite(<label> <bin-subdir> <suite-source-dir> [SERIAL] [LOCKS ...])
#   Adds `compat.<label>` running the whole suite through matrix-runner, with
#   LABELS <label> so `ctest -L compat-ipc` selects exactly that suite.
function(hl_compat_suite label bindir suitedir)
  cmake_parse_arguments(C "SERIAL" "" "LOCKS;ARGS;FIXTURE_DIRS" ${ARGN})
  if(NOT C_FIXTURE_DIRS)
    set(C_FIXTURE_DIRS "${bindir}")
  endif()

  # A shard must be able to build only the guest corpus it executes. CTest
  # does not build test dependencies, and the global guest-fixtures target is
  # intentionally an ALL target for full builds, so expose an explicit narrow
  # target for every suite label.
  get_property(_guest_outputs GLOBAL PROPERTY HL_GUEST_ALL_OUTPUTS)
  set(_suite_outputs "")
  foreach(_output IN LISTS _guest_outputs)
    foreach(_fixture_dir IN LISTS C_FIXTURE_DIRS)
      string(FIND "${_output}" "${_fixture_dir}/" _prefix)
      if(_prefix EQUAL 0)
        list(APPEND _suite_outputs "${_output}")
        break()
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES _suite_outputs)
  if(NOT _suite_outputs)
    message(FATAL_ERROR
      "compat-${label}-fixtures: no guest outputs under ${C_FIXTURE_DIRS}")
  endif()
  add_custom_target(compat-${label}-fixtures DEPENDS ${_suite_outputs})

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
  # Last, so it scales the TIMEOUT set above rather than being overwritten by it.
  hl_matrix_timeout_scale(compat.${label})
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
hl_compat_suite(isa-x86-64   ${HL_COMPAT}/isa          tests/compat/isa/x86_64
                FIXTURE_DIRS ${HL_COMPAT}/isa/x86_64)
hl_compat_suite(isa-aarch64  ${HL_COMPAT}/isa          tests/compat/isa/aarch64
                FIXTURE_DIRS ${HL_COMPAT}/isa/aarch64)

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

# --- the --repeat 10 "extended" variants ------------------------------------
# Makefile compat-core-workload-extended / compat-soak-extended: the SAME case
# set run ten times over, to shake out state that only leaks across repeats.
# They are hour-scale, so they carry their own label and are NOT in `compat`:
# select them explicitly with `ctest -L compat-extended`.
hl_compat_suite(core-workload-extended ${HL_COMPAT}/core/workload
                tests/compat/core/workload SERIAL ARGS --repeat 10)
hl_compat_suite(soak-extended ${CMAKE_BINARY_DIR}/soak tests/soak
                SERIAL ARGS --repeat 10)
foreach(_t compat.core-workload-extended compat.soak-extended)
  set_tests_properties(${_t} PROPERTIES LABELS "compat-extended")
endforeach()

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
# deny-icmp is a seccomp/BPF wrapper, so Linux-host only -- as `make
# typed-network-icmp` always was.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME compat.network-icmp-bridge
    COMMAND $<TARGET_FILE:deny-icmp> $<TARGET_FILE:matrix-runner> ${HL_MATRIX_BRIDGE}
            ${HL_ENGINE_AARCH64} ${HL_COMPAT}/network/aarch64
            ${HL_ENGINE_X86_64}  ${HL_COMPAT}/network/x86_64
            ${HL_TESTS}/compat/network icmp-bridge)
  set_tests_properties(compat.network-icmp-bridge PROPERTIES
    LABELS "compat;compat-network" RESOURCE_LOCK "hl-guest;hl-net" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # Runs the same suite through matrix-runner, so it needs the same budget. It
  # sets no TIMEOUT of its own, so the scale applies to CTest's 1500s default.
  hl_matrix_timeout_scale(compat.network-icmp-bridge)
endif()
