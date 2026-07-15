# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. The archives were built from engine commit
`a2f1c4443fab65d4c61c8c779f42d52ea62760dc`.

| Host target | Build target | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | `build/package/macos-aarch64/libhl-engine.a` | `de7c72c34d8310164a978b41e270c4fe29cae1c5ca706acd0e56dcb044c9cfe8` |
| `aarch64-unknown-linux-gnu` | `build/package/linux-aarch64/libhl-engine.a` | `d35f4ae23c1f7c0ec3707c5e3645d5310d0b0714c14b0af971e8df4419e99f28` |

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
