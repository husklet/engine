# Archive rules for the MSVC-ABI Windows target.
#
# This file exists because a toolchain file is the WRONG place to set these,
# and that is not obvious. CMake reads the toolchain file first, then includes
# Platform/Windows-GNU.cmake -- selected because clang is being driven through
# its GNU-like command line -- and that module sets CMAKE_C_ARCHIVE_CREATE
# UNCONDITIONALLY, silently discarding anything the toolchain file put there.
# The symptom was `llvm-lib qc ...` and "qc: no such file or directory": the
# archiver had been replaced but its command line had not.
#
# CMAKE_USER_MAKE_RULES_OVERRIDE names a file that is included AFTER the
# platform module, which is the only hook that wins. The toolchain file points
# at this one.
#
# What is actually being fixed: archive FORMAT. Platform/Windows-GNU.cmake's
# rule produces a GNU-format archive -- the `/` symbol table and the `//`
# long-name member. link.exe, which is what rustc drives for a
# *-windows-msvc target, reads only the MSVC format. That mismatch does not
# fail here; it fails later and quietly, in a downstream consumer, as an archive
# that appears to export nothing.
#
# llvm-ar --format=coff rather than llvm-lib: llvm-lib takes link.exe's command
# line (/OUT: and no operation letters), which would mean restating the rule
# rather than amending it, and the two write the same format through the same
# writer. Keeping `qc` also keeps CMake's own `q` (quick-append) semantics for
# the APPEND rule.
#
# The FINISH step is emptied. It would run llvm-ranlib, whose job is to add the
# symbol index a GNU archive carries separately; an MSVC-format archive has its
# symbol table already and there is nothing for ranlib to do.

set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> --format=coff qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> --format=coff q  <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")
