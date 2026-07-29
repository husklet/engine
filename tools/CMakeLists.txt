# Standalone host tools. Compatibility runners stay with the lanes that own
# their platform and fixture requirements.

if(HL_HAVE_GUEST_CC AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_executable(bench-runner
    bench_runner.c
    config.c
    process.c)
  target_compile_options(bench-runner PRIVATE
    -O2 -g -std=gnu11 -Wall -Wextra
    -D_POSIX_C_SOURCE=200809L)
  set_target_properties(bench-runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

  add_executable(nested-engine-gate
    nested_engine_gate.c
    process.c)
  target_compile_options(nested-engine-gate PRIVATE
    -O2 -g -std=gnu11 -Wall -Wextra -Werror
    -D_POSIX_C_SOURCE=200809L)
  set_target_properties(nested-engine-gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

  add_executable(direct-launch-gate
    direct_launch_gate.c
    config.c)
  target_compile_options(direct-launch-gate PRIVATE
    -O2 -g -std=gnu11 -Wall -Wextra -Werror
    -D_POSIX_C_SOURCE=200809L)
  set_target_properties(direct-launch-gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

  add_executable(test-direct-launch-gate
    ${CMAKE_SOURCE_DIR}/tests/integration/direct_launch_gate.c)
  target_compile_options(test-direct-launch-gate PRIVATE
    -O2 -g -std=gnu11 -Wall -Wextra -Werror
    -D_POSIX_C_SOURCE=200809L)
  set_target_properties(test-direct-launch-gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME unit.direct-launch-gate
    COMMAND $<TARGET_FILE:test-direct-launch-gate>
            $<TARGET_FILE:direct-launch-gate>)
  set_tests_properties(unit.direct-launch-gate PROPERTIES
    LABELS unit WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

endif()

add_executable(lane-parity-gate lane_parity_gate.c)
target_compile_options(lane-parity-gate PRIVATE
  -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Werror)
set_target_properties(lane-parity-gate PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_executable(test-lane-parity-gate
    ${CMAKE_SOURCE_DIR}/tests/integration/lane_parity_gate.c
    lane_parity_gate.c)
  target_include_directories(test-lane-parity-gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(test-lane-parity-gate PRIVATE
    -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Werror
    -DHL_LANE_PARITY_LIBRARY=1)
  set_target_properties(test-lane-parity-gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME unit.lane-parity-gate
    COMMAND $<TARGET_FILE:test-lane-parity-gate>)
  set_tests_properties(unit.lane-parity-gate PROPERTIES
    LABELS unit WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
