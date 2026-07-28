# ---------------------------------------------------------------------------
# hl-lint — a unified static-analysis target for C sources.
#
# The target is intentionally opt-in (`-DHL_LINT=ON`) and can be run independently
# via `cmake --build <dir> --target hl-lint`. It reports everything to stdout.
# ---------------------------------------------------------------------------

if(NOT HL_LINT)
  return()
endif()

find_program(HL_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
find_program(HL_CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
find_program(HL_CPPCHECK_EXECUTABLE NAMES cppcheck)
set(HL_LINT_ALLOW_GETENV_FILES "src/core/environment.c"
  CACHE STRING "Semicolon-separated source files allowed to use getenv()")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linter/src/hl_lint.c")
  message(STATUS "linter source missing: ${CMAKE_CURRENT_SOURCE_DIR}/linter/src/hl_lint.c")
  return()
endif()

add_executable(hl_lint
  linter/src/hl_lint.c)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hl_lint PRIVATE
    -O2 -g
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes
    -Werror=implicit-function-declaration -Werror=implicit-int
    -fvisibility=hidden)
endif()

set_target_properties(hl_lint PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

if(HL_LINT_STRICT)
  set(_hl_lint_strict --strict)
else()
  unset(_hl_lint_strict)
endif()

# Default paths are kept intentionally narrow: engine source + public headers.
set(_hl_lint_args
  ${_hl_lint_strict}
  --source-dir "${CMAKE_SOURCE_DIR}/src"
  --source-dir "${CMAKE_SOURCE_DIR}/include"
  --include-dir "${CMAKE_SOURCE_DIR}/include"
  --include-dir "${CMAKE_SOURCE_DIR}/src"
  --max-function-lines 350
  --max-nesting 12
  --clang-format-check
  --clang-tidy-check
  --cppcheck-check)

if(HL_CLANG_FORMAT_EXECUTABLE)
  list(APPEND _hl_lint_args --clang-format-bin "${HL_CLANG_FORMAT_EXECUTABLE}")
else()
  list(APPEND _hl_lint_args --skip-clang-format)
endif()

if(HL_CLANG_TIDY_EXECUTABLE)
  list(APPEND _hl_lint_args --clang-tidy-bin "${HL_CLANG_TIDY_EXECUTABLE}")
else()
  list(APPEND _hl_lint_args --skip-clang-tidy)
endif()

if(HL_CPPCHECK_EXECUTABLE)
  list(APPEND _hl_lint_args --cppcheck-bin "${HL_CPPCHECK_EXECUTABLE}")
else()
  list(APPEND _hl_lint_args --skip-cppcheck)
endif()

# clang-tidy's -p option takes the directory containing compile_commands.json.
list(APPEND _hl_lint_args --compile-commands-dir "${CMAKE_BINARY_DIR}")
list(APPEND _hl_lint_args --clang-tidy-checks "bugprone-*,clang-analyzer-*,performance-*")
foreach(_allowed_file IN LISTS HL_LINT_ALLOW_GETENV_FILES)
  list(APPEND _hl_lint_args --allow-getenv-file "${_allowed_file}")
endforeach()

add_custom_target(hl-lint
  COMMAND $<TARGET_FILE:hl_lint> ${_hl_lint_args}
  DEPENDS hl_lint
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
  COMMENT "Run hl-lint")
