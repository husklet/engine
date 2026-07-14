# Architecture contract

The only guest operating-system personality in the portable engine is Linux. Guest ISA and host CPU are independent
axes. Host operating-system behavior enters only through `hl_host_services`.

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
6. Process-global signal handlers, fork behavior, and `_exit` belong to the production runner boundary. Library
   interfaces must not acquire those responsibilities implicitly.
7. Logging calls use portable tags and an instance-owned `hl_log_context`. They are compiled out unless
   `HL_ENABLE_LOGGING=1`; host backends are byte sinks and never implement tag policy.

## IR rule

IR is private and versioned. Operands refer only to earlier SSA-like values; blocks end in an explicit terminator;
validation occurs before lowering. Persistent cache identity will include guest ISA, host ISA, IR ABI, codegen ABI
and every code-changing feature. Direct emitters and IR lowerers obey the same guest-state and cache-identity
contracts.

## Rebranding

All exported symbols, libraries and binaries use `hl`. Project-owned internal names use the same namespace when they
cross a file or process boundary. Linux ABI names such as `epoll` are compatibility contracts and are not rebranded.
