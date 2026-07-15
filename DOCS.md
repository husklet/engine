# hl-engine: system design and operating guide

## 1. Purpose

`hl-engine` is a standalone execution engine for running Linux programs on supported host operating systems. Linux is
the only guest operating-system personality. The engine translates guest machine code, implements the Linux ABI, and
delegates native operating-system work through a typed host-service interface.

The final system has three independent axes:

- **Guest operating system:** Linux.
- **Guest instruction set:** initially AArch64 and x86-64.
- **Host platform:** macOS first, Linux second, and Windows last, all behind the same service contract.

This ordering is an implementation priority, not an architectural dependency. The current production effort ports and
finishes the known-working macOS-hosted Linux engine. Every behavior is separated into translator, Linux ABI, and
host-service ownership as it is transferred. Once the macOS compatibility and performance gates are complete, the
same portable layers are used to finish Linux. Windows is an explicit final host target after both POSIX hosts have
proved the contract; it must not be anticipated by leaking Windows policy into the Linux front or translator.

The engine is deliberately product-neutral. It does not know about windows, browsers, compositors, GPU APIs, display
servers, application brands, or application-specific command-line modes. A caller may build any of those systems on
top of the engine by supplying files, configuration, IPC endpoints, and host services through the public API.

The project is written in C and builds reusable libraries. Bindings in another language are thin consumers of the C
API; they do not own Linux behavior or duplicate the engine. Guest compatibility and performance tests live with the
C engine. Binding tests verify only the public binding surface.

## 2. Definition of done

The engine is complete when all of the following are true:

1. The same Linux guest programs produce the same observable results on macOS and Linux hosts.
2. A guest ISA can be paired with every implemented host-CPU backend without changing Linux ABI code.
3. Translator code performs no host operating-system calls.
4. Linux ABI code owns Linux semantics and uses only validated host-service operations.
5. Host backends never interpret guest pointers, Linux syscall numbers, Linux structures, or guest policy.
6. Every public capability is implemented, rejected explicitly, or omitted from the advertised capability set.
7. No successful API call silently ignores a requested option, limit, payload, or operation.
8. Both production guest ISAs pass exact-golden compatibility, stress, fork, signal, memory, filesystem, IPC, and
   lifecycle tests on every production host.
9. Failures preserve Linux-visible errno and atomicity; an unsupported operation never partially mutates state.
10. Cold start, warm start, translation, syscall, fork, exec, and steady-state performance are measured with a
    repeatable C harness and retained as distributions, not single best-case numbers.
11. Debug logging is compiled out of release builds and is selectable by tag in debug builds.
12. The library can be embedded without acquiring process-global policy, hidden threads, ambient filesystem access,
    or application-specific behavior beyond the explicitly documented runner boundary.

## 3. Architecture

The dependency direction is:

```text
runner / embedding application
             |
             v
          engine
          /    \
         v      v
 translator  Linux ABI
         \      /
          v    v
             host-service contract
          /             |             \
 host-macos        host-linux       host-windows
   current             next            last
```

Only arrows shown above are allowed. `host-windows` is the final planned provider, not current implementation work.
Host backends do not call into guest translation, and the translator does not reach around the service contract to
native APIs.

### 3.1 Public API (`include/hl`)

The public API contains:

- engine creation, request, run, stop, and destruction;
- launch configuration and the serialized launch wire;
- fixed-width status, handle, range, byte-buffer, and metadata types;
- host-service group definitions and capability bits;
- Linux-front configuration required by an embedding application.

Public structs begin with `abi` and `size`. Additions are append-only. The validator accepts every supported prefix
version and rejects malformed combinations before any external effect occurs. Public types never contain `pid_t`, a
native file descriptor, a compiler-sized enum, a platform context structure, or a C library object.

`hl_engine_config` is authoritative. Unknown flags and nonzero reserved fields are invalid. Limits are applied before
guest startup. A field that cannot be honored is rejected; it is never accepted as a no-op.

`hl_engine_box_config` is the reusable, ABI-versioned Linux-box configuration surface. Its current slice owns Linux
identity (`uid`/`gid`), initial working directory, hostname, guest environment, read-only-root policy, host sandboxing,
and external-network isolation. `hl_engine_create` validates the complete structure and deep-copies its strings before
returning, so two engines never share mutable launch state and callers need not retain configuration storage. Applying
these values to the instance option store is transactional: validation or allocation failure destroys the candidate
instance without starting a process or changing another engine.

The serialized `hl_launch_config` remains the launcher wire format, not the preferred embedding API. Its
`lower_layers`, `publish`, `volumes`, `limits`, `network_namespace`, `translation_cache`, `network_bridge`, `ip`,
`filesystem_generation`, `egress_proxy`, `checkpoint_directory`, and `restore_directory` strings; its
`publish_external` and `translation_cache_disabled` booleans; and its sentry-only `HL_CONFIG_UNTRUSTED_ONLY` sandbox
mode do not yet have typed public engine fields. The wire's rootfs and scalar memory/pid/CPU limits are already exposed
by `hl_engine_config`; arguments are supplied to `hl_engine_run`; debug logging is deliberately a debug-build concern,
not box configuration. Remaining settings must be added append-only to `hl_engine_box_config`, with validation and
ownership rules; public callers must not be given raw access to the internal `HL_*` option registry.

### 3.2 Engine core (`src/core`)

The core owns one engine instance and coordinates its lifecycle. It validates configuration and host capabilities,
selects the guest frontend and host-CPU backend, creates the Linux ABI instance, and drives the outer guest process.

Core invariants:

- all mutable state is instance-owned;
- a request cannot race destruction into use-after-free;
- destruction transitions to a terminal state, terminates an active run, waits for host completion, closes all
  resources, and only then frees memory;
- core code includes no platform headers and calls no native operating-system APIs;
- yielding, clocks, waits, files, processes, and memory come from host services;
- configuration is validated before a child can observe it.

### 3.3 Translator (`src/translator`)

The translator has four responsibilities:

1. Decode a supported guest ISA.
2. Produce validated, private IR.
3. Optimize without changing guest-visible behavior.
4. Lower IR to the selected host CPU.

It does not implement Linux syscalls and does not access paths, native files, processes, clocks, signals, or sockets.
Executable-memory allocation and instruction-cache publication are host services.

IR blocks end with explicit terminators. Values refer only to valid prior definitions. Validation runs before lowering.
Persistent cache identity includes every code-changing input: guest ISA, host ISA, IR ABI, codegen ABI, translator
features, page assumptions, and relevant execution modes. A cache mismatch is a miss, never a best-effort load.

### 3.4 Linux ABI (`src/linux_abi`)

The Linux front is the sole owner of Linux behavior:

- syscall numbers and argument decoding;
- guest pointer validation and Linux structure layouts;
- Linux errno and signal numbers;
- file descriptors and open-file descriptions;
- processes, thread groups, PIDs, waits, and Linux limits;
- `/proc`, `/sys`, devices, namespaces, and virtual ownership;
- epoll, eventfd, timerfd, inotify, futexes, pipes, and IPC semantics;
- memory maps, protections, file-size transitions, and SIGBUS rules;
- Linux socket families, options, ancillary layouts, and network namespace policy.

Linux ABI code receives guest values, validates them, converts them to host-neutral operations, and encodes results back
into guest memory. Host callbacks receive host buffers and opaque handles only.

#### File descriptors and open-file descriptions

A guest descriptor refers to an engine-owned descriptor entry. That entry refers to an open-file-description (OFD)
object. `dup` creates another descriptor reference to the same OFD. Independent `open` calls create distinct OFDs even
when they name the same physical object.

The OFD owns shared offset, status flags, directory position, whole-file lock identity, and the opaque host handle.
Descriptor flags such as close-on-exec belong to the descriptor entry. Final OFD close releases the host handle.

These rules must survive fork:

- inherited and duplicated descriptors share the original OFD;
- independent opens remain distinct;
- directory position is shared through dup and inherited across fork;
- BSD `flock` follows OFD identity;
- POSIX record locks follow Linux process ownership rules;
- closing one alias does not destroy state held by surviving aliases;
- closing the final reference wakes waiters and releases resources.

Guest descriptor numbers are logical Linux numbers. Host-private descriptors are never guest-visible. When a shadow
descriptor is necessary for a legacy native operation, it is reserved atomically, marked close-on-exec, paired with
one typed reservation, and closed on every rollback path. Relocating a private descriptor updates its owner and closes
the old native slot.

#### Files and directories

Path resolution is component-based and confined beneath an explicit root handle. Symlink policy is part of the
operation. A path cannot escape through `..`, a final symlink, a renamed parent, or a time-of-check/time-of-use race.

Linux ABI composes Linux behavior over host primitives:

- open, read, write, append, vector I/O, and positional I/O;
- metadata, filesystem geometry, permissions, virtual ownership, and timestamps;
- allocation, punch, zero, collapse, insert, and keep-size modes;
- directory enumeration and Linux `dirent64` packing;
- sendfile, splice, readahead, sync, and sparse data/hole seeking;
- typed mapping, truncation publication, and external-change observation.

Native uid/gid metadata describes the host object. Guest ownership is virtualized by the Linux front and persists by
stable object identity without physically changing host ownership.

Directory enumeration uses a shared logical cursor. Host cookies are local implementation details and are never
assumed portable across cloned host directory streams. Dup aliases observe one cursor; fork clones retain the Linux
position; rewind is visible through every alias.

#### Memory

The memory model distinguishes guest virtual addresses, host mappings, file offsets, and translated-code mappings.
Mappings are tracked dynamically; fixed placement never replaces an occupied range unless Linux explicitly requests
replacement. `MAP_FIXED_NOREPLACE` succeeds only at the exact free address and returns `EEXIST` without changing an
occupied canary.

File truncation and extension publish transitions to every shared or private mapping of the same object. Access past
the current whole-page EOF produces Linux SIGBUS with correct metadata. Extension restores clean file-following
ranges while preserving dirty private pages. Fork snapshots mapping state coherently.

Executable JIT memory uses separate writable and executable aliases where the host requires them. Publication orders
writes, host instruction-cache maintenance, and visibility before a generated entry point can execute.

#### Events and IPC

Linux ABI owns epoll registration identity, edge/level behavior, one-shot rearming, OFD lifetime, dup/fork behavior,
signal masks, and Linux event layouts. A host adapter supplies native wait/wake primitives. Closing one watched alias
rehomes a registration to a surviving alias; closing the final OFD removes it.

SCM_RIGHTS transfers preserve OFD identity. A POSIX host may expose a scoped attachment service that borrows a
close-on-exec native duplicate from an opaque file handle. The Linux front rewrites only a private control-message
buffer and releases every borrow on success or error. It never reopens by path.

### 3.5 Host-service contract (`include/hl/host_services.h`)

The host-service contract is the portability seam. Service groups are independently versioned and capability-gated.
Typical groups cover:

- memory and executable code;
- clocks and waits;
- files, directories, mappings, and filesystem change observation;
- processes and native child lifecycle;
- synchronization objects;
- event sets, counters, timers, and transfer endpoints;
- network primitives;
- debug-log output;
- optional POSIX ancillary attachment.

A backend advertises only complete groups. Optional appended callbacks are detected using the group size. Missing
optional behavior returns a typed unsupported status without side effects.

Handles are opaque 64-bit values. A backend encodes kind, index, and generation so wrong-kind and stale handles are
rejected. Registries grow dynamically toward advertised Linux limits. Active worker threads receive stable heap nodes;
growing a pointer table never invalidates a live worker context. Destruction cancels, joins, and quiesces callbacks
before freeing nodes.

Backends translate native failures into precise `hl_status` values. Linux ABI then maps status to operation-specific
Linux errno. Native errno values are never passed through numerically between platforms.

### 3.6 Host backends (`src/host`)

`host-macos` maps the service contract to Mach, POSIX, kqueue, filesystem, process, and JIT facilities. `host-linux`
maps it to Linux native services such as `epoll`, `eventfd`, `timerfd`, inotify, `mmap`, `fallocate`, and `/proc` fd
inspection. Both implement the same observable contract.

Backends own all native descriptors and handles. They mark private descriptors, keep them out of the guest namespace,
and release them exactly once. Fork hooks repair locks, descriptors, threads, and mappings without carrying an invalid
parent-only synchronization state into the child.

The fake backend is deterministic and exists for core unit tests. It must model contract semantics faithfully enough
to test validation, rollback, lifecycle, and error propagation; it is not a production fallback.

### 3.7 Runner boundary (`src/runner`, `src/core/target`)

The runner is a generic Linux-program launcher. It accepts engine configuration, guest executable, argument and
environment data, rootfs and volume configuration, and debug settings. It contains no application-specific modes.

Process-global signal handlers, final `_exit`, native fault entry, and executable target bootstrap belong at this
boundary. The reusable libraries do not silently install process policy.

## 4. Isolation and security model

The engine is a container boundary, not a path-prefix convenience wrapper.

- Every guest path resolves beneath an explicit root or volume handle.
- Native engine descriptors are private and excluded from `/proc/self/fd` guest views.
- Guest close, dup, close-range, and exec cannot overwrite or leak engine descriptors.
- Descriptor reservations precede externally visible create/truncate effects.
- Failed operations unwind handles, mappings, callbacks, and reservations in reverse order.
- Guest pointers are range-checked before native calls; output buffers are validated before mutation.
- Linux credentials, ownership, limits, namespaces, and proc/sys content come from engine state.
- The engine never depends on a macOS guest jail. The guest personality is always Linux.
- Unsupported functionality is explicit and does not weaken confinement.

## 5. Lifecycle

The normal embedded lifecycle is:

1. Construct one host backend instance.
2. Validate its service table and required capabilities.
3. Fill `hl_engine_config`, including limits and reserved fields set to zero.
4. Create the engine.
5. Submit a launch request or serialized launch configuration.
6. Run until guest exit, explicit stop, or host failure.
7. Read the typed result.
8. Destroy the engine; destruction joins any active run and quiesces host callbacks.
9. Destroy the host backend after all engine users are gone.

Multiple engine instances must coexist without shared mutable guest state. Repeated create/run/destroy cycles must
return process descriptor and thread counts to baseline.

## 6. Configuration and logging

Configuration has one definition and one validation path. Environment variables are reserved for essential debugging
or launch integration. Project variables use the `HL_` prefix. Feature behavior is not scattered across undocumented
flags.

Logging is compiled in only with `HL_ENABLE_LOGGING=1`, normally selected using `DEBUG=1`. Release logging calls
compile to no-ops. In a debug build, `HL_LOG` selects tags:

```text
HL_LOG=log:fs,log:jit
HL_LOG=log:syscall,log:process,log:signal
HL_LOG=log:all
```

Defined domains are `fs`, `jit`, `syscall`, `process`, `network`, `signal`, and `translate`. Tag parsing and filtering
are portable engine behavior. A host backend only writes the final bytes. Log contexts are per engine/launch so one
launch cannot retain another launch's selector or sink.

Logs are diagnostic evidence, never a synchronization mechanism or an API result.

## 7. Build and run

The normal workflow uses Make and C tools. It does not require Python or Bash test scripts.

### 7.1 Build libraries and runner

```text
make all
make linux-compile
```

`make all` builds the portable libraries, selected native host library, and generic runner. `make linux-compile`
compiles and links every portable unit and the Linux host.

Use a separate output directory when comparing configurations:

```text
make BUILD=build-release DEBUG=0 all
make BUILD=build-debug DEBUG=1 all
```

### 7.2 Debug logging

```text
make BUILD=build-debug DEBUG=1 all
HL_LOG=log:fs,log:jit build-debug/bin/hl-engine-runner ...
```

### 7.3 Unit and domain tests

```text
make test
make test-macos
make check-domains
make format-check
```

`make test` runs C unit tests, domain-boundary checks, and native compatibility smoke tests. `make test-macos`
cross-builds the mac host tests and executes them on the real mac host using `mac`.

### 7.4 Linux-host production tests

On a Linux/AArch64 host:

```text
make run-linux-production-smoke
make test-linux-production-matrix
```

The smoke test proves production JIT entry, exact stdout, and guest exit status. The matrix runner executes selected
x86-64 Linux guests through the production engine and requires exact golden output, exit status, timeout handling, and
process-group cleanup. A zero exit with incorrect output is a failure.

### 7.5 macOS production tests

The `MAC` variable names the command used to execute on the mac host. The default is `mac`:

```text
make MAC=mac compat-engines
make MAC=mac compat-memory
make MAC=mac compat-filesystem
make MAC=mac compat-ipc
make MAC=mac e2e-compat
```

Production mac engines are compiled, linked, codesigned with the JIT entitlement, and actually executed. A successful
cross-compile is not a test pass.

Focused suite targets include:

```text
compat-abi               compat-abi-corpus
compat-completeness      compat-libc
compat-core              compat-core-abi
compat-core-syscall      compat-core-regress
compat-core-workload     compat-filesystem
compat-ipc               compat-isolation
compat-memory            compat-network
compat-posix             compat-process
compat-procfs            compat-signals
compat-syscall           compat-syscall-edges
compat-threads           compat-time
compat-soak
```

`make e2e-compat` is the complete production gate. It builds both guest ISAs and runs all active suites.

Remote execution is supervised. Cancellation, timeout, or bridge loss terminates the remote process group and reaps
children. Validate the supervisor with:

```text
make remote-supervisor-test
```

### 7.6 CMake and consumers

The Make build is authoritative for repository development. `CMakeLists.txt` and `hl-engine.pc.in` provide packaging
metadata for consumers. Rust `build.rs` integration links the C libraries and uses the installed public headers; it
does not compile a second implementation of engine behavior.

## 8. Testing model

Tests prove behavior, not the presence of source text. A test must execute a public API, host service, or production
guest and assert an observable contract. Reading a `.c` file and checking that it contains a function name is not a
correctness test.

### 8.1 Test layers

1. **Unit tests:** one C module or service contract, including rollback and malformed input.
2. **Host-provider tests:** the same typed service behavior against Linux and macOS implementations.
3. **Native fixture oracle:** compile and run a Linux C fixture natively to establish its exact result where useful.
4. **Production compatibility:** run the same fixture through both guest ISAs and compare exact output and exit.
5. **Cross-host compatibility:** require identical Linux-visible behavior on macOS and Linux hosts.
6. **Stress and soak:** repeat races, forks, cache churn, mappings, descriptor growth, and lifecycle teardown.
7. **Performance:** compare distributions under controlled release builds.

### 8.2 Fixture rules

Compatibility fixtures are C programs under `tests/compat`. Each active case has manifest metadata and an exact golden
file. Tests must:

- assert return values, errno, output bytes, state transitions, and atomicity;
- use barriers instead of timing sleeps for concurrency verdicts;
- test both success and failure boundaries;
- preserve canaries around rejected memory and buffers;
- verify behavior after dup, fork, exec, close, and reopen where the object model requires it;
- fail on output mismatch even if the guest exits zero;
- leave no child, remote process, descriptor, mapping, thread, or temporary rootfs behind.

Goldens express Linux behavior, not host implementation details. They must not contain native descriptor numbers,
Darwin directory cookies, addresses, timing-sensitive values, or transient paths.

### 8.3 Typed-path coverage

A compatibility fixture intended to validate the host-service path must use a staged typed rootfs or volume. `/tmp`
or an ambient native path is not sufficient if it bypasses the typed planner. Manifests declare the rootfs shape needed
by the case, and the C matrix runner stages and removes it.

### 8.4 Regression discipline

When a broad matrix stops at a failure:

1. Reproduce the case alone on both guest ISAs.
2. Run its native Linux oracle when applicable.
3. Determine whether the typed or legacy route executed.
4. Strengthen the fixture if a race or false-positive exit exists.
5. Fix the owning layer, not the golden.
6. Run focused tests, the containing suite, unit tests, and the other host.
7. Commit one coherent semantic slice.
8. Resume the broad matrix at the next failure.

Temporary diagnostics are removed before commit.

## 9. Performance methodology

Performance is part of correctness because an execution engine that is pathologically slow cannot run general Linux
software reliably. Measurements use release builds and the C performance runner:

```text
make BUILD=build-perf DEBUG=0 perf-compat
```

Report at least:

- engine creation and cold guest start;
- warm start with valid persistent cache;
- translation throughput and generated-code size;
- steady-state compute for representative integer, floating-point, vector, and branch workloads;
- syscall round trips for file, clock, pipe, event, and process operations;
- fork, exec, and fork+exec latency;
- mmap/mprotect/munmap and file-map invalidation;
- IPC throughput and wake latency;
- peak resident memory and descriptor/thread baselines;
- cache cold/warm/fork/exec behavior.

For each metric, run warmups followed by enough samples to report count, median, p90, p99, minimum, and maximum. Record
host OS, host CPU, compiler, optimization flags, guest ISA, engine commit, debug/logging state, and cache state. Compare
the same guest workload natively where possible. Do not compare a debug engine to a release native binary.

A performance change is accepted when correctness matrices remain green and the distribution is understood. A faster
path that weakens Linux behavior, isolation, invalidation, or error fidelity is a regression.

## 10. Engineering practices

- Keep filenames short and consistent within a domain.
- Use `hl_` for exported and cross-file project symbols. Preserve Linux names for Linux ABI concepts.
- Keep one authoritative definition for configuration and environment variables.
- Prefer small C helpers and explicit ownership over macros with hidden effects.
- Every allocation has a named owner and one final release path.
- Append/version public ABI changes; validate size before reading appended fields.
- Keep host-native types behind backend files.
- Use atomics only with a written ordering invariant; otherwise use the portable synchronization service.
- Never hold an engine table lock while performing an unbounded host call.
- Publish state only after all fallible preparation succeeds.
- On failure, unwind in reverse order and preserve the original error.
- Fork hooks prepare, parent-release, and child-repair every affected subsystem.
- Do not add GUI, GPU, compositor, browser, or application policy to the engine.
- Do not add source-inspection sentinel tests.
- Do not hide unsupported behavior behind success or placeholder constants.
- Run `git diff --check`, focused tests, and the relevant full suite before committing.
- Do not run repository-wide formatters while other work is active in the shared tree.

## 11. Adding functionality

### Add a Linux syscall

1. Add the guest syscall number mapping for each guest ISA.
2. Decode and validate guest arguments in Linux ABI.
3. Reuse existing host-neutral primitives or add the smallest complete service callback.
4. Define atomicity and errno mapping before mutation.
5. Cover native Linux, both guest ISAs, typed routing, error boundaries, dup/fork/exec, and malformed input as relevant.
6. Add an exact golden and activate it in the correct manifest.

### Add a host-service operation

1. Decide whether the behavior belongs to Linux ABI or is truly a host primitive.
2. Add fixed-width types and an appended callback to the appropriate versioned group.
3. Update validation while retaining supported prefix versions.
4. Implement Linux, macOS, and fake-provider behavior or omit the capability explicitly.
5. Add provider tests for success, every relevant error class, stale/wrong-kind handles, rollback, and destruction.
6. Route Linux behavior through it and add production compatibility coverage.

### Add a host platform

1. Implement service groups without including guest or translator internals.
2. Pass public ABI validation and common provider tests.
3. Run the Linux production matrix with exact goldens.
4. Add fork, signal/fault, executable-memory, descriptor, and teardown stress.
5. Run the full cross-host compatibility suite.
6. Establish native and engine performance baselines.

## 12. Completion roadmap

This checklist tracks work required to reach the design above. It is intentionally separate from the normative rules;
items describe remaining proof or implementation, not accepted permanent exceptions.

Work is completed in this order:

1. **macOS production completion.** Transfer and separate the complete working macOS-hosted Linux engine, then pass
   every active dual-ISA compatibility, stress, lifecycle, and performance gate on the real mac host.
2. **Linux production completion.** Reuse the already-separated translator, Linux ABI, and host-service contract;
   finish the Linux backend and pass the same exact-golden corpus without changing guest semantics.
3. **Windows production implementation.** Implement Windows only after macOS and Linux prove that the portable
   contract is complete. Windows is a required eventual host, but it is deliberately the final host milestone.

macOS work must not create macOS exceptions in portable code. Each transferred behavior is placed in its final owning
layer immediately so completing macOS reduces, rather than increases, the later Linux work.

### Portability and boundaries

- [ ] Remove the remaining unity-only dependencies from production target roots.
- [ ] Route every production host operation through a typed host-service group.
- [ ] Remove ambient file/mapping access from guest ELF and persistent-cache storage paths.
- [ ] Complete the Linux-host process, signal/fault, event, filesystem, and network production lanes.
- [ ] Add a Windows backend after macOS and Linux are complete and the common contract passes without POSIX leakage.

### Linux behavior

- [ ] Run every active compatibility manifest on macOS for both guest ISAs with no exclusions except explicitly
      unsupported guest-ISA inputs.
- [ ] Run the same exact-golden corpus through the production Linux host.
- [ ] Complete native Linux epoll, timerfd, eventfd, inotify, signal-mask, dup/fork, and rearm coverage.
- [ ] Complete typed directory, metadata, ownership, allocation, sparse-file, locking, and external-change coverage.
- [ ] Complete process-group, wait, signal, exec, pid-namespace, proc/sys, and limit coverage.
- [ ] Complete socket, ancillary, namespace, readiness, option, and error-fidelity coverage.

### Lifecycle and isolation

- [ ] Repeat create/run/stop/destroy races under sanitizers where supported.
- [ ] Prove descriptor, thread, mapping, child-process, and remote-process counts return to baseline after every suite.
- [ ] Prove all registries grow to advertised limits without stale-handle aliasing.
- [ ] Fault-inject every host callback boundary and verify transactional rollback.

### Translation and cache

- [ ] Run the complete instruction, SMC, code-cache, fork, thread-churn, and persistent-cache corpus on every host.
- [ ] Verify cache identity rejects every incompatible code-changing configuration.
- [ ] Prove no translator path performs ambient host I/O.

### Performance and release

- [ ] Establish reproducible native, mac-host, and Linux-host baseline distributions.
- [ ] Add tracked thresholds for cold start, warm start, translation, syscall, fork/exec, IPC, and memory overhead.
- [ ] Run long soak and application-level Linux workloads without leaks or unbounded growth.
- [ ] Verify release binaries contain no active debug logging and require no diagnostic environment flags.
- [ ] Publish the standalone C libraries, headers, pkg-config metadata, and binding integration tests.

The roadmap is complete only when evidence from the production engines proves every checkbox. Passing a narrower unit
test or finding matching source code is not sufficient.
