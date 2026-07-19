# hl-engine

`hl-engine` is the safe Rust interface to the statically linked C engine. The crate ships one native archive containing
both Linux guest backends for each supported AArch64 host:

- `aarch64-apple-darwin`
- `aarch64-unknown-linux-gnu`

The runtime is process-isolated because the two production translators intentionally own independent global JIT
state. `Guest::Aarch64` and `Guest::X86_64` select the child backend without asking applications to choose a
Cargo feature. There is no daemon, shell script, GUI, GPU, compositor, or Chrome integration.

The typed discovery and launch API is the primary integration surface:

```rust,no_run
use hl_engine::{spec::TreeSource, Engine, Guest, MachineSpec, ProcessIo, Sandbox};

let engine = Engine::new();
let capabilities = engine.capabilities();
assert!(capabilities.guests.iter().any(|platform| platform.architecture == Guest::X86_64));

let mut spec = MachineSpec::new(Guest::X86_64, "/bin/sh");
spec.process.argv.extend(["-c".into(), "echo hello".into()]);
spec.process.cwd = "/work".into();
spec.process.env.push(("TERM".into(), "xterm-256color".into()));
spec.filesystem.root = Some(TreeSource::HostDirectory("/var/lib/hl/alpine".into()));
spec.resources.memory_bytes = Some(512 * 1024 * 1024);
spec.resources.process_limit = Some(256);
spec.security.sandbox = Sandbox::Enabled;

let validation = engine.validate(&spec)?;
assert!(validation.unavailable_optional_extensions.is_empty());
let exit = engine.spawn(spec, ProcessIo::default())?.wait()?;
assert!(exit.success());
# Ok::<(), Box<dyn std::error::Error>>(())
```

`Engine::validate` performs the same preflight checks as `spawn` without creating host state. Unknown required
extension contracts fail; unknown optional contracts are reported as unavailable. `Machine` owns live process I/O,
the process domain, polling, waiting, and forced shutdown. The legacy `Config`/`Command` builder remains available as
a compatibility adapter while consumers migrate. Both launch paths serialize through the C engine's versioned
`hl_launch_config` wire format; ambient `HL_*` variables are not used.

The `extension` module defines versioned, domain-neutral `Namespace`, `Handles`, `Memory`, `Events`, `Lifecycle`, and
`Extensions` ports. Capability discovery reports only contracts implemented by the current backend. The
`engine.namespace` version 1.0 advertises exactly `directories`, `host-bind-read-only`, `immutable-files`,
`mutable-files`, and `symlinks`. It projects their atomic trees at absolute guest paths. Mutable files have bounded
initial bytes and one launch-private backing object, preserving normal open, fork, truncate, and shared-mapping
coherence while isolating launches. A host bind must grant an existing regular file
or directory; writable binds, host symlink roots, sockets, and other special nodes are rejected. Entries use the
ordinary read-only mount/VFS path, so open, stat, readlink,
canonicalization, and directory enumeration do not use extension-specific syscall hooks. Launch-private backing is
revoked when the machine exits. Other node sources, open services, provider memory, and checkpoints remain
undiscovered or return a typed unsupported error.

The public `transport` module contains the frozen, bounded provider-channel foundation: versioned
frames, request ids, deadlines, cancellation, peer-close detection, lifecycle handshake, and a
launch-scoped provider registry. It is intentionally absent from discovery until native VFS
open-file descriptions dispatch through that channel; declaring a service remains unsupported.

Preflight validates extension envelopes even when an optional provider is unavailable: schema and request bounds,
namespace paths and transaction conflicts, service registrations, memory requirements, guest environment additions,
and required/optional feature sets. Passing that structural validation does not imply runtime support. A contract is
launchable only when `EngineCapabilities::extensions` advertises its provider, version, and required features and the
result appears in `Validation::selected_extensions`.

Successful validation also returns `HostResourceEstimate` and any namespace conflicts contributed only by unavailable
optional extensions. Active namespace conflicts are errors. `SpecError` always carries a stable category and field and,
when applicable, a typed provider, path, service, or limit resource so dependency users do not need to parse messages.

Discovery also reports launch resource controls, live-update support, host versus virtual time, deterministic entropy,
observability, debugging, and checkpoint support as separate capability models. Their corresponding `MachineSpec`
fields are fully typed and preflight checks malformed bounds before checking backend availability. Today the native
backend lowers memory/process/CPU-count ceilings, host time, secure host entropy, and a read-write translation cache;
valid requests for richer accounting, virtual time, deterministic entropy, checkpoints, structured telemetry, or
authorized debugging fail with a field-addressed `Unsupported` error instead of being silently ignored.

The published crate contains a pinned static archive for each supported host target. Cargo selects and links the
matching archive without compiling C or downloading anything. Child isolation reexecutes the downstream application
and activates the native backend before Rust `main`. macOS applications must be signed with the JIT entitlement.
Exact source commit, checksums, and fixture provenance are recorded in `assets/PROVENANCE.md`.
