# The complete source/header closure consumed by the production unity
# translation units and therefore by both prebuilt Rust crate archives.
#
# This is deliberately a CMake-owned data source: the archive freshness tools
# can evaluate it in script mode, while configured builds use the exact same
# list for OBJECT_DEPENDS and expose it as archive-sources.txt.
function(hl_collect_archive_sources out_var source_dir)
  set(_patterns
    "${source_dir}/src/core/*.c" "${source_dir}/src/core/*.h"
    "${source_dir}/src/host/*.c" "${source_dir}/src/host/*.h"
    "${source_dir}/src/linux_abi/*.c" "${source_dir}/src/linux_abi/*.h"
    "${source_dir}/src/translator/*.c" "${source_dir}/src/translator/*.h"
    "${source_dir}/include/hl/*.h")
  if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE _sources RELATIVE "${source_dir}" ${_patterns})
  else()
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
      RELATIVE "${source_dir}" ${_patterns})
  endif()
  list(SORT _sources)
  set(${out_var} "${_sources}" PARENT_SCOPE)
endfunction()
