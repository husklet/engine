# Standalone host tools. Compatibility runners stay with the lanes that own
# their platform and fixture requirements.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
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
endif()
