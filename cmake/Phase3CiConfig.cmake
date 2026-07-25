# CI workflow configuration tests are host-independent: they inspect YAML and
# intentionally do not depend on a compiler or guest toolchain.
#
# Invoke the scripts through the configured shell instead of their
# /usr/bin/env shebang. Nix build sandboxes deliberately have no /usr/bin/env,
# so executing either script directly makes CTest report BAD_COMMAND even
# though the script itself is present.
find_program(HL_BASH_EXECUTABLE NAMES bash REQUIRED)

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
