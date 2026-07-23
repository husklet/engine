# ---------------------------------------------------------------------------
# Phase 4 — ONE codesigning helper, applied as a POST_BUILD step.
#
# WHY this is centralised. The engine JIT keeps a dual-mapped cache: a plain
# anonymous RW region plus an RX alias made executable through mach_vm_remap +
# VM_PROT_EXECUTE. On macOS that alias is non-MAP_JIT executable memory, so a
# full-security host (SIP-on / hardened, e.g. the macos-26 CI runner) SIGKILLs
# any process that runs the engine unless the binary carries
# packaging/macos/jit.entitlements (allow-jit +
# allow-unsigned-executable-memory + disable-executable-page-protection).
#
# The Makefile repeats that codesign invocation about twenty times, once per
# mac binary. A single forgotten repetition is not a build error — it is a
# SIGKILL at run time with no diagnostic. Here there is exactly one definition
# and every mac executable target routes through it.
#
# Off macOS the function is a no-op, matching the Makefile's `JIT_SIGN = true`.
# ---------------------------------------------------------------------------

# Prefer the ABSOLUTE Apple tool over a PATH lookup. A nix build (and any other environment with a
# controlled PATH) has no `codesign` on PATH, so the install phase died with "codesign: command not
# found" the moment it linked hl-engine-runner. /usr/bin/codesign is nonetheless visible there --
# verified inside the darwin build sandbox -- so resolving it absolutely is both sufficient and honest.
#
# Do NOT substitute nixpkgs' sigtool here. Its `codesign` accepts --entitlements and silently ignores
# it: the resulting binary carries no entitlements at all, which is precisely the SIGKILL-at-runtime,
# no-diagnostic failure this file exists to prevent. Checked with `codesign -d --entitlements -`.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND EXISTS /usr/bin/codesign)
  set(HL_CODESIGN_DEFAULT /usr/bin/codesign)
else()
  set(HL_CODESIGN_DEFAULT codesign)
endif()
set(HL_CODESIGN ${HL_CODESIGN_DEFAULT} CACHE STRING "codesign tool (Makefile CODESIGN)")
set(HL_JIT_ENTITLEMENTS ${CMAKE_CURRENT_LIST_DIR}/../packaging/macos/jit.entitlements)

# hl_codesign(<target>...) — ad-hoc sign with the JIT entitlements after link.
function(hl_codesign)
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    return()
  endif()
  foreach(_t ${ARGN})
    add_custom_command(TARGET ${_t} POST_BUILD
      COMMAND ${HL_CODESIGN} -s - --entitlements ${HL_JIT_ENTITLEMENTS} -f $<TARGET_FILE:${_t}>
      COMMENT "codesign (JIT entitlements) ${_t}"
      VERBATIM)
  endforeach()
endfunction()

# hl_codesign_file(<path>) — same, for artefacts produced by a custom command
# rather than by a CMake target (returns the COMMAND fragment to append).
function(hl_codesign_file out path)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(${out} COMMAND ${HL_CODESIGN} -s - --entitlements ${HL_JIT_ENTITLEMENTS} -f "${path}"
        PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()
