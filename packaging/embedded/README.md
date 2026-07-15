# Embedded static archives

`make package-embedded` produces complete AArch64-host archives:

- `build/package/macos-aarch64/libhl-engine.a`
- `build/package/linux-aarch64/libhl-engine.a`

Each archive contains the host implementation, Linux ABI, both AArch64 and
x86-64 guest translators, the reexec activation constructor, and the public C
API. The complete archive must be retained because backend descriptors and the
pre-main detector are not referenced by an ordinary application symbol.

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
