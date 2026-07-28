# ---------------------------------------------------------------------------
# clang-format targets over the host-independent maintained source inventory.
# ---------------------------------------------------------------------------
find_program(HL_CLANG_FORMAT NAMES clang-format)

if(NOT HL_CLANG_FORMAT)
  message(STATUS "clang-format not found -- `format`/`format-check` targets unavailable")
  return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/SourceInventory.cmake")

add_custom_target(format
  COMMAND ${HL_CLANG_FORMAT} -i ${HL_MAINTAINED_C_SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-format -i (in place)")

add_custom_target(format-check
  COMMAND ${HL_CLANG_FORMAT} --dry-run --Werror ${HL_MAINTAINED_C_SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-format --dry-run --Werror")
