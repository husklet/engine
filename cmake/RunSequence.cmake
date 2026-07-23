# Script-mode helper: run CMD0, CMD1, ... in order, failing on the first
# non-zero status. Used by the mac gates, which are a short ordered sequence
# (identity audit, then one or more bridge launches) that must stay inside ONE
# launching process for the host firewall's four-launch budget.
set(_i 0)
while(TRUE)
  if(NOT DEFINED CMD${_i})
    break()
  endif()
  separate_arguments(_cmd UNIX_COMMAND "${CMD${_i}}")
  execute_process(COMMAND ${_cmd} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "step ${_i} failed (${rc}): ${CMD${_i}}")
  endif()
  math(EXPR _i "${_i}+1")
endwhile()
