# HL engine runtime

Private host-side runtime mechanisms used by the public `hl-engine` facade.

This package owns live execution machinery, not public launch models or frozen wire schemas. The
facade remains the sole owner of native archive discovery and linking through its `build.rs`.
