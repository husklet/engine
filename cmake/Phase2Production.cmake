# ---------------------------------------------------------------------------
# Phase 2 — Linux production engines + Linux dual-backend embedded archive.
#
# Ported from the Makefile lane at lines ~1241-1615. Linux-host only (the
# macOS production engines / dual archive are Phase 4 and live behind the mac
# host guard). Additive and non-gating: `make` remains authoritative.
#
# The flavors the Makefile builds on a Linux host:
#   build/linux-production/hl-engine-linux-{aarch64,x86_64}  <- native Linux
#   build/package/linux-aarch64/libhl-engine.a              <- dual embedded
#   (the two build/production/* mac engines + mac dual .a are Phase 4)
#
# Key Makefile subtleties reproduced here:
#  * Unity TUs (src/core/target/{arch}.c, lifecycle.c) textually #include the
#    whole engine tree; the compiler emits no dep info make/cmake can see, so
#    we attach the globbed src tree as explicit OBJECT_DEPENDS (mirrors the
#    Makefile PRODUCTION_UNITY_DEPS rwildcard).
#  * The provider/environment TUs are compiled SEPARATELY (not folded into the
#    unity object) because they carry file-local put32/get32 helpers that would
#    collide at link time if merged (Makefile comment at line 1244-1245).
#  * Production/dual TUs use a DIFFERENT, minimal flag set than the Phase-1
#    libraries: -O2 + specific -D only, NO -std/-W/-fvisibility. They link the
#    hl_cpp_flags interface (CPPFLAGS only). The embedded rebuild + activation.o
#    DO use the full ENGINE_CFLAGS (hl_engine_cflags), matching the Makefile.
#  * -D_GNU_SOURCE is on target/provider/dual objects but ABSENT on the
#    lifecycle-core objects (Makefile lines 1263-1269) — preserved exactly.
# ---------------------------------------------------------------------------

# NOTE: the unity dep closure and hl_object() are HOST-AGNOSTIC helpers and are
# deliberately defined ABOVE the Linux guard below: the macOS lane
# (Phase4Mac.cmake) and the Linux-only gate sections both call hl_object(), so
# leaving it inside the guard made it undefined on a Darwin configure.
# Explicit dependency closure for the unity translation units (PRODUCTION_UNITY_DEPS).
file(GLOB_RECURSE _HL_UNITY_DEPS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/core/*.c   ${CMAKE_SOURCE_DIR}/src/core/*.h
  ${CMAKE_SOURCE_DIR}/src/host/*.c   ${CMAKE_SOURCE_DIR}/src/host/*.h
  ${CMAKE_SOURCE_DIR}/src/linux_abi/*.c ${CMAKE_SOURCE_DIR}/src/linux_abi/*.h
  ${CMAKE_SOURCE_DIR}/src/translator/*.c ${CMAKE_SOURCE_DIR}/src/translator/*.h
  ${CMAKE_SOURCE_DIR}/include/hl/*.h)

# Helper: one .c -> OBJECT library with an exact flag list (CPPFLAGS via the
# hl_cpp_flags interface + the caller's FLAGS), optionally with the unity dep
# closure. All the bespoke production flag combos live in ONE place.
#   hl_object(<name> <source> FLAGS <...> [UNITY])
function(hl_object name src)
  cmake_parse_arguments(A "UNITY" "" "FLAGS" ${ARGN})
  add_library(${name} OBJECT ${src})
  target_link_libraries(${name} PRIVATE hl_cpp_flags)
  target_compile_options(${name} PRIVATE ${A_FLAGS})
  if(A_UNITY)
    set_source_files_properties(${src} PROPERTIES OBJECT_DEPENDS "${_HL_UNITY_DEPS}")
  endif()
endfunction()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()


# ---- native Linux production engines --------------------------------------
# Provider subsystem: one TU each (put32/get32 collision avoidance).
set(_prov environment provider/client provider/demux provider/files
          provider/handles provider/namespace)
set(_prov_objs "")
foreach(p ${_prov})
  string(REPLACE "/" "_" tn "prod_prov_${p}")
  hl_object(${tn} src/core/${p}.c FLAGS -D_GNU_SOURCE -O2)
  list(APPEND _prov_objs $<TARGET_OBJECTS:${tn}>)
endforeach()

# Per-guest-arch production engine.  guest_isa in {AARCH64,X86_64}.
function(hl_linux_production arch guest_isa link_extra)
  hl_object(prod_target_${arch} src/core/target/${arch}.c
            FLAGS -D_GNU_SOURCE -O2 UNITY)
  # lifecycle-core: note NO -D_GNU_SOURCE (matches Makefile lines 1263-1269).
  hl_object(prod_life_${arch} src/core/lifecycle.c
            FLAGS -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_${guest_isa} -O2)
  add_executable(hl-engine-linux-${arch}
    $<TARGET_OBJECTS:prod_target_${arch}>
    $<TARGET_OBJECTS:prod_life_${arch}>
    ${_prov_objs})
  target_link_libraries(hl-engine-linux-${arch} PRIVATE
    hl-engine hl-translator hl-linux-abi hl-host-linux ${link_extra})
  set_target_properties(hl-engine-linux-${arch} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/linux-production)
endfunction()

# aarch64 links -pthread -lm -ldl -latomic; x86_64 lane links -pthread -lm
# (Makefile lines 1325 / 1331).
hl_linux_production(aarch64 AARCH64 "-pthread;-lm;-ldl;-latomic")
hl_linux_production(x86_64  X86_64  "-pthread;-lm")

# ---- Linux dual-backend embedded activation archive -----------------------
# libhl-engine-activation.a == build/package/linux-aarch64/libhl-engine.a.
# Members: the namespaced dual TUs + activation/dispatch + a FULL embedded
# rebuild of core/ir/abi/host with -DHL_EMBEDDED_BUILD.
set(EMBEDDED_SOURCES ${CORE_SOURCES} ${IR_SOURCES} ${LINUX_ABI_SOURCES} ${LINUX_HOST_SOURCES})

# Embedded object lib: full ENGINE_CFLAGS + _GNU_SOURCE + HL_EMBEDDED_BUILD
# (Makefile rule at lines 410-412).
add_library(hl-embedded-objs OBJECT ${EMBEDDED_SOURCES})
target_link_libraries(hl-embedded-objs PRIVATE hl_engine_cflags)
target_compile_options(hl-embedded-objs PRIVATE -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1)

# Namespaced dual TUs (minimal flag set + namespacing defines).
hl_object(dual_aarch64_target src/core/target/aarch64.c
  FLAGS -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 -DHL_TARGET_NAMESPACE=aarch64 -O2 UNITY)
hl_object(dual_x86_64_target src/core/target/x86_64.c
  FLAGS -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 -DHL_TARGET_NAMESPACE=x86_64 -O2 UNITY)
hl_object(dual_aarch64_core src/core/lifecycle.c
  FLAGS -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=aarch64 -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2)
hl_object(dual_x86_64_core src/core/lifecycle.c
  FLAGS -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=x86_64 -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2)
hl_object(dual_dispatch src/core/target/dual.c FLAGS -D_GNU_SOURCE -O2)
# activation.o uses full ENGINE_CFLAGS + _GNU_SOURCE (Makefile line 1580).
add_library(dual_activation OBJECT src/core/activation.c)
target_link_libraries(dual_activation PRIVATE hl_engine_cflags)
target_compile_options(dual_activation PRIVATE -D_GNU_SOURCE)

add_library(hl-engine-activation STATIC
  $<TARGET_OBJECTS:dual_aarch64_target> $<TARGET_OBJECTS:dual_x86_64_target>
  $<TARGET_OBJECTS:dual_aarch64_core>   $<TARGET_OBJECTS:dual_x86_64_core>
  $<TARGET_OBJECTS:dual_dispatch>       $<TARGET_OBJECTS:dual_activation>
  $<TARGET_OBJECTS:hl-embedded-objs>)
set_target_properties(hl-engine-activation PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/package/linux-aarch64
  OUTPUT_NAME hl-engine)   # emits libhl-engine.a, matching the Makefile artifact

# link-test: dual_backend_e2e_runner linked with --whole-archive (Makefile 1612).
add_executable(dual-backend-link-test tools/dual_backend_e2e_runner.c)
target_link_libraries(dual-backend-link-test PRIVATE hl_cpp_flags)
target_compile_options(dual-backend-link-test PRIVATE -D_GNU_SOURCE)
target_link_options(dual-backend-link-test PRIVATE
  -Wl,--whole-archive $<TARGET_FILE:hl-engine-activation> -Wl,--no-whole-archive
  -pthread -ldl -lm -latomic)
add_dependencies(dual-backend-link-test hl-engine-activation)
set_target_properties(dual-backend-link-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/package/linux-aarch64)
