# ---------------------------------------------------------------------------
# clang-format targets, mirroring the Makefile's `format` / `format-check`.
#
# The Makefile passes an explicit source list rather than globbing, so that
# generated or vendored code is never reformatted. We reuse the SAME lists the
# rest of the CMake build already defines, plus the extra files the Makefile
# names directly, so the two build systems format exactly the same set.
# ---------------------------------------------------------------------------
find_program(HL_CLANG_FORMAT NAMES clang-format)

if(NOT HL_CLANG_FORMAT)
  message(STATUS "clang-format not found -- `format`/`format-check` targets unavailable")
  return()
endif()

set(_hl_fmt_files
  ${CORE_SOURCES} ${IR_SOURCES} ${LINUX_ABI_SOURCES}
  ${FAKE_HOST_SOURCES} ${COMMON_HOST_SOURCES} ${LINUX_HOST_SOURCES}
  src/runner/main.c)
file(GLOB _hl_fmt_extra
  ${CMAKE_SOURCE_DIR}/include/hl/*.h
  ${CMAKE_SOURCE_DIR}/tests/unit/*.c ${CMAKE_SOURCE_DIR}/tests/unit/*.h
  ${CMAKE_SOURCE_DIR}/tools/*.c)
list(APPEND _hl_fmt_files ${_hl_fmt_extra})
list(REMOVE_DUPLICATES _hl_fmt_files)

add_custom_target(format
  COMMAND ${HL_CLANG_FORMAT} -i ${_hl_fmt_files}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-format -i (in place)")

add_custom_target(format-check
  COMMAND ${HL_CLANG_FORMAT} --dry-run --Werror ${_hl_fmt_files}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-format --dry-run --Werror")
