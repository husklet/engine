# Keep every historical Makefile CI lane represented in the CTest registry on
# each host where that lane applies. CTest treats an empty -L selection as
# success, so this test is the permanent guard against a renamed or deleted
# label silently turning a workflow step green.
add_test(NAME gate.makefile-lane-parity
  COMMAND ${CMAKE_SOURCE_DIR}/tools/check_lane_parity.sh
          ${CMAKE_CTEST_COMMAND} ${CMAKE_BINARY_DIR} ${CMAKE_SYSTEM_NAME})
set_tests_properties(gate.makefile-lane-parity PROPERTIES
  LABELS "unit;gate" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

