# Script-mode helper: fail unless every path in -DFILES=a;b;c is executable.
# Used by the compat-filesystem precondition check that the Makefile writes as
# a `test -x ... -a -x ...` shell guard.
foreach(f ${FILES})
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "missing fixture: ${f}")
  endif()
  if(NOT IS_DIRECTORY "${f}")
    execute_process(COMMAND test -x "${f}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "fixture not executable: ${f}")
    endif()
  endif()
endforeach()
