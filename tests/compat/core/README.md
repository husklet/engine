# Core legacy corpus

This slice transfers the pending top-level guest sources registered by `engine_matrix/abi.rs` and
`engine_matrix/workload.rs`. The 51 source files are byte-identical to the legacy corpus and the two
manifests preserve all 52 registrations (the IBTC program is deliberately registered in both).

Every former native-oracle assertion is represented by a checked-in deterministic stdout and exit.
ISA restrictions, the SQLite argument, link dependencies, and the original static-PIE build model
are explicit. Workload endurance files here are distinct top-level legacy sources; they do not
duplicate the separately transferred `ext_soak` corpus.

Slice two adds the non-graphics sources registered by `syscall.rs` and `regress.rs`. Every engine-relevant
case now has an exact checked-in golden and is active in the manifests. Display/GPU programs remain outside
this engine-only corpus and are tracked by the source-inventory provenance audit, not as runtime exclusions.
