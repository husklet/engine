# ---------------------------------------------------------------------------
# Phase 4 — install(), pkg-config generation, and the consumer smoke tests.
#
# Goal: the plain, boring, standard flow works and produces a usable SDK.
#
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake
#     cmake --build build
#     cmake --install build --prefix /usr/local
#
# giving  include/hl/*.h, lib/libhl-{engine,translator,linux-abi,host-<host>}.a,
# lib/libhl-engine-activation.a (aarch64 only), lib/pkgconfig/*.pc and
# bin/hl-engine-runner — the exact artefact set of the Makefile `install`
# target (Makefile 474-486).
# ---------------------------------------------------------------------------

set(HL_VERSION 0.1.10)

# The Makefile's HOST/HOST_ARCH conditionals (lines 165-207), reproduced.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(HL_PACKAGE_HOST macos)
else()
  set(HL_PACKAGE_HOST linux)
endif()
set(HL_PACKAGE_SYSTEM_LIBS "-pthread")

# The embedded activation archive exists on aarch64 only; on other hosts the
# activation header is not installed either, because nothing implements it.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(HL_HAVE_ACTIVATION TRUE)
else()
  set(HL_HAVE_ACTIVATION FALSE)
endif()

# --- the runner: a `make all` product and an installed binary ---------------
add_executable(hl-engine-runner src/runner/main.c)
target_link_libraries(hl-engine-runner PRIVATE hl_engine_cflags)
# These three archives are mutually recursive, so GNU ld needs an explicit
# --start-group/--end-group (LINK_GROUP RESCAN). Apple's linker does not support
# that feature -- and does not need it, since it rescans archives to a fixed
# point on its own -- so a Darwin link takes the plain list instead. Using
# LINK_GROUP unconditionally made a macOS configure fail outright with
# "Feature 'RESCAN' ... is not supported for the 'C' link language".
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
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
Libs: -L\${libdir} -lhl-host-@HL_PACKAGE_HOST@ -lhl-engine -lhl-translator -lhl-linux-abi @HL_PACKAGE_SYSTEM_LIBS@
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

set(HL_INSTALL_LIBS hl-engine hl-translator hl-linux-abi)
if(TARGET hl-host-linux)
  list(APPEND HL_INSTALL_LIBS hl-host-linux)
elseif(TARGET hl-host-macos)
  list(APPEND HL_INSTALL_LIBS hl-host-macos)
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

if(HL_HAVE_ACTIVATION AND TARGET hl-engine-activation)
  # Installed under the activation name; in the build tree it is the
  # package/<host>-aarch64/libhl-engine.a artefact (Makefile 481).
  install(FILES $<TARGET_FILE:hl-engine-activation>
          DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RENAME libhl-engine-activation.a)
  install(FILES ${HL_PC_DIR}/hl-engine-activation.pc
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# --- package-test: install into a staging root, link a consumer, run it -----
# The Makefile does this by re-invoking itself with DESTDIR; the CMake form
# runs `cmake --install` into a staging prefix from a driver script so the
# whole thing is one CTest case.
if(HL_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HL_PKG_ROOT ${CMAKE_BINARY_DIR}/package-root)

  add_test(NAME package.consumer-link
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DSTAGE=${HL_PKG_ROOT}
      -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DCC=${CMAKE_C_COMPILER}
      -DPACKAGE_HOST=${HL_PACKAGE_HOST}
      -DHAVE_ACTIVATION=${HL_HAVE_ACTIVATION}
      -P ${CMAKE_SOURCE_DIR}/cmake/PackageTest.cmake)
  set_tests_properties(package.consumer-link PROPERTIES
    LABELS "package" RESOURCE_LOCK hl-package WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # package-activation: the installed activation library, self-test plus the
  # guest-execution leg (posix_spawn-self path).
  if(HL_HAVE_ACTIVATION)
    set_tests_properties(package.consumer-link PROPERTIES
      LABELS "package;package-activation;package-embedded" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endif()
