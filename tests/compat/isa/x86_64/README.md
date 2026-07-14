# Legacy x86-64 fixture corpus

This directory is the exact transfer of all 15 files from the legacy `testdata/guests/x86`
corpus. The seven executable files remain checked-in, byte-identical fixtures because their ELF
link models are the behavior under test. `build.sh` records the original reproducible build
variants and toolchain assumptions; the five C and two Go sources retain provenance beside them.

The manifest reproduces every registration in `engine_matrix/container.rs::x86`: no arguments,
no environment overrides, and the original exits. Legacy substring assertions were strengthened
to deterministic, checked-in exact stdout. Only x86-64 is declared: the freestanding sources use
x86 syscall assembly, the glibc fixtures pin x86 ELF translation, and the Go fixtures pin the
x86 non-PIE rebasing path. The source comments record the limited native-AArch64 cross-check of
the two portable Go totals; no generic AArch64 engine case is invented here.
