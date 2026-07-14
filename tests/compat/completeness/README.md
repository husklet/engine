# Completeness compatibility suite

This directory contains pure-C Linux guests ported from the former Rust-driven
engine matrix. `manifest.tsv` is the authoritative case registry. There are 159
registered cases backed by 148 unique C sources; repeated execution modes never
duplicate a source.

Linux ABI cases are compiled for both guest ISAs. The AArch64 executable runs
through the AArch64 engine, the x86-64 executable through the x86-64 engine, and
the gate requires the declared exit status plus byte-identical stdout. This is
an engine compatibility contract; it does not use a native executable as an
oracle. Architecture-specific opcode cases run only through their matching
engine and compare stdout with the checked-in file under `expected/`.

Sources are grouped by what they test, not by the host implementation:

- `aarch64/`: AArch64 instruction behavior.
- `x86_64/`: x86-64 instruction behavior.
- `syscall/`: Linux ABI behavior, normally compiled for both guest ISAs.

One source may back several manifest cases. Eleven environment-driven engine
mechanism variants are retained in the manifest as `excluded-replaced`; the
portable engine does not expose those private switches. Their observable
instruction or durability behavior remains covered by the active default case.
No source-text inspection test was ported or introduced.

Every source builds as a static GNU C11 Linux executable with the flags recorded
in the manifest. The syscall programs depend only on Linux libc, except that
the exec tests self-exec and the `pf-*` tests require procfs. Cross compilers may
need the paths used by the project toolchain instead of bare compiler names.
