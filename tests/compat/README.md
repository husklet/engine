# Linux behavior fixtures

These C programs define observable Linux guest behavior: exit status, output, synchronization, descriptor semantics,
and process interactions. They do not inspect engine source or assert that a particular implementation exists.
`make compat-build` verifies that every fixture is self-contained; `make compat-native` runs the bounded oracle set
against a native Linux kernel, and `make e2e-compat` runs supported cases through both production guest ISAs.

Each fixture owns its deterministic expected result and required architecture metadata. Performance workloads remain
separate from correctness tests so timing noise cannot weaken behavioral gates.

`guest_inventory.tsv` accounts for all 879 files under the legacy guest root: 782 map to C behavior/golden tests and
97 are explicitly excluded. The exclusions are limited to 96 GUI, GPU, Chrome, Wayland, EGL/GLES, dmabuf,
compositor, and shader artifacts that do not belong in this generic container engine, plus the obsolete
`x86/build.sh` shell build helper. The TSV has one additional covered subject, marked
`provenance_scope=external-bundle`, for a versioned procfd fixture that was never a file under that root. Thus the
ledger has 880 subjects in total: 783 covered and 97 excluded. `ext_inventory.tsv` separately accounts for every
legacy Rust `engine_matrix/ext` registration and points to its current C manifest. After replacing the `elf210`
environment-hook registration with the production loader placement-seam unit test, the audit has no unaccounted
engine behavior.
