# Host-independent C sources maintained by this repository. Generated files and
# compatibility fixtures are intentionally outside these roots.
file(GLOB_RECURSE HL_MAINTAINED_C_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/include/*.c"
  "${CMAKE_SOURCE_DIR}/include/*.h"
  "${CMAKE_SOURCE_DIR}/linter/src/*.c"
  "${CMAKE_SOURCE_DIR}/linter/src/*.h"
  "${CMAKE_SOURCE_DIR}/linter/tests/*.c"
  "${CMAKE_SOURCE_DIR}/linter/tests/*.h"
  "${CMAKE_SOURCE_DIR}/src/*.c"
  "${CMAKE_SOURCE_DIR}/src/*.h"
  "${CMAKE_SOURCE_DIR}/tests/unit/*.c"
  "${CMAKE_SOURCE_DIR}/tests/unit/*.h"
  "${CMAKE_SOURCE_DIR}/tools/*.c"
  "${CMAKE_SOURCE_DIR}/tools/*.h")

list(SORT HL_MAINTAINED_C_SOURCES)

foreach(_hl_required_source IN ITEMS
    "${CMAKE_SOURCE_DIR}/linter/src/hl_lint.c"
    "${CMAKE_SOURCE_DIR}/src/host/macos/host.c"
    "${CMAKE_SOURCE_DIR}/src/host/windows/file.c"
    "${CMAKE_SOURCE_DIR}/tools/windows/tier0_probe.c")
  if(NOT _hl_required_source IN_LIST HL_MAINTAINED_C_SOURCES)
    message(FATAL_ERROR "maintained C source inventory omitted ${_hl_required_source}")
  endif()
endforeach()
