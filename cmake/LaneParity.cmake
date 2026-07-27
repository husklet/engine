# Keep every CI lane declared in cmake/CiLanes.cmake represented in the CTest
# registry on each host where it applies. CTest treats an empty -L selection as
# success, so this test is the permanent guard against a renamed or deleted
# label silently turning a workflow step green.
#
# WHEN THIS GATE APPLIES. The condition used to be a bare `HL_HAVE_GUEST_CC`,
# and that was a proxy standing in for the real question. The real question is
# "can this host's DECLARED lane set be non-empty here", and the guest cross
# compilers answer it only for the hosts whose declared set contains
# guest-backed lanes -- which is Linux and Darwin, where the lists carry compat,
# production, checkpoint and perf. GuestFixtures.cmake omits all of those
# without the compilers, so on a reduced registry there is genuinely nothing to
# compare and skipping is correct.
#
# It answers it WRONGLY for a host whose declared set contains no guest-backed
# lane at all. There the guest compilers are irrelevant and the proxy silently
# removes the only structural guard the host has -- a token declared in
# HL_CI_HOSTS with gate.ci-lane-parity absent is a declaration nothing checks,
# which is precisely the hole this gate exists to fill.
#
# So: ask CiLanes.cmake directly. It is a file of plain set() calls, and this is
# the first consumer to include() it rather than parse it textually -- the two
# shell guards still parse, so keep the format literal for them.
include(${CMAKE_CURRENT_LIST_DIR}/CiLanes.cmake)
set(_hl_host_token ${CMAKE_SYSTEM_NAME}-${HL_HOST_ARCH})
if(NOT _hl_host_token IN_LIST HL_CI_HOSTS)
  # An undeclared host has no lane lists, and check_lane_parity.sh refuses such
  # a token with exit 2 rather than checking nothing. Not an error: it is the
  # state of every host before its first declaration.
  message(STATUS
    "${_hl_host_token} is not declared in HL_CI_HOSTS -- gate.ci-lane-parity is "
    "not registered. Declaring the token and registering this gate is ONE "
    "change: a token whose lanes nothing counts is worse than no token.")
  return()
endif()

# The guest-backed lanes are declared for Linux and Darwin, so those two still
# need the corpus before their declared set can be non-empty. A host that
# declares none does not.
if(NOT HL_HAVE_GUEST_CC AND
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
  message(STATUS
    "no guest cross compilers -- gate.ci-lane-parity is not registered, because "
    "this host's declared lanes include guest-backed suites that the reduced "
    "registry does not contain")
  return()
endif()

# HL_BASH_EXECUTABLE is found once, in CMakeLists.txt, and is OPTIONAL there.
# Unreachable without it in practice -- this file already requires
# HL_HAVE_GUEST_CC, i.e. the nix devShell, which supplies bash -- but the
# duplicate REQUIRED find_program that used to sit here would have failed a
# configure rather than skipping one test.
if(NOT HL_BASH_EXECUTABLE)
  message(STATUS "bash not found -- gate.ci-lane-parity is not registered")
  return()
endif()

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
