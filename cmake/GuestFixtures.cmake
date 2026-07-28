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
# Guest fixtures are the TEST corpus: hundreds of little guest programs, each
# needing a Linux cross compiler for its arch. Building the engine itself needs
# none of them, so their absence must not be fatal -- otherwise `cmake && ninja`
# only works inside the nix devShell, and a plain `nix build` (whose sandbox
# exports no *_LINUX_CC) cannot build the library at all.
#
# So: auto-detect. With the compilers present (the devShell, or CI) everything
# is built as before; without them the guest fixtures and every suite that runs
# one are skipped, and the libraries/engines/runner still build. -DHL_GUEST_TESTS=ON
# forces the old hard failure if you expected the corpus and want to know why
# it vanished.
option(HL_GUEST_TESTS "build the guest test fixtures (needs Linux cross compilers)" AUTO)
set(HL_HAVE_GUEST_CC TRUE)

# A Windows host has no route to these compilers at all: nix does not run
# natively on Windows and no MSYS2 package supplies an x86_64-linux-gnu /
# aarch64-linux-gnu cross gcc with a static glibc. Decide it here, positively,
# rather than letting the fixture-less configure fall out of two empty
# environment variables: this is the single condition that keeps Phase3Compat,
# Phase3Gates, Phase4Mac and LaneParity out of a Windows configure
# (CMakeLists.txt gates all four on HL_HAVE_GUEST_CC), and it should be findable.
#
# What a Windows host CAN have is a corpus somebody else cross-compiled: a WSL2
# Ubuntu on the same machine hosts a real gcc-aarch64-linux-gnu plus a native
# gcc, both with a static glibc, and tools/windows/build_guest_fixtures.sh
# drives THIS file's own declarations there and exports the result. So the
# Windows arm is not "no fixtures", it is "fixtures from a cache, or none".
#
# That cache is a DEVELOPMENT corpus and the distinction matters: it is built
# against a distro glibc, not the nix one CI pins, and a golden that encodes a
# libc version will differ. "It compiled" is not the bar for a released
# artifact -- matching the nix build is. What this buys is the ability to run
# the suites at all on a Windows box, which is the difference between a lane
# that is red for a reason and a lane that does not exist.
#
# Two things are deliberately NOT done here:
#
#  * calling the WSL compilers per fixture from the generated build. It works,
#    but it puts wsl.exe on the critical path of ~3200 build edges and every
#    one of them reads and writes the DrvFs /mnt/c mount, which is roughly an
#    order of magnitude slower than the ext4 the same compiler sees natively.
#    Worse, it makes a Windows build silently depend on a distro's package
#    state: `apt upgrade` inside WSL would rebuild the corpus against a
#    different glibc with no record that anything moved.
#
#  * trusting the cache because it exists. A stale corpus is the failure this
#    port has been careful to avoid: the binaries still load, still run and
#    still produce output, so the suites go green while testing a tree nobody
#    has any more. The cache therefore carries a digest of the sources and
#    recipes it was built from, and this configure recomputes that digest and
#    refuses a mismatch instead of quietly using what it found.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(HL_HAVE_GUEST_CC FALSE)
  # Default under build-*/ so it lands on the existing .gitignore rule: the
  # corpus is ~2GB of build output and must never be committable by accident.
  set(HL_GUEST_PREBUILT_DIR "${CMAKE_SOURCE_DIR}/build-guest-fixtures"
      CACHE PATH "prebuilt guest fixture corpus (tools/windows/build_guest_fixtures.sh)")
  set(_hl_manifest "${HL_GUEST_PREBUILT_DIR}/MANIFEST.txt")

  if(NOT EXISTS "${_hl_manifest}")
    if(HL_GUEST_TESTS STREQUAL "ON")
      message(FATAL_ERROR
        "No prebuilt guest fixture corpus at ${HL_GUEST_PREBUILT_DIR}, but "
        "-DHL_GUEST_TESTS=ON was requested. Build one from a WSL2 Linux on this "
        "machine:\n"
        "    wsl -d Ubuntu -e bash -lc "
        "'tools/windows/build_guest_fixtures.sh --src <repo> --out <dir>'")
    endif()
    message(STATUS
      "Windows host and no prebuilt guest fixture corpus at ${HL_GUEST_PREBUILT_DIR} "
      "-- guest test fixtures and every suite that runs one are DISABLED. The "
      "libraries and host unit lane are the supported Windows configuration. "
      "tools/windows/build_guest_fixtures.sh builds a corpus from a WSL2 Linux.")
    return()
  endif()

  # Recompute the digest from THIS tree and compare it with what the corpus was
  # filed under. FATAL, not a warning: a warning during a 40-second configure is
  # a line nobody reads, and the whole point is that the wrong answer here is
  # invisible downstream.
  include("${CMAKE_SOURCE_DIR}/tools/guest_fixture_digest.cmake")
  file(STRINGS "${_hl_manifest}" _hl_claim REGEX "^source-digest: ")
  string(REPLACE "source-digest: " "" _hl_claim "${_hl_claim}")
  hl_guest_fixture_digest(_hl_actual "${CMAKE_SOURCE_DIR}")
  if(NOT _hl_claim STREQUAL _hl_actual)
    message(FATAL_ERROR
      "The prebuilt guest fixture corpus at ${HL_GUEST_PREBUILT_DIR} was built from "
      "different sources than this tree.\n"
      "    corpus was built from: ${_hl_claim}\n"
      "    this tree hashes to:   ${_hl_actual}\n"
      "Rebuild it, or point -DHL_GUEST_PREBUILT_DIR= at a current one:\n"
      "    wsl -d Ubuntu -e bash -lc "
      "'tools/windows/build_guest_fixtures.sh --src <repo> --out ${HL_GUEST_PREBUILT_DIR}'\n"
      "Configuring with -DHL_BUILD_TESTS=OFF, or with the corpus directory removed, "
      "returns to the supported fixture-less Windows configuration.")
  endif()

  # A .c edit does not re-run CMake by itself, so without this the digest above
  # is only checked at the configure that happened to notice. Listing every
  # digest input as a configure dependency turns "somebody edited a fixture
  # source" into a re-configure, which is where the check lives.
  hl_guest_fixture_inputs(_hl_inputs "${CMAKE_SOURCE_DIR}")
  list(TRANSFORM _hl_inputs PREPEND "${CMAKE_SOURCE_DIR}/")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_hl_inputs} "${_hl_manifest}")

  set(HL_GUEST_PREBUILT "${HL_GUEST_PREBUILT_DIR}")
  file(STRINGS "${_hl_manifest}" _hl_built REGEX "^(fixture-count|built-at|toolchain-)")
  message(STATUS "Guest fixtures: prebuilt corpus at ${HL_GUEST_PREBUILT_DIR}, digest current")
  foreach(_l ${_hl_built})
    message(STATUS "  ${_l}")
  endforeach()

  # The corpus is present and current, and there IS now something on Windows
  # that consumes it: cmake/Phase3Compat.cmake declares every compat fixture
  # here (staged from this cache, not compiled), builds matrix-runner and
  # linux-matrix -- the two host runners whose process supervision has been
  # ported -- and cmake/Phase3Gates.cmake registers the x86_64 production
  # sweeps. Both are behind HL_WINDOWS_GUEST_LANES, which is OFF until the
  # engine can start a guest; the fixtures and runners build either way, since
  # having them built is what makes the engine measurable at all.
  #
  # The one thing still missing is `compat-engines`: CMakeLists.txt names
  # hl-engine-linux-{aarch64,x86_64} and forkserver-runner in it, none of which
  # a Windows configure defines, so the target is not declared on this host
  # (CMakeLists.txt guards it) and `ninja compat-engines` is simply absent
  # rather than present-and-unbuildable. Shard targets are unaffected: the
  # per-suite compat-<label>-fixtures targets are declared here as usual.
  #
  # The default stays FALSE anyway. That is NOT about the consumers any more --
  # it is about cost and blast radius. Declaring 3,233 staging rules doubles a
  # Windows configure, and the supported Windows configuration is still the
  # libraries plus the host unit lane; a developer who wants the corpus asks for
  # it. -DHL_GUEST_TESTS=ON is that request.
  if(NOT HL_GUEST_TESTS STREQUAL "ON")
    message(STATUS
      "Guest fixtures stay OFF by default on Windows -- not because nothing "
      "consumes them (Phase3Compat now stages the whole corpus here and builds "
      "matrix-runner and linux-matrix), but because 3,233 staging rules are not "
      "what a default Windows build is for. -DHL_GUEST_TESTS=ON consumes this "
      "corpus; add -DHL_WINDOWS_GUEST_LANES=ON to also register the x86_64 "
      "guest lanes once the engine can start a guest.")
    return()
  endif()
  set(HL_HAVE_GUEST_CC TRUE)
  message(STATUS "  -DHL_GUEST_TESTS=ON -- consuming the prebuilt corpus")
  # Deliberately NOT a return: the rest of this file defines hl_guest_binary()
  # and the output registry, which is what actually stages the corpus.
endif()

# A prebuilt corpus is the answer to "no cross compiler", so do not then demand
# one; every fixture below is staged from the cache instead of compiled.
if(NOT HL_GUEST_PREBUILT)
  foreach(_a ${HL_GUEST_ARCHES})
    if(NOT HL_GUEST_CC_${_a})
      set(HL_HAVE_GUEST_CC FALSE)
      set(_hl_missing_cc ${_a})
    endif()
  endforeach()
endif()

if(NOT HL_HAVE_GUEST_CC)
  if(HL_GUEST_TESTS STREQUAL "ON")
    message(FATAL_ERROR
      "No ${_hl_missing_cc} Linux cross compiler, but -DHL_GUEST_TESTS=ON was requested. "
      "Enter the nix devShell first (`nix develop`); it exports AARCH64_LINUX_CC / "
      "X86_64_LINUX_CC exactly as the Makefile expects.")
  endif()
  message(STATUS
    "No ${_hl_missing_cc} Linux cross compiler -- guest test fixtures and the suites that "
    "run them are DISABLED. The libraries, engines and runner still build. Enter the nix "
    "devShell (`nix develop`) to get the full test corpus.")
  return()
endif()

# Dynamic loader/libc paths. The nix devShell EXPORTS all four; the Makefile
# picks them up because it declares them with `?=` (lines 76-79). Do the same,
# or the rootfs-staging cases (e.g. compat/process nonpie-dladdr) get baked
# /usr/... paths that exist on no host in this project.
function(hl_dyn name fallback)
  set(_v "${fallback}")
  if(NOT "$ENV{${name}}" STREQUAL "")
    set(_v "$ENV{${name}}")
  endif()
  set(HL_${name} "${_v}" CACHE STRING "guest ${name}")
endfunction()
hl_dyn(AARCH64_DYNAMIC_LOADER /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1)
hl_dyn(AARCH64_DYNAMIC_LIBC   /usr/lib/aarch64-linux-gnu/libc.so.6)
hl_dyn(X86_64_DYNAMIC_LOADER  /usr/x86_64-linux-gnu/lib/ld-linux-x86-64.so.2)
hl_dyn(X86_64_DYNAMIC_LIBC    /usr/x86_64-linux-gnu/lib/libc.so.6)
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

  # Prebuilt corpus (a host with no cross compiler, i.e. Windows): every
  # fixture, whatever its linkage, is staged from the cache. Uniformly, so
  # there is exactly one answer to "where did this binary come from" on such a
  # host -- a `copy`-linkage case served from the source tree while its
  # neighbours came from the cache would be one more place for the two to
  # disagree. The compile flags are not consulted at all here: they were
  # consulted by the Linux build that produced the cache, and the digest gate
  # above is what asserts that build saw this tree's recipes.
  if(HL_GUEST_PREBUILT)
    file(RELATIVE_PATH _rel "${CMAKE_BINARY_DIR}" "${output}")
    set(_cached "${HL_GUEST_PREBUILT}/${_rel}")
    if(NOT EXISTS "${_cached}")
      set_property(GLOBAL APPEND PROPERTY HL_GUEST_MISSING "${_rel}")
    endif()
    add_custom_command(OUTPUT "${output}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_cached}" "${output}"
      DEPENDS "${_cached}" ${G_DEPENDS}
      COMMENT "guest[${arch}] prebuilt ${_rel}"
      VERBATIM)
    set_property(GLOBAL APPEND PROPERTY HL_GUEST_ALL_OUTPUTS "${output}")
    return()
  endif()

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

  # The authoritative list of what the corpus IS, as build-dir-relative paths.
  # Written on every host: it is how tools/windows/build_guest_fixtures.sh knows
  # which of a Linux build tree's files are fixtures, without a second, drifting
  # copy of the answer. Nothing in the build reads it, so it cannot change what
  # any host builds.
  set(_rel "")
  foreach(_o ${_outs})
    file(RELATIVE_PATH _r "${CMAKE_BINARY_DIR}" "${_o}")
    string(APPEND _rel "${_r}\n")
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/guest-fixtures.list" "${_rel}")

  # A prebuilt corpus that is missing files the recipes declare is stale in the
  # one way the source digest cannot see: the digest says "built from these
  # sources", not "built all of them". Name every gap and stop, rather than
  # letting the build fail later with a copy error per file and no summary.
  get_property(_missing GLOBAL PROPERTY HL_GUEST_MISSING)
  if(HL_GUEST_PREBUILT AND _missing)
    list(LENGTH _missing _n)
    list(LENGTH _outs _total)
    string(REPLACE ";" "\n  " _missing "${_missing}")
    message(FATAL_ERROR
      "The prebuilt guest fixture corpus at ${HL_GUEST_PREBUILT} is missing ${_n} of "
      "the ${_total} fixtures declared up to ${target}:\n  ${_missing}\n"
      "It matches this tree's sources but was not built completely -- rebuild it "
      "with tools/windows/build_guest_fixtures.sh and read that script's failure "
      "report.")
  endif()

  # ALL: CTest does not build test dependencies, so the fixtures must be part
  # of the default build for `cmake --build build && ctest` to work.
  add_custom_target(${target} ALL DEPENDS ${_outs})
endfunction()
