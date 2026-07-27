# hl-engine: system design and operating guide

This file is normative: it defines the layering, the public contracts, the testing model and the
completion roadmap. The rest of `docs/` is subordinate detail.

| Document | Covers |
|---|---|
| `docs/arch.md` | Where each layer defined here lives, at symbol level. Read it before touching `src/core`. |
| `docs/amd64-host.md` | The x86-64 Linux host: the host-CPU seam, the host-CPU x guest-ISA matrix, and why an interpreter comes before a transliterator. |
| `docs/makefile-retirement-darwin-evidence.md` | Measured `make`-vs-CTest equivalence, recorded before the Makefile was deleted. |
| `docs/ci-green.md` | Differential compat findings, `excluded-macos` justifications, runtime self-skips, open gaps. |
| `docs/checkpoint-sink.md` | The `ckpt_sink`/`ckpt_source` interface and the embedder-supplied checkpoint store. |
| `docs/checkpoint-restore-io.md` | Restore recovery policies and external-resource reconnection. |
| `docs/perf-simd-findings.md` | Measured float/SIMD root causes: the shipped SSE NaN-gate fix, the open aarch64 back-edge poll. |
| `docs/perf-sqlite-findings.md` | Why the sqlite phase runs at ~0.4x native, with the ruled-out hypotheses. |
| `docs/compliance-ltp.md` | The manual LTP differential lane under `tests/compliance/ltp/`. |

Per-suite fixture rules live in `tests/compat/<suite>/README.md`; `tools/bench/README.md` documents the
benchmark harness.

## 1. Purpose

`hl-engine` is a standalone execution engine for running Linux programs on supported host operating systems. Linux is
the only guest operating-system personality. The engine translates guest machine code, implements the Linux ABI, and
delegates native operating-system work through a typed host-service interface.

The final system has three independent axes, the last of which is itself a pair:

- **Guest operating system:** Linux.
- **Guest instruction set:** initially AArch64 and x86-64.
- **Host platform:** macOS and Linux behind the same service contract — and, independently of the host OS, the
  **host CPU**: AArch64 and x86-64.

Host OS and host CPU are separate axes and they compose. They are named once per language and nowhere re-derived:
`HL_HOST_CPU_*` in `src/host/host_cpu.h`, `HL_HOST_ARCH` in `CMakeLists.txt`, `hostCPUs` in `flake.nix`, beside the
host-OS names `src/host/<os>/`, `CMAKE_SYSTEM_NAME` and `hostBackends`. A guest ISA equal to the host CPU permits
same-ISA transliteration; it is never a reason to treat the two as one fact. `docs/amd64-host.md` records the seam and
the current state of the host-CPU x guest-ISA matrix.

This ordering is an implementation priority, not an architectural dependency. The current production effort ports and
finishes the known-working macOS-hosted Linux engine. Every behavior is separated into translator, Linux ABI, and
host-service ownership as it is transferred. Once the macOS compatibility and performance gates are complete, the
same portable layers are used to finish Linux. Other hosts are outside the current product scope and do not affect
the definition of done.

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
          /             \
 host-macos        host-linux
```

Only arrows shown above are allowed.
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

`hl_engine_box_config` is the reusable, ABI-versioned Linux-box configuration surface. It owns Linux identity,
working directory, hostname, guest environment, root policy, sandbox mode, network isolation and publication, overlay
layers, volumes, resource-limit records, namespace and bridge identity, virtual IP, translation-cache policy,
filesystem generation, egress proxy, and checkpoint/restore directories. `hl_engine_create` validates the complete
structure and deep-copies every string before returning, so two engines never share mutable launch state and callers
need not retain configuration storage. Applying these values to the instance option store is transactional: validation
or allocation failure destroys the candidate instance without starting a process or changing another engine.

ABI 1 is the exact prefix ending at `environment`; ABI 2 appends the remaining settings. Cache enable and disable are
mutually exclusive. Checkpoint and restore directories may be selected together so a restored process tree is armed
for its next capture; an IP requires a bridge, external publication
requires publication rules, and isolated networking rejects publication, bridge, IP, and egress settings. Full host
sandboxing and sentry-only routing are distinct mutually exclusive modes. Cache, filesystem-generation, checkpoint,
and restore locations are absolute paths. A non-NULL string is always nonempty. The serialized `hl_launch_config`
remains the launcher wire format, not the preferred embedding API; public callers never receive raw access to the
internal `HL_*` option registry. Rootfs and scalar memory/pid/CPU limits live in `hl_engine_config`, arguments are
supplied to `hl_engine_run`, and debug logging is deliberately a debug-build concern rather than box configuration.

Checkpoint/restore is gated on both guest ISAs (`ctest -L checkpoint`, labels `checkpoint-aarch64` /
`checkpoint-x86_64`). Capture and restore have deliberately different contracts.

**Capture is all-or-nothing.** A successful capture freezes every live descendant at an engine safepoint, writes
each process's guest memory, CPU state, signal dispositions, identity, and descriptors into a staged process group,
then publishes the image and the `MANIFEST` last. Any failed object aborts its process group and nothing is
published (`src/linux_abi/checkpoint.c`, and docs/checkpoint-sink.md "Failure semantics"). Capture refuses
sentry/untrusted mode, connected or in-progress sockets that would need connection-state transfer, descriptors with
no recoverable path, and guest descriptor tables that exceed the checkpoint limit or change mid-capture. A process
that daemonizes out of the init descendant tree is not captured. These cases are refused, not silently omitted from
an otherwise successful image.

**Restore is permissive by default and restores what can be restored.** `HL_CHECKPOINT_POLICY` selects
`reconnect`, `discard-optional`, or `refuse`; leaving it unset restores as `discard-optional`. Under the
permissive policies a process that cannot be reconstructed is stopped along with its descendants and the rest of
the tree resumes; that is the intended contract, not a degraded mode. Capture is unaffected by the default: it
only relaxes for an explicitly permissive policy. Container init is mandatory under every policy. Every restore records the
per-process outcome in `RECOVERY.jsonl`. Detail: docs/checkpoint-restore-io.md. Where the bytes live is an
embedder decision: docs/checkpoint-sink.md.

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
- a serialized launcher wire is converted once into an owned option snapshot; the native-engine boundary clones that
  snapshot into the new engine before applying typed public overrides, so nested launch scopes cannot mask CPU,
  filesystem, identity, or isolation settings;

### 3.3 Translator (`src/translator`)

The translator has three responsibilities:

1. Decode a supported guest ISA.
2. Emit host machine code for it without changing guest-visible behavior.
3. Cache, relocate, invalidate and persist that code.

It does not implement Linux syscalls and does not access paths, native files, processes, clocks, signals, or sockets.
Executable-memory allocation and instruction-cache publication are host services.

**There is no intermediate representation. Decode and host code generation are one step.** The guest frontends under
`src/translator/guest/{aarch64,x86_64}/` are the only thing that executes guests, and they emit host machine code
**directly**. `guest/x86_64/emit.c` is a set of ARM64 host emitters (its own header says so, NEON/SSE included);
`guest/aarch64/translate.c` is a same-ISA transliterator that copies most guest instructions verbatim and mangles only
stolen-register users. Both therefore presuppose an ARM64 host CPU. Bringing up a new host CPU is new code generation
in these directories; there is no host-neutral lowering seam to reuse. `docs/amd64-host.md` is the detail.

A separate IR and per-host-CPU lowering pipeline (`include/hl/ir.h`, `include/hl/codegen.h`,
`src/translator/codegen.c`, `src/translator/ir/`, and `codegen.c` under each `src/translator/host/<cpu>/`) used to
live here. It was clean-room bootstrap scaffolding that the transferred production frontends superseded within a day;
its 17-opcode IR could express neither frontend, and `hl_codegen_*`/`hl_ir_*` had no caller anywhere in `src/`. It was
deleted, because the shape of the tree — a symmetric `host/<cpu>/codegen.c` per host CPU — read like the production
lowering path and repeatedly cost readers time. `docs/amd64-host.md` §2 keeps the history.

What remains under `src/translator/host/` is `aarch64/asm.{c,h}`, the ARM64 instruction assembler (`hl_a64_*`) the
production JIT actually uses, covered by `unit.a64_asm`.

`src/translator/reloc.c`, in the same area, is also live: `guest/*/cache.c` calls `hl_reloc_slide` when it loads a
persistent-cache artifact at a different base.

Persistent cache identity includes every code-changing input: guest ISA, host CPU, engine build, translator features,
page assumptions, and relevant execution modes. A cache mismatch is a miss, never a best-effort load. The host CPU is
part of that key because two hosts sharing a cache directory would otherwise accept and execute each other's host
code; the value hashed in is `HL_HOST_ISA_*` from `src/translator/identity.h`, pinned by `_Static_assert` to the
preprocessor-time `HL_HOST_CPU_ISA_*` in `src/host/host_cpu.h`.

Persistent artifacts use the typed File service only. The engine creates or opens the configured private cache
directory once, validates it without following a final symlink, and pins that directory handle for the run. Every
load, atomic publication, and removal is relative to that handle and accepts a single leaf name; absolute paths,
slashes, `.` and `..` are rejected. Source identity is derived from a no-follow typed open plus stable metadata,
change time, and the source path. A failed or concurrent publication cannot replace the last complete artifact, and
an invalid or truncated artifact is handled as a cold miss. The pinned handle is closed during normal target
teardown; process termination closes it on non-returning Linux exit paths.

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

`src/host/<os>/` is keyed by host OS alone and is host-CPU-neutral: `src/host/linux/` compiles and passes its provider
tests unchanged on an x86-64 host. The host CPU is therefore **not** an axis of this directory — it is an axis of the
translator (3.3) and of the target roots under `src/core/target/`. Code that must branch on it uses `HL_HOST_CPU_*`
from `src/host/host_cpu.h`, never a bare compiler predefine: `defined(__x86_64__)` says nothing about which OS's
`ucontext_t` layout and calling convention apply, and mixing the two produced an Apple-shaped register access that
could not compile against a Linux `ucontext_t`.

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

Two build systems are present while the project migrates to CMake. Neither requires Python.

* **CMake (preferred, standard flow).** Covers the core libraries, the production engines and embedded
  activation archive, the unit/compat/e2e matrix as CTest cases, codesigning, `install` and `uninstall`. This
  is the path new work should use.
* **Make.** The historical build. It is still what CI runs for the Linux compat shards and for the entire
  macOS lane, so it cannot be deleted yet. `nix build`, `checks.package` and `checks.unit` are CMake, so the
  two are exercised together on every push.

A few CTest cases and build targets shell out to `bash` scripts under `tools/` (lane parity, direct launches,
crate-archive currency, the remote-macOS lane). No test suite is implemented in a shell script.

### 7.0 CMake: the standard flow

```text
cmake -G Ninja -B build-cmake
ninja -C build-cmake
ctest --test-dir build-cmake -L unit --no-tests=error   # one label per suite
cmake --install build-cmake --prefix /usr/local
cmake --build build-cmake --target uninstall     # exact install_manifest.txt consumer
```

`ctest --test-dir build-cmake -N` and `--print-labels` enumerate the registry; a Linux `nix develop`
configure registers 396 cases, a Darwin one 195. Nothing is hand-maintained, so neither can drift.
`gate.ci-lane-parity` fails the build if any lane declared in `cmake/CiLanes.cmake` selects zero tests
— `ctest -L` on an unknown label exits 0, which would otherwise turn a mistyped CI step green. Pass
`--no-tests=error` on every label selection for the same reason.

A second configure with `-DHL_SANITIZE=ON` is the ASan/UBSan build.

No toolchain file is needed for a native build: inside the devShell `$CC` is the intended host compiler.

Installing yields a usable SDK: headers under `include/hl`, the static archives (including
`libhl-engine-activation.a`), both pkg-config files, and `bin/hl-engine-runner`. Set
`-DCMAKE_INSTALL_PREFIX` at configure time if you need the generated `.pc` files to carry a different
prefix, since they bake it in.

Cross/host toolchain files live in `cmake/toolchains/`:

| file | builds |
|---|---|
| `cmake/toolchains/aarch64-linux.cmake` | aarch64 Linux **host** archives, forcing `$AARCH64_LINUX_CC`. Only needed when `$CC` is not already the intended host compiler; a native devShell build needs no toolchain file. From an x86-64 Linux host it is a genuine cross file, which is how CI compiles the AArch64 arm of every host-CPU guard without AArch64 hardware. |
| `cmake/toolchains/x86_64-linux.cmake`  | x86-64 Linux **host** archives, forcing `$X86_64_LINUX_CC`. Not guest fixtures: guest binaries are built per ISA by `cmake/GuestFixtures.cmake` regardless of which toolchain file configured the tree. As with the aarch64 file, a native devShell build on that host needs no toolchain file. |
| `cmake/toolchains/macos-remote.cmake`  | macOS artifacts **from a Linux host**, by forwarding each compiler and binutils invocation to the macOS host through OrbStack's `mac` (see `tools/remote/`). The build directory must live inside the repo, because only that path is shared with the macOS side. |

Nix remains the toolchain authority: the toolchain files read the same `AARCH64_LINUX_CC` /
`X86_64_LINUX_CC` the devShell exports. The devShell also exports `$CC` (and
`$NATIVE_CC`) as the **native** compiler, so a bare `cmake`/`make` in the shell targets the host. The
per-ISA variables stay explicit because the guest ISA is not always the host ISA.

### 7.0.1 Nix entry points

Nix drives CMake; these are the supported top-level commands.

```text
nix develop                 # dev shell: host + guest cross compilers, $CC, $*_LINUX_CC, $*_DYNAMIC_*
nix build                   # the CMake build, installed as an SDK (packages.default)
nix flake check             # format + unit + package + rust
nix run .#fmt               # apply clang-format in the working tree
```

`*_LINUX_STATIC_CC` carries the stock `pkgsStatic.sqlite` for the guest ISA, which implies a musl cross
toolchain per ISA. That is intentional: it is what the binary cache has. If nix starts building a musl gcc
from source, the cause is almost always a changed cache key -- both the nix eval and the CI cache action
hash `flake.nix` -- rather than a newly added dependency.

`fmt` is the only app: it mutates the working tree, so it cannot be a derivation. Everything that merely
verifies is a `check`, which is also why checks can compile — they get a real stdenv, whereas a
`writeShellApplication` does not carry the cc-wrapper variables and so cannot invoke the compiler correctly.

Host OSes, host CPUs and guest ISAs are data tables in `flake.nix` (`hostBackends`, `hostCPUs`, `guestISAs`), not
`system == "..."` branches, so adding a member is an entry rather than a new branch: a Windows host backend is one
entry once `src/host/windows/` has code. `x86_64-linux` is a full member of `systems` — its devShell, `packages.default`
and `checks.{format,lint,package,unit}` all exist and are green — but read that as "first class in the build", not
"first class as a host": `checks.x86_64-linux.rust` deliberately does not exist (`hasCrateArchive` is false without a
committed x86-64 crate archive), and no guest executes on an x86-64 host yet. See the host table in `README.md` for
what is proven, and `docs/amd64-host.md` for what gates the rest.

### 7.1 Build libraries and runner

```text
cmake -G Ninja -B build
ninja -C build
```

`ninja` with no target builds every library, production engine, runner, test binary and guest
fixture. Use a separate build directory when comparing configurations; nothing is written into the
source tree, so there is no `clean` target.

```text
cmake -G Ninja -B build-debug -DHL_DEBUG_LOGGING=1
cmake -G Ninja -B build-asan  -DHL_SANITIZE=ON
```

### 7.2 Debug logging

```text
cmake -G Ninja -B build-debug -DHL_DEBUG_LOGGING=1 && ninja -C build-debug
HL_LOG=log:fs,log:jit build-debug/bin/hl-engine-runner ...
```

### 7.3 Unit and behavior tests

```text
ctest --test-dir build -L unit --no-tests=error
ctest --test-dir build -L macos --no-tests=error      # Darwin host
ninja -C build format-check
```

Architectural ownership is enforced by the library build and link graph, not by tests that inspect
source text. The `macos` lane builds the mac host tests and executes them on a real mac host.

Some unit cases are host-CPU-conditional in a way the pass count does not show: they assemble host bytes on every
host but only *execute* them under a matching `#if defined(__aarch64__)` / `#elif defined(__x86_64__)`. Running the
lane on a second host CPU therefore buys real coverage without adding a test — worth remembering when reading a lane
that is green on one host and untried on the other.

**Always pass `--no-tests=error`.** `ctest -L <label>` exits 0 when the label matches nothing, so a
typo or a renamed label reports success. Invariant I15 (`tools/check_ci_workflows.sh`) rejects any
workflow step that omits it.

### 7.4 Linux-host production tests

These lanes run guests through the `build/linux-production` engines, so they need a host CPU those engines have a
backend for. Today that is **AArch64 only** — on an x86-64 Linux host the tests are registered (the guest fixtures and
the CTest registry are host-CPU-neutral, which is what `gate.ci-lane-parity` checks) but cannot pass until the x86-64
translator backends land. `production.smoke-x86_64` is the cheapest end-to-end proof and the milestone to watch;
`docs/amd64-host.md` §7 tracks it.

On a Linux/AArch64 host:

```text
ctest --test-dir build -L production --no-tests=error
ctest --test-dir build -L production-full --no-tests=error
```

The smoke tests prove production JIT entry, exact stdout, and guest exit status. The matrix runner
executes selected x86-64 Linux guests through the production engine and requires exact golden output,
exit status, timeout handling, and process-group cleanup. A zero exit with incorrect output is a
failure.

### 7.5 The compatibility matrix and the macOS lane

Each compat suite is one CTest case driving `tools/matrix-runner` over the whole suite manifest. The
suites are selected by label:

```text
compat-abi               compat-abi-corpus
compat-completeness      compat-libc
compat-core-abi          compat-core-syscall
compat-core-regress      compat-core-workload
compat-filesystem        compat-ipc
compat-isa-aarch64       compat-isa-x86-64
compat-isolation         compat-memory
compat-network           compat-posix
compat-process           compat-procfs
compat-signals           compat-soak
compat-syscall           compat-syscall-edges
compat-threads           compat-time
```

CTest does not build what a test needs, so build the lane first. Every suite has a narrow build
target, which is exactly what a CI shard uses:

```text
cmake --build build --target compat-engines compat-memory-fixtures
ctest --test-dir build -L '^compat-memory$' --no-tests=error
```

The runner launches the engine on the host it runs on (bridge `env`). Driving a macOS build from a
Linux host is a separate path: `cmake/toolchains/macos-remote.cmake`, or
`tools/run_remote_macos_ctest.sh`, which configures and runs CTest on the mac itself.

Production mac engines are compiled, linked, codesigned with the JIT entitlement, and actually
executed. A successful cross-compile is not a test pass.

The matrix runner maps the guest's `/tmp` to a per-case scratch directory taken from the build tree.
A few cases (`memfd-seals`, and anything asserting statx btime) require that scratch to be
tmpfs-backed. When the build tree lives on a disk filesystem, point the runner at a tmpfs:
`export HL_MATRIX_SCRATCH_DIR=/dev/shm`. CI mounts a tmpfs at `/mnt/hl-scratch` for exactly this
reason (`.github/workflows/linux.yml`).

The behavioral gate is the `e2e-mac` label: lifecycle identity, signal and control gates per guest
ISA, the three embedding bindings per ISA, and the bridge jobserver. The host firewall reliably
admits at most four signed launches from one launching process, so a manifest-only firewall audit
runs first in every gate (`cmake/RunSequence.cmake`, `cmake/Phase4Mac.cmake`) and keeps each subgate
inside that budget with distinct artifact identities; the sequencing is expressed as CTest fixtures
and `RESOURCE_LOCK`s. Local shared-host runs must verify no other `mac` workload is active. There is
deliberately no firewall retry.

Remote execution is supervised. Cancellation, timeout, or bridge loss terminates the remote process
group and reaps children. Validate the supervisor with `ctest --test-dir build -R
remote-supervisor --no-tests=error`.

### 7.5.1 CI lanes and what stops one from being dropped

`cmake/CiLanes.cmake` declares, per host, which lanes CI shards (`HL_CI_SHARDED_*`), which the main
job runs directly (`HL_CI_DIRECT_*`), and which exist but no workflow runs (`HL_CI_REGISTRY_*`).
It is the single source of truth, and these guards hang off it:

| guard | asserts |
|---|---|
| `gate.ci-lane-parity` (`cmake/LaneParity.cmake`, `tools/check_lane_parity.sh`; runs in the `unit` label) | every declared lane selects **at least one** test on this host |
| `unit.ci-workflow-invariants` I13/I14 | every sharded lane is named by **exactly one** workflow shard, and no shard names an undeclared lane |
| `unit.ci-workflow-invariants` I15 | every `ctest -L` step in a workflow passes `--no-tests=error` |
| `unit.ci-workflow-invariants` I19 | every sharded lane runs on every host in `HL_CI_COMPAT_HOSTS`, unless `HL_CI_SHARDED_HOST_ONLY` declares the asymmetry (and the exemption is not stale) |
| `unit.ci-workflow-invariants` I20 | a host **not** in `HL_CI_COMPAT_HOSTS` shards nothing, so its workflow may name no lane at all |

Adding a suite is one edit to `cmake/CiLanes.cmake` plus one shard entry. Forgetting either turns a
build red instead of silently dropping coverage.

**A host is an (OS, CPU) pair, not an OS.** `HL_CI_HOSTS` declares the tokens — `Linux-aarch64`,
`Linux-x86_64`, `Darwin-aarch64`, spelled `<CMAKE_SYSTEM_NAME>-<HL_HOST_ARCH>` — one workflow file each
(`linux.yml`, `linux-x86_64.yml`, `mac.yml`). `HL_CI_COMPAT_HOSTS` is the subset that runs guests and therefore shards
the compat matrix; `Linux-x86_64` is absent, which is what I20 turns into a hard failure if a compat shard shows up in
its workflow before the declaration does. `HL_CI_HOST_CPU_ONLY` (`<host-token>:<lane>`) exempts a lane that can only be
non-empty on some CPUs of its OS; it is empty today.

**A green lane is not necessarily an equal lane, and two are not:**

- `isa-fuzz` has three cases on an AArch64 host and **one** on x86-64. The aarch64 differential oracle *is* the ARM64
  host CPU executing the same binary, so `tests/fuzz/isa/aarch64/run.sh` refuses a non-ARM host and
  `isa-fuzz.aarch64-regress{,-pie}` are registered only there. On an x86-64 host the lane is
  `isa-fuzz.x86_64-regress` alone: **half the differential ISA fuzz coverage is host-conditional.** Do not read a green
  `isa-fuzz` on an x86-64 host as full fuzz coverage.
- `perf-native` runs the host's own ISA fixtures directly, as the baseline the `perf-linux` numbers are read against,
  so which eleven measurements it contains depends on the host CPU. It is registry-only on every host, and no
  thresholds are applied: a native number is a measurement, not a verdict.

`gate.ci-lane-parity` counts tests per lane. It cannot compare them, so neither asymmetry is machine-detectable —
which is exactly why both are written down here and at the declaration.

### 7.6 Installation and consumers

CMake is the single authoritative source list for development and packaging. Install the public headers,
portable archives, selected host-provider archive, generic runner, and generated `pkg-config` metadata with:

```text
cmake --install <build-dir> --prefix /usr/local
```

Packagers can prepend a staging root with `DESTDIR`; the path recorded in `hl-engine.pc` remains `PREFIX`. The
`uninstall` target removes only files owned by this package. The `package` lane installs into an isolated staging root, compiles a pure-C
consumer using only the installed include/library contract (the same flags emitted in `hl-engine.pc`, without requiring
the `pkg-config` program on build hosts), runs it against the real host provider, uninstalls, and verifies that its
public header was removed. Rust `build.rs` integrations consume this same installed C ABI and must not compile a second
implementation of engine behavior.

An embedder that wants a whole engine links `libhl-engine-activation.a` through `hl-engine-activation.pc`, which
force-loads it: that single archive carries both guest-ISA targets, the translator and the ABI. `hl-engine.pc` is the
smaller host/ABI contract. `libhl-translator.a` is a build component of the runner, the production engines and the
activation archive, and is deliberately not installed — off an AArch64 host it is not linkable on its own
(`cmake/Phase4Install.cmake`).

Production targets link common implementation from these archives instead of recompiling it inside each guest-ISA
target. In particular, the resident fork-server's bounded argument/environment codec, exact stream transfer, and
SCM_RIGHTS descriptor transport live in `libhl-linux-abi.a`; target roots retain only per-engine warm-image and JIT
state. The transport rejects truncated or malformed ancillary data and closes received descriptors before returning
an error. Linux `stat` and `statfs` byte layouts are likewise encoded once in `libhl-linux-abi.a` from explicit,
host-neutral records. Target code supplies ownership-virtualized native metadata but neither owns nor duplicates guest
structure layouts.

### 7.7 Prebuilt crate archives (`pkgs/rust`)

The published Rust crate does **not** compile `src/`. `pkgs/rust/build.rs` links a prebuilt static archive committed to
the repository — `pkgs/rust/assets/lib/aarch64-unknown-linux-gnu/libhl-engine.a` and
`pkgs/rust/assets/lib/aarch64-apple-darwin/libhl-engine.a` — and `cargo publish` ships exactly those bytes. A crate user
runs the engine that was compiled when the archives were last regenerated, not the engine in this tree.

**Published is narrower than supported, deliberately.** The crate accepts three hosts —
`aarch64-apple-darwin`, `aarch64-unknown-linux-gnu`, `x86_64-unknown-linux-gnu` — but commits archives for only the two
aarch64 ones. Each archive is ~24 MB against the 10 MB crates.io budget documented in `pkgs/rust/Cargo.toml`, so a third
is a publication decision that needs that budget solved first, not an automatic consequence of a host becoming
supported. On an x86-64 Linux host the archive is a **local build product**:

```text
cmake --build <build-dir> --target refresh-crate-archives-linux
```

`build.rs` prints exactly that command when the asset is missing. The failure a consumer sees is "supported host, no
archive in this tree, here is how to build it" — never "unsupported target", which is what it used to say and which sent
the reader looking for missing host support instead of a missing file. `refresh-crate-archives-linux` builds the
**native** archive for whatever Linux host CPU it runs on and files it under the matching triple;
`PROVENANCE.md` records only the two published archives, and `--provenance` refuses to run anywhere it could not have
produced them.

Consequently **every change to a C source or header under `src/` or `include/` requires regenerating both archives**,
in the same commit as the change. Skipping it ships an engine missing that change, and nothing about the crate build
reveals it: the archive still links, and a stale archive still launches guests. Releases 0.1.17, 0.1.18 and 0.1.26
shipped an archive 478 commits behind the headers (launch-config ABI 9 against ABI 13) and every guest launch failed
with `launch config has an invalid prefix`.

Regenerate both, and rewrite `pkgs/rust/assets/PROVENANCE.md`, with one command:

```text
cmake --build <build-dir> --target refresh-crate-archives
```

Regenerating the two **published** archives must run on an aarch64 Linux host, and it needs the mac for the darwin
half: the macOS archive is compiled by `clang` on Apple silicon, reached through the `MAC` bridge (`mac`, see 7.5) over
the shared `/Users/x/dd` checkout. On Darwin itself `MAC` is `env` and the command builds both locally. An x86-64 Linux
host can run `--linux` — it builds its own `x86_64-unknown-linux-gnu` archive, which nothing publishes — but not
`--provenance`, which certifies bytes only the aarch64 host can have produced and refuses elsewhere rather than
restating a stale digest under a fresh manifest.

When no single machine is both hosts, `tools/refresh_crate_archives.sh` takes `--linux`, `--darwin` and
`--provenance` and each half runs on its own machine (targets `refresh-crate-archives-{linux,darwin,provenance}`).
`--darwin --emit <file>` on a mac writes the archive out for transport; `--darwin --from <file>` on the other host
installs those bytes. `--provenance` hashes the published archives as committed and reruns the freshness check, so
either aarch64 host can record the result. See `pkgs/rust/assets/PROVENANCE.md` for the copy-pasteable sequence.

Two CI gates protect this, on Linux and on `publish`:

- `check-crate-archives` recomputes the `source-manifest` hash in `PROVENANCE.md` — a digest over every C source
  and header the archives are built from (`tools/crate_archive_manifest.sh`) — plus the SHA-256 of each committed archive,
  and fails with the regeneration command when the sources have moved on. This is the *currency* check, and it is pure
  hashing: seconds, no extra compilation. Both workflows invoke the `check-crate-archives` target, which runs
  `tools/check_crate_archives.sh`. The gate also structurally validates both archives. Repacking and force-loading
  the Mach-O archive needs a Darwin host, so the Linux runner does the host-independent part (ar container, member
  list, required symbols) and the macOS workflow runs the same gate for the link test. Requiring the Mach-O link on
  Linux previously made this step red on every commit regardless of freshness.
- The same gate reads the **build stamp** out of the archive bytes. Every object in a shipped archive force-includes a
  generated header (`tools/gen_archive_stamp.sh`, `cmake/ArchiveStamp.cmake`) holding
  `HL-ARCHIVE-SOURCE-MANIFEST:<digest>` for the sources it was compiled against, so the archive states its own
  provenance instead of PROVENANCE.md asserting it. The gate requires exactly one distinct stamp, equal to the recorded
  `source-manifest`, in every member. This is the *correspondence* check: the manifest and SHA-256 above are both
  satisfied by an archive built from other sources, which is what a refresh in an incremental build directory that
  failed to recompile a changed file produces — it validated as "current" while failing the crate's terminal test.
  A stale object keeps the older digest and the archive then carries two stamps. The stamp is `static`, hence invisible
  to `nm`; the gate matches the byte string, which no linkage choice can hide. It compares archive bytes against
  recorded text only, never against the working tree, so it is enforced on the push lanes as well
  (`--skip-freshness` defers currency, not this).
- `pkgs/rust/tests/packaged_archive.rs` launches a guest through the committed archive on both hosts. This is the
  *correctness* check.

Byte-comparing a CI rebuild against the committed archive is not used: the build is deterministic for a fixed
toolchain, but the compiler records the absolute checkout directory in each object, so the same commit built at a
different path produces different bytes. The manifest hash and the stamp have no such dependence.

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
ctest --test-dir <build-dir> -L perf-linux --no-tests=error   # or -L perf-native, -L perf-macos
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

A host platform is two axes (section 1), and they are added independently. Do not mix the two checklists: a new host OS
needs a service backend, a new host CPU needs code generation, and neither implies the other.

**Add a host OS** (`src/host/<os>/`, `hostBackends` in `flake.nix`):

1. Implement service groups without including guest or translator internals.
2. Pass public ABI validation and common provider tests.
3. Run the Linux production matrix with exact goldens.
4. Add fork, signal/fault, executable-memory, descriptor, and teardown stress.
5. Run the full cross-host compatibility suite.
6. Establish native and engine performance baselines.

**Add a host CPU** (`HL_HOST_CPU_*` in `src/host/host_cpu.h`, `HL_HOST_ARCH` in `CMakeLists.txt`, `hostCPUs` in
`flake.nix`). The host backend is host-CPU-neutral, so nothing in step 1 above repeats here; what is new is the
translator side (3.3) and the CI axis:

1. Name the CPU on all three axes above, and normalise the spelling once (`arm64`/`aarch64`, `amd64`/`x86_64`).
   Branch on `HL_HOST_CPU_*`, never on a bare compiler predefine.
2. Confirm the host backend and both guest fixture corpora build unchanged; the fixtures are cross-built and
   host-CPU-neutral.
3. Provide a guest→host code path for **each** guest ISA. Both production frontends emit ARM64 directly, so on any other
   host CPU each guest ISA needs its own backend; an interpreter first, then a transliterator or emitter, is the staging
   `docs/amd64-host.md` argues for.
4. Add the host token to `HL_CI_HOSTS` in `cmake/CiLanes.cmake` and give it one workflow file. Pass
   `gate.ci-lane-parity` on it — every declared lane must select at least one test there — using `HL_CI_HOST_CPU_ONLY`
   for a lane that genuinely cannot be non-empty, and stating any lane whose *contents* differ (see 7.5.1).
5. Cross-compile the other host CPUs' arms from this one, and have that be a CI step. Every host-CPU guard has one arm
   per CPU and a runner compiles only its own; the aarch64 arms are cross-compiled from the x86-64 job for exactly
   this reason.
6. Only once guests execute: add the token to `HL_CI_COMPAT_HOSTS`, shard the compat matrix on it, run the production
   matrix with exact goldens, and establish the `perf-native` / `perf-linux` pair as baselines. That is also the point
   at which the host becomes a release gate (`publish.yml`, invariant P1) — until then it proves the build, not the
   product.

## 12. Completion roadmap

This checklist tracks work required to reach the design above. It is intentionally separate from the normative rules;
items describe remaining proof or implementation, not accepted permanent exceptions.

Work is completed in this order:

1. **macOS production completion.** Transfer and separate the complete working macOS-hosted Linux engine, then pass
   every active dual-ISA compatibility, stress, lifecycle, and performance gate on the real mac host.
2. **Linux production completion.** Reuse the already-separated translator, Linux ABI, and host-service contract;
   finish the Linux backend and pass the same exact-golden corpus without changing guest semantics.

macOS work must not create macOS exceptions in portable code. Each transferred behavior is placed in its final owning
layer immediately so completing macOS reduces, rather than increases, the later Linux work.

### Portability and boundaries

- [ ] Remove the remaining unity-only dependencies from production target roots.
- [ ] Route every production host operation through a typed host-service group.
- [x] Remove ambient mapping access from the remaining guest ELF paths. Persistent-cache storage is fully routed
      through a pinned typed File-service directory.
- [x] Complete the Linux-host process, signal/fault, event, filesystem, and network production lanes. The permanent
      `test-linux-production-typed` gate (`ctest -L compat`) runs 1148 active exact-golden cases across 16 manifests
      and both guest ISAs, with no exclusions on the Linux lane. 64 cases carry `excluded-macos`, which runs them as
      active on the Linux engine and skips them only on the macOS engine; each is a Linux-only kernel behaviour and
      is justified case by case in docs/ci-green.md.

### Linux behavior

- [x] Run every active compatibility manifest on macOS for both guest ISAs. The only exclusions are explicitly
      unsupported guest-ISA inputs and the `excluded-macos` cases above.
- [x] Run the same exact-golden corpus through the production Linux host.
- [x] Complete native Linux epoll, timerfd, eventfd, inotify, signal-mask, dup/fork, and rearm coverage.
- [x] Complete typed directory, metadata, ownership, allocation, sparse-file, locking, and external-change coverage.
- [x] Complete process-group, wait, signal, exec, pid-namespace, proc/sys, and limit coverage.
- [x] Complete socket, ancillary, namespace, readiness, option, and error-fidelity coverage.

### Lifecycle and isolation

- [x] Repeat create/run/stop/destroy races under sanitizers where supported.
- [x] Prove runner-owned descriptor, thread, and direct child-process counts return to baseline after every
      compatibility case; the C matrix runner fails immediately on drift.
- [x] Reap independently discoverable remote descendants after both normal completion and transport cancellation;
      the C supervisor integration gate proves a child that outlives its engine cannot survive the launch group.
- [x] Extend lifecycle baselines to engine mappings. Private host probes count authoritative live mapping handles;
      macOS and Linux lifecycle runners require the count to return to zero after normal completion, injected host
      services, and forced-stop teardown for both guest ISAs.
- [x] Prove registry growth and reuse without stale aliasing. Native-capacity tests cross every dynamic registry's
      initial capacity on both hosts, then close, reuse, and reject stale generation handles for each independently
      allocated macOS kind and the shared Linux generation table. Timer and directory-watch token tables are reused
      after growth. The fixed sync registry reaches its exact 65,536-object limit, reports exhaustion, and safely
      reuses a slot with a new generation.
- [ ] Fault-inject every host callback boundary and verify transactional rollback.

### Translation and cache

- [x] Run the complete instruction, SMC, code-cache, fork, thread-churn, and persistent-cache corpus on every host.
- [x] Verify cache identity rejects incompatible builds, guest ISAs, host ISAs, and effective code-generation modes;
      x86 host-counter calibration runs before lookup, and the C identity gate mutates every key field.
- [x] Prove no translator path performs ambient host I/O. Translator units compile independently; identity and
      persistence use only typed host file services, while guest syscall names remain decoded guest operations.

### Performance and release

- [x] Establish reproducible native, mac-host, and Linux-host baseline distributions. The C performance runner emits
      cold, minimum, median, p90, p99, maximum, and mean with host identity; `perf-native-aarch64`, `perf-macos`, and
      `perf-linux` exercise release fixtures, both production guest ISAs where applicable, and bounded resources.
- [x] Enforce tracked cold and p99 thresholds for cold start, persistent-cache warm start, isolated translation,
      one-million-syscall execution, fork/exec stress, file/pipe/event IPC, and latency/throughput IPC on macOS and
      Linux for both guest ISAs. `tests/perf/resource.c` also bounds retained RSS growth to 2048 pages after
      teardown and requires descriptor and thread counts to return to their baseline.
- [x] Run extended synthetic and application-level workloads without leaks or unbounded growth. The synthetic soak
      repeats all 18 dual-ISA production cases ten times; the workload gate independently repeats all 18 supported
      compute, memory, code-cache, thread/fork churn, SMC, SQLite, and database-server cases ten times. Both require
      exact golden output and stable mapping, descriptor, and thread lifecycle counts after every repetition.
- [x] Verify release binaries contain no active debug logging and require no diagnostic environment flags.
- [x] Publish standalone C libraries, headers, host-provider archive, runner, pkg-config metadata, and a staged C
  consumer integration test.

The roadmap is complete only when evidence from the production engines proves every checkbox. Passing a narrower unit
test or finding matching source code is not sufficient.
