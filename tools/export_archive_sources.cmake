if(NOT DEFINED HL_SOURCE_DIR OR NOT DEFINED HL_OUTPUT)
  message(FATAL_ERROR "HL_SOURCE_DIR and HL_OUTPUT are required")
endif()

include("${HL_SOURCE_DIR}/cmake/ArchiveSources.cmake")
hl_collect_archive_sources(_sources "${HL_SOURCE_DIR}")
if(NOT _sources)
  message(FATAL_ERROR "archive source list is empty")
endif()

string(REPLACE ";" "\n" _contents "${_sources}")
file(WRITE "${HL_OUTPUT}" "${_contents}\n")

