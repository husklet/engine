# Core legacy corpus

This slice transfers the pending top-level guest sources registered by `engine_matrix/abi.rs` and
`engine_matrix/workload.rs`. The 51 source files are byte-identical to the legacy corpus and the two
manifests preserve all 52 registrations (the IBTC program is deliberately registered in both).

Every former native-oracle assertion is represented by a checked-in deterministic stdout and exit.
ISA restrictions, the SQLite argument, link dependencies, and the original static-PIE build model
are explicit. Workload endurance files here are distinct top-level legacy sources; they do not
duplicate the separately transferred `ext_soak` corpus.
