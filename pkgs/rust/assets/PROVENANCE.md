# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. Both archives were built from source commit
`d4ba00a63df76f7851d7be5976174535716b4e4d` and include ABI9 process-domain
ownership.

| Host target | Build target | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | `build/package/macos-aarch64/libhl-engine.a` | `c9a573727eafb341c8d420299ff071d62c6060ec3bbd48032c4e32b548094b23` |
| `aarch64-unknown-linux-gnu` | `build/package/linux-aarch64/libhl-engine.a` | `3552cc22115decee1185b96ba37bdd2296996a348a2a3832260f602d973a5461` |

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
