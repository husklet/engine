# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. The archives were built from engine commit
`020c6ae7dd8276c2371159ddb50f05146456b211`.

| Host target | Build target | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | `build/package/macos-aarch64/libhl-engine.a` | `6ef383e8474b93f0dfb25663bbfdca4f590ab8041f8edddae0fbfdb55aa5d819` |
| `aarch64-unknown-linux-gnu` | `build/package/linux-aarch64/libhl-engine.a` | `62deb9935149c112d3ab92cea44636148c806489003e5d5aa736fdef79a49dc8` |

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
