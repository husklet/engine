# CI workflow configuration tests are host-independent: they inspect YAML and
# intentionally do not depend on a compiler or guest toolchain.
add_test(NAME unit.ci-workflow-invariants
  COMMAND ${CMAKE_SOURCE_DIR}/tools/check_ci_workflows.sh invariants)
set_tests_properties(unit.ci-workflow-invariants PROPERTIES
  LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME unit.publish-gating
  COMMAND ${CMAKE_SOURCE_DIR}/tools/check_ci_workflows.sh publish-gate)
set_tests_properties(unit.publish-gating PROPERTIES
  LABELS "unit" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
