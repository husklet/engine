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
- custom heuristics in C code (line length, indentation width/combination, function size/nesting, simple alloc/free leak hinting)

## getenv rule

`getenv()` is tracked by default and only permitted in files passed via
`--allow-getenv-file` (or through CMake cache variable `HL_LINT_ALLOW_GETENV_FILES`).

## CI/Nix

`flake.nix` exposes a `lint` check entry:

- `nix build .#checks.<platform>.lint`

It runs the same `hl-lint` target in non-strict mode.
