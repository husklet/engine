# ---------------------------------------------------------------------------
# hl-engine — CMake build, PHASE 1 ONLY (core independent static libraries).
#
# This file is ADDITIVE and NON-GATING. It lives alongside the hand-written
# Makefile, which remains the authoritative build. Nothing here modifies or
# replaces a make target; `make` keeps working exactly as before. CMake writes
# only into its own out-of-tree build directory.
#
# Scope (mirrors the Makefile SOURCES vars, see CORE_SOURCES / IR_SOURCES /
# LINUX_ABI_SOURCES / FAKE_HOST_SOURCES / LINUX_HOST_SOURCES):
#     libhl-engine.a       <- CORE_SOURCES
#     libhl-translator.a   <- IR_SOURCES        (incl. aarch64 host codegen)
#     libhl-linux-abi.a    <- LINUX_ABI_SOURCES
#     libhl-host-fake.a    <- FAKE_HOST_SOURCES
#     libhl-host-linux.a   <- LINUX_HOST_SOURCES (Linux host only)
#
# NOT in scope this phase: unity production engines, the test matrix,
# codesigning, packaging, the Rust crate.
#
# Toolchain authority stays with nix: configure with one of the toolchain
# files under cross/, which read the compiler path from the same environment
# variables the flake devShell exports and the Makefile consumes
# (AARCH64_LINUX_CC / X86_64_LINUX_CC). Do not hardcode compiler paths here.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(hl_engine_phase1 LANGUAGES C)

# --- sanitizers -------------------------------------------------------------
# The Makefile's `sanitize` target re-invokes make into a second BUILD dir with
# ASan+UBSan CFLAGS and runs the unit suite. The CMake equivalent is a separate
# configure, which is the idiomatic form and avoids a recursive build:
#   cmake -G Ninja -B build-asan -DCMAKE_TOOLCHAIN_FILE=cross/aarch64-linux.cmake -DHL_SANITIZE=ON
#   ninja -C build-asan && ctest --test-dir build-asan -L unit
option(HL_SANITIZE "build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(HL_SANITIZE)
  # Matches the Makefile's flags exactly (-O1 -g -fno-omit-frame-pointer).
  add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# Mirror the Makefile's DEBUG knob (CPPFLAGS: -DHL_ENABLE_LOGGING=$(DEBUG)).
set(HL_DEBUG_LOGGING "0" CACHE STRING "HL_ENABLE_LOGGING value (Makefile DEBUG)")

# --- flags in ONE place, as INTERFACE libraries (no directory-scope leakage) -
# hl_cpp_flags  == Makefile CPPFLAGS      := -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG)
# hl_engine_cflags == CPPFLAGS + ENGINE_CFLAGS
#                  := $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fvisibility=hidden
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
  -fvisibility=hidden)

# --- source lists (kept identical to the Makefile SOURCES variables) --------
set(PROVIDER_SOURCES
  src/core/environment.c src/core/provider/client.c
  src/core/provider/demux.c src/core/provider/files.c src/core/provider/handles.c
  src/core/provider/namespace.c)

set(CORE_SOURCES
  src/core/bus.c src/core/cli.c src/core/config.c src/core/engine.c src/core/fatal.c
  src/core/host_services.c src/core/launch.c src/core/log.c
  src/core/options.c src/core/target/bus.c src/core/target/native.c
  src/core/target/run.c src/core/target/services.c ${PROVIDER_SOURCES})

set(IR_SOURCES
  src/translator/arena.c src/translator/codegen.c src/translator/digest.c
  src/translator/identity.c src/translator/persist.c src/translator/reloc.c
  src/translator/window.c src/translator/guest/x86_64/decode.c
  src/translator/guest/x86_64/address.c src/translator/host/aarch64/codegen.c
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
  src/translator/guest/x86_64/flags.c
  src/translator/host/x86_64/codegen.c src/translator/ir/interpreter.c
  src/translator/ir/ir.c)

set(LINUX_ABI_SOURCES
  src/linux_abi/affinity.c src/linux_abi/container/key.c src/linux_abi/container/pidmap.c
  src/linux_abi/container/ports.c src/linux_abi/container/snapshot.c
  src/linux_abi/container/vfs/gmap.c src/linux_abi/container/shm.c src/linux_abi/device.c
  src/linux_abi/dns.c src/linux_abi/image.c
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

# --- archives ---------------------------------------------------------------
# CMake names a target `foo` as libfoo.a, matching the Makefile artifacts.
# Each links the engine flag interface PRIVATE so the exact ENGINE_CFLAGS apply.
add_library(hl-engine     STATIC ${CORE_SOURCES})
add_library(hl-translator STATIC ${IR_SOURCES})
add_library(hl-linux-abi  STATIC ${LINUX_ABI_SOURCES})
add_library(hl-host-fake  STATIC ${FAKE_HOST_SOURCES})
foreach(_lib hl-engine hl-translator hl-linux-abi hl-host-fake)
  target_link_libraries(${_lib} PRIVATE hl_engine_cflags)
endforeach()

# libhl-host-linux is Linux-host-specific (src/host/linux/*). On a macOS host
# the Makefile builds libhl-host-macos instead; that host lib is a later phase.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(hl-host-linux STATIC ${LINUX_HOST_SOURCES})
  target_link_libraries(hl-host-linux PRIVATE hl_engine_cflags)
endif()

# --- Phase 2: Linux production engines + dual embedded activation archive ----
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase2Production.cmake)

# --- Phase 3/4 -------------------------------------------------------------
option(HL_BUILD_TESTS "build the unit + compat + e2e test matrix" ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Codesign.cmake)

if(HL_BUILD_TESTS)
  enable_testing()
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Units.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GuestFixtures.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Compat.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Gates.cmake)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase4Mac.cmake)
endif()

# --- Phase 4: install rules, pkg-config, packaging smoke tests -------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase4Install.cmake)

# --- clang-format targets ---------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Format.cmake)
