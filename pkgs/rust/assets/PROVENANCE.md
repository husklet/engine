# Native engine archives

The crate links one target-specific archive containing both guest backends and
the activation constructor. No native executable is stored or extracted at
runtime. The archives contain the sources committed as
`8a073e87`.

| Host target | Build target | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | `build/package/macos-aarch64/libhl-engine.a` | `fcd6252449c45e71d128594925b117ab8e12814d6d3770f50d0adcbd3304b6a1` |
| `aarch64-unknown-linux-gnu` | `build/package/linux-aarch64/libhl-engine.a` | `b31fa07119e932f3b09422b03b10a9a419c9876e0d46044e051d81225de8ab08` |

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
