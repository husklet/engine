# Embedded static archives

Building the `hl-engine-activation` target produces one complete archive for the
host it was built on:

```text
<build-dir>/package/<macos|linux>-<aarch64|x86_64>/libhl-engine.a
```

That directory is `HL_PACKAGE_ARCH_DIR` in CMake, derived once from
`CMAKE_SYSTEM_NAME` and `HL_HOST_ARCH`. It encodes the **host** platform, not a
guest ISA, and both halves matter: it used to be the literal `linux-aarch64`, so
an x86-64 host would have written its archive under the aarch64 name. On the two
supported hosts today the paths are `package/macos-aarch64/libhl-engine.a` and
`package/linux-aarch64/libhl-engine.a`.

Each archive contains the host implementation, Linux ABI, both AArch64 and
x86-64 guest translators, the reexec activation constructor, and the public C
API. The complete archive must be retained because backend descriptors and the
pre-main detector are not referenced by an ordinary application symbol.

An archive is only linkable by its own host platform: it carries host machine
code for one host CPU and one host OS. See the host table in the top-level
`README.md` for which hosts run guests today.

Use these final-link arguments:

```text
macOS: -Wl,-force_load,/absolute/path/libhl-engine.a
Linux: -Wl,--whole-archive /absolute/path/libhl-engine.a -Wl,--no-whole-archive -pthread -ldl -lm -latomic
```

Cargo build integration emits the equivalent `cargo:rustc-link-arg` values.
Do not also link the split `hl-engine`, translator, Linux ABI, or host archives.

The final macOS executable—not the archive—must be signed with
`packaging/macos/jit.entitlements`. The required `allow-jit` entitlement is a
property of the consuming executable, so signing an intermediate static
archive has no effect. Ad-hoc development signing is:

```text
codesign -s - --entitlements packaging/macos/jit.entitlements -f APP
```
