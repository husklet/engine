# ---------------------------------------------------------------------------
# Guest test fixtures — ONE helper over (arch x linkage).
#
# The Makefile spends ~500 lines on ~40 near-identical pattern-rule PAIRS
# (aarch64 + x86_64) that cross-compile the tests/compat, tests/e2e, tests/soak
# and tests/perf guest programs. They differ only along four axes:
#
#     arch     : aarch64 | x86_64          (which nix cross compiler)
#     linkage  : static | static-pie | nonpie | dynamic
#     flags    : -std=gnu11, -I<suite>, -march=..., ...
#     libs     : -pthread -lm -lrt -lutil -ldl -lsqlite3
#
# So this file provides exactly one primitive, hl_guest_binary(), plus a sweep
# helper hl_guest_suite() that applies one recipe to a whole source directory.
#
# WHY custom commands and not a CMake toolchain per arch: a single CMake
# project has a single C compiler. These fixtures must be built with TWO
# foreign cross compilers at once, and they are opaque test inputs, not
# libraries anything links against. Driving the nix compilers directly through
# add_custom_command is the honest expression of what the Makefile does and
# keeps nix as the sole toolchain authority (we never construct a compiler
# path; we read $AARCH64_LINUX_STATIC_CC / $X86_64_LINUX_STATIC_CC, the same
# variables the Makefile reads).
#
# make-semantics note: in make an explicit rule beats a pattern rule. The same
# precedence is reproduced here by a global registry — hl_guest_binary() records
# every output it defines, and hl_guest_suite() SKIPS any case already defined.
# Therefore special cases must be declared BEFORE the generic suite sweep.
# ---------------------------------------------------------------------------

# --- compiler discovery (nix is the authority; never hardcode a store path) --
function(_hl_guest_cc var env_static env_dynamic)
  if(NOT "$ENV{${env_static}}" STREQUAL "")
    separate_arguments(_cc UNIX_COMMAND "$ENV{${env_static}}")
  elseif(NOT "$ENV{${env_dynamic}}" STREQUAL "")
    separate_arguments(_cc UNIX_COMMAND "$ENV{${env_dynamic}}")
  else()
    set(_cc "")
  endif()
  set(${var} "${_cc}" PARENT_SCOPE)
endfunction()

# The *_STATIC_CC variants carry the -I/-L for the per-arch static sqlite and
# static glibc that nix supplies (see flake.nix linuxArmSqlite/linuxX86Sqlite);
# they must be passed through verbatim, hence the list split.
_hl_guest_cc(HL_GUEST_CC_aarch64 AARCH64_LINUX_STATIC_CC AARCH64_LINUX_CC)
_hl_guest_cc(HL_GUEST_CC_x86_64  X86_64_LINUX_STATIC_CC  X86_64_LINUX_CC)
# Dynamic-link cases (nonpie_dladdr, dynamic-guest) use the plain compiler:
# the STATIC variant's -L<glibc-static> would shadow the shared libc.
_hl_guest_cc(HL_GUEST_DYNCC_aarch64 AARCH64_LINUX_CC AARCH64_LINUX_STATIC_CC)
_hl_guest_cc(HL_GUEST_DYNCC_x86_64  X86_64_LINUX_CC  X86_64_LINUX_STATIC_CC)

set(HL_GUEST_ARCHES aarch64 x86_64)
foreach(_a ${HL_GUEST_ARCHES})
  if(NOT HL_GUEST_CC_${_a})
    message(FATAL_ERROR
      "No ${_a} Linux cross compiler. Enter the nix devShell first (`nix develop`); "
      "it exports AARCH64_LINUX_CC / X86_64_LINUX_CC exactly as the Makefile expects.")
  endif()
endforeach()

# Dynamic loader/libc paths, mirroring the Makefile defaults (lines 76-79).
set(HL_AARCH64_DYNAMIC_LOADER "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
    CACHE STRING "aarch64 guest dynamic loader")
set(HL_AARCH64_DYNAMIC_LIBC "/usr/lib/aarch64-linux-gnu/libc.so.6"
    CACHE STRING "aarch64 guest libc")
set(HL_X86_64_DYNAMIC_LOADER "/usr/x86_64-linux-gnu/lib/ld-linux-x86-64.so.2"
    CACHE STRING "x86_64 guest dynamic loader")
set(HL_X86_64_DYNAMIC_LIBC "/usr/x86_64-linux-gnu/lib/libc.so.6"
    CACHE STRING "x86_64 guest libc")
# In-guest loader path (the rootfs view), distinct from the host path above.
set(HL_GUEST_LOADER_aarch64 /lib/ld-linux-aarch64.so.1)
set(HL_GUEST_LOADER_x86_64  /lib64/ld-linux-x86-64.so.2)

define_property(GLOBAL PROPERTY HL_GUEST_OUTPUTS
  BRIEF_DOCS "every guest fixture output path defined so far"
  FULL_DOCS  "used to give explicitly-declared special cases precedence over the generic suite sweep, mirroring make's explicit-over-pattern rule precedence")
set_property(GLOBAL PROPERTY HL_GUEST_OUTPUTS "")

# hl_guest_binary(<arch> <output-abs-path> <source>
#                 [LINKAGE static|static-pie|nonpie|dynamic|freestanding|copy]
#                 [FLAGS ...] [LIBS ...] [DEPENDS ...])
#
#   static      -> -static                        (ET_EXEC, guest_base active)
#   static-pie  -> -static-pie                    (guest_base inert)
#   nonpie      -> -static -fno-pie -no-pie       (explicit non-PIE cases)
#   dynamic     -> -no-pie -rdynamic + loader/rpath
#   freestanding-> -nostdlib -static -fno-stack-protector -Wl,-e,_start
#   copy        -> committed binary input, copied through
#
# FLAGS land before the source, LIBS after it, exactly as in the Makefile
# recipes (link order matters for -lm/-lrt with static archives).
function(hl_guest_binary arch output source)
  cmake_parse_arguments(G "" "LINKAGE" "FLAGS;LIBS;DEPENDS" ${ARGN})
  if(NOT G_LINKAGE)
    set(G_LINKAGE static)
  endif()

  get_property(_seen GLOBAL PROPERTY HL_GUEST_OUTPUTS)
  if("${output}" IN_LIST _seen)
    return()   # an explicit declaration already owns this output
  endif()
  set_property(GLOBAL APPEND PROPERTY HL_GUEST_OUTPUTS "${output}")

  get_filename_component(_dir "${output}" DIRECTORY)

  if(G_LINKAGE STREQUAL "copy")
    add_custom_command(OUTPUT "${output}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${output}"
      DEPENDS "${source}" ${G_DEPENDS}
      COMMENT "guest[${arch}] copy ${output}"
      VERBATIM)
    set_property(GLOBAL APPEND PROPERTY HL_GUEST_ALL_OUTPUTS "${output}")
    return()
  endif()

  set(_cc ${HL_GUEST_CC_${arch}})
  if(G_LINKAGE STREQUAL "static")
    set(_link -static)
  elseif(G_LINKAGE STREQUAL "static-pie")
    set(_link -static-pie)
  elseif(G_LINKAGE STREQUAL "nonpie")
    # A non-PIE ET_EXEC guest keeps the translator's guest_base folding path
    # live; -static-pie would make the case pass trivially (Makefile 985-992).
    set(_link -static -fno-pie -no-pie)
  elseif(G_LINKAGE STREQUAL "freestanding")
    set(_link -nostdlib -static -fno-stack-protector -Wl,-e,_start)
  elseif(G_LINKAGE STREQUAL "raw")
    # Caller supplies every link flag itself (the handful of one-off recipes).
    set(_link "")
  elseif(G_LINKAGE STREQUAL "raw-dyn")
    set(_cc ${HL_GUEST_DYNCC_${arch}})
    set(_link "")
  elseif(G_LINKAGE STREQUAL "dynamic")
    set(_cc ${HL_GUEST_DYNCC_${arch}})
    set(_link -no-pie -rdynamic
        "-Wl,--dynamic-linker=${HL_GUEST_LOADER_${arch}},-rpath,/lib")
  else()
    message(FATAL_ERROR "hl_guest_binary: unknown LINKAGE '${G_LINKAGE}'")
  endif()

  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
    COMMAND ${_cc} ${_link} ${G_FLAGS} "${source}" ${G_LIBS} -o "${output}"
    DEPENDS "${source}" ${G_DEPENDS}
    COMMENT "guest[${arch}] ${output}"
    VERBATIM COMMAND_EXPAND_LISTS)
  set_property(GLOBAL APPEND PROPERTY HL_GUEST_ALL_OUTPUTS "${output}")
endfunction()

# hl_guest_suite(SRC_DIR <dir> OUT_DIR <abs dir> [ARCHES ...] [GLOB <pattern>]
#                [RELATIVE_NAMES] [NAMES ...] [SOURCE_DIRS ...]
#                [LINKAGE ...] [FLAGS ...] [LIBS ...]
#                [EXCLUDE_aarch64 ...] [EXCLUDE_x86_64 ...])
#
# Sweeps a suite directory and defines one fixture per (case, arch), skipping
# any output an explicit hl_guest_binary() call already claimed.
#   RELATIVE_NAMES keeps the source's sub-path in the output name (the Makefile
#   does this for filesystem/dentry, filesystem/pcachex, process/procexe and
#   completeness/<arch>), otherwise the basename is used (corpus dirs flatten).
function(hl_guest_suite)
  cmake_parse_arguments(S "RELATIVE_NAMES" "SRC_DIR;OUT_DIR;LINKAGE;GLOB"
    "ARCHES;NAMES;SOURCE_DIRS;FLAGS;LIBS;EXCLUDE_aarch64;EXCLUDE_x86_64" ${ARGN})
  if(NOT S_ARCHES)
    set(S_ARCHES ${HL_GUEST_ARCHES})
  endif()
  if(NOT S_GLOB)
    set(S_GLOB "*.c")
  endif()

  # SOURCE_DIRS, when given, REPLACES SRC_DIR as the glob root (SRC_DIR then
  # only anchors the relative output names). Globbing both would let a
  # subdirectory sweep silently claim the whole suite under the subdirectory's
  # linkage — which is how the filesystem/ and process/ cases briefly ended up
  # static-pie instead of static.
  set(_globdirs ${S_SOURCE_DIRS})
  if(NOT _globdirs)
    set(_globdirs ${S_SRC_DIR})
  endif()
  set(_sources "")
  foreach(_d ${_globdirs})
    file(GLOB _found CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${_d}/${S_GLOB}")
    list(APPEND _sources ${_found})
  endforeach()
  list(SORT _sources)

  foreach(_src ${_sources})
    if(S_RELATIVE_NAMES)
      file(RELATIVE_PATH _name "${CMAKE_SOURCE_DIR}/${S_SRC_DIR}" "${_src}")
      string(REGEX REPLACE "\\.c$" "" _name "${_name}")
    else()
      get_filename_component(_name "${_src}" NAME_WE)
    endif()
    if(S_NAMES AND NOT "${_name}" IN_LIST S_NAMES)
      continue()
    endif()
    foreach(_arch ${S_ARCHES})
      if("${_name}" IN_LIST S_EXCLUDE_${_arch})
        continue()
      endif()
      hl_guest_binary(${_arch} "${S_OUT_DIR}/${_arch}/${_name}" "${_src}"
        LINKAGE ${S_LINKAGE} FLAGS ${S_FLAGS} LIBS ${S_LIBS})
    endforeach()
  endforeach()
endfunction()

# Two output-naming conventions exist in the Makefile and both must be kept:
#
#   hl_guest_named -> <dir>/<arch>/<name>    compat suites, one dir per arch
#                                            (build/compat/ipc/aarch64/msg)
#   hl_guest_pair  -> <dir>/<name>-<arch>    e2e/perf, arch as a name suffix
#                                            (build/e2e/guest-exit-aarch64)
function(hl_guest_named outdir name source)
  foreach(_arch ${HL_GUEST_ARCHES})
    hl_guest_binary(${_arch} "${outdir}/${_arch}/${name}" "${source}" ${ARGN})
  endforeach()
endfunction()

function(hl_guest_pair outdir name source)
  foreach(_arch ${HL_GUEST_ARCHES})
    hl_guest_binary(${_arch} "${outdir}/${name}-${_arch}" "${source}" ${ARGN})
  endforeach()
endfunction()

# Collect everything declared so far into a buildable target.
function(hl_guest_finalize target)
  get_property(_outs GLOBAL PROPERTY HL_GUEST_ALL_OUTPUTS)
  # ALL: CTest does not build test dependencies, so the fixtures must be part
  # of the default build for `cmake --build build && ctest` to work.
  add_custom_target(${target} ALL DEPENDS ${_outs})
endfunction()
