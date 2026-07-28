# CMake toolchain file — x86-64 Windows, MSVC ABI, compiled by clang.
#
# The SECOND Windows toolchain, beside x86_64-windows.cmake. Why a second file
# rather than a variant of the first or a -D option on it:
#
#   * almost nothing survives the switch. The compiler is invoked with a
#     different --target, the archiver is llvm-lib instead of llvm-ar and takes
#     /OUT: instead of `qc`, static libraries are named `foo.lib` instead of
#     `libfoo.a`, the include search has to be pointed at the MSVC and Windows
#     SDK roots by hand, and the sibling file's one substantive decision --
#     -static, to drop libwinpthread-1.dll -- is meaningless here because there
#     is no winpthreads. An if/else covering that is not a variant of a file, it
#     is two files sharing a name.
#   * a -D option would be worse. The ABI would then be a property of a build
#     directory's cache rather than of its configure line, and the two ABIs
#     produce archives that link into different worlds while having similar
#     names. Making the choice a toolchain file makes it impossible to change
#     without reconfiguring.
#
# What this target is FOR. rustup's default host triple on Windows is
# x86_64-pc-windows-msvc, so the Rust crate at pkgs/rust is built against the
# MSVC ABI on a stock machine. mingw-w64 static archives are not linkable there:
# GNU-format archives, mingw CRT references, and a second C runtime alongside
# the one rustc already links. So the crate's archive has to be built for this
# target, and clang is what can do it -- the engine is GNU C (file-scope
# __asm__, __uint128_t, __attribute__((constructor))) that cl.exe rejects, while
# clang accepts all three AND emits MSVC-ABI COFF.
#
# What it costs: cmake/toolchains/msvc-posix/, which supplies the POSIX seam
# mingw-w64 ships and the MSVC target does not. See that directory's headers.
#
# Usage (any shell; nothing needs to be pre-sourced):
#     cmake -G Ninja -B build-win-msvc \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-windows-msvc.cmake
#
# Overrides, all optional:
#     WINDOWS_X86_64_MSVC_CC   the clang to use (else clang from PATH)
#     HL_MSVC_TOOLS_DIR        the MSVC toolset root (else discovered)
#     HL_WINDOWS_SDK_DIR       the Windows Kits root (else discovered)
#     HL_WINDOWS_SDK_VERSION   e.g. 10.0.22621.0 (else the newest found)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# --- the compiler ----------------------------------------------------------
# Same defensive split as the three sibling toolchain files: the variable is
# normally a bare compiler path but may carry extra front-end args.
if(NOT "$ENV{WINDOWS_X86_64_MSVC_CC}" STREQUAL "")
  separate_arguments(_hl_cc UNIX_COMMAND "$ENV{WINDOWS_X86_64_MSVC_CC}")
  list(POP_FRONT _hl_cc CMAKE_C_COMPILER)
  if(_hl_cc)
    string(JOIN " " _hl_cc_extra ${_hl_cc})
  endif()
else()
  find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
endif()

# The triple is set as a compiler PROPERTY rather than as a bare --target in the
# flags. CMake passes CMAKE_C_COMPILER_TARGET to its own compiler-ABI probe as
# well as to every compile, so a mismatch between the probe's ABI and the
# build's cannot arise.
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)

# --- MSVC and Windows SDK roots -------------------------------------------
# clang's MSVC auto-detection finds nothing under MSYS2 (measured: a bare
# --target=x86_64-pc-windows-msvc compile fails on <fcntl.h>), and requiring a
# Developer Command Prompt would make a plain `cmake -B ...` fail in a way that
# says nothing about why. So the roots are discovered here, with an override for
# each, and a FATAL_ERROR naming the missing piece if discovery fails.
#
# An already-populated INCLUDE/LIB environment -- i.e. this IS running inside a
# Developer Command Prompt -- is honoured and short-circuits discovery, because
# a developer who set those meant them.
if(NOT DEFINED HL_MSVC_TOOLS_DIR AND NOT "$ENV{VCToolsInstallDir}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" HL_MSVC_TOOLS_DIR)
endif()

if(NOT DEFINED HL_MSVC_TOOLS_DIR)
  find_program(_hl_vswhere NAMES vswhere
    PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
          "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
    NO_DEFAULT_PATH)
  if(_hl_vswhere)
    execute_process(
      COMMAND "${_hl_vswhere}" -latest -products * -nologo
              -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
              -property installationPath
      OUTPUT_VARIABLE _hl_vs_root OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_hl_vs_root)
      file(TO_CMAKE_PATH "${_hl_vs_root}" _hl_vs_root)
      # The toolset is versioned and there may be several; take the newest,
      # which is what a Developer Command Prompt's default would select.
      file(GLOB _hl_toolsets "${_hl_vs_root}/VC/Tools/MSVC/*")
      list(SORT _hl_toolsets)
      list(REVERSE _hl_toolsets)
      list(GET _hl_toolsets 0 HL_MSVC_TOOLS_DIR)
    endif()
  endif()
endif()

if(NOT DEFINED HL_WINDOWS_SDK_DIR)
  foreach(_hl_candidate "$ENV{ProgramFiles\(x86\)}/Windows Kits/10" "$ENV{ProgramFiles}/Windows Kits/10")
    file(TO_CMAKE_PATH "${_hl_candidate}" _hl_candidate)
    if(IS_DIRECTORY "${_hl_candidate}/Include")
      set(HL_WINDOWS_SDK_DIR "${_hl_candidate}")
      break()
    endif()
  endforeach()
endif()

if(NOT DEFINED HL_WINDOWS_SDK_VERSION AND HL_WINDOWS_SDK_DIR)
  file(GLOB _hl_sdk_versions RELATIVE "${HL_WINDOWS_SDK_DIR}/Include" "${HL_WINDOWS_SDK_DIR}/Include/10.*")
  list(SORT _hl_sdk_versions)
  list(REVERSE _hl_sdk_versions)
  if(_hl_sdk_versions)
    list(GET _hl_sdk_versions 0 HL_WINDOWS_SDK_VERSION)
  endif()
endif()

if(NOT HL_MSVC_TOOLS_DIR OR NOT IS_DIRECTORY "${HL_MSVC_TOOLS_DIR}/include")
  message(FATAL_ERROR
    "x86_64-windows-msvc: no MSVC toolset found. This target compiles against the "
    "MSVC/UCRT headers, so the Visual Studio Build Tools (workload "
    "'Desktop development with C++') are required. Install them, or point "
    "-DHL_MSVC_TOOLS_DIR=<...>/VC/Tools/MSVC/<version> at an existing one. "
    "For a mingw-w64 build instead, use cmake/toolchains/x86_64-windows.cmake.")
endif()
if(NOT HL_WINDOWS_SDK_DIR OR NOT HL_WINDOWS_SDK_VERSION)
  message(FATAL_ERROR
    "x86_64-windows-msvc: no Windows SDK found under 'Windows Kits/10'. Install "
    "one, or set -DHL_WINDOWS_SDK_DIR and -DHL_WINDOWS_SDK_VERSION.")
endif()

set(_hl_sdk_include "${HL_WINDOWS_SDK_DIR}/Include/${HL_WINDOWS_SDK_VERSION}")
set(_hl_sdk_lib "${HL_WINDOWS_SDK_DIR}/Lib/${HL_WINDOWS_SDK_VERSION}")

# -isystem, not -imsvc: -imsvc is a clang-cl driver flag and this is the
# gcc-style driver, which rejects it outright.
#
# ORDER IS LOAD-BEARING. clang searches -isystem entries in command-line order,
# and the POSIX seam below is emitted BEFORE these, so a seam header wins the
# search for <sys/types.h>, <sys/stat.h> and <winternl.h> and its #include_next
# then resumes here to reach the real UCRT or SDK header. Emitting these first
# would make the seam invisible: the UCRT header would be found directly, and
# the missing declarations would come back as hundreds of errors that name
# ssize_t rather than naming the ordering.
#
# The clang RESOURCE directory is listed first among these, and that is not
# cosmetic either. It holds clang's own intrinsics headers -- <emmintrin.h>,
# <immintrin.h> and friends -- and the MSVC toolset ships headers with the SAME
# NAMES. Normally clang searches its resource directory ahead of everything, but
# an explicit -isystem is inserted BEFORE it, so naming the MSVC root this way
# silently hands <emmintrin.h> to MSVC's copy.
#
# The consequence is not a missing declaration, which is why it is worth this
# paragraph. MSVC's header defines __m128 as a union with __declspec(intrin_type)
# -- a memory-backed aggregate -- where clang's defines it as a native vector
# (__attribute__((__vector_size__(16)))). Everything still compiles until the
# x86-64 interpreter's inline asm, which constrains __m128 operands to a vector
# register with "x"; against MSVC's union the operand is indirect and the
# BACKEND rejects it: "don't know how to handle tied indirect register inputs",
# reported at src/translator/guest/x86_64/interp.c, a file with nothing wrong
# with it, in a build whose include flags look correct. Listing the resource
# directory first restores clang's own intrinsics and the TU compiles.
execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
  OUTPUT_VARIABLE _hl_clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(_hl_clang_resource)
  file(TO_CMAKE_PATH "${_hl_clang_resource}" _hl_clang_resource)
else()
  message(FATAL_ERROR
    "x86_64-windows-msvc: `${CMAKE_C_COMPILER} -print-resource-dir` produced nothing. "
    "That directory holds clang's intrinsics headers and must precede the MSVC "
    "toolset's same-named copies; without it the x86-64 interpreter fails to compile.")
endif()

set(_hl_msvc_includes
  "-isystem \"${_hl_clang_resource}/include\""
  "-isystem \"${HL_MSVC_TOOLS_DIR}/include\""
  "-isystem \"${_hl_sdk_include}/ucrt\""
  "-isystem \"${_hl_sdk_include}/shared\""
  "-isystem \"${_hl_sdk_include}/um\""
  "-isystem \"${_hl_sdk_include}/winrt\"")
string(JOIN " " _hl_msvc_includes ${_hl_msvc_includes})

# The library roots go into the link flags rather than through
# link_directories(): a toolchain file is read before project(), so directory
# properties set here would not survive to the targets that need them.
set(_hl_msvc_libpaths
  "-L\"${HL_MSVC_TOOLS_DIR}/lib/x64\""
  "-L\"${_hl_sdk_lib}/ucrt/x64\""
  "-L\"${_hl_sdk_lib}/um/x64\"")
string(JOIN " " _hl_msvc_libpaths ${_hl_msvc_libpaths})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_hl_msvc_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_hl_msvc_libpaths}")

# --- the POSIX seam ---------------------------------------------------------
# -isystem (not -I) so that the seam's own headers are exempt from the engine's
# -Wall -Wextra -Wconversion, exactly as the UCRT's are: a warning inside a
# compatibility header is not actionable from a call site.
#
# The prelude is force-included rather than being a header somebody must
# remember to include, because the names it supplies (PATH_MAX, O_ACCMODE,
# clock_gettime) are ones the source already expects from headers it already
# includes.
get_filename_component(_hl_seam "${CMAKE_CURRENT_LIST_DIR}/msvc-posix" ABSOLUTE)

set(CMAKE_C_FLAGS_INIT
  "${_hl_cc_extra} -isystem \"${_hl_seam}/include\" ${_hl_msvc_includes} -include hl_msvc_prelude.h")

# --- build type and C runtime ----------------------------------------------
# An EMPTY build type, restated here because this platform is the one that will
# not leave it empty on its own. CMakeLists.txt keeps CMAKE_BUILD_TYPE unset so
# that CMake injects no -O or -DNDEBUG of its own and each target's flags are
# exactly what the tree asked for; Platform/Windows-MSVC.cmake overrides that
# with a CMAKE_BUILD_TYPE_INIT of Debug, and the two consequences were both
# real and both silent:
#
#   * -O0 replaced the -O2 the archive objects are built with. The guest-target
#     unity TU does not COMPILE at -O0: interp.c's SSE interpreter uses inline
#     asm with "x" constraints, and unoptimised operands reach the backend
#     indirect, which it rejects with "Don't know how to handle indirect
#     register inputs yet for constraint 'x'" -- a backend message, at a line
#     that is correct, in a file that builds everywhere else.
#   * the DEBUG CRT. CMake's runtime-library default is
#     MultiThreaded$<$<CONFIG:Debug>:Debug>DLL, so a Debug config selects
#     msvcrtd -- and rustc links the release UCRT. That is precisely the two-CRT
#     split (two heaps, two errno, two stdio states) that /MD exists to avoid,
#     arriving through a variable nobody set.
set(CMAKE_BUILD_TYPE_INIT "")

# /MD -- the release DLL runtime -- pinned rather than left to the default,
# which is config-dependent as above. Not a preference: rustc links this
# target's objects against the dynamic release UCRT, and an archive built /MT
# drags a SECOND, statically linked CRT into every downstream image, with an
# LNK4098 defaultlib warning on every consumer that is easy to silence and
# wrong to. Set through CMake's abstraction rather than as a bare
# -fms-runtime-lib flag, so it cannot end up contradicting what CMake injects.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# --- archiving --------------------------------------------------------------
# The archive must be in the MSVC format, not the GNU one. The rules that do
# that CANNOT be set here: CMake includes Platform/Windows-GNU.cmake after this
# file and that module overwrites them unconditionally. They live in the rules
# override beside this file, which is included at the only point that wins.
find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/x86_64-windows-msvc-rules.cmake")

# Static libraries are `foo.lib` here, not `libfoo.a`. CMake would otherwise
# keep the GNU spelling, because it takes those from the compiler's frontend
# variant rather than from the target triple -- and pkgs/rust/build.rs looks for
# `hl-engine.lib`, because that is what rustc's native-library search expects on
# a *-windows-msvc target.
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")

# --- linking ----------------------------------------------------------------
# lld-link is used through the clang driver, so no linker is named here. What is
# named is the import-library set every executable in this tree needs: the
# mingw-w64 lane gets these from its own default library list, and the MSVC one
# has no equivalent default.
#
# Spelled -lfoo and not foo.lib. This is the gcc-style driver: a bare `foo.lib`
# is a positional input and is looked for as a FILE in the working directory,
# which fails with "no such file or directory" and no mention of the search
# path. -l goes through the -L list above and finds the import library there.
set(CMAKE_C_STANDARD_LIBRARIES
  "-lkernel32 -lntdll -ladvapi32 -lbcrypt -lws2_32 -lsynchronization -luserenv"
  CACHE STRING "libraries linked into every C target" FORCE)

# Deliberately absent:
#  * -static — the sibling mingw file's reason for it is libwinpthread-1.dll,
#    which does not exist on this target. The UCRT is a system component and is
#    always dynamic here; /MD above is the deliberate form of that.
#  * -municode — same reasoning as the mingw file: a UTF-8 activeCodePage
#    manifest is the per-target answer, not a toolchain-wide one.
#  * CMAKE_FIND_ROOT_PATH_MODE_* — no cross case. A Windows host cross-compiled
#    from elsewhere is not a supported configuration for this file either.
