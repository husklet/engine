# Architecture contract

The engine has one guest operating-system personality: Linux. Guest ISA and host CPU are independent axes. The
portable contract requires host operating-system behavior to enter through `hl_host_services`.

## Boundary rules

1. Translator code consumes guest bytes/state and produces or lowers validated IR. It cannot include platform headers,
   inspect paths, allocate native processes, or invoke host syscalls.
2. Linux ABI owns Linux numbers, structures, errno, guest pointers, fds/OFDs, pids, signals, `/proc`, `/sys`, event
   semantics and namespaces. Host services receive validated host buffers and opaque handles only.
3. Host backends own native handles and translate native failure into `hl_status`; raw errno/Mach/NT status never
   becomes a guest result without Linux-side operation-specific mapping.
4. Engine state is opaque and instance-owned. New code cannot add mutable process globals.
5. Public structures begin with `abi,size`, are append-only within an ABI, and use fixed-width types. No `pid_t`, native
   fd, compiler-sized enum or Rust layout leaks into the public boundary.
6. Process-global signal handlers, fork behavior, and `_exit` belong to the target runner boundary. Ordinary library
   interfaces must not acquire those responsibilities implicitly. The separately packaged embedded activation archive
   is an explicit runner boundary: its retained constructor recognizes only a capability-bearing reexec child and is
   otherwise dormant before application `main`.
7. Logging calls use portable tags and an instance-owned `hl_log_context`. They are compiled out unless
   `HL_ENABLE_LOGGING=1`; host backends are byte sinks and never implement tag policy.

## Rust package boundary

The Rust surface separates declarative launch data from live provider behavior:

- `hl-engine-api` owns backend-independent discovery, launch-policy, validation, control-data, and negotiated
  identifiers;
- `hl-engine-provider` owns live provider ports, requests, replies, authority, and lifecycle contracts and depends
  only on `hl-engine-api`;
- `hl-engine` owns native assets, backend lowering, machine lifecycle, and compatibility reexports.

Provider implementations do not depend on `hl-engine`. This keeps product policy and backend machinery out of
provider contracts and prevents the API/provider dependency cycle. Existing callers may continue to import both
planes through `hl_engine::extension`.

The native facade keeps composite models whose fields still carry live backend values. In particular, process domains,
terminal sizes, and virtual-network bridge operations retain their established native methods and error types in
`hl-engine`; the pure CPU, identity, filesystem, namespace, resource, security, time, observability, capability, and
validation values they compose live in `hl-engine-api`. This avoids either duplicating public types or smuggling FFI
callbacks and host filesystem operations into the API package.

## IR rule

IR is private and versioned. Operands refer only to earlier SSA-like values; blocks end in an explicit terminator;
validation occurs before lowering. Persistent cache identity will include guest ISA, host ISA, IR ABI, codegen ABI
and every code-changing feature. Direct emitters and IR lowerers obey the same guest-state and cache-identity
contracts.

## Current target boundary

`src/core/target/aarch64.c` and `src/core/target/x86_64.c` are the executable target roots. They still textually include
parts of the translator and Linux ABI and therefore compile as unity objects. Independently compiled core, translator,
Linux ABI, and host-service sources live in their corresponding archives. New shared behavior belongs in those
archives; shrinking the two target roots must preserve both guest ISAs and the public lifecycle contract.

macOS and Linux AArch64 are production hosts. Their packaged activation archives contain both Linux guest ISAs and are
executed end to end by the C and Rust behavioral gates. The unity roots still reach host mechanisms directly,
including Mach fault and process inspection, `kqueue`/`epoll`, JIT write protection, instruction-cache maintenance,
and parts of fork and signal handling. The translator cache receives the engine's bound services and routes code
mapping, publication, clocks, and persistence through their typed groups. Remaining direct host paths are
implementation debt, not exceptions to the boundary rules.

## Rebranding

All exported symbols, libraries and binaries use `hl`. Project-owned internal names use the same namespace when they
cross a file or process boundary. Linux ABI names such as `epoll` are compatibility contracts and are not rebranded.
