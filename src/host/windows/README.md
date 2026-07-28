# Windows host backend

The Win32/NT implementation of `hl_host_services`, built as `libhl-host-windows.a`.

Implemented and advertised: `memory` (including `CODE_MAPPING`), `clock`, `log`, `sync`, `file`, `process`,
`counter`, `event` (including `EVENT_TIMER`), `stream`.
The remaining groups are deliberately **not** advertised — a capability bit is set only when every callback
of its group is real, so `transfer`, `network`, `shared_memory`, `directory`, `watch`, `terminal` and
`posix_attachment` are absent rather than stubbed. Guest fork and signal semantics remain owned by the Linux
ABI rather than a host passthrough.

| File | Owns |
| --- | --- |
| `internal.h` | the host struct, the handle table types, the region model, the KernelBase and ntdll entry points |
| `host.c` | handle table, Win32 error mapping, `log`, the `sync` bridge, create/destroy |
| `memory.c` | the whole memory group, over `VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2` placeholders |
| `fault.h` / `fault.c` | fault interception: the process-wide VEH, exception→fault classification, context accessors, resume, the `longjmp` replacement pad |
| `clock.c` | the whole clock group |
| `ntpath.c` | NTSTATUS mapping, the UTF-8/UTF-16 boundary, the pinned-root resolver, the symlink format |
| `file.c` | the whole file group, over `NtCreateFile` with `OBJECT_ATTRIBUTES.RootDirectory` |
| `process.c` | the whole process group, over `CreateProcess` plus a child-side bootstrap |
| `counter.c` | the whole counter group: a value plus a manual-reset event, and thread-pool subscriptions |
| `event.c` | the whole event group: a `WaitForMultipleObjects` pollset and waitable timers |
| `stream.c` | the whole stream group: overlapped named pipes with a standing zero-byte read |

`counter`, `event` and `stream` carry no typed refusal at all — every callback of each is real. Three
decisions in them are worth knowing before calling:

* the pollset is `WaitForMultipleObjects`, not IOCP. Its caller registers one constant interest per object
  and re-derives every object's readiness itself on each wake, so what it needs is a wakeup bus and an exact
  token; IOCP's scalability advantage is spent above this seam before it arrives. Past 63 waitable objects
  the set fans out to `CreateThreadpoolWait` and blocks on one aggregate event.
* a timer expiry always reports the caller's token verbatim with `HL_HOST_READY_TIMER` set. That exactness
  is load-bearing rather than tidy: a POSIX guest-timer drain discards any record failing either test,
  silently, so an approximate answer stops every guest timer without reporting an error.
* a stream is a named pipe, because an anonymous pipe cannot be opened overlapped and therefore cannot
  become *signalled* when data arrives — only be asked whether data is already there. The read end carries a
  standing zero-byte overlapped read whose event is what a pollset waits on. The write end is synchronous:
  write readiness is a quota question `NtQueryInformationFile` answers exactly, so a registered write end
  contributes no waitable handle and a full pipe draining does not by itself wake a pollset.

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

`fault.h` is the fault-interception seam and is **not** part of `hl_host_services`; it is a primitive the
fault path links against directly. One process-wide vectored handler is installed at engine init, classifies
an exception into a Linux-shaped `(signal, si_code, address, access)` tuple, and hands it to one installed
callback that mutates the `CONTEXT` and asks either to resume or to decline. `__try`/`__except` is not an
option on this toolchain — it compiles and segfaults — and no frame-scoped mechanism covers the fault sites
the engine actually has. Two things that follow and are easy to get wrong: `longjmp` out of the handler is
forbidden (Win64 implements it over `RtlUnwindEx`, which from inside a vectored handler means unwinding
through in-progress kernel dispatch), so `HL_WINDOWS_FAULT_PAD_ARM` plus a context edit replaces it; and a
**kernel** write into an inaccessible page raises nothing at all, so `hl_windows_fault_probe` must fault the
range in from user mode before any host path hands guest memory to a kernel call.
