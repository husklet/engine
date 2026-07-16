# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. Both archives were built from source commit
`f109f2ae2df2e3783f6fb228dd2e9c47c7ddc981` and include ABI9 process-domain
ownership.

| Host target | Build target | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | `build/package/macos-aarch64/libhl-engine.a` | `1dc039ea04017d177a9b453b782e14dadc73744c58fa1a2e5930a219257fc8d6` |
| `aarch64-unknown-linux-gnu` | `build/package/linux-aarch64/libhl-engine.a` | `4cafba94eedf30a3ad77b2f7247fcf15f23cef7b7d0667d2c4b7ccdea1e7535a` |

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
