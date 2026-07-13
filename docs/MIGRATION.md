# Implementation handoff

`src/production` is the complete transferred runtime. Its explicit end-to-end target is the behavioral oracle and
source pool for decomposition; the new default libraries remain independently compiled.

## Next work packages

1. Replace textual `.c` inclusion in the snapshot with private headers and object libraries without changing behavior.
2. Add macOS memory/JIT and clock host-service implementations, then migrate cache allocation/publication behind them.
3. Introduce instance-owned engine and Linux ABI state cluster by cluster; keep hot tables direct until assembly and
   performance comparison proves no regression.
4. Migrate syscall-independent control exits, memory operations, faults and safepoints into IR; add native/QEMU state
   differential tests for every instruction family.
5. Migrate fd/OFD and event behavior into `src/linux_abi`, using the imported epoll/eventfd/high-fd/fork fixtures.
6. Link the production runner against the migrated macOS backend and compare both guest architectures to the snapshot.
7. Implement host-linux only after the macOS backend proves the service seam. Do not add a guest-syscall passthrough.

## Required proof per package

- C behavioral tests through the public/domain API.
- Existing imported guest fixtures executed through old and new runners with the same result/output.
- `make format-check`, `make test`, and `make compat-build`.
- Linked size, startup, translation, pcache cold/warm and steady workload distributions for hot-path moves.
- No new platform token outside `src/host/<platform>`, runner packaging, or the not-yet-migrated `src/production`.

When a compatibility component is fully migrated, delete its snapshot copy in the same commit. Do not maintain two
semantic implementations or permanent fallback flags.
