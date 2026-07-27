# CMake toolchain file — x86-64 Windows (mingw-w64 clang, MSYS2 UCRT/CLANG64).
#
# NOT a nix-authored toolchain, unlike its two siblings, and the asymmetry is
# deliberate rather than an omission. Nix does not run natively on Windows and
# supplies no compiler here, so where aarch64-linux.cmake and x86_64-linux.cmake
# FATAL_ERROR on an unset $AARCH64_LINUX_CC / $X86_64_LINUX_CC, this file reads an
# OPTIONAL $WINDOWS_X86_64_CC and otherwise takes clang from PATH. Pretending to a
# nix contract that does not exist here would be worse than saying so.
#
# Inside an MSYS2 CLANG64 or UCRT64 shell no toolchain file is needed at all: the
# `clang` on PATH already IS the intended host compiler, and a bare
# `cmake -G Ninja -B build-win` works (DOCS.md 7.0 makes the same point for the
# Linux and Darwin natives). This file exists to (a) pin the compiler when PATH
# also carries MSVC or a stray Chocolatey LLVM, and (b) hold the static-runtime
# link decision below, which is not a compiler-flag question and so has no home
# in CMakeLists.txt's hl_engine_cflags.
#
# MSVC is not a supported alternative: the engine source is GNU C and three
# load-bearing constructs (__attribute__((constructor)), file-scope __asm__,
# __uint128_t) are rejected by it. See docs/windows/toolchain.md 1.
#
# Usage (any shell, with clang on PATH or WINDOWS_X86_64_CC set):
#     cmake -G Ninja -B build-win \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-windows.cmake
#     cmake --build build-win --target hl-translator

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Same defensive split as the two Linux files: the variable is normally a bare
# compiler path, but may carry extra front-end args.
if(NOT "$ENV{WINDOWS_X86_64_CC}" STREQUAL "")
  separate_arguments(_hl_cc UNIX_COMMAND "$ENV{WINDOWS_X86_64_CC}")
  list(POP_FRONT _hl_cc CMAKE_C_COMPILER)
  if(_hl_cc)
    string(JOIN " " _hl_cc_extra ${_hl_cc})
    set(CMAKE_C_FLAGS_INIT "${_hl_cc_extra}")
  endif()
else()
  find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
endif()

# Static runtime. mingw-w64's C runtime is the OS's UCRT either way; what -static
# removes is the dependency on libwinpthread-1.dll (plus libunwind/libc++ under
# CLANG64). Not cosmetic: the test harness copies binaries into scratch
# directories and runs them from varying working directories, and Windows
# resolves DLLs relative to the image and the cwd, so a dynamic link turns a copy
# into a load failure that the build tree never reproduces.
#
# -static rather than `-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic`, whose ordering is
# documented as being overridden by later implicit dependencies.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Deliberately absent:
#  * -fvisibility=hidden removal — an hl_engine_cflags question, and a toolchain
#    file cannot subtract from an INTERFACE library. Handled in CMakeLists.txt.
#  * -municode — it would require a wmain in src/runner/main.c and every
#    tools/*_runner.c, threading wchar_t** through byte-oriented portable code.
#    The problem it solves (non-ASCII argv) is a UTF-8 activeCodePage manifest's
#    job, which is a per-target linker input. docs/windows/build-system.md 2.3.
#  * CMAKE_EXECUTABLE_SUFFIX — CMake sets .exe itself.
#  * CMAKE_FIND_ROOT_PATH_MODE_* — the two Linux files scope those to their cross
#    case. This file has no cross case: a Windows host cross-compiled from
#    elsewhere is not a supported configuration, so nothing must be kept from
#    probing the host. (Cross-compiling the Windows ARMS from Linux CI under
#    pkgsCross.mingwW64 is a separate, flake-side idea and would not use this file.)
