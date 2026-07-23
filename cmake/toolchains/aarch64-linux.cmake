# CMake toolchain file — aarch64 Linux.
#
# Nix is the single toolchain authority. The C compiler is taken from
# $AARCH64_LINUX_CC, exported by the flake devShell (see flake.nix devShells:
# on an aarch64-linux host this is the native `${pkgs.stdenv.cc}/bin/cc`; on a
# darwin host it is the aarch64 cross gcc). This is the SAME variable the
# Makefile reads at lines 64-75. Nothing here is a hardcoded store path.
#
# Usage (inside `nix develop`):
#     cmake -G Ninja -B build-cmake-aarch64 \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake
#     ninja -C build-cmake-aarch64

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if("$ENV{AARCH64_LINUX_CC}" STREQUAL "")
  message(FATAL_ERROR
    "AARCH64_LINUX_CC is not set. Enter the nix devShell first: `nix develop`.")
endif()

# The var is normally a bare compiler path, but the flake's *_STATIC_CC variants
# append `-L...` link args. Split defensively: first token = compiler, rest =
# extra front-end args (harmless for archive-only compilation).
separate_arguments(_hl_cc UNIX_COMMAND "$ENV{AARCH64_LINUX_CC}")
list(POP_FRONT _hl_cc CMAKE_C_COMPILER)
if(_hl_cc)
  string(JOIN " " _hl_cc_extra ${_hl_cc})
  set(CMAKE_C_FLAGS_INIT "${_hl_cc_extra}")
endif()
