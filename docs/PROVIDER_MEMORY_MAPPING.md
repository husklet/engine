# Provider memory mapping boundary

This note records why launch allocation cannot yet be made guest-visible safely and the smallest
coherent implementation boundary. It supplements `CAP.md`; it does not advertise a capability.

## Current executable contract

`Memory::allocate` returns a `HostResource` containing an opaque `ResourceId`, region metadata,
transfer tokens, coherency, and inheritance policy. The engine validates allocation bounds, retains
the resource for the machine lifetime, and invokes `Memory::release` during rollback or teardown.
Discovery names this subset `memory-allocation`.

No current value gives the engine authority over storage:

- `Region` contains only offset, size, and protections. It has no bytes or backing capability.
- `TransferHandle(u64)` is an untyped integer. Treating it as a native descriptor would let a
  provider name any descriptor in the engine process and would make ownership, duplication, and
  close behavior ambiguous.
- `MemoryRequirement` has no guest address. The correct dynamic association is the existing
  `OpenHandle::map` result, which identifies a `ResourceId` and range.
- The provider service codec and native provider file adapter implement read, write, seek, stat,
  poll, and close, but not map.
- The activation ABI transfers only the provider channel. It has no bounded resource manifest or
  resource descriptors, and the child has no `ResourceId` to mapping-capability table.

Consequently, copying bytes into anonymous memory would not provide provider/guest coherence, while
casting a transfer token to a descriptor would violate least authority. Either shortcut would make
discovery untruthful.

## Required safe slice

Implement guest-visible mapping as one transaction spanning the existing narrow ports:

1. Replace the integer transfer token used for mappings with an owned, non-serializable host
   capability. On Unix this can own an `OwnedFd`; the provider API must expose only bounded
   duplication/export, never a raw guest fd. `HostResource` must associate each `Region` with one
   backing capability and a checked backing offset.
2. Validate resource ids for uniqueness per provider; region ordering, overlap, size, alignment,
   and protection subsets; backing lengths; sharing/coherency compatibility; total bytes; and the
   per-contract mapping limit before activation has side effects.
3. Extend the frozen provider protocol with a versioned map request/reply. The request carries the
   engine-copied offset, length, protections, sharing mode, credentials, deadline, and cancellation
   identity. The reply carries only `ResourceId`, resource offset, and length.
4. During activation, duplicate selected backing capabilities and send them with a bounded manifest
   over `SCM_RIGHTS`. Extend the activation request version instead of reusing its reserved field.
   The child adopts every descriptor into private host ownership and installs an immutable
   `(ProviderId, ResourceId, region) -> host handle` table before the first guest instruction.
5. Add a composite host memory adapter beside the provider file adapter. When Linux `mmap` targets
   a provider open-file description, call `OpenHandle::map`, validate its reply against the adopted
   resource table, and invoke the ordinary host `map_file`. Continue using the existing Linux VMA
   ledger for `mprotect`, `msync`, partial/full `munmap`, faults, and process teardown.
6. Keep the adopted backing descriptor alive until the last mapping and provider resource ownership
   are both gone. On launch failure, unmap and close in reverse order before calling provider
   release. Do not claim transfer, import, or fork inheritance in this slice.

## Conformance gate

The first discovery feature should be `memory-map-shared`, enabled only after a real generic test:

1. A provider allocates a page-backed owned descriptor and exposes a service whose `map` returns the
   allocated `ResourceId`.
2. A guest opens the projected service and maps it `MAP_SHARED` with read/write protections.
3. Provider bytes written before launch are read by the guest; a guest store plus `msync` is read
   through the provider's backing capability.
4. After guest `munmap` and machine exit, mapping, descriptor, open handle, and provider resource
   release counters each reach exactly one, with a zero-leak failure-path variant.
5. Oversized, out-of-range, overlapping, executable, private/shared-mismatched, stale-resource, and
   malformed protocol replies fail with typed Linux or launch errors before touching guest memory.

Until this gate passes, `memory-allocation` remains the only discovered memory feature. Import,
descriptor transfer, explicit coherency operations, and fork duplication/invalidation remain
undiscovered.
