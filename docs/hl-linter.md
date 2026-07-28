# hl-lint

`hl-lint` is a lightweight C lint pipeline for the engine tree.

## Run

```bash
cmake -S . -B build-linter -DHL_LINT=ON
cmake --build build-linter --target hl-lint
```

Output is printed to stdout.

## What it runs

With `-DHL_LINT=ON` (default `HL_LINT_STRICT=OFF`), it runs:

- `clang-format --dry-run --Werror` checks
- `clang-tidy` with a conservative check set (`bugprone-*`, `clang-analyzer-*`, `performance-*`)
- `cppcheck`
- deterministic policy checks implemented in C (currently the centralized
  `getenv()` rule)

The C driver executes analyzers directly with argv vectors; it does not invoke
a shell. Analyzer output is captured with a fixed upper bound and stdout/stderr
are merged so diagnostics retain their normal ordering.

Lexical line-length, function-length, and brace-depth metrics are available via
`--max-line-length`, `--max-function-lines`, and `--max-nesting`. They are
disabled by default because they are not substitutes for clang-format or
control-flow analysis. Allocation counting and indentation guessing are
deliberately not lint rules.

## Policy and exit status

`getenv()` is tracked by default and only permitted in files passed via
`--allow-getenv-file` (or through CMake cache variable `HL_LINT_ALLOW_GETENV_FILES`).

Exit status `0` means the enabled stages completed without fatal policy or
infrastructure errors. In non-strict mode, analyzer findings are reported as
warnings and remain nonfatal. Policy errors, invalid command lines, missing
executables, spawn failures, and internal failures remain fatal. Strict mode
also makes warnings fatal.

With `HL_BUILD_TESTS=ON`, `ctest -L lint` verifies analyzer argv contracts,
exit semantics, bounded output capture, nonzero child exits, missing
executables, and argument paths containing spaces.

## CI/Nix

`flake.nix` exposes a `lint` check entry:

- `nix build .#checks.<platform>.lint`

It runs the same `hl-lint` target in non-strict mode.
