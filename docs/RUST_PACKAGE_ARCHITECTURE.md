# Rust package architecture

## Status and scope

The Rust package split described here is implemented. The workspace now separates reusable API
values, live provider ports, frozen provider protocol, and the native-backed compatibility facade.
This document records the current tree at commit `cde125b30`; it is no longer a proposal for
`rust-api`, `rust-provider`, `rust-protocol`, or a separate `rust-runtime` directory.

Structural completion does not mean the capability work in [`CAP.md`](CAP.md) is complete. The
split clarifies ownership and dependency direction. Capability discovery and real guest conformance
remain the only proof that sockets, devices, provider memory, domain exec, networking, live control,
checkpointing, or observation are usable.

## Workspace navigation

The workspace manifest is [`pkgs/Cargo.toml`](../pkgs/Cargo.toml):

```text
pkgs/
  Cargo.toml                    # workspace: api, provider, protocol, rust

  api/                          # package hl-engine-api
    src/
      lib.rs                    # Version and root reexports
      types.rs                  # Guest, Sandbox, Stdio, Access, Mount
      extension.rs              # declarative extension values and identities
      checkpoint.rs             # stable checkpoint schema and codec
      observability.rs          # stable event schema, queue, and codec
    tests/contracts.rs

  provider/                     # package hl-engine-provider
    src/
      lib.rs                    # provider-port reexports
      extension.rs              # manifest, negotiation, preparation
      authority.rs              # launch authority, ids, typed errors
      namespace.rs              # atomic namespace installation port
      handle.rs                 # Handles/OpenHandle and owned I/O requests
      memory.rs                 # allocation/import resources
      lifecycle.rs              # events and lifecycle callbacks
    tests/providers.rs

  protocol/                     # package hl-engine-protocol
    src/
      lib.rs                    # frozen protocol reexports
      frame.rs                  # envelope, message kinds, transport errors
      service.rs                # service protocol root and golden tests
      service/
        model.rs                # request/reply/failure values
        codec.rs                # bounded request/reply encoding
        input.rs                # little-endian cursor and error mapping
        namespace.rs            # namespace transaction codec/validation

  rust/                         # package hl-engine, crate hl_engine
    build.rs                    # selects and links one native archive
    assets/                     # native archives, fixture, provenance
    src/
      lib.rs                    # stable facade and compatibility reexports
      spec.rs                   # current discovery and MachineSpec models
      control.rs                # current live-control models
      extension.rs              # API/provider compatibility reexports
      transport.rs              # live Unix channel and registry
      engine.rs                 # public Engine composition surface
      engine/
        discovery.rs            # truthful current-backend capabilities
        lowering.rs             # MachineSpec -> internal Launch
        launch.rs               # native spawn, I/O, PTY, service activation
        validation.rs           # complete side-effect-free preflight
        validation/
          conflict.rs           # namespace conflicts/resource estimates
          extension.rs          # extension shape validation
          policy.rs             # resource/network/time/etc. validation
      service/
        mod.rs                  # live service runtime composition
        descriptor.rs           # open-file-description ownership
        provider.rs             # provider request dispatch
        server.rs               # channel server lifecycle
      ffi.rs, wire.rs           # native ABI and launch serialization
      child.rs, machine.rs      # live process/machine ownership
      projection.rs             # projected namespace host backing
      command.rs, config.rs,
      container.rs              # legacy compatibility builders
```

Package introductions are also available in [`pkgs/README.md`](../pkgs/README.md) and each package's
own README.

## Implemented dependency graph

The resolved Cargo graph is deliberately one-way:

```text
hl-engine-api
    ^
    |
hl-engine-provider
    ^
    |
hl-engine-protocol
    ^
    |
hl-engine (facade + runtime)
```

The exact direct dependencies are:

- `hl-engine-api`: standard library only;
- `hl-engine-provider` -> `hl-engine-api`;
- `hl-engine-protocol` -> `hl-engine-api`, `hl-engine-provider`;
- `hl-engine` -> all three lower packages.

There is currently no `hl-engine-runtime` package. Native lowering and live runtime ownership remain
private modules inside `pkgs/rust`. This is intentional current state, not an omitted directory.
Extracting another package is justified only if a real backend substitution or distribution seam
appears; moving private files solely to mirror an old target diagram would add navigation without
new ownership.

No lower package imports `hl-engine`. This removed the former `spec <-> extension` ownership cycle:
`Version`, declarative provider identities, extension selections, namespace entries, service
registrations, and memory requirements now have one owner in `hl-engine-api`. Provider traits consume
those values without depending on the facade.

The live `PauseGuard<'_>` remains with `Machine` in the facade, avoiding a lower API package that
borrows a concrete runtime implementation. `SpawnError` likewise remains in `pkgs/rust` because it
combines declarative `SpecError` with native engine failure.

## Package responsibilities

### `hl-engine-api`

`hl-engine-api` owns backend-independent copied, cloneable, or serializable contract values that
must be shared by callers and providers:

- `Version` and foundational launch values;
- provider IDs, features, extension specifications and capabilities;
- namespace entries and service/memory requirements;
- checkpoint manifest and observability schemas.

It owns no FFI, host process lifecycle, Unix streams, native asset, build script, provider
implementation trait, or backend lowering. At present, the complete `MachineSpec`, discovery models,
network values, and live-control request models still live in the facade. Moving those is possible
future structural work, not something this document claims has happened.

### `hl-engine-provider`

`hl-engine-provider` owns the live substitution seams implemented outside engine core:

- extension manifest negotiation and preparation;
- namespace transactions;
- handle/open-file-description operations;
- memory allocation and import;
- event and lifecycle callbacks;
- launch-scoped handle authority and typed provider failures.

It depends only on API contract values. It owns no transport framing, Unix I/O, engine machine,
backend dispatch, native resource, or product policy. Contract tests use two unrelated providers to
prove these ports are not tied to a graphics or Husklet domain name.

### `hl-engine-protocol`

`hl-engine-protocol` owns byte-exact, bounded values crossing an engine/provider channel:

- the frozen frame header and message kinds;
- service request/reply encoding, Linux failures, and bounds;
- namespace-install transaction encoding and validation.

It contains no socket creation, threads, provider calls, descriptor table, or live authority.
Golden tests retain exact frame and service bytes and reject malformed, trailing, and oversized
input. Live channels and dispatch remain runtime mechanisms in `hl-engine`.

### `hl-engine`

`hl-engine` remains the supported dependency and compatibility entry point. It:

- reexports API/provider contracts at established `hl_engine::*` paths;
- owns `Engine`, `Machine`, process I/O, PTYs, control, and legacy builders;
- validates and lowers `MachineSpec` into the native launch ABI;
- owns live provider channels, service dispatch, descriptor lifecycle, and projections;
- is the only package with native FFI, `build.rs`, or static engine assets.

The 2,385-line engine orchestration file has been split by responsibility. The 1,907-line service
runtime has also been split into descriptor, provider-dispatch, and server modules. Those are
implemented internal module boundaries, not new public abstractions.

## Public compatibility

Downstream users continue importing the facade:

```rust
use hl_engine::{Engine, Machine, MachineSpec, ProcessIo, SpawnError, SpecError};
use hl_engine::extension::{
    ExtensionProvider, Handles, Lifecycle, Memory, Namespace, OpenHandle, ProviderId,
};
use hl_engine::control::{AttachRequest, NetworkUpdate, ResourceUpdate, Signal};
use hl_engine::{checkpoint, network, observability, transport};
```

The facade reexports the exact lower-package types rather than maintaining copies:

- `hl_engine_api::{checkpoint, observability, Access, Guest, Mount, Sandbox, Stdio}`;
- `hl_engine_api::extension::*` through `hl_engine::extension`;
- `hl_engine_provider::*` through `hl_engine::extension`;
- protocol frame types through `hl_engine::transport`.

`hl_engine::transport` also contains live `Channel`, `TransportLimits`, and `ProviderRegistry`; those
are runtime types, not protocol crate responsibilities. Public-path compile tests guard the current
compatibility surface. Any later removal or narrowing is a separate semver decision.

## Build, MSRV, and asset constraints

- All four packages use edition 2021 and `rust-version = "1.81"`.
- `pkgs/Cargo.toml` uses resolver 2 and explicitly lists `api`, `provider`, `protocol`, and `rust`.
- Every package denies unsafe Rust by default. The facade's private FFI module is the only narrow
  exception.
- `pkgs/rust/assets` and `pkgs/rust/build.rs` have one owner: `hl-engine`. Lower packages contain no
  native archive and perform no native linking.
- `build.rs` selects `aarch64-apple-darwin` or `aarch64-unknown-linux-gnu`; Linux additionally links
  `pthread`, `dl`, `m`, and `atomic`.
- The facade deliberately fails compilation on unsupported execution hosts. API, provider, and
  protocol remain host-independent so contracts and provider implementations can be developed
  without a native engine archive.
- Native archive publication remains atomic: clean private native build, empty-output `/bin/true`
  smoke, focused regressions, exact archive hash, one asset copy, provenance update, then a clean
  facade rebuild. Structural Rust changes never overwrite published archives.

## Completed structural work

The following architecture work is complete in the current tree:

1. Cargo workspace and acyclic package dependency direction.
2. Backend-independent extension identities/configuration, checkpoint, observation, and foundational
   launch values extracted to `hl-engine-api`.
3. Provider seams extracted to `hl-engine-provider`, with unrelated-provider contract tests.
4. Frozen framing and service/namespace codecs extracted to `hl-engine-protocol`, with golden and
   malformed-input tests.
5. Facade compatibility reexports and compile tests.
6. Engine orchestration split into discovery, validation, lowering, and launch responsibilities.
7. Validation split into conflicts/estimates, extension shape validation, and policy validation.
8. Live service runtime split into descriptor ownership, provider dispatch, and server lifecycle.
9. Protocol service implementation split into model, codec, input/error, and namespace transaction
   modules.

These changes preserve behavior. They do not make an undiscovered capability usable.

## Remaining structural work

Potential structural work is intentionally smaller than the earlier proposal:

- move more complete declarative launch/discovery/control values from `pkgs/rust` to `pkgs/api` when
  that can be done without coupling API models to native `Error`, `Machine`, or `Terminal`;
- remove transitional facade aliases only after an explicit public API/semver review;
- add Cargo-metadata dependency-direction enforcement in CI if the repository's CI surface supports
  it;
- capture a pinned public-API diff in release gates.

A separate runtime crate is not a pending requirement by itself. Introduce one only with a proven
backend or packaging seam.

## Remaining capability work from CAP.md

The following is implementation work and must not be inferred from the package split:

- authoritative discovery for every meaningful capability combination;
- registered provider negotiation, transactional preparation, rollback, activation, and lifecycle;
- Unix socket, device, service, generated/shared file, and writable namespace projections;
- complete provider ioctl, mmap, transfer, event, cancellation, and open-file-description semantics;
- PTY and provider activation in one launch;
- a real machine process domain with typed `Machine::exec` and complete process control;
- typed routes, DNS, egress authority, provider network transport, and live network updates;
- complete resource enforcement/accounting, structured bounded events, and live control;
- later checkpoint/restore, virtual time, deterministic entropy, cache control, and debugging.

Each item is complete only when discovery advertises it, validation accepts exactly its implemented
shape, lowering succeeds, and real guest conformance passes. Modeled types, codecs, request decoding,
or a clean package boundary are necessary foundations but never compatibility evidence.

## Review and completion gates

For structural changes:

1. Cargo metadata shows only the dependency edges documented above.
2. Public compatibility compile tests and API diff show no unintended facade changes.
3. Workspace tests, examples, doc tests, and strict Clippy pass.
4. API/provider/protocol remain host-independent and asset-free.
5. The production `/bin/true` smoke remains empty on stdout and stderr.
6. Containers compiles against the facade and focused integrations pass.
7. A frozen-asset full scenario matrix shows no regression before release.

For capability changes, additionally require the corresponding real guest conformance gate in
`CAP.md`. Do not close a capability item merely because its package or module now exists.
