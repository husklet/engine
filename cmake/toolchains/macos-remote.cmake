# Toolchain: build macOS (arm64) artifacts FROM the Linux side, by forwarding
# every compiler invocation to the macOS host via tools/remote/mac-cc.
#
# This replaces the Makefile's `MAC=mac` recipe prefix -- the one part of the
# Makefile that previously had no CMake equivalent, because it is remote
# execution rather than a toolchain. Modelling it as a compiler wrapper makes it
# an ordinary cross build.
#
# Usage (build dir MUST be inside the repo -- only that path is shared with the
# macOS host):
#   cmake -G Ninja -B build-macos -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-remote.cmake
#   ninja -C build-macos
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
get_filename_component(_hl_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_C_COMPILER "${_hl_root}/tools/remote/mac-cc" CACHE FILEPATH "remote macOS C compiler")
# The binutils live on the macOS side too, so forward them the same way -- CMake
# probes for ar/ranlib locally and would otherwise fail to find a usable pair.
set(CMAKE_AR      "${_hl_root}/tools/remote/mac-ar"      CACHE FILEPATH "remote macOS ar")
set(CMAKE_RANLIB  "${_hl_root}/tools/remote/mac-ranlib"  CACHE FILEPATH "remote macOS ranlib")
set(CMAKE_STRIP   "${_hl_root}/tools/remote/mac-strip"   CACHE FILEPATH "remote macOS strip")
set(CMAKE_NM      "${_hl_root}/tools/remote/mac-nm"      CACHE FILEPATH "remote macOS nm")
# Darwin-specific tooling CMake insists on locating during compiler detection.
set(CMAKE_INSTALL_NAME_TOOL "${_hl_root}/tools/remote/mac-install_name_tool" CACHE FILEPATH "remote install_name_tool")
set(CMAKE_OTOOL   "${_hl_root}/tools/remote/mac-otool"   CACHE FILEPATH "remote macOS otool")
set(CMAKE_LIBTOOL "${_hl_root}/tools/remote/mac-libtool" CACHE FILEPATH "remote macOS libtool")
# Search the host filesystem for programs, but never for libraries/headers --
# those must come from the macOS side.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM SEARCH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
