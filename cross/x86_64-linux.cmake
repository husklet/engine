# CMake toolchain file — x86_64 Linux (cross from an aarch64 host).
#
# Nix is the single toolchain authority. The C compiler is taken from
# $X86_64_LINUX_CC, exported by the flake devShell (see flake.nix devShells:
# `${linuxX86Compiler}`, the pkgsCross.gnu64 gcc). This is the SAME variable
# the Makefile reads at line 74. Nothing here is a hardcoded store path.
#
# NOTE: the Makefile's Phase-1 core .a targets (`make all` / `make
# linux-compile`) are built for the native host only; there is no make target
# that emits these five archives for a foreign arch. This toolchain file
# therefore demonstrates the nix-cross mechanism (and confirms the sources
# cross-compile cleanly), but has no make counterpart to symbol-diff against.
#
# Usage (inside `nix develop`):
#     cmake -G Ninja -B build-cmake-x86_64 \
#           -DCMAKE_TOOLCHAIN_FILE=cross/x86_64-linux.cmake
#     ninja -C build-cmake-x86_64

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if("$ENV{X86_64_LINUX_CC}" STREQUAL "")
  message(FATAL_ERROR
    "X86_64_LINUX_CC is not set. Enter the nix devShell first: `nix develop`.")
endif()

separate_arguments(_hl_cc UNIX_COMMAND "$ENV{X86_64_LINUX_CC}")
list(POP_FRONT _hl_cc CMAKE_C_COMPILER)
if(_hl_cc)
  string(JOIN " " _hl_cc_extra ${_hl_cc})
  set(CMAKE_C_FLAGS_INIT "${_hl_cc_extra}")
endif()

# Cross builds must not probe the host for libraries/headers.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
