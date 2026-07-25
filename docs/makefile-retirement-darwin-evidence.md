# Darwin Makefile-retirement evidence

Measured on Apple silicon through the shared checkout on 2026-07-25. The
Darwin CMake configure completed, `gate.makefile-lane-parity` passed, and the
CTest JSON registry contained 197 tests.

| Historical lane | CTest label | Registered tests |
|---|---|---:|
| compatibility suites | `compat` | 28 |
| direct production launches | `compat-direct` | 6 |
| macOS host services | `macos` | 14 |
| embedding and lifecycle gates | `e2e-mac` | 16 |
| dual-backend archive | `embedding` | 1 |
| package consumer | `package` | 1 |
| activation package | `package-activation` | 1 |
| embedded archive | `package-embedded` | 1 |
| macOS performance | `perf-macos` | 28 |
| host unit tests | `unit` | 100 |

The permanent parity test reads the historical Makefile lane inventory and
fails when a required CTest label is absent or empty. This complements the
counts above: the registry evidence proves each replacement lane is populated,
while the test prevents a later one-sided target change from silently removing
coverage.
