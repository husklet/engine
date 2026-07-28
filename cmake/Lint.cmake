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
# Ratchet, not approval: these files predate the tagged logging boundary and
# must be removed from this list as their direct diagnostics are migrated.
set(HL_LINT_ALLOW_STDIO_FILES
  src/core/checkpoint_channel.c
  src/core/dispatch.c
  src/core/lifecycle.c
  src/core/target/aarch64.c
  src/core/target/run.c
  src/core/target/x86_64.c
  src/linux_abi/checkpoint.c
  src/linux_abi/container/netns.c
  src/linux_abi/container/state.c
  src/linux_abi/elf.c
  src/linux_abi/fork.c
  src/linux_abi/parse.c
  src/linux_abi/sentry.c
  src/linux_abi/syscall/dispatch.c
  src/linux_abi/syscall/event.c
  src/linux_abi/syscall/inotify.c
  src/linux_abi/syscall/io.c
  src/linux_abi/syscall/proc.c
  src/linux_abi/x86.c
  src/runner/main.c
  src/translator/guest/aarch64/cache.c
  src/translator/guest/aarch64/signal.c
  src/translator/guest/aarch64/translate.c
  src/translator/guest/x86_64/avx.c
  src/translator/guest/x86_64/cache.c
  src/translator/guest/x86_64/dispatch.h
  src/translator/guest/x86_64/signal.c
  src/translator/guest/x86_64/translate.c
  CACHE STRING "Legacy files temporarily allowed direct console output")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linter/src/hl_lint.c")
  message(STATUS "linter source missing: ${CMAKE_CURRENT_SOURCE_DIR}/linter/src/hl_lint.c")
  return()
endif()

if(WIN32)
  set(_hl_lint_process_source linter/src/process_windows.c)
else()
  set(_hl_lint_process_source linter/src/process_posix.c)
endif()

add_executable(hl_lint
  linter/src/hl_lint.c
  ${_hl_lint_process_source})

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
foreach(_allowed_file IN LISTS HL_LINT_ALLOW_STDIO_FILES)
  list(APPEND _hl_lint_args --allow-stdio-file "${_allowed_file}")
endforeach()

add_custom_target(hl-lint
  COMMAND $<TARGET_FILE:hl_lint> ${_hl_lint_args}
  DEPENDS hl_lint
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
  COMMENT "Run hl-lint")

if(HL_BUILD_TESTS)
  add_executable(hl_lint_fake_analyzer
    linter/tests/fake_analyzer.c)
  set_target_properties(hl_lint_fake_analyzer PROPERTIES
    OUTPUT_NAME "hl lint fake analyzer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

  add_executable(hl_lint_process_helper
    linter/tests/process_helper.c)
  set_target_properties(hl_lint_process_helper PROPERTIES
    OUTPUT_NAME "hl lint process helper"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

  add_executable(hl_lint_process_test
    linter/tests/test_process.c
    ${_hl_lint_process_source})
  target_include_directories(hl_lint_process_test PRIVATE
    "${CMAKE_SOURCE_DIR}/linter/src")
  set_target_properties(hl_lint_process_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

  add_test(NAME lint.process
    COMMAND $<TARGET_FILE:hl_lint_process_test>
      $<TARGET_FILE:hl_lint_process_helper>)
  set_tests_properties(lint.process PROPERTIES LABELS "lint")

  add_test(NAME lint.clang-tidy-argv
    COMMAND $<TARGET_FILE:hl_lint>
      --strict
      --skip-clang-format
      --skip-cppcheck
      --skip-custom
      --clang-tidy-bin $<TARGET_FILE:hl_lint_fake_analyzer>
      --compile-commands-dir "${CMAKE_BINARY_DIR}"
      --clang-tidy-checks "bugprone-*,performance-*"
      --source-file "${CMAKE_SOURCE_DIR}/linter/tests/fixture.c")
  set_tests_properties(lint.clang-tidy-argv PROPERTIES
    LABELS "lint"
    PASS_REGULAR_EXPRESSION "fake-analyzer: clang-tidy argv ok")

  add_test(NAME lint.cppcheck-argv
    COMMAND $<TARGET_FILE:hl_lint>
      --strict
      --skip-clang-format
      --skip-clang-tidy
      --skip-custom
      --cppcheck-bin $<TARGET_FILE:hl_lint_fake_analyzer>
      --include-dir "${CMAKE_SOURCE_DIR}/include"
      --source-file "${CMAKE_SOURCE_DIR}/linter/tests/fixture.c")
  set_tests_properties(lint.cppcheck-argv PROPERTIES
    LABELS "lint"
    PASS_REGULAR_EXPRESSION "fake-analyzer: cppcheck argv ok")

  foreach(_case IN ITEMS clean warning-nonstrict warning-strict error
      stdio-error stdio-allowed usage)
    add_test(NAME lint.exit-${_case}
      COMMAND "${CMAKE_COMMAND}"
        -DHL_LINT=$<TARGET_FILE:hl_lint>
        -DHL_LINT_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DHL_LINT_CASE=${_case}
        -P "${CMAKE_SOURCE_DIR}/linter/tests/assert_exit.cmake")
    set_tests_properties(lint.exit-${_case} PROPERTIES LABELS "lint")
  endforeach()
endif()
