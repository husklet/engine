# Rust packages

The Rust workspace separates stable contracts from live implementation:

- [`api`](api/README.md) owns backend-independent values shared by callers and providers.
- [`provider`](provider/README.md) owns live provider ports and launch-scoped authority.
- [`protocol`](protocol/README.md) owns frozen, bounded engine/provider wire formats.
- [`rust`](rust/README.md) is the supported `hl-engine` facade and native-backed runtime.

The dependency direction is `api <- provider <- protocol <- rust`. The facade is the only package
that links native assets or owns processes, Unix channels, descriptor state, PTYs, and backend
lowering.

See [`docs/RUST_PACKAGE_ARCHITECTURE.md`](../docs/RUST_PACKAGE_ARCHITECTURE.md) for exact ownership,
compatibility rules, completed structural work, and the distinction between package structure and
unfinished engine capabilities.
