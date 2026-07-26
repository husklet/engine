# ---------------------------------------------------------------------------
# hl-engine — the build.
#
# This is the authoritative and only build system. The hand-written Makefile it
# replaced is gone; CI invokes CTest labels and CMake targets exclusively, and
# cmake/CiLanes.cmake + gate.ci-lane-parity are what keep a lane from being
# silently dropped. CMake writes only into out-of-tree build directories.
#
# Layout:
#     libhl-engine.a       <- CORE_SOURCES
#     libhl-translator.a   <- IR_SOURCES        (incl. the ARM64 host assembler)
#     libhl-linux-abi.a    <- LINUX_ABI_SOURCES
#     libhl-host-fake.a    <- FAKE_HOST_SOURCES
#     libhl-host-linux.a   <- LINUX_HOST_SOURCES (Linux host only)
#     libhl-host-macos.a   <- MACOS_HOST_SOURCES (Darwin host only)
# Phase 2 adds the production engines, Phase 3 the test matrix, Phase 4 the
# macOS lane, install rules and packaging.
#
# Toolchain authority stays with nix: configure with one of the toolchain files
# under cmake/toolchains/, which read the compiler path from the environment
# variables the flake devShell exports (AARCH64_LINUX_CC / X86_64_LINUX_CC).
# Do not hardcode compiler paths here.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(hl_engine LANGUAGES C)

find_program(HL_BASH_EXECUTABLE NAMES bash REQUIRED)

# The host-CPU axis. DOCS.md section 1's "host platform" is really two axes, host
# OS and host CPU, and only the first ever had a name here. Name it once and
# normalise the spelling. Values match HL_HOST_CPU_* in src/host/host_cpu.h.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(HL_HOST_ARCH aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(HL_HOST_ARCH x86_64)
else()
  message(FATAL_ERROR
    "hl-engine has no host-CPU support for CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'. "
    "Supported host CPUs are aarch64 and x86_64.")
endif()

# Where the packaged host archives land. The name encodes the HOST platform, not
# a guest ISA: spelled inline, an x86_64 host overwrites the aarch64 artifact and
# gets published as the aarch64 crate asset.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(HL_PACKAGE_ARCH_DIR ${CMAKE_BINARY_DIR}/package/macos-${HL_HOST_ARCH})
else()
  set(HL_PACKAGE_ARCH_DIR ${CMAKE_BINARY_DIR}/package/linux-${HL_HOST_ARCH})
endif()

# ---------------------------------------------------------------------------
# Static-analysis target family (lint, format checks, CI-quality gates).
# ---------------------------------------------------------------------------
option(HL_LINT "Run the C lint pipeline (clang-format, clang-tidy, cppcheck, custom C heuristics)" OFF)
option(HL_LINT_STRICT "Treat lint findings as hard errors" OFF)

# clang-tidy and check-based tooling need a compile DB for reliable parsing.
if(HL_LINT)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for clang-tidy" FORCE)
endif()

# --- sanitizers -------------------------------------------------------------
# A sanitized run is a separate configure, not a second build inside this one:
#   cmake -G Ninja -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake -DHL_SANITIZE=ON
#   ninja -C build-asan && ctest --test-dir build-asan -L unit
option(HL_SANITIZE "build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(HL_SANITIZE)
  add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

set(HL_DEBUG_LOGGING "0" CACHE STRING "HL_ENABLE_LOGGING value")

# --- flags in ONE place, as INTERFACE libraries (no directory-scope leakage) -
# Interface libs are used instead of add_compile_options() so that Phase-2
# production TUs (which need a DIFFERENT, minimal flag set) do not inherit the
# engine warning/visibility flags. CMAKE_BUILD_TYPE stays empty so CMake injects
# no -O/-DNDEBUG of its own. (-MMD -MP omitted: Ninja emits its own depfiles and
# dep flags never affect object contents.)
add_library(hl_cpp_flags INTERFACE)
target_include_directories(hl_cpp_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(hl_cpp_flags INTERFACE HL_ENABLE_LOGGING=${HL_DEBUG_LOGGING})

add_library(hl_engine_cflags INTERFACE)
target_link_libraries(hl_engine_cflags INTERFACE hl_cpp_flags)
target_compile_options(hl_engine_cflags INTERFACE
  -O2 -g
  -std=c11
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes
  -Werror=implicit-function-declaration -Werror=implicit-int
  -fvisibility=hidden)

# --- source lists -----------------------------------------------------------
set(PROVIDER_SOURCES
  src/core/environment.c src/core/provider/client.c
  src/core/provider/demux.c src/core/provider/files.c src/core/provider/handles.c
  src/core/provider/namespace.c)

set(CORE_SOURCES
  src/core/bus.c src/core/checkpoint_channel.c src/core/cli.c src/core/config.c src/core/engine.c src/core/fatal.c
  src/core/host_services.c src/core/launch.c src/core/log.c
  src/core/options.c src/core/target/bus.c src/core/target/native.c
  src/core/target/run.c src/core/target/services.c ${PROVIDER_SOURCES})

set(IR_SOURCES
  src/translator/arena.c src/translator/digest.c src/translator/guest_fetch.c
  src/translator/identity.c src/translator/persist.c src/translator/reloc.c
  src/translator/window.c src/translator/guest/x86_64/decode.c
  src/translator/guest/x86_64/address.c
  src/translator/guest/x86_64/glue.c src/translator/guest/x86_64/avx.c
  src/translator/host/aarch64/asm.c
  src/translator/guest/x86_64/cpuid.c src/translator/guest/x86_64/cmpxchg.c
  src/translator/guest/x86_64/legacy.c
  src/translator/guest/x86_64/lower/alu.c
  src/translator/guest/x86_64/lower/crypto.c
  src/translator/guest/x86_64/lower/mov.c
  src/translator/guest/x86_64/lower/repstr.c
  src/translator/guest/x86_64/lower/shift.c
  src/translator/guest/x86_64/lower/sse4x.c
  src/translator/guest/x86_64/lower/trace.c
  src/translator/guest/x86_64/lower/x87.c
  src/translator/guest/x86_64/lower/x87_stack.c
  src/translator/guest/x86_64/rep.c
  src/translator/guest/x86_64/rotate.c
  src/translator/guest/x86_64/x87math.c
  src/translator/guest/x86_64/x87state.c
  src/translator/guest/aarch64/signal.c
  src/translator/guest/x86_64/signal.c
  src/translator/guest/x86_64/operand.c
  src/translator/guest/x86_64/flags.c)

set(LINUX_ABI_SOURCES
  src/linux_abi/affinity.c src/linux_abi/container/key.c src/linux_abi/container/pidmap.c
  src/linux_abi/container/ports.c src/linux_abi/container/snapshot.c
  src/linux_abi/container/vfs/gmap.c src/linux_abi/container/shm.c src/linux_abi/device.c
  src/linux_abi/dns.c src/linux_abi/image.c src/linux_abi/logical_vma.c
  src/linux_abi/fdcache.c
  src/linux_abi/epoll.c src/linux_abi/eventfd.c src/linux_abi/fork_codec.c
  src/linux_abi/inotify.c src/linux_abi/pipe.c src/linux_abi/placement.c
  src/linux_abi/errno.c src/linux_abi/limits.c src/linux_abi/linux_abi.c src/linux_abi/number.c
  src/linux_abi/open_plan.c src/linux_abi/parse.c src/linux_abi/readonly.c
  src/linux_abi/seccomp_vm.c src/linux_abi/shared.c src/linux_abi/stat.c src/linux_abi/watch.c
  src/linux_abi/xattr.c
  src/linux_abi/syscall/misc.c)

set(FAKE_HOST_SOURCES src/host/fake/host.c)

set(COMMON_HOST_SOURCES
  src/host/child.c src/host/fork_wire.c src/host/private.c src/host/range.c
  src/host/resolve.c src/host/sync.c)

set(LINUX_HOST_SOURCES
  src/host/linux/directory.c src/host/linux/host.c src/host/linux/process.c
  src/host/linux/range.c src/host/linux/system.c src/host/clock.c src/host/file.c
  ${COMMON_HOST_SOURCES})

set(MACOS_HOST_SOURCES
  src/host/macos/directory.c src/host/macos/host.c src/host/macos/process.c
  src/host/macos/range.c src/host/macos/system.c)

# --- archives ---------------------------------------------------------------
# Each links the engine flag interface PRIVATE so the exact ENGINE_CFLAGS apply.
add_library(hl-engine     STATIC ${CORE_SOURCES})
add_library(hl-translator STATIC ${IR_SOURCES})
add_library(hl-linux-abi  STATIC ${LINUX_ABI_SOURCES})
add_library(hl-host-fake  STATIC ${FAKE_HOST_SOURCES})
foreach(_lib hl-engine hl-translator hl-linux-abi hl-host-fake)
  target_link_libraries(${_lib} PRIVATE hl_engine_cflags)
endforeach()

# libhl-host-linux is Linux-host-specific (src/host/linux/*); a macOS host
# builds libhl-host-macos instead.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(hl-host-linux STATIC ${LINUX_HOST_SOURCES})
  target_link_libraries(hl-host-linux PRIVATE hl_engine_cflags)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  add_library(hl-host-macos STATIC
    ${MACOS_HOST_SOURCES} ${COMMON_HOST_SOURCES}
    src/host/clock.c src/host/file.c)
  target_link_libraries(hl-host-macos PRIVATE hl_engine_cflags)
endif()

# --- Phase 2: Linux production engines + dual embedded activation archive ----
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase2Production.cmake)

# --- Phase 3/4 -------------------------------------------------------------
option(HL_BUILD_TESTS "build the unit + compat + e2e test matrix" ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Codesign.cmake)

if(HL_BUILD_TESTS)
  enable_testing()
  # Host-side unit tests need no guest cross compiler.
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Units.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3CiConfig.cmake)
  # Sets HL_HAVE_GUEST_CC and returns early when the Linux cross compilers are
  # absent (e.g. a plain `nix build`, whose sandbox exports no *_LINUX_CC).
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GuestFixtures.cmake)
  if(HL_HAVE_GUEST_CC)
    # These build and run guest programs, so they depend on the fixtures above.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Compat.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Gates.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase4Mac.cmake)

    # Engines + runners every compat suite needs, so a CI shard can build just
    # its own lane:
    #   cmake --build <dir> --target compat-engines compat-<lane>-fixtures
    # Declared here because the two hosts build the engines in different
    # phases (Phase 2 on Linux, Phase4Mac on Darwin).
    set(_compat_engines hl-engine-linux-aarch64 hl-engine-linux-x86_64
        matrix-runner forkserver-runner)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      list(APPEND _compat_engines linux-production-remote-supervisor deny-icmp)
    else()
      list(APPEND _compat_engines hl-remote-supervisor)
    endif()
    add_custom_target(compat-engines DEPENDS ${_compat_engines})
  endif()
endif()

# --- Phase 4: install rules, pkg-config, packaging smoke tests -------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase4Install.cmake)

# Lane parity covers the complete registry, including Phase4Install's package
# labels.  A host-unit-only configure intentionally has no guest cross
# compilers and therefore no compat/production lanes to audit.
if(HL_BUILD_TESTS AND HL_HAVE_GUEST_CC)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LaneParity.cmake)
endif()

# Preserve the Linux-to-macOS transport entry point.  The real Darwin build
# and CTest registry remain authoritative; this target only drives them over
# OrbStack's shared-checkout `mac` command.
if(HL_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_target(perf-macos-remote
    COMMAND ${HL_BASH_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/run_remote_macos_ctest.sh
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/build-remote-macos
            perf-macos
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
endif()

add_custom_target(check-crate-archives
  COMMAND ${HL_BASH_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/check_crate_archives.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL)

add_custom_target(refresh-crate-archives
  COMMAND ${CMAKE_COMMAND} -E env
          BUILD=${CMAKE_BINARY_DIR}/crate-archives
          ${HL_BASH_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/refresh_crate_archives.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL)

# The halves, for a host that is not both aarch64 Linux and Apple silicon.
foreach(_half linux darwin provenance)
  add_custom_target(refresh-crate-archives-${_half}
    COMMAND ${CMAKE_COMMAND} -E env
            BUILD=${CMAKE_BINARY_DIR}/crate-archives
            ${HL_BASH_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/refresh_crate_archives.sh --${_half}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
endforeach()

# --- clang-format targets ---------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Format.cmake)

# --- lint target -------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Lint.cmake)

# --- rustfmt / clippy as tests ------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/RustLint.cmake)
