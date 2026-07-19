# Engine requirements

## Why Husklet needs the engine

Husklet is a macOS application for persistent Linux workspaces without a VM. A user selects an OCI image,
opens a workspace, and receives a Linux terminal. The workspace may also contain host directories, resource
limits, a VPN policy, Docker-compatible container services, and graphical Linux applications such as Chrome,
GTK applications, or Zed.

Guest programs are ordinary Linux programs and should observe ordinary Linux mechanisms:

```text
Husklet workspace configuration
  -> container image, overlay, mounts, identity, network, resources
  -> engine Linux machine/domain
  -> guest processes and PTYs
  -> projected sockets, files, libraries, and devices
  -> host services selected by Husklet
```

For graphics, guest Wayland clients connect to a projected Unix socket. Guest GL, Vulkan, and CUDA libraries
translate calls into a neutral GPU protocol. A host GPU service executes that protocol through WebGPU, and a
compositor presents Wayland surfaces as native macOS windows. A workspace may similarly project a Docker
socket or route network operations through a host-selected VPN transport.

Husklet owns product policy: which image to use, which features a workspace enables, how settings appear,
which host implementation is selected, and which authority that implementation receives. Containers own
container lifecycle, image/storage composition, and Docker semantics. The engine owns the reusable execution
mechanisms that make a declared Linux environment real.

## The engine is a toolbox, not a Husklet backend

The engine must survive uses that have nothing to do with Husklet: CI sandboxes, language runners, IDEs,
serverless workers, compatibility layers, deterministic tests, device-forwarding tools, container products,
and applications not yet imagined. Therefore every addition must describe a general execution capability,
not a product, protocol, device brand, or current workaround.

The correct response to a missing capability is to extend the engine's generic vocabulary and lowering—not
to encode policy in environment variables, hardcode one device, or make Husklet mount around the API.

| Husklet need | Engine primitive | Forbidden engine concept |
| --- | --- | --- |
| Wayland connection | Projected Unix socket/endpoint | `WaylandSocket` |
| GPU render node | Provider-backed character device, handles, memory, events | `HuskletGpu` or `MetalDevice` |
| GL/Vulkan/CUDA libraries | Declarative namespace files and read-only projections | CUDA-specific library injection |
| Docker access | Projected Unix socket with Linux socket semantics | `DockerSocket` |
| VPN | Routes, DNS policy, and an authorized network transport | `VpnConfig`, `Socks5Mode`, or a VPN brand |
| Workspace shell | Machine domain, process execution, PTY, signals | `WorkspaceTerminal` |
| Native UI status | Typed lifecycle/resource/fault events | Husklet callbacks or stderr parsing |

An engine primitive is sufficiently generic when:

- its names make sense without knowing Husklet exists;
- at least several unrelated systems could use it unchanged;
- policy and secrets remain with the caller while mechanism and Linux semantics remain with the engine;
- the backend can discover, validate, lower, observe, and clean it up through one typed contract;
- unsupported behavior fails explicitly before launch instead of becoming a silent approximation;
- adding another provider or backend does not require modifying engine core dispatch for its domain name.

Generic does not mean one untyped escape hatch. Byte blobs, arbitrary callbacks, string maps, shell commands,
and ambient environment variables merely move the coupling out of sight. The API should expose small typed
atoms—paths, namespace entries, handles, mappings, endpoints, routes, credentials, quotas, events, processes,
and lifecycles—that callers compose. Extension configuration may carry provider-owned versioned bytes, but
the engine still validates its schema, authority, resource bounds, and interaction with the machine.

## Responsibility boundary

The engine should provide:

- a declarative description of a Linux machine and its process domain;
- capability discovery and complete preflight validation;
- filesystems, namespace nodes, mounts, sockets, devices, networking, identity, resources, security, and PTYs;
- provider registration for mechanisms implemented outside engine core;
- explicit launch-scoped authority for host resources;
- process/domain control, structured observation, and deterministic cleanup;
- backend-independent contracts with backend-specific lowering hidden behind them.

The engine should not provide:

- workspace, settings-screen, image-selection, or application policy;
- GPU, Docker, Wayland, Chrome, VPN-brand, or macOS product concepts;
- host-service discovery by executable name or hardcoded application paths;
- implicit authority from environment variables, global state, or guessed host resources;
- claims of compatibility that its active backend has not passed in conformance tests.

Husklet must be able to express its entire workspace using these tools. If it cannot, modify the engine API
or implementation. Do not preserve an inadequate API by moving engine work into Husklet or containers.

## Contract

- `EngineCapabilities` reports only behavior implemented by the active backend.
- `Engine::validate` performs complete, side-effect-free preflight with field-addressed errors.
- `MachineSpec` is declarative, cloneable, inspectable, and contains no host authority or secrets.
- Host authority is granted separately at spawn and scoped to one machine/domain.
- Optional features may degrade explicitly; required features fail before the guest starts.
- No public behavior enters through ambient `HL_*` variables, hardcoded device metadata, or magic paths.
- Process, exec, fork, checkpoint, and restore preserve declared capability lifetimes and ownership.

## One execution lifecycle

Every backend and provider participates in the same lifecycle:

```text
discover capabilities
  -> construct MachineSpec
  -> validate and negotiate providers
  -> prepare resources transactionally
  -> supply launch-scoped authority
  -> spawn machine domain
  -> exec/control/observe processes
  -> update supported live policy
  -> stop and release resources deterministically
```

`MachineSpec` says what Linux environment is requested. It is safe to log, compare, persist, and validate.
Authority says which live host resources this launch may use and is neither serializable nor ambient.
Preparation resolves providers and acquires resources without exposing backend details. `Machine` is the live
control surface. Capabilities and validation connect all four stages so callers never discover a missing
feature after partially starting a guest.

The API must preserve this separation even when a backend could implement a shortcut. For example, a macOS
backend may lower a host directory directly and a Linux backend may use a mount namespace; both implement the
same typed projection. A provider may use a Unix socket, shared memory, or an in-process object internally;
the guest still observes the declared Linux handle/device/socket contract.

## How the API should evolve

When a use case cannot be represented:

1. Describe the Linux-visible behavior and lifecycle, not the current host implementation.
2. Find the smallest reusable atom missing from the existing spec, extension, authority, or control model.
3. Add a typed capability and validation rule before adding lowering.
4. Implement it per backend without branching on the caller or provider name.
5. Add backend-neutral contract tests and real guest conformance tests.
6. Advertise it only on backends that pass those tests.
7. Remove the caller workaround only after capability discovery proves the replacement exists.

Prefer extending an existing complete concept—such as `NamespaceEntry`, `HandleOperation`, `NetworkSpec`, or
`Machine`—over creating a parallel API. If an existing model advertises an operation that lowering rejects,
finish the implementation or stop advertising it; do not introduce a second narrower model for the working
subset.

## Existing Rust API

The current Rust engine already has the correct broad shape. Work from these contracts instead of replacing
them with a Husklet-specific facade:

- `Engine::{capabilities, validate, spawn, spawn_with_authority}` discovers and starts machines.
- `MachineSpec` contains guest CPU/process, identity, filesystem, namespaces, network, resources, security,
  time, entropy, translation cache, checkpoint, observability, debug, and extensions.
- `Machine` exposes initial process information, stdio/terminal ownership, wait, signals, pause, shutdown,
  attach, process enumeration, resource/network updates, hotplug, events, and force-stop.
- `ExtensionSpec` selects a provider/version/features and declares namespace entries, services, memory, and
  versioned configuration.
- `NamespaceEntry` models directories, files, symlinks, devices, host binds, sockets, and services.
- `Handles`/`OpenHandle` model open, read, write, positioned I/O, seek, truncate, metadata, ioctl, map, poll,
  and handle transfer with Linux credentials.
- `Memory`, `Events`, and `Lifecycle` model provider allocation/import/export, asynchronous events, and
  deterministic start/fork/exec/exit/stop hooks.
- `EngineCapabilities` and `Validation` model supported features, negotiated extensions, degradation,
  conflicts, limits, and host resource estimates.

The container repository already lowers `ContainerSpec` into this API. Its `Device` contract lets a product
compose mounts, process environment, engine extensions, and handle authority without teaching containers the
device domain. Husklet should select those devices; containers should validate and merge their requirements;
the engine should provide the mechanisms those requirements need.

The principal problem is incomplete lowering, not a lack of promising types. Today the active runtime only
implements a subset: namespace directories/files/symlinks/read-only host binds, provider read/write/poll
handles, basic launch resources, and limited process control. Several modeled entries and live operations
still return unsupported. Capability discovery must describe that implemented subset truthfully while the
missing generic mechanisms below are completed.

### Modeled versus usable

The existence of a public type is not proof that a caller can use it. Treat this table as the starting audit;
update it whenever lowering and conformance change.

| Area | Already modeled | Usable in the active backend | Gap |
| --- | --- | --- | --- |
| Guest/process | architecture, executable, argv, environment, cwd, umask, PTY, domain | arm64/x86-64 Linux launch, stdio, PTY, domain identity; PTY and provider services may be activated together | true exec within an existing machine |
| Identity | uid, gid, supplementary groups, hostname, domain name | uid/gid and hostname subset | complete group/domain behavior and namespace-visible identity |
| Root/storage | host tree, image layer, overlay, ownership, read-only root, coherence | host tree, overlay, binds, ownership, generation-file coherence | provider roots, fully abstract coherence authority, complete Linux VFS semantics |
| Namespaces | private/host/shared mount, PID, UTS, IPC, network, user, cgroup | backend-selected subset | shared handles and truthful per-namespace discovery/lowering |
| Projected namespace | directory, file sources, symlink, device, bind, socket, service | directory, immutable/mutable file, symlink, read-only host bind, Unix socket, service-backed character/block device | shared/generated files, writable binds, and complete device operations |
| Provider handles | open, read/write, positioned I/O, seek, truncate, metadata, ioctl, map, poll, transfer | authority plus read/write/poll-selected service transport, including launches with a PTY | complete operations, mappings, transfer, and cancellation |
| Provider framework | manifest, negotiation, prepare, namespace, handles, memory, events, lifecycle | two hardwired built-in provider IDs; bounded launch allocation with validated resources and release/rollback | runtime provider registry and preparation; guest mappings, import/transfer, events, and full lifecycle |
| Network | host/none/virtual, namespace, interfaces, forwarding, listeners | basic launch modes/interfaces/forwards | routes, DNS, egress authority, UDP completeness, live updates, accounting |
| Resources | memory, processes, threads, CPU, rlimits, I/O, provider budgets, accounting | memory limit, process limit, CPU count at launch | remaining limits, live updates, effective values, accounting/events |
| Control | signal, pause, shutdown, attach, processes, updates, hotplug, events | initial-process signal/pause/shutdown/attach/wait | process enumeration, domain exec, updates, hotplug, structured events |
| Time/entropy/cache | host/offset/frozen time, rates, deterministic entropy, cache policies | host time/entropy; limited cache behavior | virtual time, deterministic entropy, complete cache policy/discovery |
| Checkpoint/debug/observation | detailed request and capability models | explicitly unsupported | backend implementations, authority, event transport, conformance |

Do not compensate for a gap by adding a second application abstraction that claims the feature. Either use
the modeled generic contract and finish its lowering, or add the missing generic atom to that contract.

## Required capability work

| Priority | Capability | Current state | Required result |
| --- | --- | --- | --- |
| P0 | Unix socket completeness | `NamespaceEntry::Socket` validates and connects to an explicitly granted host Unix socket | Complete credentials, ancillary `SCM_RIGHTS`, shutdown, peer-close, and all poll semantics |
| P0 | Provider devices | `DeviceEntry`, ioctl, map, transfer, memory, and events are modeled; handles v1 implements only read/write/poll | Implement character devices backed by provider handles, including ioctl, mmap, descriptor transfer, readiness, and lifecycle |
| P0 | Provider activation | Extension manifests, negotiation, preparation, and lifecycle traits exist but are not wired into engine construction/spawn | Register providers with the engine; negotiate, validate, prepare, activate, roll back, and stop them transactionally |
| P0 | Terminal plus providers | Validation and execution support a controlling terminal together with projected provider services | Extend conformance to provider memory, device mapping, lifecycle failure, and teardown while the PTY is active |
| P0 | Writable projections | Extension host binds accept only read-only regular files/directories | Implement `BindAccess::ReadWrite` and coherent writable files/directories; keep host authority explicit |
| P0 | Network egress | `NetworkSpec` has no egress field; legacy SOCKS behavior remains outside the typed API | Add typed route/DNS/egress policy and launch-scoped transport authority |
| P0 | Domain-scoped exec | Containers respawn with a domain identity and repeat device extensions | Add typed exec on an existing machine/domain so namespace, network, devices, and authorities are inherited once |
| P1 | Live control | resource/network update, hotplug, events, and process enumeration return `Unsupported` | Implement the existing control models over one native control channel |
| P1 | Resource coverage | launch supports memory, process count, and CPU count only | Implement limits already modeled by `ResourceSpec`, accounting, pressure/OOM events, and truthful capabilities |
| P1 | Namespace completeness | standard Linux mounts and some personalities remain hardcoded | Make standard mounts, virtual files, ownership, and device discovery declarative |
| P1 | Structured observability | capabilities advertise no events, metrics, tracing, or accounting | Provide bounded structured events and counters without parsing stderr |
| P2 | Checkpoint, virtual time, deterministic entropy, translation cache policy | typed models exist but backend rejects them | Implement only when product workflows need them; keep undiscovered until conformance tests pass |

## Namespace and host endpoints

The existing extension vocabulary is the right abstraction. Implement it instead of adding a graphics API.

```rust
ExtensionSpec {
    provider,
    namespace: vec![
        NamespaceEntry::HostBind(library),
        NamespaceEntry::Socket(wayland),
        NamespaceEntry::Device(render_node),
        NamespaceEntry::File(device_metadata),
    ],
    services,
    memory,
    ..
}
```

Required semantics:

- Install all entries atomically before the first guest instruction.
- Detect conflicts against rootfs, mounts, other providers, and standard Linux nodes during validation.
- Preserve byte-exact Linux paths; do not encode lists using delimiter-separated strings.
- Support regular files, directories, symlinks, Unix sockets, character/block devices, generated files,
  shared bytes, read-only binds, and writable binds as advertised features.
- Apply mode, uid, gid, file type, timestamps, xattrs, and link behavior consistently through
  `open`, `stat`, `readlink`, `readdir`, `/proc`, and `/sys` views.
- Make socket lifetime explicit: borrowed host listener, connected endpoint, or provider-created listener.
- Preserve Unix socket operations needed by Wayland and Docker: stream framing, half-close, poll/epoll,
  peer credentials, ancillary descriptor transfer, and nonblocking connect/accept.

Husklet can then remove writable socket mounts. GL/Vulkan/CUDA libraries remain read-only host binds;
Wayland, GPU transport, and Docker become socket entries.

## Provider registration and preparation

Providers are engine plugins, not application-side launch patches. Engine construction accepts a registry
of implementations; `MachineSpec` selects provider IDs, versions, features, and non-secret configuration.

```rust
let engine = Engine::builder()
    .provider(graphics)
    .provider(network)
    .build()?;

let launch = engine.prepare(&spec)?;
let machine = launch.spawn(io, authorities)?;
```

Preparation must:

- resolve every provider and negotiate an exact manifest version and feature set;
- ask each provider to validate its tagged configuration before any side effect;
- return the effective namespace, services, memory, authority requirements, and lifecycle;
- validate the combined result against the rootfs, standard namespace, resources, security policy, and
  every other provider;
- acquire resources transactionally and roll them back in reverse order on failure;
- bind the prepared launch to one spec digest so it cannot be reused under different policy;
- keep secrets and live host objects out of serializable specs and diagnostics.

Discovery is derived from registered providers and backend support. The engine must not hardcode known
provider IDs or claim a provider feature its runtime cannot lower.

## Generic provider authority

The public extension module already models more than `HandlesAuthority` can grant. Replace the special
authority container with a provider grant containing only the ports selected for that launch:

```rust
pub struct ProviderAuthority {
    pub handles: Option<Arc<dyn Handles>>,
    pub memory: Option<Arc<dyn Memory>>,
    pub events: Option<Arc<dyn Events>>,
}

pub struct Authorities { /* ProviderId -> ProviderAuthority */ }

launch.spawn(io, authorities)
```

Providers return lifecycle ownership during preparation; callers grant only the host authority declared by
the prepared provider. Spawn rejects missing or excess grants. This keeps cleanup in the engine without
placing ambient authority in the spec.

Authority requirements:

- Validate provider, version, selected features, operations, quotas, and authority before side effects.
- Support every advertised `HandleOperation`: read, write, positioned I/O, seek, truncate, metadata,
  ioctl, map, poll, and transfer.
- Carry Linux credentials and deadlines on every request; cancellation must release blocked operations.
- Preserve open-file-description semantics across dup, fork, exec, close-on-exec, and descriptor passing.
- Map provider memory with declared protections, sharing, inheritance, coherency, and bounded lifetime.
- Deliver readiness/completion events without polling loops or unbounded queues.
- Run lifecycle callbacks in deterministic order and tear down all resources when the domain exits.
- Permit provider activation together with pipe I/O or a PTY; activation transport must be independent
  of stdio selection.

This supports a render node without engine GPU knowledge. A graphics provider can declare
`/dev/dri/renderD128`, answer typed ioctls, allocate/import memory, transfer descriptors, and publish
completion events. Device identity and sysfs/uevent files are provider namespace entries, never hardcoded
engine strings.

## Network and VPN

`NetworkSpec` needs policy, while host mechanisms remain authority:

```rust
pub struct NetworkSpec {
    pub mode: NetworkMode,
    pub namespace: Option<Namespace>,
    pub interfaces: Vec<Interface>,
    pub routes: Vec<Route>,
    pub dns: DnsPolicy,
    pub egress: EgressPolicy,
    pub port_forwards: Vec<Rule>,
    pub external_listeners: bool,
}

pub enum EgressPolicy {
    Direct,
    Deny,
    Provider { provider: ProviderId, policy: Vec<RoutePolicy> },
}

pub trait NetworkTransport {
    fn connect(&self, request: ConnectRequest) -> Result<Box<dyn Socket>, NetworkError>;
    fn bind(&self, request: BindRequest) -> Result<Box<dyn Socket>, NetworkError>;
    fn resolve(&self, request: ResolveRequest) -> Result<Vec<Address>, NetworkError>;
}
```

The engine must not model `Socks5`, WireGuard, OpenVPN, or a VPN brand. Husklet selects a provider that
implements `NetworkTransport`; the spec selects routes and DNS policy. Provider configuration may name a
non-secret endpoint, while credentials, keys, and live tunnel handles remain launch authority.

Required behavior:

- IPv4 and IPv6 TCP, UDP, DNS, nonblocking connect, bind/listen/accept, socket options, and cancellation.
- Route selection by destination CIDR, protocol, and port, with explicit direct/deny/provider fallback.
- DNS policy that prevents leaks when egress is provider-routed.
- Stable virtual network identity shared by all processes in the container domain.
- Live replacement through `Machine::update_network`, with atomic cutover and structured failure events.
- Accurate `/proc/net`, interface ioctls, routing views, and source-address behavior.

This is the typed destination for Husklet workspace VPN settings. Until it exists, accepting a VPN setting
as if applied is incorrect.

## Machine domains and exec

A container is one engine-owned domain, not a set of unrelated launches carrying the same string identity.

```rust
let mut machine = engine.spawn(spec, io, authorities)?;
let process = machine.exec(ProcessSpec { .. }, ProcessIo { .. })?;
```

The domain owns rootfs/overlay, namespace, network, provider grants, accounting, and cleanup. `exec` adds a
process with its own argv, environment, cwd, credentials, stdio/PTY, and limits. It must inherit the domain's
devices and mounts without re-registering providers or duplicating sockets. Health checks use the same API.

Expose typed process identities and support:

- enumerate processes;
- signal one process, a process group, or the domain;
- attach/detach streams repeatedly without stealing output;
- resize the selected PTY immediately;
- wait for one process or domain quiescence;
- graceful shutdown followed by deadline-bound force;
- fork/exec lifecycle notifications for provider resources.

## Filesystems and mounts

Core container storage remains `FilesystemSpec`; optional device artifacts remain extension namespace
entries. Both paths must share one VFS and conflict model.

Required core behavior:

- image root, overlay lower/upper/work layers, read-only root, host binds, volumes, and ownership maps;
- read-only and read-write file/directory binds without delimiter/path restrictions;
- mount flags and propagation represented as enums/bitflags, not strings;
- coherent external writes and cache invalidation without exposing a generation-file path;
- Linux-compatible symlinks, hard links, rename, mmap, locking, xattrs, sparse files, and file watches;
- declarative `/proc`, `/sys`, `/dev`, `/tmp`, `/run`, resolver files, hostname, and device discovery;
- atomic launch rollback and deterministic cleanup for every owned layer and projected resource.

## Resources, security, and live control

Implement and advertise the existing `ResourceSpec` fields Husklet/containers can expose: memory
reservation/limit, process/thread limit, CPU count/quota/affinity, open files, file size, locked memory,
stack, address space, and I/O rates. `ResourceUpdate` must identify fields that are mutable and return the
effective values.

Accounting must report per-process and per-machine CPU, resident/virtual memory, I/O, process/thread count,
and limit violations. OOM, process-limit, provider-quota, and forced-shutdown events must be typed.

Security discovery must state sandbox compatibility with host networking, provider transport, executable
memory, descriptor passing, and projected nodes. Provider allowlists and budgets are enforced before
provider preparation.

## Observability

Husklet needs one bounded event stream, not stderr parsing or backend environment flags:

```rust
pub enum MachineEvent {
    ProcessStarted(ProcessInfo),
    ProcessExited(ProcessExit),
    ResourcePressure(ResourcePressure),
    NetworkChanged(NetworkState),
    Extension(ProviderEvent),
    Fault(Fault),
}
```

Events carry machine/domain/process identity, monotonic timestamp, sequence, and typed context. Queue limits,
overflow behavior, and sampling are capabilities. Metrics and tracing may be optional but must use the same
identities so Husklet can profile launch, syscall, GPU submission, frame presentation, and teardown latency.

## Capability discovery

Extend capabilities rather than adding boolean guesses in callers:

- namespace node kinds and bind access;
- socket operations and ancillary-data support;
- provider operations, memory types, PTY coexistence, lifecycle, and hotplug;
- network families, transports, route/DNS policy, and live mutation;
- domain exec/process control;
- resource launch/live/accounting fields;
- observability event kinds and queue limits.

`Validation` should return selected versions/features, optional degradation, namespace conflicts, estimated
resources, and effective policy. Errors identify category, field path, provider/path resource, and actionable
context. Backend-specific limits never leak into Husklet constants.

## Husklet migration ledger

Each workaround remains until its engine contract passes the corresponding guest conformance test.

| Current Husklet mechanism | Required engine contract | Completion condition |
| --- | --- | --- |
| Mount Wayland socket read-write and set `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` | `SocketEntry` plus typed guest endpoint placement | Wayland passes without a host socket bind; standard guest variables may be derived from the declared endpoint |
| Mount GPU transport socket and set `HL_GPU_EXEC` | Registered device provider with handles, memory, events, lifecycle, and `DeviceEntry` | GL/Vulkan/CUDA pass without private transport variables or application-installed device metadata |
| Bind GL/Vulkan/CUDA/NVML libraries and build loader variables | Read-only projections plus typed loader/device configuration | Provider namespace/configuration supplies libraries and manifests without delimiter-built environment strings |
| Mount Docker socket and set `DOCKER_HOST` | `SocketEntry` with Unix stream and credential semantics | Docker clients pass with no writable host bind mount |
| Launch each terminal/health command as another machine sharing a domain string | `Machine::exec` and typed process control | Processes share one namespace, network, device lifetime, accounting scope, and cleanup boundary |
| Store VPN settings without applying them | Routes, DNS, `EgressPolicy`, and launch-scoped `NetworkTransport` | TCP/UDP/DNS leak, cancellation, and reconnect tests pass; UI reports effective state |
| Spawn host daemon/native helpers from product code | Explicit host-service port or provider lifecycle when guest execution owns the service | Engine owns guest-facing lifecycle; unrelated product helpers remain Husklet composition |
| Select runtime behavior through private `HL_*` variables | Typed spec fields, provider configuration, or debug/observability APIs | Engine behavior has no private ambient configuration; unknown settings fail validation |
| Rely on hardcoded `/proc`, `/sys`, `/dev`, `/tmp`, or `/run` personalities | Declarative standard namespace profile with discovery | Validation reports every installed node and Linux behavior tests pass |
| Poll or parse stderr to infer runtime state | Bounded `MachineEvent` stream and accounting snapshots | UI and profiling use typed identities/events with documented overflow behavior |

## Implementation order

Implement capabilities in dependency order. A later stage must not bypass an unfinished earlier stage.

### 1. Make discovery authoritative

- Add contract tests that compare every advertised capability with validation and lowering.
- Split coarse flags where combinations matter, especially PTY plus providers, socket operations, handle
  operations, namespace entry kinds, writable access, and live control.
- Ensure validation has no side effects and spawn cannot reject a spec that validation accepted because of
  an undisclosed backend limitation.

This stage changes no guest behavior. It establishes a truthful base on which generic callers can plan.

### 2. Wire the provider lifecycle

- Register `ExtensionProvider` implementations with engine construction.
- Resolve manifests and versions during validation.
- Prepare all selected providers transactionally before launch.
- Return explicit authority requirements and reject missing or excess grants.
- Start providers before guest execution and deliver ordered process/fork/exec/exit callbacks.
- Stop and roll back providers in deterministic reverse order.

Prove this with two unrelated test providers. A provider path that only works for graphics is not complete.

### 3. Complete namespace projections

- Lower every advertised `NamespaceEntry` kind through one conflict and metadata model.
- Implement Unix sockets, devices, services, shared/generated files, and writable projections.
- Make endpoint ownership and host authority explicit.
- Test the same projected socket with a generic echo protocol, descriptor passing, and peer credentials before
  testing Wayland or Docker.

### 4. Complete provider I/O

- Implement every selected `HandleOperation`, provider memory, mappings, transfer, events, cancellation, and
  quotas.
- Preserve Linux open-file-description behavior through dup, fork, exec, and close-on-exec.
- Decouple provider activation transport from process stdio so PTYs remain available.
- Test generic character-device and shared-memory providers before GPU conformance.

### 5. Make a machine a real process domain

- Add `Machine::exec` with process-specific argv, environment, cwd, credentials, limits, and I/O.
- Reuse the machine filesystem, namespaces, network, provider preparation, and accounting.
- Complete process enumeration, process-group/domain signals, repeated attach, and domain shutdown.
- Make containers create one machine per container and use exec for terminal sessions and health checks.

### 6. Add authorized network transports

- Extend network policy with routes, DNS, egress selection, and leak behavior.
- Add a provider-neutral transport authority for connect/bind/resolve.
- Implement TCP/UDP over IPv4/IPv6, nonblocking behavior, cancellation, and live replacement.
- Prove direct, deny, and two unrelated provider transports before integrating a Husklet VPN.

### 7. Finish control and observation

- Implement resource limits and accounting before exposing live mutation.
- Carry process/domain/provider identities through events, metrics, and traces.
- Bound every queue and define overflow, backpressure, cancellation, and teardown behavior.
- Use the same control channel for process enumeration, updates, hotplug, faults, and lifecycle events.

Checkpointing, virtual time, deterministic entropy, and debugging remain later capabilities. Their models stay
available, but no backend advertises them until implementation and conformance exist.

## Review gate for a new engine API

Before accepting a new public type or operation, reviewers should be able to answer:

1. What Linux-visible behavior or lifecycle does it represent?
2. Which layer owns its policy, mechanism, authority, and cleanup?
3. Can a caller use it without importing a backend or product type?
4. Can at least two unrelated providers/backends implement it without engine name-based branching?
5. Is configuration typed or provider-versioned, bounded, and validated before side effects?
6. Are secrets and live host handles outside the serializable spec?
7. Does capability discovery express every meaningful combination and limit?
8. Are failure, cancellation, rollback, and teardown observable and deterministic?
9. Do contract tests cover the abstraction and real guest tests cover its Linux behavior?
10. Which Husklet workaround can be deleted only after those tests pass?

Reject an API when its main justification is “Husklet currently needs this exact object.” Restate the need as
a reusable Linux mechanism first. Also reject a nominally generic API when its only practical implementation
requires recognizing a provider name, parsing a private environment variable, or accepting an unchecked byte
channel.

## Conformance gates

An engine capability is complete only after a real guest test proves it and discovery advertises it:

| Use case | Required proof |
| --- | --- |
| Workspace terminal | PTY input/output, resize, signals, exec, fork, and teardown under provider activation |
| Chrome/GTK/Zed | Wayland socket projection, shared-memory and descriptor transfer, multiple windows/popups, clipboard and input under load |
| OpenGL/Vulkan | projected libraries/ICD, render-node ioctl/map/poll/transfer, frame presentation, resize, and process restart |
| CUDA/NVML | projected libraries, provider configuration, kernel submit/readback, memory limits, fork/exec, and truthful unsupported calls |
| Docker socket | projected host Unix socket, concurrent clients, peer credentials, descriptor lifecycle, daemon restart |
| Workspace mounts | read/write bind coherence, ownership, symlinks, mmap, locks, watches, overlay copy-up and whiteouts |
| VPN | routed TCP/UDP/DNS with leak tests, cancellation, reconnect, route replacement, and direct/deny fallback |
| Limits | each launch and live limit enforced, observable, and cleaned after restart |

Only after these gates pass should Husklet remove its corresponding mount, environment, subprocess, or
compatibility path and depend on the advertised typed capability.
