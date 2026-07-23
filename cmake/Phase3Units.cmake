# ---------------------------------------------------------------------------
# Phase 3a — the ~80 host unit test binaries (Makefile UNIT_NAMES, lines 224-...)
# as CMake targets plus add_test().
#
# The Makefile has ONE pattern rule for the common case,
#     $(BUILD)/tests/test_%: tests/unit/test_%.c <4 archives, listed TWICE> -lm
# and ~30 explicit rules that override it with a narrower link set (a couple of
# raw .c files, or only libhl-linux-abi, or the Linux host archive). That shape
# is preserved here: hl_unit() is the pattern rule, and the explicit cases pass
# LIBS/SOURCES/FLAGS.
#
# The doubled archive list is deliberate and load-bearing: the four archives are
# mutually recursive, so a single pass leaves undefined symbols. It is
# reproduced with $<TARGET_FILE:...> repeated rather than target_link_libraries
# target names, because CMake de-duplicates the latter and would silently drop
# the second pass.
#
# Weak host-private symbols: src/core/launch.c declares hl_host_private_init,
# hl_host_process_fd_private_add and _remove `extern __attribute__((weak))`, so
# test_launch links against libhl-host-fake with them unresolved-and-NULL by
# design. See the report note: the previously observed "undefined
# hl_host_private_init" was a toolchain mismatch (the devShell's poisoned $CC is
# the x86_64 cross gcc while the archives are the host arch), not a missing
# provider. CMake takes its compiler from the cmake/toolchains/ toolchain file, so the two
# always agree here.
# ---------------------------------------------------------------------------

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

set(_HL_UNIT_DEFAULT_LIBS hl-engine hl-translator hl-linux-abi hl-host-fake)

# hl_unit(<name> [SOURCES extra.c ...] [LIBS <targets>] [DOUBLE]
#         [FLAGS ...] [INCLUDES ...] [LINK ...] [NO_TEST])
#   name       -> tests/unit/test_<name>.c  ->  build/tests/test_<name>
#   LIBS       -> replaces the default four archives
#   DOUBLE     -> emit the archive list twice (default for the pattern rule)
function(hl_unit name)
  cmake_parse_arguments(U "NO_TEST;SINGLE;NO_LIBS" "BINARY;OUTNAME;SOURCE"
    "SOURCES;LIBS;FLAGS;INCLUDES;LINK" ${ARGN})
  if(NOT U_BINARY)
    set(U_BINARY test_${name})
  endif()
  if(NOT U_SOURCE)
    set(U_SOURCE tests/unit/test_${name}.c)
  endif()
  if(NOT U_LIBS AND NOT U_SOURCES AND NOT U_NO_LIBS)
    set(U_LIBS ${_HL_UNIT_DEFAULT_LIBS})
    set(U_LINK ${U_LINK} -lm)
  endif()

  add_executable(${U_BINARY} ${U_SOURCE} ${U_SOURCES})
  target_link_libraries(${U_BINARY} PRIVATE hl_engine_cflags)
  target_include_directories(${U_BINARY} PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/unit ${U_INCLUDES})
  target_compile_options(${U_BINARY} PRIVATE ${U_FLAGS})
  # The Makefile compiles and links in ONE command, so -pthread is also a
  # COMPILE flag there. That matters: on glibc -pthread implies -D_REENTRANT,
  # which is what makes nanosleep/kill visible under strict -std=c11. Splitting
  # it into link-only would break those TUs, so mirror the Makefile exactly.
  if("-pthread" IN_LIST U_LINK)
    target_compile_options(${U_BINARY} PRIVATE -pthread)
  endif()

  # Archives. The Makefile lists the four mutually-recursive archives TWICE to
  # get a second resolution pass; LINK_GROUP RESCAN (--start-group/--end-group)
  # is the same guarantee expressed once, and unlike a repeated
  # target_link_libraries list it survives CMake's de-duplication.
  # NOTE: this must go through target_link_libraries, not target_link_options —
  # link options are emitted BEFORE the object files, and an archive placed
  # before its referring objects contributes nothing.
  if(U_LIBS)
    list(REMOVE_DUPLICATES U_LIBS)
    list(LENGTH U_LIBS _n)
    if(_n GREATER 1)
      list(JOIN U_LIBS "," _grp)
      target_link_libraries(${U_BINARY} PRIVATE "$<LINK_GROUP:RESCAN,${_grp}>")
    else()
      target_link_libraries(${U_BINARY} PRIVATE ${U_LIBS})
    endif()
  endif()
  target_link_libraries(${U_BINARY} PRIVATE ${U_LINK})
  set_target_properties(${U_BINARY} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  if(U_OUTNAME)
    set_target_properties(${U_BINARY} PROPERTIES OUTPUT_NAME ${U_OUTNAME})
  endif()

  # Every mac binary that links the engine needs the JIT entitlement or the
  # kernel SIGKILLs it; hl_codesign() is a no-op off macOS (Phase 4).
  hl_codesign(${U_BINARY})

  if(NOT U_NO_TEST)
    add_test(NAME unit.${name} COMMAND ${U_BINARY})
    # make runs unit binaries from the repository root and some of them open
    # repo-relative data (test_lifecycle_identity reads tests/e2e/*.tsv), so the
    # working directory is part of the contract, not an implementation detail.
    set_tests_properties(unit.${name} PROPERTIES
      LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endfunction()

# --- explicit cases first (they replace the pattern rule) -------------------
set(_abi   hl-linux-abi)
set(_hostl hl-host-linux)

# Manifest-only firewall audit: deliberately links NO engine archive so a mac
# gate build never pulls GNU archives into a Darwin executable (Makefile 566).
hl_unit(lifecycle_identity NO_LIBS)

hl_unit(linux_abi           LINK -pthread)
hl_unit(watch               LIBS ${_abi} SINGLE LINK -pthread)
hl_unit(native              LIBS hl-engine hl-translator ${_hostl} SINGLE LINK -pthread)
hl_unit(private             LIBS ${_hostl} SINGLE)
hl_unit(directory_services  LIBS hl-engine ${_hostl} SINGLE LINK -pthread)
hl_unit(fdcache             LIBS ${_abi} hl-host-fake SINGLE
                            INCLUDES ${CMAKE_SOURCE_DIR}/src/linux_abi)
hl_unit(resolve_services    LIBS ${_hostl} SINGLE LINK -pthread)
hl_unit(linux_fork          LIBS ${_abi} ${_hostl} SINGLE LINK -pthread)
hl_unit(pipe_linux          LIBS ${_abi} hl-engine ${_abi} ${_hostl} SINGLE LINK -pthread
                            INCLUDES ${CMAKE_SOURCE_DIR}/src/linux_abi)
hl_unit(eventfd_fork        LIBS ${_abi} ${_hostl} SINGLE LINK -pthread
                            INCLUDES ${CMAKE_SOURCE_DIR}/src/linux_abi)
hl_unit(seccomp_vm          LIBS ${_abi} SINGLE
                            INCLUDES ${CMAKE_SOURCE_DIR}/src/linux_abi)
hl_unit(fork_wire           LIBS ${_abi} ${_hostl} SINGLE LINK -pthread
                            INCLUDES ${CMAKE_SOURCE_DIR}/src/linux_abi
                                     ${CMAKE_SOURCE_DIR}/src/host)
hl_unit(limits              LIBS ${_abi} SINGLE LINK -pthread)

# Cases compiled straight against a handful of .c files (no archive at all).
hl_unit(resolve      SOURCES src/host/resolve.c)
hl_unit(reloc        SOURCES src/translator/reloc.c)
hl_unit(digest       SOURCES src/translator/digest.c)
hl_unit(x87_stack    SOURCES src/translator/guest/x86_64/lower/x87_stack.c)
hl_unit(lower_x87    SOURCES src/translator/guest/x86_64/lower/x87.c
                             src/translator/guest/x86_64/lower/x87_stack.c)
hl_unit(lower_sse4x  SOURCES src/translator/guest/x86_64/lower/sse4x.c)
hl_unit(lower_repstr SOURCES src/translator/guest/x86_64/lower/repstr.c)
hl_unit(lower_crypto SOURCES src/translator/guest/x86_64/lower/crypto.c)
hl_unit(lower_trace  SOURCES src/translator/guest/x86_64/lower/trace.c)
hl_unit(window       SOURCES src/translator/window.c)
hl_unit(identity     SOURCES src/translator/identity.c)
hl_unit(clock        SOURCES src/host/clock.c src/host/fake/host.c)
hl_unit(child        SOURCES src/host/child.c)
hl_unit(directory    SOURCES src/host/linux/directory.c)
hl_unit(process      SOURCES src/host/linux/process.c)
hl_unit(file         SOURCES src/host/file.c)
hl_unit(range        SOURCES src/host/range.c src/host/linux/range.c)
hl_unit(system       SOURCES src/host/linux/system.c src/host/private.c)

# --- the generic sweep (UNIT_NAMES) ----------------------------------------
set(HL_UNIT_NAMES
  a64_asm address affinity arena avx bus child cli clock codegen config cpuid
  cmpxchg decoder device digest directory directory_services emit epoll eventfd
  eventfd_fork fatal fdcache file flags fork_wire glue gmap host_services
  identity image inotify ir key launch legacy lifecycle_identity linux_abi
  linux_fork lower_alu lower_crypto lower_mov lower_repstr lower_shift
  lower_sse4x lower_trace lower_x87 misc native open_plan operand owner persist
  pidmap pipe pipe_linux placement ports private process range rep resolve
  resolve_services rotate shared shm signal_aarch64 signal_x86_64 snapshot
  system seccomp_vm stat engine errno limits log namespace number options parse
  profile readonly reloc target_bus watch window x87_stack x87math x87state
  xattr_cache)

foreach(_u ${HL_UNIT_NAMES})
  if(NOT TARGET test_${_u})
    hl_unit(${_u})
  endif()
endforeach()

# --- extra Linux-host binaries that `make unit` also builds/runs ------------
# run-unit-linux + test-native-capacity (Makefile lines 2694-2700 / 615-620).
hl_unit(linux BINARY tests_linux OUTNAME linux SOURCE tests/unit/linux.c
        LIBS hl-engine ${_hostl} SINGLE LINK -pthread NO_TEST)
add_test(NAME unit.linux COMMAND tests_linux)
set_tests_properties(unit.linux PROPERTIES LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

hl_unit(native_capacity BINARY native-capacity SOURCE tests/unit/test_native_capacity.c
        LIBS hl-engine ${_hostl} SINGLE LINK -pthread NO_TEST)
add_test(NAME unit.native-capacity COMMAND native-capacity)
set_tests_properties(unit.native-capacity PROPERTIES LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Debug-logging variants: same TU with HL_ENABLE_LOGGING forced to 1.
foreach(_d log fatal)
  add_executable(test-${_d}-debug tests/unit/test_${_d}.c src/core/${_d}.c)
  target_include_directories(test-${_d}-debug PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests/unit)
  target_compile_definitions(test-${_d}-debug PRIVATE HL_ENABLE_LOGGING=1)
  target_compile_options(test-${_d}-debug PRIVATE
    -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -fvisibility=hidden)
  set_target_properties(test-${_d}-debug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  hl_codesign(test-${_d}-debug)
  add_test(NAME unit.debug-${_d} COMMAND test-${_d}-debug)
  set_tests_properties(unit.debug-${_d} PROPERTIES LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
