# Source-manifest stamp force-included into every crate-archive object.
#
# hl_archive_stamp_setup(<source-closure>) declares the generated header and the
# `hl-archive-stamp` target; hl_stamp_archive_object(<target>) force-includes it
# into one OBJECT library. The header is regenerated whenever any file in the
# closure changes, so a changed source rewrites the stamp, which is an explicit
# compiler input and therefore recompiles every archive object -- and any object
# that escapes that still carries the old digest for the gate to find.
#
# Only the objects that end up inside pkgs/rust/assets/lib/*/libhl-engine.a are
# stamped; the native production engines are built from the same sources but are
# not shipped, and stamping them would rebuild them on every edit for nothing.

function(hl_archive_stamp_setup)
  set(_dir "${CMAKE_BINARY_DIR}/archive-stamp")
  set(HL_ARCHIVE_STAMP_HEADER "${_dir}/hl_archive_stamp.h" PARENT_SCOPE)
  add_custom_command(
    OUTPUT "${_dir}/hl_archive_stamp.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
    COMMAND ${HL_BASH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_archive_stamp.sh"
            "${_dir}/hl_archive_stamp.h"
    DEPENDS ${ARGN}
            "${CMAKE_SOURCE_DIR}/tools/gen_archive_stamp.sh"
            "${CMAKE_SOURCE_DIR}/tools/crate_archive_manifest.sh"
            "${CMAKE_SOURCE_DIR}/tools/export_archive_sources.cmake"
            "${CMAKE_SOURCE_DIR}/cmake/ArchiveSources.cmake"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Stamping crate-archive objects with the source manifest"
    VERBATIM)
  add_custom_target(hl-archive-stamp DEPENDS "${_dir}/hl_archive_stamp.h")
endfunction()

# Force-include the stamp. The compiler records -include files in its depfile,
# so a rewritten stamp recompiles the object without further bookkeeping.
function(hl_stamp_archive_object target)
  target_compile_options(${target} PRIVATE -include ${HL_ARCHIVE_STAMP_HEADER})
  add_dependencies(${target} hl-archive-stamp)
endfunction()
