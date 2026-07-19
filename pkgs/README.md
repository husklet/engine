# Rust packages

The Rust workspace separates stable contracts from live implementation:

- [`api`](api/README.md) owns backend-independent values shared by callers and providers.
- [`provider`](provider/README.md) owns live provider ports and launch-scoped authority.
- [`protocol`](protocol/README.md) owns frozen, bounded engine/provider wire formats.
- [`runtime`](runtime/README.md) owns private live Unix transport and provider registration.
- [`rust`](rust/README.md) is the supported `hl-engine` facade and native-backed engine.

The dependency direction is acyclic: `api <- provider <- protocol`, runtime depends only on those
lower contracts, and the facade composes all four. The facade is the only package that links native
assets or owns processes, descriptor state, PTYs, and backend lowering.

See [`docs/RUST_PACKAGE_ARCHITECTURE.md`](../docs/RUST_PACKAGE_ARCHITECTURE.md) for exact ownership,
compatibility rules, completed structural work, and the distinction between package structure and
unfinished engine capabilities.
