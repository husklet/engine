# Linux behavior fixtures

These C programs define observable Linux guest behavior: exit status, output, synchronization, descriptor semantics,
and process interactions. They do not inspect engine source or assert that a particular implementation exists.
`make compat-build` verifies that every fixture is self-contained; `make compat-native` runs the bounded oracle set
against a native Linux kernel, and `make e2e-compat` runs supported cases through both production guest ISAs.

Each fixture owns its deterministic expected result and required architecture metadata. Performance workloads remain
separate from correctness tests so timing noise cannot weaken behavioral gates.
