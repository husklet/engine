# HL Engine API

`hl-engine-api` contains backend-independent contracts shared by engine callers,
providers, and implementations. It has no native engine asset, build script, FFI,
host-process lifecycle, or backend lowering.

The `hl-engine` crate remains the compatibility entry point and reexports these
contracts at its established module paths.
