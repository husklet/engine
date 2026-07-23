# Script-mode helper: run CMD with the given args and require exit status EXPECT.
# Reproduces the Makefile's `<cmd> <guest>; test $$? -eq 42` lifecycle checks,
# where a non-zero status is the SUCCESS condition and CTest would otherwise
# treat it as failure.
set(_args)
foreach(v ARG1 ARG2 ARG3 ARG4)
  if(DEFINED ${v})
    list(APPEND _args "${${v}}")
  endif()
endforeach()
execute_process(COMMAND "${CMD}" ${_args} RESULT_VARIABLE rc)
if(NOT rc STREQUAL "${EXPECT}")
  message(FATAL_ERROR "${CMD}: exit ${rc}, expected ${EXPECT}")
endif()
