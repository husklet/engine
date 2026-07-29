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

# The shell. OPTIONAL, and found exactly once: cmake/Phase3CiConfig.cmake and
# cmake/LaneParity.cmake used to repeat this call with REQUIRED, so three
# separate sites could fail a configure over a tool that only two CTest cases
# and a handful of developer/release targets actually need. A Windows host has
# no bash unless MSYS2/Git-for-Windows is installed, and nothing in the compile
# graph needs one (cmake/ArchiveStamp.cmake's command is only pulled in by the
# Linux and Darwin production lanes). So: find it, say so when it is missing,
# and let each bash-backed target/test gate on it -- the pattern
# cmake/Format.cmake, cmake/Lint.cmake and cmake/RustLint.cmake already use for
# clang-format, the linter and cargo.
find_program(HL_BASH_EXECUTABLE NAMES bash)
if(NOT HL_BASH_EXECUTABLE)
  message(STATUS
    "bash not found -- the shell-backed CTest cases (unit.ci-workflow-invariants, "
    "unit.publish-gating, gate.ci-lane-parity, gate.archive-closure) and the "
    "crate-archive targets (check-crate-archives, refresh-crate-archives*) are NOT "
    "registered. The libraries, engines and runner still build. Install MSYS2 or "
    "Git for Windows to get them.")
endif()

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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(HL_PACKAGE_ARCH_DIR ${CMAKE_BINARY_DIR}/package/windows-${HL_HOST_ARCH})
else()
  set(HL_PACKAGE_ARCH_DIR ${CMAKE_BINARY_DIR}/package/linux-${HL_HOST_ARCH})
endif()

# Whether the C link needs an EXPLICIT archive group. The engine archives are
# mutually recursive, so GNU ld has to be handed --start-group/--end-group,
# which CMake spells $<LINK_GROUP:RESCAN,...>. Two of this tree's linkers reject
# that generator expression at CONFIGURE time -- "Feature 'RESCAN' ... is not
# supported for the 'C' link language" -- and neither needs it, because both
# rescan archives to a fixed point on their own:
#
#   * Apple's linker, on a Darwin host.
#   * the MSVC linker, reached through the clang driver when the target ABI is
#     *-windows-msvc (cmake/toolchains/x86_64-windows-msvc.cmake). Note this is
#     an ABI test and NOT a host-OS test: the mingw-w64 Windows toolchain drives
#     ld.lld with the GNU driver and DOES need the group.
#
# Named once here because three call sites need the same answer and each had
# been deriving it separately -- Phase 4's from the host OS alone, which was
# right until a second Windows ABI existed.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_C_COMPILER_TARGET MATCHES "windows-msvc")
  set(HL_LINK_GROUP_RESCAN FALSE)
else()
  set(HL_LINK_GROUP_RESCAN TRUE)
endif()

# ---------------------------------------------------------------------------
# Static-analysis target family (lint, format checks, CI-quality gates).
# ---------------------------------------------------------------------------
option(HL_LINT "Run the C lint pipeline (clang-format, clang-tidy, cppcheck, custom C heuristics)" OFF)
option(HL_LINT_STRICT "Treat lint findings as hard errors" OFF)
option(HL_BUILD_NESTED_ENGINES "Build self-contained Linux engines for recursive execution tests" OFF)

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

# --- x86-64 same-ISA transliterator -----------------------------------------
# Inert on hosts other than x86-64.
option(HL_TRANSLIT "use the x86-64 same-ISA transliterator instead of the interpreter" OFF)

# --- flags in ONE place, as INTERFACE libraries (no directory-scope leakage) -
# Interface libs are used instead of add_compile_options() so that Phase-2
# production TUs (which need a DIFFERENT, minimal flag set) do not inherit the
# engine warning/visibility flags. CMAKE_BUILD_TYPE stays empty so CMake injects
# no -O/-DNDEBUG of its own. (-MMD -MP omitted: Ninja emits its own depfiles and
# dep flags never affect object contents.)
add_library(hl_cpp_flags INTERFACE)
target_include_directories(hl_cpp_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(hl_cpp_flags INTERFACE HL_ENABLE_LOGGING=${HL_DEBUG_LOGGING}
  HL_TRANSLIT_DEFAULT=$<BOOL:${HL_TRANSLIT}>)

add_library(hl_engine_cflags INTERFACE)
target_link_libraries(hl_engine_cflags INTERFACE hl_cpp_flags)
target_compile_options(hl_engine_cflags INTERFACE
  -O2 -g
  -std=c11
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes
  -Werror=implicit-function-declaration -Werror=implicit-int)

# -fvisibility=hidden is an ELF/Mach-O concept with no PE/COFF equivalent:
# Windows exports are per-symbol (__declspec(dllexport)) or by a .def file, and
# only mean anything at a DLL boundary. This project builds STATIC archives and
# executables exclusively, so on Windows the flag is at best a no-op and at
# worst a per-TU warning. Appended (rather than left inline above) so the flag
# ORDER on Linux and Darwin is byte-identical to what it was.
# Nothing replaces it: introducing a dllexport/dllimport HL_API macro is a
# public-ABI decision, not a Windows-port detail.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_compile_options(hl_engine_cflags INTERFACE -fvisibility=hidden)
endif()

# --- engine libraries -------------------------------------------------------
add_subdirectory(engine)

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
    add_subdirectory(tools)
    # These build and run guest programs, so they depend on the fixtures above.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Compat.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase3Gates.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Phase4Mac.cmake)

    # Engines + runners every compat suite needs, so a CI shard can build just
    # its own lane:
    #   cmake --build <dir> --target compat-engines compat-<lane>-fixtures
    # Declared here because the two hosts build the engines in different
    # phases (Phase 2 on Linux, Phase4Mac on Darwin).
    #
    # Not declared on Windows. A Windows host can now HAVE the fixture corpus --
    # it is cross-built from a WSL2 Linux and staged from a digest-checked cache
    # -- so HL_HAVE_GUEST_CC no longer implies the engines exist. The engines
    # named here are Phase-2 Linux production targets and Phase4Mac Darwin ones,
    # and neither phase runs on Windows, so declaring the target would give
    # `ninja compat-engines` a dependency with no rule.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
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
if(HL_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND HL_BASH_EXECUTABLE)
  add_custom_target(perf-macos-remote
    COMMAND ${HL_BASH_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/run_remote_macos_ctest.sh
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/build-remote-macos
            perf-macos
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
endif()

# Release/developer targets, all four of them shell scripts. Gated on the shell
# rather than on the OS: tools/refresh_crate_archives.sh refuses a non-Linux
# `uname -s` of its own accord, so the OS test would be a second, weaker copy of
# a rule the script already enforces. Absent bash they are simply not declared.
if(HL_BASH_EXECUTABLE)
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
endif()

# --- clang-format targets ---------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Format.cmake)

# --- lint target -------------------------------------------------------------
add_subdirectory(linter)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Lint.cmake)

# --- rustfmt / clippy as tests ------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/RustLint.cmake)
