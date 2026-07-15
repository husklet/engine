# Embedded engine artifacts

The four executables in `bin/` are production HL engine artifacts built from
engine commit `aa8e01e5` for an AArch64 host. They are selected at compile time;
the crate never downloads or compiles native code.

| Host target | Guest | SHA-256 |
|---|---|---|
| `aarch64-apple-darwin` | AArch64 | `fa93d846906036641653d31b9a2ee42ff8282d77bb8a000aa92b706aeb5a499c` |
| `aarch64-apple-darwin` | x86-64 | `01d37f566dcc1a804e7067fc9d11c943d36b0b38334e8445aa0ff867f879738c` |
| `aarch64-unknown-linux-gnu` | AArch64 | `8bffdc847843624cc120841f4801f164fe949a459e28124a6c26c5477ac575f9` |
| `aarch64-unknown-linux-gnu` | x86-64 | `f1ec2958e06917d4643186cb2c1db1b2ea58c7c9185d45210b664ba9b912a22d` |

The source build targets are `build/production/hl-engine-linux-{aarch64,x86_64}`
for macOS and `build/linux-production/hl-engine-linux-{aarch64,x86_64}` for
Linux. Linux artifacts are stripped. macOS artifacts are stripped and then
ad-hoc signed with `packaging/macos/jit.entitlements`; both signatures were
verified with `codesign --verify --strict` after signing.

# Alpine test fixture

`alpine/alpine-minirootfs-3.24.1-aarch64.tar.gz` is the unmodified official
Alpine Linux 3.24.1 AArch64 minirootfs, downloaded from:

`https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.1-aarch64.tar.gz`

Its SHA-256 is
`f55a90f69052c5bd6f92cb09a8f47065970830b194c917a006fb94028e721259`,
matching the adjacent official `.sha256` publication. Package license metadata
is contained in the archive at `lib/apk/db/installed`; corresponding source
packages are published in Alpine's `v3.24/main` repository.
