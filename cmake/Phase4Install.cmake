# ---------------------------------------------------------------------------
# Phase 4 — install(), pkg-config generation, and the consumer smoke tests.
#
# Goal: the plain, boring, standard flow works and produces a usable SDK.
#
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake
#     cmake --build build
#     cmake --install build --prefix /usr/local
#
# giving  include/hl/*.h, lib/libhl-{engine,linux-abi,host-<host>}.a,
# lib/libhl-engine-activation.a, lib/pkgconfig/*.pc and bin/hl-engine-runner.
#
# libhl-engine-activation.a + hl-engine-activation.pc are THE complete engine an embedder links: one
# force-loaded archive with both guest-ISA targets, the translator and the whole ABI (pkgs/rust/build.rs
# already links exactly it). hl-engine.pc is the smaller host/ABI contract package.c exercises.
#
# libhl-translator.a is deliberately NOT published: a build component of the runner, both production
# engines and the activation archive, never linkable alone. Off an AArch64 host it carries 84 undefined
# ARM64-emitter references (e_*, emit_*, hl_x86_emit_*, rm_*) defined only by the src/core/target/x86_64.c
# unity TU, which is in no published archive but the activation one -- and nothing else installed
# references a translator symbol, so -lhl-translator handed a consumer half of a component.
# ---------------------------------------------------------------------------

set(HL_VERSION 0.1.10)

# The Makefile's HOST/HOST_ARCH conditionals (lines 165-207), reproduced.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(HL_PACKAGE_HOST macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # -lhl-host-windows names an archive that does not exist yet (M4). Emitting
  # `windows` anyway is still right: the alternative is emitting `linux`, i.e. a
  # .pc file that names the WRONG archive rather than a missing one, which is
  # exactly the class of path-literal bug docs/amd64-host.md 8.1 records.
  set(HL_PACKAGE_HOST windows)
else()
  set(HL_PACKAGE_HOST linux)
endif()
set(HL_PACKAGE_SYSTEM_LIBS "-pthread")

# Whether an embedded activation archive is installable. Darwin names its
# equivalent target hl-engine-dual; both install under the activation name.
if(TARGET hl-engine-activation OR TARGET hl-engine-dual)
  set(HL_HAVE_ACTIVATION TRUE)
else()
  set(HL_HAVE_ACTIVATION FALSE)
endif()

# --- the runner: a `make all` product and an installed binary ---------------
add_executable(hl-engine-runner src/runner/main.c)
target_link_libraries(hl-engine-runner PRIVATE hl_engine_cflags)
# These three archives are mutually recursive, so a linker that makes a single
# pass needs an explicit --start-group/--end-group (LINK_GROUP RESCAN); one that
# rescans to a fixed point on its own rejects the feature outright and takes the
# plain list instead. HL_LINK_GROUP_RESCAN in CMakeLists.txt is that decision,
# made once for the three sites that need it.
if(NOT HL_LINK_GROUP_RESCAN)
  target_link_libraries(hl-engine-runner PRIVATE hl-engine hl-translator hl-linux-abi)
else()
  target_link_libraries(hl-engine-runner PRIVATE
    "$<LINK_GROUP:RESCAN,hl-engine,hl-translator,hl-linux-abi>")
endif()
set_target_properties(hl-engine-runner PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
hl_codesign(hl-engine-runner)

# --- pkg-config -------------------------------------------------------------
# Written through configure_file so `prefix=` follows CMAKE_INSTALL_PREFIX
# instead of the Makefile's fixed PREFIX default.
set(HL_PC_DIR ${CMAKE_BINARY_DIR}/pkgconfig)
file(MAKE_DIRECTORY ${HL_PC_DIR})

file(WRITE ${HL_PC_DIR}/hl-engine.pc.in
"prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: hl-engine
Description: Portable Linux guest translation and ABI engine
Version: @HL_VERSION@
Libs: -L\${libdir} -lhl-host-@HL_PACKAGE_HOST@ -lhl-engine -lhl-linux-abi @HL_PACKAGE_SYSTEM_LIBS@
Cflags: -I\${includedir}
")
configure_file(${HL_PC_DIR}/hl-engine.pc.in ${HL_PC_DIR}/hl-engine.pc @ONLY)

if(HL_HAVE_ACTIVATION)
  # The activation archive must be force-loaded whole: its registration
  # constructors have no referenced symbol, so a normal -l would drop them.
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(HL_ACTIVATION_LIBS "-Wl,-force_load,\${libdir}/libhl-engine-activation.a")
  else()
    set(HL_ACTIVATION_LIBS
      "-Wl,--whole-archive \${libdir}/libhl-engine-activation.a -Wl,--no-whole-archive -pthread -ldl -lm -latomic")
  endif()
  file(WRITE ${HL_PC_DIR}/hl-engine-activation.pc.in
"prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: hl-engine-activation
Description: Complete embedded Linux activation engine
Version: @HL_VERSION@
Libs: @HL_ACTIVATION_LIBS@
Cflags: -I\${includedir}
")
  configure_file(${HL_PC_DIR}/hl-engine-activation.pc.in
                 ${HL_PC_DIR}/hl-engine-activation.pc @ONLY)
endif()

# --- install rules ----------------------------------------------------------
# Pin the library directory to "lib" BEFORE GNUInstallDirs computes its default.
# On a 64-bit Linux that is not Debian-derived, GNUInstallDirs picks "lib64", so a
# plain `cmake --install` produced lib64/ while `nix build` produced lib/ -- nixpkgs'
# cmake hook passes -DCMAKE_INSTALL_LIBDIR=lib. Two different SDK layouts from one
# project is a trap for anyone writing -L or a pkg-config path, and it is what made
# the CI install assertions (which check lib/) fail. Cache entry, so an explicit
# -DCMAKE_INSTALL_LIBDIR on the command line still wins.
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "object code libraries (relative to prefix)")
include(GNUInstallDirs)

set(HL_INSTALL_LIBS hl-engine hl-linux-abi)
if(TARGET hl-host-linux)
  list(APPEND HL_INSTALL_LIBS hl-host-linux)
elseif(TARGET hl-host-macos)
  list(APPEND HL_INSTALL_LIBS hl-host-macos)
elseif(TARGET hl-host-windows)
  # Not built yet (M4). Asks the build graph rather than the OS, so the arm needs
  # no further edit when CMakeLists.txt starts defining the target.
  list(APPEND HL_INSTALL_LIBS hl-host-windows)
endif()

install(TARGETS ${HL_INSTALL_LIBS} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hl-engine-runner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The per-guest-ISA production engines are the artifact an embedder actually
# runs, and the Makefile's `install` ships them alongside the runner. Install
# them too so `cmake --install` yields the same layout the make-based package
# does (bin/hl-engine-linux-{aarch64,x86_64}). Linux lane only: the macOS
# engines are built and codesigned by Phase4Mac.cmake.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  foreach(_engine_arch aarch64 x86_64)
    if(TARGET hl-engine-linux-${_engine_arch})
      install(TARGETS hl-engine-linux-${_engine_arch} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
  endforeach()
endif()

file(GLOB HL_PUBLIC_HEADERS ${CMAKE_SOURCE_DIR}/include/hl/*.h)
if(NOT HL_HAVE_ACTIVATION)
  list(REMOVE_ITEM HL_PUBLIC_HEADERS ${CMAKE_SOURCE_DIR}/include/hl/activation.h)
endif()
install(FILES ${HL_PUBLIC_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hl)
install(FILES ${HL_PC_DIR}/hl-engine.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Standard CMake does not generate an uninstall target.  Preserve the
# historical Makefile contract with an exact install-manifest consumer: it
# removes only files installed by this build tree and therefore cannot clobber
# unrelated content sharing the prefix.
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/Uninstall.cmake.in
  ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake
  @ONLY)
add_custom_target(uninstall
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake
  COMMENT "Removing files recorded by install_manifest.txt")

set(HL_ACTIVATION_TARGET "")
if(TARGET hl-engine-activation)
  set(HL_ACTIVATION_TARGET hl-engine-activation)
elseif(TARGET hl-engine-dual)
  set(HL_ACTIVATION_TARGET hl-engine-dual)
endif()

if(HL_HAVE_ACTIVATION AND HL_ACTIVATION_TARGET)
  # Installed under the activation name; in the build tree it is the
  # package/<host>-aarch64/libhl-engine.a artefact (Makefile 481).
  install(FILES $<TARGET_FILE:${HL_ACTIVATION_TARGET}>
          DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RENAME libhl-engine-activation.a)
  install(FILES ${HL_PC_DIR}/hl-engine-activation.pc
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# --- gate.archive-closure ---------------------------------------------------
# Derived from HL_INSTALL_LIBS above, not from a second hand-written list, so
# what is checked cannot drift from what is installed. Runs in the `unit` lane
# because it is two nm sweeps and one link.
#
# The installed binaries used to be providers too, because libhl-translator.a's
# ARM64 emitter references had no definition in any archive. Unpublishing it
# removed the exception: the installed archives now close over themselves plus
# the system toolchain on both hosts, so the gate asserts that directly.
#
# Linux host only: Apple's nm spells the selectors differently and a Mach-O
# probe takes another system-library set. The macOS artefact is Phase4Mac's
# hl-engine-dual, which mac-dual-backend-link-test already force-loads whole.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND HL_BUILD_TESTS)
  if(DEFINED CMAKE_NM AND NOT CMAKE_NM STREQUAL "")
    set(HL_NM_EXECUTABLE ${CMAKE_NM})
  else()
    find_program(HL_NM_EXECUTABLE NAMES nm REQUIRED)
  endif()

  set(_closure_archives "")
  foreach(_lib IN LISTS HL_INSTALL_LIBS)
    list(APPEND _closure_archives $<TARGET_FILE:${_lib}>)
  endforeach()
  if(HL_HAVE_ACTIVATION AND HL_ACTIVATION_TARGET)
    list(APPEND _closure_archives $<TARGET_FILE:${HL_ACTIVATION_TARGET}>)
  endif()

  add_test(NAME gate.archive-closure
    COMMAND ${HL_BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_archive_closure.sh
            ${HL_NM_EXECUTABLE} ${CMAKE_C_COMPILER} ${_closure_archives})
  set_tests_properties(gate.archive-closure PROPERTIES
    LABELS "unit;gate" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# --- gate.windows-imports ---------------------------------------------------
# The Windows sibling of gate.archive-closure: same species (a binutils sweep
# over what this build produced, in the `unit` lane), opposite host, and a much
# sharper reason. cmake/CheckPeImports.cmake carries the full argument; the
# short form is that a USER32 import loads IMM32, whose DLL_THREAD_DETACH
# handler access-violates inside a process cloned by RtlCloneUserProcess -- 50
# of 50 runs with the import, 0 of 50 without, in two otherwise byte-identical
# binaries. It cannot be caught at runtime by anything this project runs: the
# fault needs a guest to fork AND THEN create and join a thread, which is a JVM
# or a Go runtime, never a smoke test.
#
# Registered here rather than in a Windows test file because there is no
# Windows test file: cmake/Phase3Units.cmake returns early on this host and
# cmake/Phase3Gates.cmake is not even included. Phase4Install is the one place
# that is unconditionally included on every host AND already owns a gate of
# exactly this shape.
#
# CMake script mode, not a shell script, because bash is OPTIONAL on Windows
# (CMakeLists.txt found it non-REQUIRED for exactly this host) and a gate that
# disappears with the shell is not a gate.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND HL_BUILD_TESTS)
  # Not REQUIRED: a missing tool must fail the TEST, loudly, with the reason --
  # not the configure of three other agents' working trees. The script names
  # all three candidates when it finds none.
  find_program(HL_READOBJ_EXECUTABLE NAMES llvm-readobj)
  find_program(HL_PE_OBJDUMP_EXECUTABLE NAMES llvm-objdump objdump)
  if(DEFINED CMAKE_NM AND NOT CMAKE_NM STREQUAL "")
    set(HL_PE_NM_EXECUTABLE ${CMAKE_NM})
  else()
    find_program(HL_PE_NM_EXECUTABLE NAMES llvm-nm nm)
  endif()

  # Layer 1's subjects: every PE image this project SHIPS. hl_lint is
  # deliberately absent -- it is a developer tool that is never cloned, and
  # gating it would invite an allowlist edit for a non-engine reason.
  set(_pe_images "")
  foreach(_t hl-engine-runner hl-engine-windows-aarch64 hl-engine-windows-x86_64)
    if(TARGET ${_t})
      list(APPEND _pe_images $<TARGET_FILE:${_t}>)
    endif()
  endforeach()

  # Layer 2's subjects: the archives that feed those links. This is what gives
  # the gate teeth BEFORE anything links, which is where the port is today --
  # an archive that references wsprintfW is a link that will import USER32.
  set(_pe_archives "")
  foreach(_t hl-translator hl-host-fake hl-engine hl-linux-abi hl-host-windows)
    if(TARGET ${_t})
      list(APPEND _pe_archives $<TARGET_FILE:${_t}>)
    endif()
  endforeach()
  if(_pe_images STREQUAL "" AND _pe_archives STREQUAL "")
    message(FATAL_ERROR
      "gate.windows-imports has no subject: no PE image target and no archive "
      "target exists in this configure. That is not a Windows-port milestone, "
      "it is a broken target list -- fix it rather than dropping the gate.")
  endif()
  # `|`, not `;`: a semicolon inside an add_test() argument is a list separator
  # and would arrive as separate argv entries.
  list(JOIN _pe_images "|" _pe_images_arg)
  list(JOIN _pe_archives "|" _pe_archives_arg)

  add_test(NAME gate.windows-imports
    COMMAND ${CMAKE_COMMAND}
      -DHL_PE_IMAGES=${_pe_images_arg}
      -DHL_PE_ARCHIVES=${_pe_archives_arg}
      -DHL_READOBJ=${HL_READOBJ_EXECUTABLE}
      -DHL_OBJDUMP=${HL_PE_OBJDUMP_EXECUTABLE}
      -DHL_NM=${HL_PE_NM_EXECUTABLE}
      -DHL_SKIP_CODE=77
      -P ${CMAKE_SOURCE_DIR}/cmake/CheckPeImports.cmake)
  # SKIP_RETURN_CODE, so "nothing was built to inspect" reports as NOT RUN
  # rather than as a pass. A green tick would assert that the import surface was
  # audited when it was not, and a lane that reports coverage it does not have
  # is worse than a lane that reports none.
  set_tests_properties(gate.windows-imports PROPERTIES
    LABELS "unit;gate" SKIP_RETURN_CODE 77
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# --- gate.ci-lane-parity, for a host with no guest corpus --------------------
# CMakeLists.txt includes cmake/LaneParity.cmake only under HL_HAVE_GUEST_CC.
# That condition is a proxy: it is right for Linux and Darwin, whose declared
# lane lists are mostly guest-backed suites, and wrong for a host whose declared
# list contains none of them -- there the corpus is irrelevant and the effect is
# that the ONE guard against a declared-but-empty lane is missing on exactly the
# newest, least-proven host. A token in HL_CI_HOSTS with nothing counting its
# lanes is a declaration, not a check.
#
# LaneParity.cmake now decides for itself (it reads HL_CI_HOSTS and returns
# early when this host's token is undeclared, or when the guest-backed lanes it
# would have to count are absent), so this only has to reach it. The condition
# is the exact complement of CMakeLists.txt's, so the file is included once and
# never twice; when that include site is relaxed, delete this block.
if(HL_BUILD_TESTS AND NOT HL_HAVE_GUEST_CC)
  include(${CMAKE_CURRENT_LIST_DIR}/LaneParity.cmake)
endif()

# --- package-test: install into a staging root, link a consumer, run it -----
# The Makefile does this by re-invoking itself with DESTDIR; the CMake form
# runs `cmake --install` into a staging prefix from a driver script so the
# whole thing is one CTest case.
if(HL_BUILD_TESTS)
  set(HL_PKG_ROOT ${CMAKE_BINARY_DIR}/package-root)

  add_test(NAME package.consumer-link
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DSTAGE=${HL_PKG_ROOT}
      -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DCC=${CMAKE_C_COMPILER}
      -DPACKAGE_HOST=${HL_PACKAGE_HOST}
      -DHAVE_ACTIVATION=${HL_HAVE_ACTIVATION}
      -DIS_DARWIN=$<BOOL:$<STREQUAL:${CMAKE_SYSTEM_NAME},Darwin>>
      -DCODESIGN=${HL_CODESIGN}
      -DJIT_ENTITLEMENTS=${HL_JIT_ENTITLEMENTS}
      -P ${CMAKE_SOURCE_DIR}/cmake/PackageTest.cmake)
  # TIMEOUT well under the CI step's own bound (tools/ci_run.sh gives the lane
  # 1320s). CTest's 1500s default is larger than that bound, so a hung guest
  # launch in the activation leg expired the STEP first: the annotation said
  # "TIMED OUT" with no test named, and the driver's progress output was lost.
  # Expiring here instead makes ctest print "package.consumer-link (Timeout)"
  # plus everything the driver had printed up to the step that hung.
  set_tests_properties(package.consumer-link PROPERTIES
    LABELS "package" RESOURCE_LOCK hl-package TIMEOUT 900
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # package-activation: the installed activation library, self-test plus the
  # guest-execution leg (posix_spawn-self path).
  if(HL_HAVE_ACTIVATION AND HL_ACTIVATION_TARGET)
    set_tests_properties(package.consumer-link PROPERTIES
      LABELS "package;package-activation;package-embedded" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endif()
