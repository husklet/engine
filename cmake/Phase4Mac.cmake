# ---------------------------------------------------------------------------
# Phase 4 — the macOS host lane.
#
# SCOPE, stated honestly. The Makefile has TWO mac paths:
#
#   (a) building ON macOS  (HOST=macos, MAC=env) — every `$(MAC) clang ...`
#       recipe is just `clang ...`. That is what this file expresses.
#   (b) building on Linux and driving a macOS machine through the OrbStack
#       `mac` command prefix (MAC=mac). That is a remote-execution transport,
#       not a toolchain: it runs a command on a *different host*. CMake has no
#       model for "compile this object on another machine", so this file does
#       NOT try to fake it. On a Linux host the mac lane is simply absent
#       (return below), exactly like `make` skipping it via HOST=linux.
#
# When configured on macOS, the Phase-1 archives are already the mac archives
# (the host compiler is clang), so this file only adds what is mac-specific:
# libhl-host-macos, the signed production engines, the dual archive, and the
# bridge-driven gates.
#
# EVERY executable here goes through hl_codesign() — see cmake/Codesign.cmake
# for why a missed signature is a silent SIGKILL rather than a build error.
# ---------------------------------------------------------------------------

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  return()
endif()

# --- libhl-host-macos.a -----------------------------------------------------
set(MACOS_HOST_SOURCES
  src/host/macos/directory.c src/host/macos/host.c src/host/macos/process.c
  src/host/macos/range.c src/host/macos/system.c)
add_library(hl-host-macos STATIC
  ${MACOS_HOST_SOURCES} ${COMMON_HOST_SOURCES} src/host/clock.c src/host/file.c)
target_link_libraries(hl-host-macos PRIVATE hl_engine_cflags)

set(HL_MAC_LIBS hl-engine hl-translator hl-linux-abi hl-host-macos)

# --- production engines -----------------------------------------------------
foreach(_arch aarch64 x86_64)
  string(TOUPPER ${_arch} _ISA)
  hl_object(mac_target_${_arch} src/core/target/${_arch}.c FLAGS -O2 UNITY)
  hl_object(mac_life_${_arch} src/core/lifecycle.c
    FLAGS -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_${_ISA} -O2)
  hl_object(mac_lifetarget_${_arch} src/core/target/${_arch}.c
    FLAGS -DHL_ENGINE_NO_MAIN=1 -O2 UNITY)

  add_executable(hl-engine-linux-${_arch}
    $<TARGET_OBJECTS:mac_target_${_arch}> $<TARGET_OBJECTS:mac_life_${_arch}>)
  target_link_libraries(hl-engine-linux-${_arch} PRIVATE ${HL_MAC_LIBS})
  set_target_properties(hl-engine-linux-${_arch} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/production)
  hl_codesign(hl-engine-linux-${_arch})
endforeach()

add_executable(hl-remote-supervisor tools/remote_supervisor.c)
target_compile_options(hl-remote-supervisor PRIVATE
  -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes)
set_target_properties(hl-remote-supervisor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/production)

# --- embedding / lifecycle runners -----------------------------------------
# One helper for the whole family: lifecycle, binding, stdio, dir, pty. The
# Makefile writes ~12 near-identical link+codesign recipes for these.
#   hl_mac_runner(<name> <runner-source> <arch> [SCENARIO <s>] [NO_ISA])
function(hl_mac_runner name source arch)
  cmake_parse_arguments(R "NO_ISA" "SCENARIO" "" ${ARGN})
  string(TOUPPER ${arch} _ISA)
  set(_flags -O2)
  if(NOT R_NO_ISA)
    list(APPEND _flags -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_${_ISA})
  endif()
  if(R_SCENARIO)
    list(APPEND _flags -DHL_LIFECYCLE_SCENARIO="${R_SCENARIO}")
  endif()
  hl_object(macrunner_${name} ${source} FLAGS ${_flags})
  add_executable(${name}
    $<TARGET_OBJECTS:macrunner_${name}>
    $<TARGET_OBJECTS:mac_lifetarget_${arch}>
    $<TARGET_OBJECTS:mac_life_${arch}>)
  target_link_libraries(${name} PRIVATE ${HL_MAC_LIBS})
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
  hl_codesign(${name})
endfunction()

foreach(_arch aarch64 x86_64)
  hl_mac_runner(lifecycle-${_arch} tools/lifecycle_e2e_runner.c ${_arch})
  hl_mac_runner(binding-${_arch}   tools/binding_e2e_runner.c   ${_arch})
  hl_mac_runner(stdio-${_arch}     tools/stdio_e2e_runner.c     ${_arch})
  hl_mac_runner(dir-${_arch}       tools/dir_e2e_runner.c       ${_arch})
  # Scenario-tagged lifecycle runners: the identity manifest
  # (tests/e2e/lifecycle.tsv) requires one distinct binary per scenario.
  foreach(_s exit signal clock force)
    hl_mac_runner(lifecycle-${_s}-${_arch} tools/lifecycle_e2e_runner.c ${_arch}
                  SCENARIO ${_s})
  endforeach()
endforeach()
hl_mac_runner(pty-aarch64 tools/pty_binding_e2e_runner.c aarch64 NO_ISA)

# --- mac dual-backend embedded archive -------------------------------------
add_library(hl-mac-embedded-objs OBJECT
  ${CORE_SOURCES} ${IR_SOURCES} ${LINUX_ABI_SOURCES}
  ${MACOS_HOST_SOURCES} ${COMMON_HOST_SOURCES} src/host/clock.c src/host/file.c)
target_link_libraries(hl-mac-embedded-objs PRIVATE hl_engine_cflags)
target_compile_options(hl-mac-embedded-objs PRIVATE -DHL_EMBEDDED_BUILD=1)

foreach(_arch aarch64 x86_64)
  string(TOUPPER ${_arch} _ISA)
  hl_object(macdual_target_${_arch} src/core/target/${_arch}.c
    FLAGS -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 -DHL_TARGET_NAMESPACE=${_arch} -O2 UNITY)
  hl_object(macdual_core_${_arch} src/core/lifecycle.c
    FLAGS -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=${_arch}
          -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_${_ISA} -O2)
endforeach()
hl_object(macdual_dispatch src/core/target/dual.c FLAGS -O2)
add_library(macdual_activation OBJECT src/core/activation.c)
target_link_libraries(macdual_activation PRIVATE hl_engine_cflags)

add_library(hl-engine-dual STATIC
  $<TARGET_OBJECTS:macdual_target_aarch64> $<TARGET_OBJECTS:macdual_target_x86_64>
  $<TARGET_OBJECTS:macdual_core_aarch64>   $<TARGET_OBJECTS:macdual_core_x86_64>
  $<TARGET_OBJECTS:macdual_dispatch>       $<TARGET_OBJECTS:macdual_activation>
  $<TARGET_OBJECTS:hl-mac-embedded-objs>)
set_target_properties(hl-engine-dual PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/package/macos-aarch64
  OUTPUT_NAME hl-engine)   # -> build/package/macos-aarch64/libhl-engine.a

add_executable(mac-dual-backend-link-test tools/dual_backend_e2e_runner.c)
target_include_directories(mac-dual-backend-link-test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(mac-dual-backend-link-test PRIVATE
  -Wl,-force_load,$<TARGET_FILE:hl-engine-dual>)
add_dependencies(mac-dual-backend-link-test hl-engine-dual)
set_target_properties(mac-dual-backend-link-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/package/macos-aarch64)
hl_codesign(mac-dual-backend-link-test)

# `make test-dual-backend` (Makefile 1708): one process force-loading the dual
# archive, launching both guest ISAs plus the exit70 / spin / output guests, to
# prove the two backends coexist in a single embedder.
add_test(NAME dual-backend.mac-link
  COMMAND $<TARGET_FILE:mac-dual-backend-link-test>
          ${CMAKE_BINARY_DIR}/e2e/guest-exit-aarch64
          ${CMAKE_BINARY_DIR}/e2e/guest-exit-x86_64
          ${CMAKE_BINARY_DIR}/e2e/guest-exit70-aarch64
          ${CMAKE_BINARY_DIR}/e2e/guest-exit70-x86_64
          ${CMAKE_BINARY_DIR}/e2e/guest-spin-aarch64
          ${CMAKE_BINARY_DIR}/e2e/guest-output-aarch64)
set_tests_properties(dual-backend.mac-link PROPERTIES
  LABELS "embedding" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --- mac host-service unit tests -------------------------------------------
# These open "/", "/tmp" and resolve localhost DNS, so they are NOT part of the
# sandboxed `unit` lane; they get their own label and a per-test timeout so a
# hung DNS/fork case names itself instead of stalling the whole run.
set(HL_MACOS_TEST_TIMEOUT 180 CACHE STRING "per-test cap for the macOS host-service tests")
function(hl_mac_test name)
  cmake_parse_arguments(M "" "" "SOURCES;LIBS;FLAGS;LINK" ${ARGN})
  add_executable(${name} ${M_SOURCES})
  target_link_libraries(${name} PRIVATE hl_engine_cflags ${M_LIBS} ${M_LINK})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
  target_compile_options(${name} PRIVATE ${M_FLAGS})
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  hl_codesign(${name})
  add_test(NAME macos.${name} COMMAND ${name})
  set_tests_properties(macos.${name} PROPERTIES
    LABELS "macos" TIMEOUT ${HL_MACOS_TEST_TIMEOUT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

set(_mac_svc src/host/macos/host.c src/host/macos/system.c src/host/sync.c
             src/host/resolve.c src/core/host_services.c src/core/log.c
             src/host/clock.c src/host/file.c src/host/private.c)
hl_mac_test(macos          SOURCES tests/unit/macos.c ${_mac_svc})
hl_mac_test(macos-destroy  SOURCES tests/unit/test_macos_destroy.c ${_mac_svc})
hl_mac_test(range-macos    SOURCES tests/unit/test_range.c src/host/range.c src/host/macos/range.c)
hl_mac_test(system-macos   SOURCES tests/unit/test_system.c src/host/macos/system.c src/host/private.c)
hl_mac_test(private-macos  SOURCES tests/unit/test_private.c src/host/private.c src/host/macos/system.c)
hl_mac_test(child-macos    SOURCES tests/unit/test_child.c src/host/child.c)
hl_mac_test(directory-macos SOURCES tests/unit/test_directory.c src/host/macos/directory.c)
hl_mac_test(directory-services-macos SOURCES tests/unit/test_directory_services.c
            LIBS ${HL_MAC_LIBS} FLAGS -DHL_TEST_MACOS=1)
hl_mac_test(process-macos  SOURCES tests/unit/test_process.c src/host/macos/process.c)
hl_mac_test(native-macos   SOURCES tests/unit/test_native.c LIBS ${HL_MAC_LIBS})
hl_mac_test(native-capacity-macos SOURCES tests/unit/test_native_capacity.c LIBS ${HL_MAC_LIBS})
hl_mac_test(resolve-services-macos SOURCES tests/unit/test_resolve_services.c
            LIBS hl-host-macos FLAGS -DHL_TEST_HOST_MACOS=1)
hl_mac_test(dns-fork-macos SOURCES tests/unit/test_dns_fork_macos.c src/linux_abi/dns.c)

# The Objective-C DNS fork test needs the ObjC language + Foundation.
enable_language(OBJC)
add_executable(dns-objc-fork-macos tests/unit/test_dns_objc_fork_macos.m src/linux_abi/dns.c)
target_include_directories(dns-objc-fork-macos PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(dns-objc-fork-macos PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(dns-objc-fork-macos PRIVATE "-framework Foundation")
set_target_properties(dns-objc-fork-macos PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
hl_codesign(dns-objc-fork-macos)
add_test(NAME macos.dns-objc-fork COMMAND dns-objc-fork-macos)
set_tests_properties(macos.dns-objc-fork PROPERTIES
  LABELS "macos" TIMEOUT ${HL_MACOS_TEST_TIMEOUT})

# --- the six mac e2e gates --------------------------------------------------
# FIREWALL CONSTRAINT (Makefile comment at line 1846): the host firewall
# reliably admits at most FOUR signed launches from one launching process, so
# the gates must be separate top-level processes and must never be joined
# through one umbrella process. CTest satisfies this natively: every add_test()
# is its own process, and each gate below stays under four launches. The gates
# use per-process isolation (unique /tmp/<pid> paths, per-child signals), so
# they may run concurrently with each other — but each holds `hl-mac-bridge`
# against the sequential bridge runner.
set(_BR $<TARGET_FILE:bridge-runner>)
set(_E2E ${CMAKE_BINARY_DIR}/e2e)

# The firewall identity audit that every gate runs first.
add_test(NAME e2e.lifecycle-identity COMMAND test_lifecycle_identity)

# Each bridge leg is its own CTest case: one launching process per case keeps
# the four-launch firewall budget trivially satisfied and names failures.
foreach(_arch aarch64 x86_64)
  add_test(NAME e2e.lifecycle-signal-exit-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:lifecycle-exit-${_arch}> --expect-exit 139
            ${_E2E}/guest-exit139-${_arch})
  add_test(NAME e2e.lifecycle-signal-fault-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:lifecycle-signal-${_arch}> --expect-signal 11
            ${_E2E}/guest-fault-${_arch})
  add_test(NAME e2e.lifecycle-control-clock-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:lifecycle-clock-${_arch}> --clock-spy
            ${_E2E}/clock-injected-${_arch})
  add_test(NAME e2e.lifecycle-control-force-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:lifecycle-force-${_arch}> --force-stop
            ${_E2E}/guest-spin-${_arch})
  add_test(NAME e2e.embedding-fd-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:binding-${_arch}> ${_E2E}/fd-binding-${_arch})
  add_test(NAME e2e.embedding-stdio-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:stdio-${_arch}> ${_E2E}/stdio-binding-${_arch})
  add_test(NAME e2e.embedding-dir-${_arch}
    COMMAND ${_BR} env $<TARGET_FILE:dir-${_arch}> ${_E2E}/dir-binding-${_arch})
endforeach()

# The bridge runner owns a remote process group and a shared capture mount; two
# concurrent bridge launches starve each other past the transport deadline
# (Makefile `.NOTPARALLEL: e2e-compat`). One lock expresses exactly that,
# instead of serialising the entire test run.
get_property(_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t ${_all_tests})
  if(_t MATCHES "^e2e\\.")
    set_tests_properties(${_t} PROPERTIES
      LABELS "e2e;e2e-mac" RESOURCE_LOCK hl-mac-bridge
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 1800)
  endif()
endforeach()

# jobserver hygiene gate
add_test(NAME e2e.bridge-jobserver
  COMMAND $<TARGET_FILE:bridge-jobserver-test> ${_BR} env
          $<TARGET_FILE:lifecycle-signal-aarch64> --expect-signal 11
          ${_E2E}/guest-fault-aarch64)
set_tests_properties(e2e.bridge-jobserver PROPERTIES
  LABELS "e2e;e2e-mac" RESOURCE_LOCK hl-mac-bridge
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ===========================================================================
# perf-macos (Makefile 2579)
# ===========================================================================
# Same case list and same perf-runner invocation as the Linux lane, but with
# the macOS thresholds (PERF_MAC_LIMIT_*) and the macOS production engines.
# The --host-os/--host-release/--host-arch triple is what perf-runner records
# in its report; the Makefile fills it from `$(MAC) uname`, which on a macOS
# host is a plain local uname -- exactly what CMake already knows.
# The sample counts live in the Linux-guarded section of Phase3Gates.cmake, so
# on a Darwin configure they are not set yet. Same defaults, same cache names.
set(HL_PERF_WARMUPS 3     CACHE STRING "perf warmup iterations")
set(HL_PERF_SAMPLES 25    CACHE STRING "perf samples")
set(HL_PERF_HEAVY_SAMPLES 7 CACHE STRING "perf samples for heavy cases")
set(HL_PERF_OP_SAMPLES 7  CACHE STRING "perf samples for OS-op cases")

set(PERF_MAC_LIMIT_startup         30000 25000)
set(PERF_MAC_LIMIT_compute         750000 650000)
set(PERF_MAC_LIMIT_syscall-startup 40000 30000)
set(PERF_MAC_LIMIT_syscall-1m      100000 80000)
set(PERF_MAC_LIMIT_fork-stress     8000000 7000000)
set(PERF_MAC_LIMIT_mmap            150000 120000)
set(PERF_MAC_LIMIT_file            100000 80000)
set(PERF_MAC_LIMIT_pipe            300000 250000)
set(PERF_MAC_LIMIT_event           350000 300000)
set(PERF_MAC_LIMIT_ipc-latency     200000 150000)
set(PERF_MAC_LIMIT_ipc-throughput  100000 80000)
set(PERF_MAC_LIMIT_translation     50000 40000)
set(PERF_MAC_LIMIT_warm-cache      750000 650000)

set(_HL_MAC_UNAME --host-os ${CMAKE_HOST_SYSTEM_NAME}
                  --host-release ${CMAKE_HOST_SYSTEM_VERSION}
                  --host-arch ${CMAKE_HOST_SYSTEM_PROCESSOR})

# hl_perf_mac(<case> <arch> <warmups> <samples> <payload> <expect-status>)
function(hl_perf_mac case arch warmups samples payload expect)
  list(GET PERF_MAC_LIMIT_${case} 0 _cold)
  list(GET PERF_MAC_LIMIT_${case} 1 _p99)
  add_test(NAME perf.mac-${case}-${arch}
    COMMAND $<TARGET_FILE:perf-runner> --label ${case}-${arch} ${_HL_MAC_UNAME}
            --warmups ${warmups} --samples ${samples} --expect ${expect}
            --max-cold-us ${_cold} --max-p99-us ${_p99} --
            ${CMAKE_BINARY_DIR}/production/hl-engine-linux-${arch} ${payload})
  set_tests_properties(perf.mac-${case}-${arch} PROPERTIES
    LABELS "perf;perf-macos" RUN_SERIAL TRUE TIMEOUT 1800
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

set(_MC ${CMAKE_BINARY_DIR}/compat)
set(_MP ${CMAKE_BINARY_DIR}/perf)
foreach(_arch aarch64 x86_64)
  hl_perf_mac(startup ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
              ${CMAKE_BINARY_DIR}/e2e/guest-exit-${_arch} 42)
  hl_perf_mac(compute ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
              ${_MC}/core/workload/${_arch}/busyloop 0)
  hl_perf_mac(syscall-startup ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
              ${_MC}/syscall/${_arch}/gettid 0)
  hl_perf_mac(syscall-1m ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_HEAVY_SAMPLES}
              ${_MP}/syscall-${_arch} 0)
  hl_perf_mac(fork-stress ${_arch} 1 ${HL_PERF_HEAVY_SAMPLES}
              ${_MC}/process/${_arch}/forkstorm 0)
  hl_perf_mac(translation ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_SAMPLES}
              ${_MP}/translate-${_arch} 0)
  foreach(_op mmap file pipe event ipc-latency ipc-throughput)
    hl_perf_mac(${_op} ${_arch} ${HL_PERF_WARMUPS} ${HL_PERF_OP_SAMPLES}
                ${_MP}/${_op}-${_arch} 0)
  endforeach()

  # warm-cache: a persistent code-cache directory, wiped first (Makefile
  # HL_PERF_CACHE_MAC). Two ordered steps, so it goes through RunSequence.
  list(GET PERF_MAC_LIMIT_warm-cache 0 _wc_cold)
  list(GET PERF_MAC_LIMIT_warm-cache 1 _wc_p99)
  add_test(NAME perf.mac-warm-cache-${_arch}
    COMMAND ${CMAKE_COMMAND}
      "-DCMD0=${CMAKE_COMMAND} -E rm -rf ${_MP}/cache-warm-mac-${_arch}"
      "-DCMD1=$<TARGET_FILE:perf-runner> --label mac-warm-cache-${_arch} --host-os ${CMAKE_HOST_SYSTEM_NAME} --host-release ${CMAKE_HOST_SYSTEM_VERSION} --host-arch ${CMAKE_HOST_SYSTEM_PROCESSOR} --warmups ${HL_PERF_WARMUPS} --samples ${HL_PERF_SAMPLES} --max-cold-us ${_wc_cold} --max-p99-us ${_wc_p99} -- $<TARGET_FILE:config-e2e-runner> env ${CMAKE_BINARY_DIR}/production/hl-engine-linux-${_arch} ${_MP}/translate-${_arch} 0 1 ${_MP}/cache-warm-mac-${_arch}"
      -P ${CMAKE_SOURCE_DIR}/cmake/RunSequence.cmake)
  set_tests_properties(perf.mac-warm-cache-${_arch} PROPERTIES
    LABELS "perf;perf-macos" RUN_SERIAL TRUE TIMEOUT 1800
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_test(NAME perf.mac-resource-${_arch}
    COMMAND ${CMAKE_BINARY_DIR}/production/hl-engine-linux-${_arch}
            ${_MP}/resource-${_arch})
  set_tests_properties(perf.mac-resource-${_arch} PROPERTIES
    LABELS "perf;perf-macos" RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
