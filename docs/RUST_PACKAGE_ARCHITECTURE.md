# Rust package architecture

## Purpose

`pkgs/rust` is currently one 9,775-line crate that combines five different responsibilities:

- public, backend-independent discovery and launch models;
- provider contracts and their frozen transport protocol;
- validation and lowering into the native ABI;
- native process, descriptor, PTY, and projection ownership;
- the compatibility facade used by existing `hl_engine::*` consumers.

The largest files make those boundaries especially difficult to see: `engine.rs` is 2,384 lines,
`service.rs` is 1,907 lines, `extension.rs` is 686 lines, `spec.rs` is 672 lines, and `wire.rs` is
669 lines. Splitting those files into arbitrary modules would only distribute the coupling. The
package split below follows the three planes in `docs/CAP.md` and gives dependencies one direction.

This is a structural plan, not a claim that modeled capabilities are implemented. Discovery and
conformance remain authoritative.

## Current dependency findings

The current module graph contains these meaningful clusters:

```text
extension <----> spec
    ^              ^
    |              |
transport       control <----> machine
    ^                         |
    |                         v
 service <----------------- engine
    ^                         |
    |                         v
 provider authority       projection
                              |
                              v
                     config -> wire -> native FFI
                                      |
                                      v
                                child/terminal/domain
```

Two cycles prevent a clean crate extraction:

1. `extension` imports `spec::Version`, while `spec` embeds `ExtensionCapability`,
   `ExtensionSpec`, `ProviderId`, `Feature`, `ServiceId`, and `HostBindEntry`.
2. `control::PauseGuard` borrows `Machine`, while `Machine` consumes the other public control
   models.

There is also a less obvious ownership problem: `SpawnError` in the declarative spec module embeds
the facade's native `Error`. A backend-independent model crate cannot depend back on the native
facade. Spawn errors therefore belong at the facade/runtime boundary, while validation errors stay
with the API models.

`engine.rs` is currently the composition root, validator, capability reporter, namespace conflict
checker, launch lowerer, process starter, provider service starter, and legacy command entry point.
Its imports accurately reveal the coupling: it reaches into `spec`, `extension`, `service`,
`transport`, `projection`, `wire`, `ffi`, process ownership, and legacy configuration.

## Target workspace

Keep `pkgs/rust` as the published compatibility package and create private sibling packages under
`pkgs/`. Package names are explicit at the Cargo boundary; Rust module names remain concise.

```text
pkgs/
  rust/                         # package hl-engine; stable public facade
    Cargo.toml
    build.rs
    assets/
    src/
      lib.rs
      engine.rs                 # composition and public Engine methods only
      command.rs                # legacy builder compatibility
      container.rs              # legacy builder compatibility
      machine.rs                # live facade and PauseGuard
    tests/                      # public compatibility and end-to-end tests

  rust-api/                     # package hl-engine-api
    Cargo.toml
    src/
      lib.rs
      identity.rs               # Version, Guest, Domain and opaque identities
      capability.rs             # truthful discovery models
      machine.rs                # MachineSpec and launch-plane models
      process.rs                # ProcessSpec and ProcessIo descriptions
      filesystem.rs
      namespace.rs
      network.rs
      resource.rs
      security.rs
      time.rs
      checkpoint.rs             # checkpoint request/manifest schema
      observation.rs            # event and observation schemas
      control.rs                # requests/results/errors, no live Machine reference

  rust-provider/                # package hl-engine-provider
    Cargo.toml
    src/
      lib.rs
      manifest.rs
      namespace.rs
      handle.rs
      memory.rs
      event.rs
      lifecycle.rs
      authority.rs
      error.rs

  rust-protocol/                # package hl-engine-protocol
    Cargo.toml
    src/
      lib.rs
      frame.rs                  # frozen provider envelope
      channel.rs
      service.rs                # bounded service request/reply codec
      observation.rs            # stable event codec
      checkpoint.rs             # stable checkpoint codec

  rust-runtime/                 # package hl-engine-runtime; private implementation
    Cargo.toml
    src/
      lib.rs
      validate.rs
      lower.rs
      projection.rs
      provider.rs               # negotiation/preparation/rollback/activation
      service/
        mod.rs
        dispatch.rs
        server.rs
        descriptor.rs
      process/
        mod.rs
        child.rs
        terminal.rs
        domain.rs
      native/
        mod.rs
        config.rs
        wire.rs
        ffi.rs
        error.rs
```

The intended dependency graph is acyclic:

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
hl-engine-runtime
      ^
      |
  hl-engine
```

More precisely, `provider` and `protocol` both depend on `api`; `protocol` also depends on
`provider` for provider IDs and operation models; `runtime` depends on all three; and the facade
depends on `api`, `provider`, and `runtime`. No lower package imports the facade.

## Boundary rules

### `hl-engine-api`

This package contains cloneable, inspectable, backend-independent values. It has no FFI, host
processes, files opened as authority, Unix streams, static archives, or `build.rs`. It owns:

- discovery and validation result models;
- `MachineSpec` and its component specs;
- provider selection values used by a spec, but not provider implementation traits;
- live-control request/result/error values that do not retain a live machine;
- stable checkpoint and observability schemas.

Move `Version` out of `spec.rs` into `identity.rs`. Move the declarative provider atoms needed by
both specs and contracts (`ProviderId`, `Feature`, `ServiceId`, extension selection/specification)
into API modules. This resolves `spec <-> extension` without inventing a generic `common` package.
The values are genuinely API-domain identities shared across planes.

`SpawnError` does not belong here while it wraps native `Error`. The API package owns `SpecError`;
the facade owns the composed spawn error.

### `hl-engine-provider`

This package owns provider seams: manifests, preparation, namespace transactions, open handles,
memory, events, lifecycle, and launch-scoped grants. It depends only on API values and the standard
library. It must not know `Machine`, the native ABI, transport framing, Unix sockets, or a product
provider name.

The current `extension.rs` should be split by its cohesive ports, not one type per file. Keep
`Handles` and `OpenHandle` with their request/reply models in `handle.rs`; keep allocation/import
models with `Memory` in `memory.rs`. Do not create traits for native implementations that have no
substitution seam.

### `hl-engine-protocol`

This package owns bytes crossing engine/provider and persisted-schema boundaries. Move the current
`transport.rs` framing and the codec/server-independent half of `service.rs` here. It can encode and
decode provider contract values, but cannot open sockets, spawn threads, call providers, or allocate
guest descriptors.

Keep frozen constants and compatibility tests beside their codecs. `Frame`, message kinds, request
IDs, limits, error categories, checkpoint envelope, and observation envelope are protocol values.
The Unix `Channel`, dispatcher, server loop, readiness threads, and descriptor ownership are runtime
mechanisms and stay in `hl-engine-runtime`.

### `hl-engine-runtime`

This private package implements validation, lowering, native invocation, provider activation, and
live resource ownership. It is the only package allowed to depend on native ABI details. The current
`engine.rs` should become:

- `validate.rs`: side-effect-free complete validation and capability/lowering consistency;
- `lower.rs`: validated `MachineSpec` to an internal launch plan;
- `projection.rs`: temporary namespace backing and cleanup;
- `provider.rs`: registry, negotiation, preparation, authority checking, rollback, and activation;
- `service/*`: request dispatch, server lifecycle, and open-file-description tables;
- `native/*`: configuration serialization, FFI declarations, and native error translation;
- `process/*`: child, PTY, domain, and process resource ownership.

The runtime should expose a small crate-private surface to the facade: capabilities, validate,
spawn, and live-machine operations. It should not reexport native FFI or serialization models.

### `hl-engine` facade

Existing consumers continue depending on `hl-engine` and importing `hl_engine::*`. The facade:

- reexports API modules under their current public paths;
- reexports provider contracts under `hl_engine::extension`;
- preserves root reexports such as `Engine`, `Machine`, `MachineSpec`, `ProcessIo`, `Config`,
  `Container`, `Command`, `Child`, `Terminal`, and `Exit`;
- owns `Engine` construction/composition and `Machine` live handles;
- retains legacy builders until callers migrate to the typed launch plane.

Resolve `control <-> machine` by keeping the borrowing `PauseGuard<'_>` in the facade next to
`Machine`. The API package contains `Signal`, `SignalTarget`, `ProcessInfo`, `ShutdownPolicy`,
updates, attachment requests, and `ControlError`, but no object that borrows a facade implementation.
The facade can publicly reexport `PauseGuard` at the same root path.

## Public compatibility

The split must not force downstream source changes. Before moving implementation, capture the
current public paths with compile-only compatibility tests. At minimum preserve:

```rust
use hl_engine::{Engine, Machine, MachineSpec, ProcessIo, SpawnError, SpecError};
use hl_engine::extension::{
    ExtensionProvider, Handles, Lifecycle, Memory, Namespace, OpenHandle, ProviderId,
};
use hl_engine::control::{AttachRequest, NetworkUpdate, ResourceUpdate, Signal};
use hl_engine::{checkpoint, network, observability, transport};
```

`hl_engine::transport` is currently public even though CAP.md calls it a foundation rather than a
discovered capability. Preserve that path during the structural migration by reexporting protocol
types. Deprecating or narrowing it is a separate semver decision, not part of the refactor.

Use `pub use hl_engine_api::...` and `pub use hl_engine_provider as extension` rather than copied
facade models. Exact public wire/API ownership remains in one lower package and duplicate types
cannot drift.

Run `cargo public-api` (with a checked-in or CI-pinned tool version) before and after each extraction
when available. The existing `tests/traits.rs`, downstream containers build, and public examples are
required compatibility gates but are not substitutes for an API diff.

## Cargo, MSRV, and packaging constraints

- Keep edition 2021 and `rust-version = "1.81"` in every published or independently built package.
  Do not adopt workspace inheritance until the workspace itself declares and tests these values.
- The package currently has no third-party Rust dependencies. Preserve that property during the
  mechanical split. A new dependency needs a capability reason and MSRV/license review.
- Put a workspace manifest at `pkgs/Cargo.toml` only after sibling packages exist. Use resolver 2,
  list every member explicitly, and exclude temporary packaging/build trees.
- Keep `[lints]` equivalent across packages. `unsafe_code = "deny"` remains the default; only the
  private runtime FFI module has the current narrow `allow(unsafe_code)` exception.
- `pkgs/rust/assets` remains single-owner under the facade package. Neither API, provider, nor
  protocol packages ship the 15 MiB rootfs/archive payloads or native static archives.
- `build.rs` remains only in `hl-engine`. It selects exactly one supported host archive and links
  `pthread`, `dl`, `m`, and `atomic` on Linux. Runtime is an rlib implementation detail and must not
  gain another build script or independently link the archive.
- Keep the facade's target compile error for unsupported hosts. Pure API/provider/protocol packages
  should compile on arbitrary Rust hosts so tooling, documentation, provider development, and spec
  validation do not require a supported execution host.
- Preserve the facade package include list, README, examples, testdata, asset provenance, and exact
  archive packaging protocol. Internal sibling packages need explicit include lists before any are
  published.
- Native archive publication remains atomic: clean private build, empty-output `/bin/true` smoke,
  focused regressions, exact hash, one copy, provenance update, clean `hl-engine` Cargo rebuild.
  A package refactor must never regenerate or overwrite native archives.

## Staged migration

Each stage is a small reviewable commit and leaves all tests green. Do not combine native behavior
changes with package motion.

1. **Freeze architecture and compatibility.** Add this document, public-path compile tests, and a
   CI dependency-direction check. Record `cargo public-api` output if the tool is available.
2. **Create `hl-engine-api`.** Move identity, network values, capability models, machine/process
   specs, validation errors, checkpoint schemas, observation schemas, and control data. Resolve
   `SpawnError` ownership. Reexport every existing path from `hl-engine`.
3. **Create `hl-engine-provider`.** Move provider IDs/spec atoms only if not already in API, then
   move the cohesive provider port modules. Keep facade `extension` reexports. Add two unrelated
   mock-provider compile/contract tests.
4. **Create `hl-engine-protocol`.** Move pure frame and service codecs with golden/malformed/limit
   tests. Preserve `hl_engine::transport` reexports. Do not move live threads or descriptors yet.
5. **Create `hl-engine-runtime`.** Move native FFI, wire encoding, config files, projections,
   process ownership, live transport, and service dispatch without changing behavior. Keep static
   archive ownership and `build.rs` in the facade.
6. **Decompose orchestration.** Split current `engine.rs` into validation, lowering, provider
   preparation, and spawn composition. Split `service.rs` into dispatch, server, and descriptor
   ownership. This is internal motion with characterization tests before extraction.
7. **Remove temporary bridges.** Delete duplicate aliases and transitional `pub(crate)` shims,
   enforce dependency direction in CI, run API diff, all Rust tests, strict linting, examples,
   empty-output production smoke, containers downstream build, and the full scenario matrix.

The capability implementation order in `CAP.md` continues independently after this structural
migration: authoritative discovery, provider lifecycle, namespace projections, provider I/O,
machine-domain exec, network transports, then live control and observation. The package split makes
ownership visible; it must not advertise or simulate those unfinished capabilities.

## Dependency enforcement

Once the workspace exists, CI should fail if:

- API depends on provider, protocol, runtime, or facade;
- provider depends on protocol, runtime, or facade;
- protocol depends on runtime or facade;
- runtime or any lower package owns a `build.rs` or native archive;
- API/provider/protocol source contains `unsafe`, FFI declarations, process spawning, or ambient
  engine configuration;
- facade-compatible public paths disappear without an explicit semver decision.

Use Cargo metadata for crate-edge enforcement and ordinary Rust tests for behavior. Do not test the
architecture by searching production source text for implementation details; dependency checks inspect
Cargo's resolved graph, while compile tests exercise public paths.

## Completion gate

The refactor is complete only when all of the following are proven from the new workspace:

1. the resolved package graph is acyclic and follows the declared direction;
2. the facade's public API diff contains no unintended removals or type changes;
3. every existing Rust unit, integration, example, and doc test passes on supported hosts;
4. strict linting passes at MSRV and the current toolchain;
5. pure API/provider/protocol crates build on an unsupported execution host without native assets;
6. the production `/bin/true` smoke has empty stdout and stderr;
7. containers compiles and its focused engine integration tests pass against the facade;
8. a frozen-asset full scenario matrix shows no regression.

