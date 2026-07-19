# Rust package architecture

## Status and scope

The Rust package split described here is implemented. The workspace now separates reusable API
values, live provider ports, frozen provider protocol, live transport runtime, and the native-backed
compatibility facade. This document records the implemented tree; it is no longer a package-split
proposal.

Structural completion does not mean the capability work in [`CAP.md`](CAP.md) is complete. The
split clarifies ownership and dependency direction. Capability discovery and real guest conformance
remain the only proof that sockets, devices, provider memory, domain exec, networking, live control,
checkpointing, or observation are usable.

## Workspace navigation

The workspace manifest is [`pkgs/Cargo.toml`](../pkgs/Cargo.toml):

```text
pkgs/
  Cargo.toml                    # workspace: api, provider, protocol, runtime, rust

  api/                          # package hl-engine-api
    src/
      lib.rs                    # Version and root reexports
      types.rs                  # Guest, Sandbox, Stdio, Access, Mount
      extension.rs              # declarative extension values and identities
      checkpoint.rs             # stable checkpoint schema and codec
      observability.rs          # stable event schema, queue, and codec
      control.rs                # backend-independent control requests/results
      spec/                     # discovery and launch-policy value domains
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

  runtime/                      # private package hl-engine-runtime
    src/
      lib.rs                    # live-runtime exports
      transport.rs              # Unix channels and launch ProviderRegistry

  rust/                         # package hl-engine, crate hl_engine
    build.rs                    # selects and links one native archive
    assets/                     # native archives, fixture, provenance
    src/
      lib.rs                    # stable facade and compatibility reexports
      spec/                     # native MachineSpec/process/network composition
      control.rs                # live Machine-bound control surfaces
      extension.rs              # API/provider compatibility reexports
      transport.rs              # compatibility reexport of runtime transport
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
hl-engine-protocol     hl-engine-runtime
          ^                    ^
          +---------+----------+
                    |
           hl-engine (facade)
```

The exact direct dependencies are:

- `hl-engine-api`: standard library only;
- `hl-engine-provider` -> `hl-engine-api`;
- `hl-engine-protocol` -> `hl-engine-api`, `hl-engine-provider`;
- `hl-engine-runtime` -> `hl-engine-api`, `hl-engine-provider`, `hl-engine-protocol`;
- `hl-engine` -> all four lower packages.

`hl-engine-runtime` is private and non-publishable. It owns the acyclic live Unix provider transport
and launch registry. Native lowering, FFI, process ownership, and service descriptor dispatch remain
in the facade because moving them currently creates facade/model or native-link ownership cycles.

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
- checkpoint manifest and observability schemas;
- discovery/capability, filesystem, namespace, resource, security, time, validation, and pure
  control request/result models.

It owns no FFI, host process lifecycle, Unix streams, native asset, build script, provider
implementation trait, or backend lowering. Native `MachineSpec`, `ProcessSpec`, and `NetworkSpec`
composition remains in the facade where it owns `Domain`, bridge operations, native errors, and
live handles.

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
input. Live channels are runtime mechanisms in `hl-engine-runtime`; descriptor dispatch remains in
the facade.

### `hl-engine-runtime`

`hl-engine-runtime` owns live Unix channel framing, handshake/cancellation behavior, and the
launch-scoped `ProviderRegistry`. It is an internal substitution boundary and has no native archive,
FFI, process ownership, backend lowering, or product policy. The facade reexports these types at the
existing `hl_engine::transport` path.

### `hl-engine`

`hl-engine` remains the supported dependency and compatibility entry point. It:

- reexports API/provider contracts at established `hl_engine::*` paths;
- owns `Engine`, `Machine`, process I/O, PTYs, control, and legacy builders;
- validates and lowers `MachineSpec` into the native launch ABI;
- composes live provider channels with service dispatch, descriptor lifecycle, and projections;
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

`hl_engine::transport` reexports live `Channel`, `TransportLimits`, and `ProviderRegistry` from the
runtime crate. Public-path compile tests guard the current
compatibility surface. Any later removal or narrowing is a separate semver decision.

## Build, MSRV, and asset constraints

- All five packages use edition 2021 and `rust-version = "1.81"`.
- `pkgs/Cargo.toml` uses resolver 2 and explicitly lists `api`, `provider`, `protocol`, `runtime`, and `rust`.
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
10. Pure discovery, launch-policy, validation, and control-data models extracted to `hl-engine-api`.
11. Live Unix transport and provider registry extracted to private `hl-engine-runtime`.

These changes preserve behavior. They do not make an undiscovered capability usable.

## Remaining structural work

Potential structural work is intentionally smaller than the earlier proposal:

- move remaining native composition only if `Domain`, network, and error ownership can be separated
  without weakening their domain contracts;
- remove transitional facade aliases only after an explicit public API/semver review;
- add Cargo-metadata dependency-direction enforcement in CI if the repository's CI surface supports
  it;
- capture a pinned public-API diff in release gates.

Further runtime extraction must preserve the facade as sole native archive/build-script owner and
must demonstrate an acyclic responsibility boundary.

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
