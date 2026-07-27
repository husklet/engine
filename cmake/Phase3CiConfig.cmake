# CI workflow configuration tests are host-independent: they inspect YAML and
# intentionally do not depend on a compiler or guest toolchain.
#
# Invoke the scripts through the configured shell instead of their
# /usr/bin/env shebang. Nix build sandboxes deliberately have no /usr/bin/env,
# so executing either script directly makes CTest report BAD_COMMAND even
# though the script itself is present.
# HL_BASH_EXECUTABLE is found once, in CMakeLists.txt, and is OPTIONAL there:
# this file used to repeat the find_program with REQUIRED, which made a missing
# shell fail the whole configure over two YAML-inspecting tests. Their subject is
# .github/workflows/*.yml, which CI itself checks on Linux and macOS on every
# push, so skipping them on a shell-less host reduces no coverage that matters.
if(NOT HL_BASH_EXECUTABLE)
  message(STATUS
    "bash not found -- unit.ci-workflow-invariants and unit.publish-gating are not registered")
  return()
endif()

add_test(NAME unit.ci-workflow-invariants
  COMMAND ${HL_BASH_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/check_ci_workflows.sh invariants)
set_tests_properties(unit.ci-workflow-invariants PROPERTIES
  LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME unit.publish-gating
  COMMAND ${HL_BASH_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/check_ci_workflows.sh publish-gate)
set_tests_properties(unit.publish-gating PROPERTIES
  LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
