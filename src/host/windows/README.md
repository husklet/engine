# Windows host backend

The Win32/NT implementation of `hl_host_services`, built as `libhl-host-windows.a`.

Implemented and advertised: `memory` (including `CODE_MAPPING`), `clock`, `log`, `sync`, `file`, `process`.
The remaining groups are deliberately **not** advertised — a capability bit is set only when every callback
of its group is real, so `stream`, `event`, `counter`, `transfer`, `network`, `shared_memory`, `directory`,
`watch` and `posix_attachment` are absent rather than stubbed. Guest fork, epoll and signal semantics remain
owned by the Linux ABI rather than a host passthrough.

| File | Owns |
| --- | --- |
| `internal.h` | the host struct, the handle table types, the region model, the KernelBase and ntdll entry points |
| `host.c` | handle table, Win32 error mapping, `log`, the `sync` bridge, create/destroy |
| `memory.c` | the whole memory group, over `VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2` placeholders |
| `clock.c` | the whole clock group |
| `ntpath.c` | NTSTATUS mapping, the UTF-8/UTF-16 boundary, the pinned-root resolver, the symlink format |
| `file.c` | the whole file group, over `NtCreateFile` with `OBJECT_ATTRIBUTES.RootDirectory` |
| `process.c` | the whole process group, over `CreateProcess` plus a child-side bootstrap |

Two file-group callbacks return a typed `HL_STATUS_NOT_SUPPORTED`, and the group is still advertised
because both are genuine absences rather than gaps: `make_fifo`, because a named pipe is not a directory
entry on Windows and cannot be made into one, and `set_owner` for a real uid, because a guest uid has no
total mapping onto a SID. `set_owner(-1, -1)` — the encoding callers actually send — succeeds.

Guest symlinks are a tagged file payload (`!<symlink>` plus a UTF-8 BOM, on a `FILE_ATTRIBUTE_SYSTEM`
file) rather than a reparse point: native symlink creation fails `ERROR_PRIVILEGE_NOT_HELD` on an
unelevated process even with the unprivileged-create flag. `ntpath.c`'s resolver is what gives them link
behaviour, so anything that must see through a symlink has to go through it.

`process` is **launch**, not fork. `spawn_cloned` / `spawn_prepared` re-execute this image, hand the child
its entry point as an offset from the module base through an inherited pagefile section, and inherit exactly
the handles on a `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. No address-space clone is involved, because the child
of these two callbacks always cold-loads. The consequence is the one thing to know before calling them: a
`entry_context` that points into the parent's committed memory cannot exist in the child, so it is refused
at spawn time with `HL_STATUS_NOT_SUPPORTED` rather than crashing a child. A scalar carried in the pointer,
or `NULL`, crosses intact. Guest `fork(2)` is a separate problem with a separate primitive and is not served
here.

`terminate` maps `FORCE` (and `SIGNAL + SIGKILL`) onto `TerminateProcess`, and `INTERRUPT` (and
`SIGNAL + SIGINT`) onto `GenerateConsoleCtrlEvent(CTRL_C_EVENT)`; children are created in their own process
group so that event is addressable. Every other `SIGNAL + n` returns a typed `HL_STATUS_NOT_SUPPORTED` with
the signal number in `detail`: Windows cannot deliver a catchable signal to another process, and reporting
`TerminateProcess` as `SIGTERM` would be a lie the caller could not detect.

`src/host/sync.c` is shared: its registry bookkeeping is host independent and only its primitive layer is
per-host, so this backend selects the `SRWLOCK` arm there rather than carrying a copy.
