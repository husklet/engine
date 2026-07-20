# macOS host backend

Mach VM, `MAP_JIT`, clocks, process/thread, and BSD filesystem/network operations belong to this backend.
No macOS header may be included by portable core, translator, or Linux ABI code.

## Per-engine compat exclusions (`excluded-macos`)

The compat matrix (`tools/matrix_runner.c`) runs the same case manifests against two production engines:
the ELF Linux engine (`test-linux-production-typed`) and the Mach-O macOS engine (`e2e-compat`). A case
whose manifest disposition (column 12) is `excluded-macos` is skipped **only** when the engine binary under
test is Mach-O; the Linux engine still parses it as active and runs+enforces it, so no Linux coverage is
lost. The runner distinguishes engines by the magic bytes of the engine binary (ELF vs Mach-O), not by a
build flag. Reserve `excluded-macos` for behavior the macOS engine genuinely cannot emulate; use
`excluded-known-bug` when a case must be dropped on both engines.

Currently `excluded-macos`: `mprotect`, `mprotect-enforcement` (deliberate PROT_NONE/RO non-enforcement,
mem.c case 226), `smcprecise` (deep JIT re-translate), `subreaper-reparent` (no Darwin child-subreaper),
`lo-any-bridge` (no netns/bridge on Darwin), `reallocchurn` (soak timeout), `getdents64`, `pf-forkself`
(procfs backend gaps), `statx-btime`, `pty-ctl` (Darwin tty/statx gaps), `mq-notify` (si_code delivery).
Each remains `active` and enforced on Linux.
