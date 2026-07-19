# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. The macOS archive includes ABI9 process-domain ownership; the Linux
archive remains the target-specific archive shipped with this crate revision.

| Host target | Source commit | Build target | SHA-256 |
|---|---|---|---|
| `aarch64-apple-darwin` | `b7de85947` | `build-package/package/macos-aarch64/libhl-engine.a` | `99495ce8cbe3708244213a8c51eac58bf37a1cbe5e4674973656ac295076fb0a` |
| `aarch64-unknown-linux-gnu` | not recorded | `build-cap-linux/package/linux-aarch64/libhl-engine.a` | `900e3ff97dbea86e61d64e79a82290e905c289fd6ed55e3fb4551e7359cf32ff` |

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
