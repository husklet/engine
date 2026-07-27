# Windows host backend

The Win32/NT implementation of `hl_host_services`, built as `libhl-host-windows.a`.

Implemented and advertised: `memory` (including `CODE_MAPPING`), `clock`, `log`, `sync`. The remaining
groups are deliberately **not** advertised — a capability bit is set only when every callback of its group
is real, so `file`, `stream`, `event`, `counter`, `transfer`, `network`, `shared_memory`, `directory`,
`watch`, `process` and `posix_attachment` are absent rather than stubbed. Guest fork, epoll and signal
semantics remain owned by the Linux ABI rather than a host passthrough.

| File | Owns |
| --- | --- |
| `internal.h` | the host struct, the handle table types, the region model, the KernelBase entry points |
| `host.c` | handle table, Win32 error mapping, `log`, the `sync` bridge, create/destroy |
| `memory.c` | the whole memory group, over `VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2` placeholders |
| `clock.c` | the whole clock group |

`src/host/sync.c` is shared: its registry bookkeeping is host independent and only its primitive layer is
per-host, so this backend selects the `SRWLOCK` arm there rather than carrying a copy.
