# gate.windows-imports -- assert the Windows engine binaries import nothing that
# will kill a cloned process.  Run in CMake script mode (`cmake -P`), because a
# Windows host is the one host where bash is optional (CMakeLists.txt finds it
# non-REQUIRED) and this gate must not be the thing that goes missing there.
#
# WHY THIS EXISTS, stated in full here so that nobody who trips it has to go
# looking, and so that nobody "fixes" it by widening the allowlist:
#
#   Guest fork() on Windows is served by RtlCloneUserProcess.  A clone inherits
#   the parent's address space copy-on-write, but NOT the win32k/CSR-side
#   regions that USER32 and its dependents keep pointers into.  IMM32.DLL --
#   which is loaded whenever USER32.dll is imported, and never on its own --
#   dereferences one of those pointers in its DLL_THREAD_DETACH handler.  So
#   inside a cloned process, the first thread that is created and then EXITS
#   dies 0xC0000005 at IMM32.DLL+0x7f72.  The fault is in thread DETACH, not in
#   thread creation and not in thread attach: the thread runs to completion,
#   with correct __thread storage, and dies on the way out.
#
#   Measured rather than reasoned: 50 of 50 runs died with a USER32 import, 0 of
#   50 without it, in an otherwise byte-identical binary.  Deterministic in both
#   directions -- there is no flakiness to average away and nothing to retry
#   past.  The whole difference between the two binaries was ONE call to
#   wsprintfW.  Removing it left the image KERNEL32-only and made plain
#   kernel32!CreateThread clean in a clone.
#
#   It has to be a BUILD gate because the failure needs a guest to fork AND THEN
#   create and join a thread.  That is a JVM, a Go runtime, a Node worker -- no
#   smoke test does it, and no compat case would go red before a user's build
#   hung.  A runtime check would be a hope; this is a gate.
#
# Two layers, because the authoritative subject does not exist yet:
#
#   1. PE import tables (llvm-readobj --coff-imports, or objdump -p).  This is
#      the real check and the only complete one.  It acquires subjects at the
#      milestone where the runner first links.
#   2. Undefined symbols in the static archives (llvm-nm --undefined-only)
#      matched against a list of named traps.  INCOMPLETE by construction -- a
#      name not on the list is not caught -- but it bites now, on the archives
#      that already build, and it catches wsprintfW, which is the call that
#      actually happened.  An early warning, not a substitute for layer 1.
#
# Inputs (all -D on the cmake -P command line).  The two path lists are
# `|`-separated, not `;`-separated: they are built from $<TARGET_FILE:...>
# generator expressions in an add_test() COMMAND, where a `;` would be split
# into separate argv entries before this file ever sees it.
#   HL_PE_IMAGES    PE images to inspect.  Entries that do not exist are
#                   reported and skipped: a target that has not linked yet is
#                   the normal state of this port, not a gate failure.
#   HL_PE_ARCHIVES  static archives for layer 2.  Same treatment.
#   HL_READOBJ      llvm-readobj, or empty.
#   HL_OBJDUMP      llvm-objdump / objdump, or empty.  Fallback for layer 1.
#   HL_NM           llvm-nm, or empty.  Layer 2 is skipped without it.
#   HL_SKIP_CODE    exit code meaning "no subject" (CTest SKIP_RETURN_CODE).

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED HL_SKIP_CODE)
  set(HL_SKIP_CODE 77)
endif()
string(REPLACE "|" ";" HL_PE_IMAGES "${HL_PE_IMAGES}")
string(REPLACE "|" ";" HL_PE_ARCHIVES "${HL_PE_ARCHIVES}")

# --- the allowlist ----------------------------------------------------------
# KERNEL32 and ntdll are the two the engine is allowed to depend on directly.
# The api-ms-win-* apisets are the UCRT's own contract DLLs: a mingw -static
# link emits api-ms-win-crt-{stdio,runtime,heap,string,...}, they resolve to
# ucrtbase, and none of them drags in win32k.  Everything else is refused,
# whether or not this file can explain why -- an unlisted DLL is an unaudited
# DLL_THREAD_DETACH handler, which is precisely the hazard.
set(_allow_exact "kernel32.dll" "ntdll.dll")
set(_allow_prefix "api-ms-win-")

# Named for the failure message only.  Being absent from this list is not a
# pass; being present just means the message can say more.
set(_known_bad
  "user32.dll"   "imports IMM32, whose DLL_THREAD_DETACH faults in a clone"
  "imm32.dll"    "THE faulting module -- experiment-rtlclone.md 3.5"
  "gdi32.dll"    "win32k-backed, same class as USER32"
  "shell32.dll"  "pulls in the shell/COM apartment machinery"
  "ole32.dll"    "COM apartments are per-thread state a clone does not carry"
  "shlwapi.dll"  "loads SHELL32")

# --- layer 2's named traps --------------------------------------------------
# Symbol -> the DLL it comes from.  Matched with and without mingw's __imp_
# prefix.  wsprintfW is first because it is the one that actually happened.
set(_trap_symbols
  "wsprintfW"          "USER32.dll (this is the exact call the experiment found)"
  "wsprintfA"          "USER32.dll"
  "wvsprintfW"         "USER32.dll"
  "wvsprintfA"         "USER32.dll"
  "MessageBoxW"        "USER32.dll"
  "MessageBoxA"        "USER32.dll"
  "CharNextW"          "USER32.dll"
  "CharUpperW"         "USER32.dll"
  "CommandLineToArgvW" "SHELL32.dll (the -municode alternative in build-system.md 2.3 -- do not reach for it)"
  "SHGetFolderPathW"   "SHELL32.dll"
  "ShellExecuteW"      "SHELL32.dll"
  "PathFileExistsW"    "SHLWAPI.dll"
  "CoInitializeEx"     "ole32.dll"
  "CoTaskMemFree"      "ole32.dll"
  "CoCreateInstance"   "ole32.dll"
  "ImmDisableIME"      "IMM32.dll")

function(hl_reason_for name out)
  string(TOLOWER "${name}" _lower)
  set(${out} "" PARENT_SCOPE)
  list(LENGTH _known_bad _n)
  math(EXPR _last "${_n} - 1")
  foreach(_i RANGE 0 ${_last} 2)
    list(GET _known_bad ${_i} _dll)
    if(_lower STREQUAL _dll)
      math(EXPR _j "${_i} + 1")
      list(GET _known_bad ${_j} _why)
      set(${out} "${_why}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

function(hl_allowed name out)
  string(TOLOWER "${name}" _lower)
  if(_lower IN_LIST _allow_exact)
    set(${out} TRUE PARENT_SCOPE)
    return()
  endif()
  foreach(_p IN LISTS _allow_prefix)
    string(FIND "${_lower}" "${_p}" _at)
    if(_at EQUAL 0)
      set(${out} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} FALSE PARENT_SCOPE)
endfunction()

# --- the message ------------------------------------------------------------
# Whoever trips this has almost certainly never read the experiment, so the
# failure has to carry the reason rather than a citation to it.
#
# The body goes out through message(NOTICE), which emits verbatim; only the
# one-line verdict is a FATAL_ERROR, which CMake re-wraps and indents. Putting
# the whole explanation in the FATAL_ERROR turned every measured number into
# reflowed prose.
function(hl_explain_and_fail detail)
  message(NOTICE "")
  message(NOTICE "================================================================")
  message(NOTICE "gate.windows-imports FAILED")
  message(NOTICE "================================================================")
  message(NOTICE "")
  message(NOTICE "${detail}")
  message(NOTICE "WHAT IS WRONG")
  message(NOTICE "  The Windows engine image may import KERNEL32.dll, ntdll.dll and the")
  message(NOTICE "  api-ms-win-* apiset stubs, and nothing else.")
  message(NOTICE "")
  message(NOTICE "WHY")
  message(NOTICE "  Guest fork() on this host is RtlCloneUserProcess. A clone does not")
  message(NOTICE "  receive the win32k/CSR-side memory that USER32's dependents point into.")
  message(NOTICE "  IMM32.DLL -- which is loaded whenever USER32.dll is imported, and only")
  message(NOTICE "  then -- dereferences such a pointer in its DLL_THREAD_DETACH handler.")
  message(NOTICE "  So inside a cloned process, the first thread that is created and then")
  message(NOTICE "  RETURNS dies 0xC0000005 at IMM32.DLL+0x7f72.")
  message(NOTICE "")
  message(NOTICE "  The fault is in thread DETACH, not in creation and not in attach: the")
  message(NOTICE "  thread runs to completion with correct __thread storage, and dies on")
  message(NOTICE "  the way out. Measured, in two otherwise byte-identical binaries:")
  message(NOTICE "      with a USER32 import    50 of 50 runs died 0xC0000005")
  message(NOTICE "      without it              50 of 50 runs clean, __thread correct")
  message(NOTICE "  Deterministic in both directions. There is no flakiness here to retry")
  message(NOTICE "  past, and no way to see it in a smoke test: it needs a guest to fork")
  message(NOTICE "  AND THEN create and join a thread. That is a JVM, a Go runtime, a Node")
  message(NOTICE "  worker -- so the first report would be a user's build hanging, not a")
  message(NOTICE "  red lane. Which is why this is a build gate and not a runtime check.")
  message(NOTICE "")
  message(NOTICE "HOW IT USUALLY HAPPENS")
  message(NOTICE "  One call to wsprintfW. It lives in USER32, it reads like a CRT")
  message(NOTICE "  function, and it is the obvious reach in path handling. That single")
  message(NOTICE "  call is the entire difference between the experiment's two binaries.")
  message(NOTICE "  CommandLineToArgvW (SHELL32) is the second most likely way in.")
  message(NOTICE "")
  message(NOTICE "WHAT TO DO")
  message(NOTICE "  Delete the call, not the gate. snprintf and the UCRT cover every use")
  message(NOTICE "  the engine has had for these so far. Widening the allowlist does not")
  message(NOTICE "  make the crash go away -- it makes it happen in a user's JVM build")
  message(NOTICE "  instead of here. If an import genuinely cannot be avoided, then the")
  message(NOTICE "  clone-based fork strategy has to change, and that is a much larger")
  message(NOTICE "  decision than this file.")
  message(NOTICE "")
  message(FATAL_ERROR
    "gate.windows-imports: refused import surface (see the report above)")
endfunction()

# --- layer 1: PE import tables ----------------------------------------------
set(_images_checked 0)
set(_images_missing "")

foreach(_image IN LISTS HL_PE_IMAGES)
  if(_image STREQUAL "")
    continue()
  endif()
  if(NOT EXISTS "${_image}")
    list(APPEND _images_missing "${_image}")
    continue()
  endif()

  set(_out "")
  set(_names "")
  if(HL_READOBJ)
    execute_process(COMMAND "${HL_READOBJ}" --coff-imports "${_image}"
      OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR
        "gate.windows-imports: ${HL_READOBJ} --coff-imports failed on\n"
        "  ${_image}\n${_err}")
    endif()
    # `Import {` blocks; the DLL is the `Name:` line, symbols are `Symbol:`.
    string(REPLACE "\n" ";" _lines "${_out}")
    foreach(_line IN LISTS _lines)
      string(STRIP "${_line}" _line)
      if(_line MATCHES "^Name:[ \t]+(.+)$")
        list(APPEND _names "${CMAKE_MATCH_1}")
      endif()
    endforeach()
  elseif(HL_OBJDUMP)
    execute_process(COMMAND "${HL_OBJDUMP}" -p "${_image}"
      OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR
        "gate.windows-imports: ${HL_OBJDUMP} -p failed on\n  ${_image}\n${_err}")
    endif()
    string(REPLACE "\n" ";" _lines "${_out}")
    foreach(_line IN LISTS _lines)
      string(STRIP "${_line}" _line)
      if(_line MATCHES "^DLL Name:[ \t]+(.+)$")
        list(APPEND _names "${CMAKE_MATCH_1}")
      endif()
    endforeach()
  else()
    message(FATAL_ERROR
      "gate.windows-imports: no PE import reader.  Looked for llvm-readobj,\n"
      "llvm-objdump and objdump.  Install LLVM binutils -- they ship in the\n"
      "MSYS2 clang64/ucrt64 toolchain beside clang.  This gate is not optional:\n"
      "without it a USER32 import reaches a shipped engine, and every guest that\n"
      "forks and then creates and joins a thread crashes 0xC0000005 in IMM32's\n"
      "thread-detach handler.")
  endif()

  math(EXPR _images_checked "${_images_checked} + 1")
  message(STATUS "gate.windows-imports: ${_image}")
  set(_bad "")
  foreach(_name IN LISTS _names)
    hl_allowed("${_name}" _ok)
    if(_ok)
      message(STATUS "    ok      ${_name}")
    else()
      list(APPEND _bad "${_name}")
      message(STATUS "    REFUSED ${_name}")
    endif()
  endforeach()
  if(_names STREQUAL "")
    message(FATAL_ERROR
      "gate.windows-imports: ${_image} has no import table at all.  A PE image\n"
      "that imports nothing is not a stripped success, it is a sign the reader\n"
      "parsed the wrong thing.  Check the tool and the file format.")
  endif()
  if(NOT _bad STREQUAL "")
    set(_detail "  ${_image}\n  imports, outside the allowlist:\n")
    foreach(_name IN LISTS _bad)
      hl_reason_for("${_name}" _why)
      if(_why)
        string(APPEND _detail "      ${_name}  -- ${_why}\n")
      else()
        string(APPEND _detail "      ${_name}\n")
      endif()
    endforeach()
    hl_explain_and_fail("${_detail}")
  endif()
endforeach()

# --- layer 2: named traps in the archives -----------------------------------
set(_archives_checked 0)
set(_hits "")

if(HL_NM)
  foreach(_archive IN LISTS HL_PE_ARCHIVES)
    if(_archive STREQUAL "" OR NOT EXISTS "${_archive}")
      continue()
    endif()
    execute_process(COMMAND "${HL_NM}" --undefined-only "${_archive}"
      OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR
        "gate.windows-imports: ${HL_NM} --undefined-only failed on\n"
        "  ${_archive}\n${_err}")
    endif()
    math(EXPR _archives_checked "${_archives_checked} + 1")
    string(REPLACE "\n" ";" _lines "${_out}")
    list(LENGTH _trap_symbols _n)
    math(EXPR _last "${_n} - 1")
    foreach(_line IN LISTS _lines)
      string(STRIP "${_line}" _line)
      # `U symbol`, and mingw's dllimport thunks as `U __imp_symbol`.
      if(NOT _line MATCHES "^[UwW][ \t]+(.+)$")
        continue()
      endif()
      set(_sym "${CMAKE_MATCH_1}")
      string(REGEX REPLACE "^__imp_" "" _sym "${_sym}")
      foreach(_i RANGE 0 ${_last} 2)
        list(GET _trap_symbols ${_i} _trap)
        if(_sym STREQUAL _trap)
          math(EXPR _j "${_i} + 1")
          list(GET _trap_symbols ${_j} _from)
          list(APPEND _hits "${_archive}|${_sym}|${_from}")
        endif()
      endforeach()
    endforeach()
  endforeach()
endif()

if(NOT _hits STREQUAL "")
  set(_detail "  a static archive references a symbol that only a refused DLL\n  exports, so the link that consumes it WILL acquire that import:\n\n")
  foreach(_hit IN LISTS _hits)
    string(REPLACE "|" ";" _parts "${_hit}")
    list(GET _parts 0 _where)
    list(GET _parts 1 _sym)
    list(GET _parts 2 _from)
    string(APPEND _detail "      ${_sym}  <- ${_from}\n        in ${_where}\n")
  endforeach()
  hl_explain_and_fail("${_detail}")
endif()

# --- verdict ----------------------------------------------------------------
if(_images_checked EQUAL 0 AND _archives_checked EQUAL 0)
  # No subject.  SKIP rather than pass: a green tick here would say the import
  # surface was audited, and it was not.  This is the expected state until
  # something links (build-system.md M5) -- but it is stated every run, so it
  # cannot quietly become permanent.
  message(STATUS
    "gate.windows-imports: SKIPPED -- nothing to inspect.\n"
    "  no PE image and no static archive from this build tree exists yet.\n"
    "  Not built (or not requested):")
  foreach(_image IN LISTS _images_missing)
    message(STATUS "      ${_image}")
  endforeach()
  message(STATUS
    "  The gate has no subject until an archive or an executable is built.\n"
    "  Build hl-translator/hl-host-fake for the symbol sweep, or link\n"
    "  hl-engine-runner (build-system.md M5) for the real import-table check.")
  cmake_language(EXIT ${HL_SKIP_CODE})
endif()

if(NOT _images_missing STREQUAL "")
  # Loud, because "0 images checked, 0 violations" is exactly what a broken
  # gate looks like.  The import table is the only complete check; the symbol
  # sweep below it is a named-trap list and cannot be relied on alone.
  message(STATUS
    "gate.windows-imports: NOT LINKED YET, so the authoritative import-table\n"
    "  check ran on ${_images_checked} image(s) and skipped:")
  foreach(_image IN LISTS _images_missing)
    message(STATUS "      ${_image}")
  endforeach()
endif()

message(STATUS
  "gate.windows-imports: ${_images_checked} PE image(s) import only\n"
  "  KERNEL32/ntdll/api-ms-win-*; ${_archives_checked} archive(s) reference no\n"
  "  known win32k-dragging symbol.")
