# Filesystem compatibility corpus

These 14 byte-preserved guests come from the legacy `ext_fs` and `ext_fsx` suites. `manifest.tsv` is
the authoritative ABI4 registration and records each original path, ISA scope, build flags, rootfs
dependency, exit status, and deterministic checked-in golden. The four storage cases retain their
Alpine-overlay execution contract; the remaining Linux and portable verdict cases run bare on both
production guest ISAs. Stateful shared-memory, FIFO, xattr, and temporary-path cases clean up their
resources and are safe to soak repeatedly.
