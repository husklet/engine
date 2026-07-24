# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. Both archives were built from source commit
`0d55391e6cfd43947a31f656ef8f6df192ef22bf`, at which `include/hl/config.h`
declares `HL_CONFIG_ABI 13` (checkpoint policy); `src/core/config.c` at that
commit accepts launch ABIs 13, 12, 11 and 10.

| Host target | Build target | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | `build/package/macos-aarch64/libhl-engine.a` | `71306b2b12db1706cd512705288403191c46e495e25c2cd4ba67bf59b810c957` |
| `aarch64-unknown-linux-gnu` | `build/package/linux-aarch64/libhl-engine.a` | `26efe1677c7b8f54e4d7b78edf99740452519f8fffd432e21b453767db8bdf7b` |

These archives are what `cargo publish` ships; the crate never compiles `src/`.
Whenever `HL_CONFIG_ABI` changes, or any engine fix must reach crate users, both
archives must be regenerated (`make package-embedded-linux` on aarch64 Linux and
`make package-embedded-macos` on an Apple silicon mac) and this file updated.
`pkgs/rust/tests/packaged_archive.rs` launches a guest through the committed
archive and fails if it has fallen behind the headers; CI runs it on both hosts.

Cargo links the selected archive with whole-archive semantics so the private
pre-main activation constructor is retained in downstream executables. A
macOS application using the engine must be signed with the repository's JIT
entitlements before distribution.

# Alpine test fixture

`alpine/alpine-minirootfs-3.24.1-aarch64.tar.gz` is the unmodified official
Alpine Linux 3.24.1 AArch64 minirootfs, downloaded from:

`https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.1-aarch64.tar.gz`

Its SHA-256 is
`f55a90f69052c5bd6f92cb09a8f47065970830b194c917a006fb94028e721259`,
matching the adjacent official `.sha256` publication. Package license metadata
is contained in the archive at `lib/apk/db/installed`; corresponding source
packages are published in Alpine's `v3.24/main` repository.
