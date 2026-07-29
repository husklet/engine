# Assert that every declared CI lane selects a configured test. The manifest is
# generated after all real tests and before this guard, so it cannot count
# itself.
include(${CMAKE_CURRENT_LIST_DIR}/CiLanes.cmake)

set(_hl_host_token ${CMAKE_SYSTEM_NAME}-${HL_HOST_ARCH})
if(NOT _hl_host_token IN_LIST HL_CI_HOSTS)
  message(STATUS
    "${_hl_host_token} is not declared in HL_CI_HOSTS -- gate.ci-lane-parity is "
    "not registered")
  return()
endif()
if(NOT HL_HAVE_GUEST_CC AND
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
  message(STATUS
    "no guest cross compilers -- gate.ci-lane-parity is not registered, because "
    "this host's declared lanes include guest-backed suites")
  return()
endif()

function(_hl_lane_manifest_tests directory output)
  set(_records "")
  get_property(_tests DIRECTORY "${directory}" PROPERTY TESTS)
  foreach(_test IN LISTS _tests)
    get_property(_labels TEST "${_test}" DIRECTORY "${directory}" PROPERTY LABELS)
    foreach(_label IN LISTS _labels)
      string(APPEND _records "test\t${_test}\t${_label}\n")
    endforeach()
  endforeach()
  get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_child IN LISTS _children)
    _hl_lane_manifest_tests("${_child}" _child_records)
    string(APPEND _records "${_child_records}")
  endforeach()
  set(${output} "${_records}" PARENT_SCOPE)
endfunction()

set(_hl_lane_manifest "")
foreach(_host IN LISTS HL_CI_HOSTS)
  string(APPEND _hl_lane_manifest "host\t${_host}\n")
endforeach()
foreach(_entry IN LISTS HL_CI_HOST_CPU_ONLY)
  string(REPLACE ":" ";" _parts "${_entry}")
  list(LENGTH _parts _part_count)
  if(NOT _part_count EQUAL 2)
    message(FATAL_ERROR "HL_CI_HOST_CPU_ONLY entry must be <host>:<lane>: ${_entry}")
  endif()
  list(GET _parts 0 _host)
  list(GET _parts 1 _lane)
  string(APPEND _hl_lane_manifest "reserve\t${_host}\t${_lane}\n")
endforeach()
foreach(_os Linux Darwin Windows)
  string(TOUPPER "${_os}" _upper)
  foreach(_kind SHARDED DIRECT REGISTRY)
    foreach(_lane IN LISTS HL_CI_${_kind}_${_upper})
      string(APPEND _hl_lane_manifest "lane\t${_os}\t${_lane}\n")
    endforeach()
  endforeach()
endforeach()
_hl_lane_manifest_tests("${CMAKE_SOURCE_DIR}" _hl_test_records)
string(APPEND _hl_lane_manifest "${_hl_test_records}")

set(_hl_lane_manifest_path "${CMAKE_BINARY_DIR}/lane-parity.manifest")
file(GENERATE OUTPUT "${_hl_lane_manifest_path}" CONTENT "${_hl_lane_manifest}")
add_test(NAME gate.ci-lane-parity
  COMMAND $<TARGET_FILE:lane-parity-gate>
          "${_hl_lane_manifest_path}" "${CMAKE_SYSTEM_NAME}" "${HL_HOST_ARCH}")
set_tests_properties(gate.ci-lane-parity PROPERTIES
  LABELS "unit;gate" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
