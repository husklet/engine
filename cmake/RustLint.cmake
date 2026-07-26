# rustfmt and clippy as CTest cases.
#
# linux.yml gates on both, but cargo only exists inside `nix develop`, so work
# done in a plain shell reaches CI unformatted and fails there instead of here.
# Registering them locally is the difference between a 2-second check and a
# round trip through a runner.

find_program(HL_CARGO_EXECUTABLE NAMES cargo)

if(NOT HL_CARGO_EXECUTABLE)
  # Absent outside the dev shell. The lane still runs in CI, so this cannot
  # silently reduce coverage -- but say so rather than passing quietly.
  message(STATUS "cargo not found: rust.fmt and rust.clippy will not be registered")
  return()
endif()

set(_manifest ${CMAKE_SOURCE_DIR}/pkgs/rust/Cargo.toml)

add_test(NAME rust.fmt
  COMMAND ${HL_CARGO_EXECUTABLE} fmt --manifest-path ${_manifest} --check)

add_test(NAME rust.clippy
  COMMAND ${HL_CARGO_EXECUTABLE} clippy --manifest-path ${_manifest}
          --all-targets --locked -- -D warnings)

set_tests_properties(rust.fmt rust.clippy PROPERTIES
  LABELS "unit;rust" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
