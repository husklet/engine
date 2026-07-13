# Compatibility fixtures

These C fixtures were copied from the audited source repository at the commit in `SOURCE_SNAPSHOT`. They exercise
Linux guest behavior rather than implementation text. `make compat-build` verifies that every imported fixture remains
self-contained; `make compat-native` runs a bounded subset against the native Linux kernel as an initial oracle.

As the runner becomes functional, a C compatibility driver should execute the same binaries through both the snapshot
runner and `hl-engine-runner`, capture stdout/stderr/exit status, and compare declared deterministic fields. Fixtures
must move with their expected results and required architecture metadata. Benchmarks were deliberately not copied:
performance workloads will be selected and owned separately rather than mixed with correctness.
