# Content digest of every input that decides the guest fixture corpus.
#
# The corpus is ~3200 cross-compiled Linux guest programs. A host that cannot
# cross-compile them (Windows) has to consume a corpus built elsewhere, and a
# consumed build artifact that nothing ties back to its inputs is the exact
# failure this project has already paid for once with the prebuilt crate
# archives: a stale artifact still links, still runs, and still passes -- it
# simply tests a tree nobody has any more. So the corpus is filed under a digest
# of the sources and recipes that produced it, and the consuming configure
# recomputes that digest and refuses a mismatch.
#
# Byte-comparing a rebuilt corpus would be stronger but is not available: the
# compiler records the absolute source directory in every object, so two
# checkouts of one commit at different paths produce different bytes. A digest
# of the INPUTS has no such dependence.
#
# Two inputs decide a fixture:
#   1. its source          -- tests/{compat,e2e,perf,soak}
#   2. the recipe that compiles it -- the cmake files that call hl_guest_*()
# Both are hashed. The set is a deliberate over-approximation (a header only
# one arch reads still invalidates the whole corpus); over-reporting costs a
# rebuild, under-reporting costs a silently wrong test result.
#
# Implemented in pure CMake, with no shell and no external hashing tool,
# because the consumer is a Windows configure where neither is guaranteed.
#
# Usable two ways:
#   include(tools/guest_fixture_digest.cmake)   -> defines the two functions
#   cmake -DHL_SOURCE_DIR=<root> -P tools/guest_fixture_digest.cmake
#                                               -> prints the digest on stdout

# The cmake files that declare guest fixtures. Listed, not globbed: cmake/ holds
# a dozen other files whose churn has nothing to do with the corpus, and folding
# them in would invalidate it on every unrelated build change.
set(HL_GUEST_DIGEST_RECIPES
  cmake/GuestFixtures.cmake
  cmake/Phase3Compat.cmake
  cmake/Phase3Gates.cmake
  cmake/Phase4Mac.cmake
  cmake/LaneParity.cmake)

# hl_guest_fixture_inputs(<out-var> <source-root>)
#
# Every file that feeds the corpus, as paths relative to <source-root>, sorted.
# Also the exact set a consumer should re-run configure on: see the
# CMAKE_CONFIGURE_DEPENDS use in cmake/GuestFixtures.cmake.
function(hl_guest_fixture_inputs out root)
  set(_files "")
  foreach(_r ${HL_GUEST_DIGEST_RECIPES})
    if(EXISTS "${root}/${_r}")
      list(APPEND _files "${_r}")
    endif()
  endforeach()

  foreach(_suite compat e2e perf soak)
    if(NOT IS_DIRECTORY "${root}/tests/${_suite}")
      continue()
    endif()
    file(GLOB_RECURSE _found RELATIVE "${root}" "${root}/tests/${_suite}/*")
    foreach(_f ${_found})
      # expected/ holds golden OUTPUT, which no fixture is compiled from. It is
      # edited whenever a test's observed behaviour is re-baselined, and folding
      # it in would demand a full corpus rebuild for a text file no compiler
      # ever reads.
      if(_f MATCHES "/expected/")
        continue()
      endif()
      get_filename_component(_base "${_f}" NAME)
      # Compiler inputs, plus the committed guest binaries the `copy` linkage
      # ships (go_goro_x86, g_x64, hx, ...). Those have no extension at all,
      # which is what distinguishes them from manifest.tsv and README.md.
      if(_f MATCHES "\\.(c|h|S|s|inc)$" OR NOT _base MATCHES "\\.")
        list(APPEND _files "${_f}")
      endif()
    endforeach()
  endforeach()

  list(REMOVE_DUPLICATES _files)
  list(SORT _files)
  set(${out} "${_files}" PARENT_SCOPE)
endfunction()

# hl_guest_fixture_digest(<out-var> <source-root>)
#
# SHA-256 over "<per-file sha256>  <relative path>" lines. Hashing the paths as
# well as the contents makes the digest sensitive to a file being added,
# removed or renamed, not only edited.
function(hl_guest_fixture_digest out root)
  hl_guest_fixture_inputs(_files "${root}")
  set(_acc "")
  foreach(_f ${_files})
    file(SHA256 "${root}/${_f}" _h)
    string(APPEND _acc "${_h}  ${_f}\n")
  endforeach()
  string(SHA256 _digest "${_acc}")
  set(${out} "${_digest}" PARENT_SCOPE)
endfunction()

# Script mode: print the digest (and, with HL_LIST_INPUTS, the file list) so the
# builder script can record what it built without reimplementing any of this.
if(CMAKE_SCRIPT_MODE_FILE)
  if(NOT DEFINED HL_SOURCE_DIR)
    message(FATAL_ERROR
      "usage: cmake -DHL_SOURCE_DIR=<repo root> [-DHL_LIST_INPUTS=ON] -P tools/guest_fixture_digest.cmake")
  endif()
  if(HL_LIST_INPUTS)
    hl_guest_fixture_inputs(_l "${HL_SOURCE_DIR}")
    foreach(_f ${_l})
      message("${_f}")
    endforeach()
  else()
    hl_guest_fixture_digest(_d "${HL_SOURCE_DIR}")
    message("${_d}")
  endif()
endif()
