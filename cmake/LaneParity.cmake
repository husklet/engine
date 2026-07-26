# Keep every CI lane declared in cmake/CiLanes.cmake represented in the CTest
# registry on each host where it applies. CTest treats an empty -L selection as
# success, so this test is the permanent guard against a renamed or deleted
# label silently turning a workflow step green.
#
# GuestFixtures.cmake intentionally omits all guest-backed suites when the cross
# compilers are unavailable (notably checks.*.unit). In that reduced registry
# there are no compatibility lanes to compare. The full CI CMake build runs
# inside `nix develop`, has both compilers, and registers this gate.
if(NOT HL_HAVE_GUEST_CC)
  return()
endif()

find_program(HL_BASH_EXECUTABLE NAMES bash REQUIRED)

# The host is the (OS, CPU) PAIR, not the OS: the two Linux host CPUs register
# different tests (perf.native-*, isa-fuzz.aarch64-*), so a lane empty on one and
# full on the other is an asymmetry the gate must name. HL_HOST_ARCH comes from
# CMakeLists.txt; do not re-derive it here.
add_test(NAME gate.ci-lane-parity
  COMMAND ${HL_BASH_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/check_lane_parity.sh
          ${CMAKE_CTEST_COMMAND} ${CMAKE_BINARY_DIR}
          ${CMAKE_SYSTEM_NAME} ${HL_HOST_ARCH})
set_tests_properties(gate.ci-lane-parity PROPERTIES
  LABELS "unit;gate" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
