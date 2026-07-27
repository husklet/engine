# Prior art: how Cygwin implements `fork()`

Cygwin is the only long-lived, production-quality implementation of POSIX `fork()` on Windows. This
document records **how it actually works** — read from the source, not from the overview docs, several of
which are a decade out of date — what it costs, where it fails, and how much of it transfers to the
narrower problem the Windows host has.

Everything below was read from a clone of `github.com/cygwin/cygwin` at commit **`fa7b0cd`** (tip of
`master`, committed 2026-07-22). Paths are relative to `winsup/`. Line numbers are from that commit and
will drift; the claim next to each is what matters.

Where I could not establish something from the source or from a citable measurement, it says so. §7
collects the open questions.

---

## 0. The one-paragraph version

Cygwin does not clone the address space. It **re-executes its own image** with `CreateProcessW`, hands
the child a `child_info_fork` block through `STARTUPINFO.lpReserved2`, and then the two processes
**rebuild the child's address space by hand** over a four-signal handshake: the child pulls the cygheap,
the user heap, and the DLL/main-image `.data`/`.bss` out of the parent with `ReadProcessMemory`; the
parent pushes the child's **stack** in with `WriteProcessMemory`; private file mappings are re-created
and their pages read across; dynamically-loaded DLLs are re-`LoadLibrary`'d and coerced to the parent's
addresses. The child then resumes at the `fork()` call site via a `longjmp` into a `jmp_buf` the parent
saved before `CreateProcessW`. Everything that must land at a matching address — cygheap, user heap,
thread stacks, mmap arena — is placed in a **fixed 64-bit memory map** (`local_includes/memory_layout.h`)
precisely so the child can reserve it again.

It works. It is roughly two orders of magnitude slower than a Linux `fork()`, and it is not perfectly
reliable by construction — the project's own documentation says so.

---

## 1. The mechanism, step by step

### 1.1 The child process is created by `CreateProcessW`, re-executing the same image

There is no `RtlCloneUserProcess`, no `NtCreateUserProcess` clone, and no address-space inheritance
anywhere in the tree. A grep of the whole of `winsup/` for `RtlCloneUserProcess`, `NtCreateProcess`,
`NtCreateUserProcess` and `RtlCreateUserProcess` returns **nothing**. The only process creation call on
the fork path is:

```c
rc = CreateProcessW (forking_progname,      /* image to run */
                     GetCommandLineW (),    /* Take same space for command
                                               line as in parent to make
                                               sure child stack is allocated
                                               in the same memory location
                                               as in parent. */
                     sa, sa,
                     TRUE,                  /* inherit handles */
                     c_flags, NULL, 0, &si, &pi);
```
— `cygwin/fork.cc:381-394`

Three details in that call are load-bearing:

- **The image is the *parent's own* executable**, resolved through `dlls.main_executable->forkedntname()`
  (`fork.cc:369-373`) — which is either the original path or, on the second attempt, a hardlink in
  `/var/run/cygfork/` (§1.6).
- **`GetCommandLineW()` is passed verbatim**, not because the child parses it, but because the command
  line is copied into the child's initial stack allocation and its length therefore perturbs the layout.
  Cygwin keeps it byte-identical so the child's stack lands where the parent's did. (This matters less
  than it used to — see §1.4 — but the comment is still in the code.)
- **`bInheritHandles = TRUE`**, which is the entirety of the handle-table transfer for handles that are
  marked inheritable (§1.7).

The child is created suspended only when some fd type needs parent-side work first
(`fork.cc:285-286`, `408-412`); otherwise it runs immediately and blocks on the handshake.

### 1.2 The `child_info_fork` block and the handshake

`child_info_fork` (`cygwin/local_includes/child_info.h:108-123`) derives from `child_info`
(`child_info.h:48-104`) and is passed to the child in the *reserved* fields of `STARTUPINFO`:

```c
si.lpReserved2 = (LPBYTE) &ch;
si.cbReserved2 = sizeof (ch);
```
— `fork.cc:341-342`

The child recovers it in `get_cygwin_startup_info()` (`cygwin/dcrt0.cc:513-565`), validating an
"improbable string" (`PROC_MAGIC_GENERIC`), a layout checksum (`CURR_CHILD_INFO_MAGIC`,
`child_info.h:36`), and `sizeof (child_info_fork)` against its own — that is how a mismatched
`cygwin1.dll` is caught rather than crashing. The global that holds it is `child_proc_info`, declared
`NO_COPY` (`dcrt0.cc:389`), i.e. placed in `.data_cygwin_nocopy`, a section deliberately excluded from
the `.data` range that gets copied (`cygwin/cygwin.sc.in:44-57`, `cygwin/local_includes/winsup.h:19-21`).
That exclusion is what lets the child keep its *own* startup block while overwriting the rest of `.data`
with the parent's.

The block carries: the parent's process handle (duplicated inheritably with `PROCESS_VM_READ |
PROCESS_VM_OPERATION | PROCESS_DUP_HANDLE | ...`, `cygwin/sigproc.cc:932-940`), the `subproc_ready` and
`forker_finished` events, the `cygheap` base and high-water mark, the parent's stack geometry, the
signal mask, rlimits, and the `jmp_buf`.

The handshake is **two rounds**, four cross-process signals:

| # | Who | What |
|---|---|---|
| 1 | child | `SetEvent(subproc_ready)` then blocks on `forker_finished` — `fork.cc:139`, `62-92` |
| 2 | parent | wakes from `ch.sync()` (`fork.cc:426`), writes the child's **stack** and the linked-DLL `.data`/`.bss`, then `SetEvent(forker_finished)` — `fork.cc:498-530` |
| 3 | child | wakes on a now-parent-identical stack, runs the fixups (shm, DLL reload, fd table), `SetEvent(subproc_ready)`, blocks again — `fork.cc:178-189` |
| 4 | parent | wakes from the second `ch.sync()` (`fork.cc:531`), writes the loaded-DLL `.data`/`.bss`, attaches, `SetEvent(forker_finished)` — `fork.cc:541-574` |

`child_info::sync()` (`sigproc.cc:1124-1170`) waits on `{subproc_ready, hProcess}` so that a child that
dies instead of signalling is detected immediately and its exit code captured for the retry logic (§2).

The whole of `dofork` runs under `hold_everything` (`fork.cc:679`,
`cygwin/local_includes/sigproc.h:158-191`), which in a documented order takes the pthread lock (running
`pthread_atfork` prepare handlers and `__fp_lock_all()`, `thread.cc:2138-2151`), then blocks signal
processing, then the process lock. Around the copy itself the parent additionally holds `__malloc_lock()`
and `cygheap->lock()` (`fork.cc:346-347`). The `cygheap` lock is recent — commit `8a5d3952`,
2025-09-19, *"another thread may simultaneously be doing a cmalloc/cfree while the cygheap is being
copied to the child"* — which is a useful datum on its own: this class of bug was still being found
twenty-five years in.

**Cygwin does not suspend the parent's other threads.** `pthread::suspend_all_except_self()` exists
(`cygwin/local_includes/thread.h:467-470`) but its only caller is the SIGSTOP path in
`exceptions.cc:914`. The copy is therefore *not* atomic with respect to peer threads writing to
already-allocated memory; only the allocators and signal delivery are locked.

### 1.3 Which regions are copied, and by whom

This is the part the overview documentation gets wrong. `doc/highlights.xml:246-261` still describes the
parent filling in `.data`/`.bss` before running the child, and copying "its stack and heap into the
child" in one step. The code has not done that for a long time. The actual split:

| Region | Direction | Call site |
|---|---|---|
| **cygheap** (Cygwin's internal heap) | child **pulls** (`ReadProcessMemory`) | `mm/cygheap.cc:102-103` |
| **`cygwin1.dll` `.data` + `.bss`** | child pulls | `dcrt0.cc:581-585` |
| **user heap** (`sbrk` arena) | child pulls | `dcrt0.cc:581-585` |
| **main image `.data` + `.bss`** | child pulls | `dcrt0.cc:605-608` |
| **private (`MAP_PRIVATE`) mmap pages** | child pulls, page-run at a time | `mm/mmap.cc:1897-1903` |
| **the stack** | parent **pushes** (`WriteProcessMemory`) | `fork.cc:498-501` |
| **`_impure_ptr`** (when forking off a non-main thread) | parent pushes | `fork.cc:490-501` |
| **statically-linked DLLs' `.data` + `.bss`** | parent pushes | `fork.cc:514-526` |
| **dynamically-loaded DLLs' `.data` + `.bss`** | parent pushes, after the child has reloaded them | `fork.cc:541-560` |

`child_copy()` (`fork.cc:769-821`) is the single primitive; its second argument selects
`WriteProcessMemory` vs `ReadProcessMemory` and it `TerminateProcess`es the child on any short copy.

Everything else is **re-created, not copied**:

- **Shared (`MAP_SHARED`) mappings** are re-mapped from the inherited section handle, not copied
  (`mm/mmap.cc:1791-1808`, `1711`). The mapping is re-established at the parent's exact address with
  `NtMapViewOfSection(..., ViewShare, ...)` (`mm/mmap.cc:232-234`); a `base != address` result is a
  hard failure (`mm/mmap.cc:1712-1720`).
- **Private anonymous mappings** are re-`VirtualAlloc`'d at the parent's address, deliberately as
  `PAGE_READWRITE` *regardless of the eventual protection* so `ReadProcessMemory` can write into them,
  then re-protected to match `VirtualQueryEx` of the parent, page-run by page-run
  (`mm/mmap.cc:1700-1708`, `1851-1929`).
- **`PAGE_NOACCESS` and `PAGE_WRITECOPY` pages** need a dance: the parent's page is temporarily flipped
  to `PAGE_READONLY` with `VirtualProtectEx` so it can be read, then flipped back
  (`mm/mmap.cc:1879-1912`); a written-to `WRITECOPY` page reads back as `READWRITE`, which is not a legal
  protection to *set* on such a view, so it is shifted back to `WRITECOPY` by `<<= 1`
  (`mm/mmap.cc:1887-1895`). The comment above the copy is worth quoting because it explains why Cygwin
  gets no benefit from Windows COW at all:

  > `/* Copy-on-write pages must be copied to the child to circumvent a strange notion how copy-on-write
  > is supposed to work. */` — `mm/mmap.cc:1867-1868`

- **SysV shared memory** is re-created first, before anything else, because a duplicated socket can
  otherwise occupy the address a shm segment needs (`fork.cc:171-179`).
- **Cygwin's own helper threads** — the signal thread and the `cygthread` pool — are *not* cloned; the
  child creates fresh ones during its normal startup (`dcrt0.cc:737`, `767`), because the child really
  is running `dll_crt0_0()` from scratch.

### 1.4 The stack, and how execution resumes at the right point

This is the cleverest and most fragile part.

**Saving.** Before `CreateProcessW`, the parent records its stack geometry into the shared block — `Tib.StackBase`, `DeallocationStack`, and a `stacklimit` derived from the *current* stack pointer rounded down to a page (`fork.cc:305-331`) — so the child commits exactly the live part of the stack and no more. Then, in `dofork`:

```c
ischild = !!setjmp (grouped.ch.jmp);

volatile char * volatile stackp;
#if defined(__x86_64__)
  __asm__ volatile ("movq %%rsp,%0": "=r" (stackp));
#elif defined(__aarch64__)
  __asm__ volatile ("mov %0, sp" : "=r" (stackp));
#endif
```
— `fork.cc:699-708`

The `jmp_buf` lives *in the shared block* (`child_info_fork::jmp`, `child_info.h:112`), so it reaches
the child through `STARTUPINFO`, not through copied memory.

**Reserving.** The child, still on its own OS-assigned stack, calls `child_info_fork::alloc_stack()`
from `_dll_crt0` (`dcrt0.cc:1062`, implementation at `dcrt0.cc:392-466`). If its stack is not already at
the parent's address it `VirtualAlloc(MEM_RESERVE)`s the parent's whole stack range, commits
`[stacklimit, stackbase)`, and installs guard pages of the parent's `guardsize`
(`dcrt0.cc:405-439`). Failure here is `api_fatal` — no recovery.

**Jumping.** At the end of `dll_crt0_1`, with `__in_forkee == FORKING`, the child rewrites its own TEB to
the parent's stack values and jumps:

```c
PTEB teb = NtCurrentTeb ();
teb->Tib.StackBase  = (PVOID) fork_info->stackbase;
teb->Tib.StackLimit = (PVOID) fork_info->stacklimit;
teb->DeallocationStack = (fork_info->guardsize == (size_t) -1) ? NULL
                                                               : (PVOID) fork_info->stackaddr;
longjmp (fork_info->jmp, true);
```
— `dcrt0.cc:866-875`

**The trick.** At the instant of the `longjmp`, the child's copy of the parent's stack contains
*nothing* — it is freshly committed, zero-filled memory. The frame `dofork` is about to resume into,
including the entire `frok grouped` object, is zeros. This works because the very first thing
`frok::child()` does is signal the parent and block:

```c
int frok::child (volatile char * volatile here)
{
  HANDLE& hParent = ch.parent;
  sync_with_parent ("after longjmp", true);
```
— `fork.cc:135-139`

`sync_with_parent` reads `fork_info->forker_finished` (from the *uncopied* startup block) and waits
(`fork.cc:66-73`). While the child is parked there, the parent runs `child_copy(hchild, /*write=*/true,
"stack", stack_here, ch.stackbase, ...)` (`fork.cc:498-501`) and overwrites `[stack_here, stackbase)` —
which is `dofork`'s frame and everything above it, but **not** the deeper frames of `frok::child` and
`sync_with_parent`, which live below `stack_here`. When the child wakes, its stack *is* the parent's,
`hParent` (a reference to an address in the rewritten region) now reads the real parent handle, and
execution continues as if `fork()` had returned.

The direct evidence that this is deliberate is in `dofork`:

```c
res = grouped.child (stackp);
...
__in_forkee = FORKED;
ischild = true;	/* might have been reset by fork mem copy */
```
— `fork.cc:714-721`

`ischild` is a `dofork` local, therefore inside the copied range, therefore clobbered with the parent's
`false`. It has to be re-asserted by hand. Similarly, the `WAIT_FAILED` branch of `sync_with_parent`
re-reads `fork_info->forker_finished` and retries the wait, because the handle value it captured may have
been invalidated underneath it (`fork.cc:80-86`).

**Where the stack lives.** Since Windows 10 1511, Cygwin no longer trusts the OS-assigned main-thread
stack at all. `_dll_crt0` moves the main thread onto a stack carved from Cygwin's own pthread stack arena
before anything else runs:

> `/* Starting with Windows 10 rel 1511, the main stack of an application is not reproducible if a 64 bit
> process has been started from a 32 bit process. ... we now always move the main thread stack to the
> stack area reserved for pthread stacks. This allows a reproducible stack space under our own control
> and avoids collision with the OS. */` — `dcrt0.cc:1021-1026`

The move is done in inline asm (`dcrt0.cc:1037-1054`) followed by `VirtualFree` of the OS stack;
allocation is `create_new_main_thread_stack()` (`cygwin/create_posix_thread.cc:236-271`), which draws from
`THREAD_STORAGE_LOW..HIGH` = `0x600000000..0x800000000` via `VirtualAlloc2` with a
`MEM_ADDRESS_REQUIREMENTS` extended parameter (`create_posix_thread.cc:150-172`). **This is Cygwin
deciding that determinism is worth more than whatever the OS chose** — and it is the single most
directly transferable idea in this document.

### 1.5 The heap, and why it has a fixed base

`user_heap_info::init()` (`mm/heap.cc:57-179`) has two arms selected by whether `base` is already set —
which it is exactly when the struct came from the parent's cygheap:

```c
/* If we're the forkee, we must allocate the heap at exactly the same place
   as our parent.  If not, we (almost) don't care where it ends up.  */
```
— `mm/heap.cc:60-62`

Fresh: reserve at `USERHEAP_START` = `0xa00000000` (`memory_layout.h:47`), falling back to a scan for the
first free region large enough and finally to *any* address (`mm/heap.cc:66-125`). Forkee: reserve at
the inherited `base` in a loop that shrinks the reservation until it fits, then commit what the parent
had committed, and **fail hard if the address is not exact**:

```c
if (p != base)
  api_fatal ("heap allocated at wrong address %p (mapped) != %p (expected)", p, base);
```
— `mm/heap.cc:162-164`

The cygheap is the same story at a different address: reserve the whole
`CYGHEAP_STORAGE_LOW..HIGH` = `0x800000000..0xa00000000` region, commit up to the parent's high-water
mark, `ReadProcessMemory` the contents (`mm/cygheap.cc:87-103`).

The reason Cygwin has its own heap rather than using `HeapAlloc` is exactly this: the process heap's
address is the OS's choice and cannot be reproduced. The whole fixed layout is written down in one place:

```
EXECUTABLE_ADDRESS          0x100400000    CYGWIN_DLL_ADDRESS          0x180040000
SHARED_REGIONS  0x1a0000000..0x200000000   REBASED_DLL_STORAGE  0x200000000..0x400000000
AUTOBASED_DLL   0x400000000..0x600000000   THREAD_STORAGE       0x600000000..0x800000000
CYGHEAP_STORAGE 0x800000000..0xa00000000   USERHEAP_START              0xa00000000
MMAP_STORAGE  0x001000000000..0x700000000000  (grows down; user heap grows up)
```
— `local_includes/memory_layout.h:12-55`

The header was created specifically to *enable* ASLR on the Cygwin DLL — commit `c0776fa7`, 2022-10-26,
"This is to prepare for ASLR support" — and the two commits either side of it moved the cygheap and the
shared memory regions out from behind the DLL for the same reason (`2f9b8ff0`: *"One reason that ASLR is
tricky is the fact that the cygheap is placed at the end of the DLL"*; `60675f1a`). `cygwin1.dll` has
been linked `--dynamicbase --disable-high-entropy-va` since commit `943433b0`, 2022-10-28
(`cygwin/Makefile.am:623`). So the modern position is: **the DLL may float, but everything the fork has
to reproduce must not.**

### 1.6 DLLs, and why `rebaseall` exists

A Windows DLL is not position-independent in the ELF sense: it has a preferred base, and if two DLLs'
preferred ranges collide the loader relocates one. Cygwin's constraint is that every module must sit at
the *same* address in parent and child, because copied `.data` contains pointers into them.

- **Statically linked DLLs** are resolved by the loader before `cygwin1.dll` gets control. Cygwin cannot
  fix them after the fact. `doc/highlights.xml:296-309` is explicit: *"collisions among statically-linked
  dlls ... are resolved before `cygwin1.dll` initializes and cannot be fixed afterward. This problem can
  only be solved by removing the base address conflicts which cause the problem, usually using the
  `rebaseall` tool."* That is the entire reason `rebaseall` exists: it rewrites every Cygwin DLL's
  preferred base into a contiguous non-overlapping layout in `REBASED_DLL_STORAGE`
  (`memory_layout.h:23-26`), so no collision arises and no relocation is needed.
- **Dynamically loaded DLLs** Cygwin *can* fix, and the machinery is elaborate. In the child, before any
  dynamic allocation, `dlls.reserve_space()` `VirtualAlloc(MEM_RESERVE)`s every loaded DLL's range to
  stop anything else taking it (`dll_init.cc:689-696`). It was moved earlier for exactly this reason —
  commit `023c107a`, 2019-03-26: *"threadinterface->Init and sigproc_init allocate windows object handles
  using unpredictable memory regions, which may collide with dynamically loaded dlls."* Then
  `load_after_fork_impl()` (`dll_init.cc:719-828`) does, per DLL: release the reservation,
  `LoadLibraryExW(..., DONT_RESOLVE_DLL_REFERENCES)` as a probe, and if it lands wrong, `FreeLibrary`,
  `VirtualAlloc`-reserve the *wrong* address so it cannot be chosen again, and **recurse** — up to
  `DLL_RETRY_MAX = 6` levels, unwinding the blocking reservations on the way out
  (`dll_init.cc:756-779`). If it still misses:

  ```c
  fabort ("unable to remap %W (using %W) to same address as parent (%p) - try running rebaseall", ...);
  ```
  — `dll_init.cc:771-772`

- **The `/var/run/cygfork` hardlink scheme** (`cygwin/forkable.cc`, `cygwin/local_includes/dll_init.h:149-160`)
  is a *different* problem with the same shape. `fork()` retries the whole operation once with
  "forkables" enabled (`fork.cc:610-618`); on that attempt the child image and every DLL are loaded
  through per-user, per-executable, per-generation NTFS hardlinks in `/var/run/cygfork/`, plus a
  `.local` file to trigger DotLocal DLL redirection (`forkable.cc:311-323`, `676-731`). Commit
  `ece7282f`, 2016-12-07, gives the motive: *"To support in-cygwin package managers, the fork()
  implementation must not rely on .exe and .dll files to stay in their original location, as the package
  manager's job is to replace these files."* The directory must be created by hand and must be NTFS,
  or the feature silently disables itself (`forkable.cc:465-508`, `doc/highlights.xml:204-223`).

### 1.7 Handles

Handle transfer is two mechanisms, not one:

1. **Bulk inheritance.** `CreateProcessW(..., bInheritHandles = TRUE, ...)` (`fork.cc:389`). Every
   handle currently marked inheritable appears in the child at the *same numeric value*. For fds this is
   the normal case, since Cygwin sets inheritance from the `close_on_exec` flag
   (`fhandler/base.cc:1656-1663`).
2. **Explicit duplication for the rest.** In the child, `dtable::fixup_after_fork()`
   (`cygwin/dtable.cc:933-959`) walks the fd table and, for every fd that is `close_on_exec` or otherwise
   needs work, calls `fhandler_base::fork_fixup()`, which pulls the handle across with `DuplicateHandle`
   from the parent's process handle:

   ```c
   else if (!DuplicateHandle (parent, h, GetCurrentProcess (), &h,
                              0, !close_on_exec (), DUPLICATE_SAME_ACCESS))
   ```
   — `fhandler/base.cc:1644-1645`

   This is why the parent handle in `child_info` carries `PROCESS_DUP_HANDLE` for forks specifically
   (`sigproc.cc:935-936`).

Some fd types (currently only sockets) need work in the parent *between* `CreateProcessW` and the memory
copy; that is the only reason the child is ever started suspended (`fork.cc:281-286`, `408-412`,
`dtable.cc:1104-1112`).

Note what is *not* replicated: the child's handle table is not made numerically identical to the
parent's in general. It is identical for inheritable handles and reconstructed for the rest; Cygwin's own
bookkeeping lives in the cygheap, which is copied, so the *logical* table survives even where the raw
numbers would not.

### 1.8 The fixups the child runs afterwards

For completeness, since this is the part our code most directly parallels:

- `_pei386_runtime_relocator()` — pseudo-relocations re-applied, explicitly because read-only sections
  are not copied (`dcrt0.cc:598-601`).
- `fixup_shms_after_fork()`, then `dlls.load_after_fork()`, then `cygheap->fdtab.fixup_after_fork()` — in
  that order, and the ordering is commented as a real bug fix (`fork.cc:171-184`).
- `_cygtls::fixup_after_fork()` — clears per-thread event handles, timers, select state
  (`cygwin/cygtls.cc:78-92`).
- `fixup_hooks_after_fork()` — re-patches IAT hooks (`cygwin/hookapi.cc:458-462`).
- `pthread::atforkchild()` → `MTinterface::fixup_after_fork()` (`cygwin/thread.cc:345-360`), which
  re-creates the Win32 object behind every mutex, condvar, rwlock and semaphore
  (`thread.cc:1926-1943`, `2691-2703`) and marks every *other* pthread invalid, since it does not exist in
  the child (`thread.cc:1096-1108`).

---

## 2. ASLR and the "fork retry" problem

### The failure mode

Every step in §1.3–§1.6 that says "at the parent's address" is a place the child can lose. The child
must obtain, at exact addresses chosen by the parent:

- the cygheap (`0x800000000`), the user heap (`0xa00000000`), the main thread's stack, every pthread
  stack, every mmap'd region, and every loaded DLL.

Anything already occupying one of those ranges in the child — because Windows placed an ASLR'd module
there, because a security product injected a DLL, or because the child's own startup allocated there
before the reservation ran — is fatal. `doc/highlights.xml:285-333` names the three causes: DLL base
collisions, ASLR of stacks/heaps/mapped files/static DLLs, and DLL injection by "BLODA" (badly-behaved
antivirus and similar). Its conclusion is the project's official position and I will not soften it:

> *"In summary, current Windows implementations make it impossible to implement a perfectly reliable
> fork, and occasional fork failures are inevitable."* — `doc/highlights.xml:330-333`

### The retry loop

There are **three independent retry mechanisms**, and it is worth not confusing them:

1. **Per-DLL retry, in the child.** The recursive reserve-and-retry in `load_after_fork_impl`, bounded at
   `DLL_RETRY_MAX = 6` (`dll_init.cc:719`, `768-772`). Always active.
2. **Whole-fork retry with hardlinks.** `fork()` calls `dofork` once with `with_forkables = false`; if
   that returns negative it calls it again with `with_forkables = true` (`fork.cc:607-618`). Exactly one
   extra attempt, and only useful if `/var/run/cygfork/` exists on NTFS.
3. **`proc_retry`, the `CreateProcessW` retry loop.** `frok::parent` is a `while (1)` around
   `CreateProcessW` + `ch.sync` (`fork.cc:366-435`). When the child dies instead of signalling,
   `frok::error()` calls `child_info::proc_retry()` (`sigproc.cc:1172-1214`), which decrements a counter
   and zeroes `exit_code` for *recoverable* statuses — `STATUS_DLL_INIT_FAILED`,
   `STATUS_CONTROL_C_EXIT`, and Cygwin's own `EXITCODE_RETRY`; a zero `exit_code` makes `error()` return
   `false` and the loop `continue`s. Statuses that are *not* retried include
   `STATUS_ACCESS_VIOLATION`, `STATUS_DLL_NOT_FOUND` and `STATUS_ILLEGAL_INSTRUCTION`
   (`sigproc.cc:1183-1187`). The child requests a retry by killing itself:

   ```c
   if (retry > 0)
     TerminateProcess (GetCurrentProcess (), EXITCODE_RETRY);
   ```
   — `sigproc.cc:1216-1232` (`child_info_fork::abort`)

   whose only callers are the heap-at-wrong-address path (`mm/heap.cc:157`) and every `fabort` in
   `dll_init.cc` (`dll_init.cc:30`).

**A finding worth flagging.** `child_info::retry_count` is a static initialised to **0**
(`sigproc.cc:893`) and is assigned in exactly one place: `set_proc_retry()`, driven from the
`CYGWIN=proc_retry:N` environment option (`environ.cc:72-76`, table entry `environ.cc:122`).
`parse_options()` only invokes a setting's handler when the keyword is *present* in `$CYGWIN`
(`environ.cc:203-226`). On my reading, **mechanism 3 is therefore inert in a stock environment**: the
retry count is zero, `child_info_fork::abort` does not `EXITCODE_RETRY`, and a heap or DLL placement
failure becomes an `api_fatal` or an `EAGAIN` from `fork()` on the first try. The documentation's
"though it will retry a few times automatically" (`doc/highlights.xml:311-315`) and the `proc_retry`
description in `doc/cygwinenv.xml:76-82` (framed around *"errors [that] usually occur when processes are
being started while a user is logging off"*, not address-space collisions) are both consistent with this
but neither states it. **I did not run Cygwin to confirm.** If we are going to lean on "Cygwin retries
and it mostly works", this is the assumption to check first.

### How modern Windows changes the picture

- **High-entropy VA is explicitly disabled** on the Cygwin DLL (`--disable-high-entropy-va`,
  `Makefile.am:623`), keeping images in the low 4 GB region where the fixed layout has room.
- **`VirtualAlloc2` / `NtMapViewOfSectionEx` with `MEM_ADDRESS_REQUIREMENTS`** are used where available
  (`wincap.has_extended_mem_api()`) to constrain allocations into the fixed arenas rather than hoping
  (`create_posix_thread.cc:150-172`, `mm/mmap.cc:191-213`). This is a genuine improvement over the
  old scan-and-pray loops and is available unconditionally on Windows 11.
- **But not during fork.** `MapView()` refuses the extended API on the fork path:

  > `/* Don't call NtMapViewOfSectionEx during fork. It requires autoloading a function under loader
  > lock (STATUS_DLL_INIT_FAILED). */` — `mm/mmap.cc:189-191`

  A real constraint we will hit too: **anything reached through a lazy-import thunk is unsafe in the
  child.** Resolve every NT entry point at startup, in the parent.

### Observed reliability

I could not find a quantitative failure-rate measurement — no per-fork failure probability, no
long-running study. What exists is qualitative and consistent:

- Cygwin's own documentation says perfect reliability is impossible (above).
- The failure is common enough that Chromium shipped a document titled *"Handling repeated failures of
  rebaseall to allow cygwin remaps"*.
- The user-visible symptoms (`fork: Resource temporarily unavailable`, `unable to remap ... - try running
  rebaseall`, `*** recreate_mmaps_after_fork_failed`) recur on the mailing list across two decades.

The honest summary: **on a clean machine with `rebaseall` applied and no injected DLLs it works well
enough to run bash, make and gcc all day; on a machine with security software injecting into every
process it can fail persistently and the only fix is to remove the offending software.** That is a
reliability profile we would not accept for a production engine.

---

## 3. Cost

### What I found

I could not find a clean, isolated `fork()`-only microbenchmark for Cygwin from a citable source. The
best real numbers are from the December 2020 – January 2021 `cygwin-apps` thread *"Optimising cygwin fork
performance"*, using the `fork-benchmark` tool, which measures **fork + exec + process teardown of 1000
processes**:

| Reporter | Machine | Result |
|---|---|---|
| Marco Atzeri | i5-8250U, Win10, bare metal | ~40 s, degrading to ~62 s over repeated runs |
| Brian Inglis | AMD A10-9700, after cygserver tuning | ~33–35 s, stable |
| Hamish McIntyre-Bhatty | after moving VirtualBox → KVM | ~13 s |

That is **13–62 ms per fork+exec+exit**. A Linux `fork()` + `_exit()` + `wait()` on comparable hardware
is on the order of 50–300 µs. So the order of magnitude is **~50–500×**, and even the best Cygwin figure
is two orders of magnitude off. A blog post (`hamishmb.com`, *"Adventures in Cygwin's fork performance"*)
reports a direct Linux-vs-Cygwin comparison of the same benchmark at 0.23 s vs ~13 s; **I could not fetch
that page directly (HTTP 403) and am relaying it from a search summary — treat it as indicative only.**

### Why it costs what it does, from the code

The cost is structural and each part is visible above:

- A full `CreateProcessW`: a new process object, a new image mapping, a full ntdll loader run, every
  statically linked DLL loaded and initialised — before any Cygwin code executes.
- **Four cross-process context switches** on the critical path (§1.2), each a scheduler round trip.
- **Every dirty page copied eagerly**, through `ReadProcessMemory`/`WriteProcessMemory` — user heap,
  cygheap, `.data`/`.bss` of the image and every DLL, the live stack, and every private mmap page. No
  copy-on-write anywhere; the code explicitly circumvents Windows COW (`mm/mmap.cc:1867-1868`). Cost is
  **linear in the parent's dirty footprint**, which for us would be the entire guest.
- Every dynamically loaded DLL re-`LoadLibrary`'d, sometimes several times through the retry recursion.
- Per-mmap-region `VirtualQueryEx`/`VirtualProtectEx`/`VirtualProtect` round trips.

`fork.cc:264-271` also notes that Cygwin's own `spawn` family, which maps onto Win32 cleanly, was
measured to make compilation "twenty to thirty percent" faster than fork+exec
(`doc/highlights.xml:263-272`) — the project's own recommendation is to avoid fork wherever the caller
can.

---

## 4. What Cygwin's fork cannot do

Established from the source:

- **No `PTHREAD_PROCESS_SHARED` synchronisation at all.** `pthread_mutexattr_setpshared` returns
  `EINVAL` for anything but `PTHREAD_PROCESS_PRIVATE` (`thread.cc:4030-4042`); the same for condvars
  (`4275-4285`), rwlocks (`4505-4515`). And the fork fixup `api_fatal`s if it ever sees one:
  ```c
  if (pshared != PTHREAD_PROCESS_PRIVATE)
    api_fatal ("pthread_mutex::_fixup_after_fork () doesn't understand PROCESS_SHARED mutex's");
  ```
  — `thread.cc:1926-1930`

  **This is the single feature we depend on that Cygwin does not have.** `src/linux_abi/thread.c:148-159`
  documents that hl's cross-process futex table is a `MAP_SHARED` region with
  `PTHREAD_PROCESS_SHARED` mutexes and condvars, created once before any guest fork.

- **Only the calling thread survives**, like POSIX. All other `pthread` objects are marked invalid
  (`thread.cc:1096-1108`).
- **No `vfork`.** `vfork()` is `return fork()` (`fork.cc:760-765`).
- **The copy is not atomic w.r.t. peer threads.** Only the allocators, signals and the process lock are
  held (§1.2); a peer thread writing to already-allocated memory during the copy produces a torn child.
  The 2025 cygheap-lock commit shows this class is still live.
- **Failures below the retry line are `api_fatal`, not `errno`.** Stack reservation
  (`dcrt0.cc:407-412`), mmap re-creation (`dcrt0.cc:610-611`), pthread object re-creation
  (`thread.cc:1939-1941`) all kill the process rather than failing the call.
- **Binaries must remain at their original paths** across a fork unless the hardlink scheme is enabled
  (`doc/highlights.xml:196-223`).
- **`fork()` is not cheap enough to be a design primitive** (§3), which is why Cygwin implements
  `posix_spawn` on a native path (`fork.cc:639-655`).

---

## 5. Applicability to us

### 5.1 What our problem actually is

Two different things in the engine are called "fork", and they have very different requirements. Getting
this distinction right is most of the design.

**(a) The isolation clone — `spawn_cloned` / `spawn_prepared`.**
`include/hl/host_services.h:444-459`. `hl_production_start_process` (`src/core/lifecycle.c:100-146`)
forks and the child then runs `hl_production_entry` → `hl_run_linux_guest(...)`
(`src/core/lifecycle.c:76-89`), which **loads the guest from scratch in the child**. At the moment of
this fork there is no guest address space, no JIT arena, and no guest threads. What must survive is the
*engine's* state: the entry context, the `hl_linux_abi` fd/OFD table, the host handle registry, and a
shared result page.

**(b) The guest's own `fork(2)`/`clone(2)`.**
`src/linux_abi/syscall/proc.c:1823` (and `:2528` for `clone3`) call POSIX `fork()` **directly** — not
through `hl_host_services`. This one is a genuine, arbitrary-point address-space clone: the guest may
have gigabytes mapped, the JIT arena is live and dual-mapped, engine threads may be mid-translation, and
the child must resume at the exact instruction after the syscall with identical memory.

Note that (b) is not currently behind a host seam at all. **Adding one is a prerequisite for the
Windows port and is a change to `src/linux_abi/`, not just `src/host/windows/`.**

### 5.2 What transfers from Cygwin

**Transfers directly:**

1. **The fixed memory map.** `memory_layout.h` is the right idea and we should copy the *shape* of it:
   one header, absolute constants, every fork-critical arena inside a reserved region. We already have
   the seed — `HL_LINUX_SNAPSHOT_BASE = 0x50000000000` (`src/linux_abi/container/snapshot.h:6`) and the
   deterministic bump allocator in `src/linux_abi/container/snapshot.c:21-30`, built for checkpoint
   restore, which needs exactly the same property fork does.
2. **Moving the main thread onto our own stack.** `dcrt0.cc:1021-1059`. Trading an OS-chosen stack for a
   reproducible one is unambiguously correct for us too.
3. **Resolve every NT entry point eagerly.** `mm/mmap.cc:189-191`. Lazy-import thunks are unsafe in a
   forked/cloned child.
4. **The child-side fixup discipline.** Cygwin's `fixup_after_fork` family and our
   `fork_child_hooks` (`src/linux_abi/syscall/proc.c:267-330`) are the same design already: reinitialise
   every process-private lock, re-create every kernel object that does not survive, drop every cache
   that referred to the parent. Our version is if anything more thorough.
5. **The two-phase validate-then-commit fork plan.** `hl_linux_abi_fork_prepare` (`:286`), `_parent`
   (`:442`) and `_child` (`:494`) in `src/linux_abi/linux_abi.c` already do what Cygwin does with
   `fixup_before_fork` / `fixup_after_fork`, and do it better: phase one of `_child`
   (`linux_abi.c:520-539`) validates every record and pre-allocates every replacement lock, so phase two
   (`:540-556`) cannot fail. Cygwin's equivalents `api_fatal` instead.

**Should be deliberately *not* copied:**

1. **The `CreateProcessW` re-exec plus `WriteProcessMemory` transfer, for case (b).** Cost is linear in
   the parent's dirty footprint. For a guest with a 2 GB heap this is not a slow fork, it is a
   non-starter. Windows *does* give us a COW address-space clone; Cygwin cannot use it (see §5.3) but we
   probably can.
2. **The `longjmp`-onto-an-empty-stack trick.** It exists only because the child is a *different image
   run from scratch*. A real clone resumes on the actual stack and needs none of it.
3. **DLL rebasing and the hardlink scheme.** These are consequences of re-executing the image. They
   disappear entirely if the child is a clone.
4. **`api_fatal` on placement failure.** Our `spawn_*` returns `hl_host_result`; a placement collision
   must be an `HL_STATUS_*`, not a process kill.
5. **Copying memory the child is going to discard.** For case (a) the child re-loads the guest anyway.

### 5.3 Is `RtlCloneUserProcess` viable for us?

Cygwin does not use it. The reason is not that it does not work — it is that Cygwin cannot use it. A
Cygwin process is an arbitrary user program linked against arbitrary Windows DLLs, and `fork()` must
work for all of them. `RtlCloneUserProcess`'s constraints are exactly the ones that arbitrary programs
violate. Kaz Kylheku put it on the `cygwin` list in January 2018: *"ntdll.dll does not know about Cygwin
fork, unlike its own fork"*, and proposed making it a run-time-switchable experimental mode; nothing
came of it.

**What it actually does** (source: Hunt & Hackett, *The Definitive Guide To Process Cloning on Windows*,
`github.com/huntandhackett/process-cloning`, read at `Readme.md:104-252`; corroborated in part by Bill
Demirkapi's write-up. **I have not yet verified any of this on this machine — see §7.**):

- It wraps `NtCreateUserProcess` called with **no image filename and no process parameters**, which is
  what selects clone mode (`Readme.md:104`, `114-134`).
- The child **receives a replica of the parent's address space**: all private pages duplicated
  copy-on-write, and most mapped regions inherited (`Readme.md:104`, `354`).
- It **clones the calling thread only**, which resumes *at the instruction after the syscall*,
  distinguished by the return status `STATUS_PROCESS_CLONED` (0x129) in the child and anything else in
  the parent (`Readme.md:136-155`). Semantically this is `fork()`.
- Inheritable handles are copied **preserving their indices** (`Readme.md:240`).
- Unless `RTL_CLONE_PROCESS_FLAGS_NO_SYNCHRONIZE` is passed, it drains the ntdll thread-pool work queue
  and takes the loader lock, PEB lock, TLS/FLS locks and heap manager locks across the clone — using
  unexported internals, which is precisely why one should call it rather than open-code
  `NtCreateUserProcess` (`Readme.md:226-230`).
- Cloning is **fast** — "the order of milliseconds" — because it is COW, at the cost of commit charge
  (`Readme.md:354`).

**Its real constraints, and how each lands on us:**

| Constraint | Impact on the engine |
|---|---|
| **Loading further DLLs in the clone deadlocks on ntdll locks, or faults on the CSR port heap** (`Readme.md:249`) | Acceptable *if* we load every DLL up front and never `LoadLibrary` after start. Needs verification for the mingw-w64/UCRT startup path. |
| **Only the calling thread survives** (`Readme.md:242`) | Identical to POSIX. Our `fork_child_hooks` already assumes this. |
| **Non-inheritable / exclusive handles, ALPC ports, Ob-callback-protected types leave holes** (`Readme.md:240`) | We must mark every handle in our registry inheritable before cloning, exactly as Cygwin does per-fd, and treat a hole as a bug. |
| **SRW locks, critical sections, condition variables use `NtWaitForAlertByThreadId`; cloned thread IDs differ** (`Readme.md:244`) | Any lock held by a vanished parent thread is unrecoverable. Same hazard as Linux fork; same cure (reinit in the child) — **except** for locks we do not own, notably the **UCRT heap and stdio locks**. This is the sharpest risk and the strongest argument for §5.4. |
| **Some sections are mapped `ViewUnmap` and vanish in the clone** — CSR shared memory, CSR port heap, GDI shared handle table (`Readme.md:246`) | Only affects OS-internal regions. Our own mappings are `ViewShare` by default because every Win32 `MapViewOfFile*` uses it. |
| **Console I/O needs `FreeConsole` + `AttachConsole(ATTACH_PARENT_PROCESS)`** (`Readme.md:251`) | A guest writing to a console needs this in the child. Cheap, but must not be forgotten. |
| **Window/graphics APIs do not work** (`Readme.md:252`) | Irrelevant to us. |
| **Cannot specify a token or a job at creation** (`Readme.md:220`) | Job assignment is available post-hoc via `NtAssignProcessToJobObject`. |
| **Cannot clone another process** (`Readme.md:256`) | Irrelevant — we always clone ourselves. |
| **Officially undocumented** | A real risk. Mitigate by keeping the Cygwin-style path as a fallback for case (a) and by pinning behaviour with a probe test in CI. |

One property is worth calling out because it maps *exactly* onto machinery we already have. A section
mapped `ViewShare` stays **genuinely shared** with the parent in the clone — it is not COW
(`Readme.md:417`). That is precisely the Linux `memfd` + `MAP_SHARED` behaviour that
`hl_linux_memory_repair_code` already handles: on Linux the fork child re-creates a *private* memfd and
re-maps both aliases `MAP_FIXED` at the same addresses so the arena's contents and every baked pointer
stay valid (`src/host/linux/host.c:717-790`). The Windows equivalent — create a fresh pagefile-backed
section, copy the content, `MapViewOfFile3`/`NtMapViewOfSection` both aliases back over the inherited
addresses — is the same algorithm with different calls.

And one simplification we already have and should not give away: **on Linux, the guest-fork child does
not preserve the JIT arena at all.** `jit_after_fork` sets `preserve = 0` on Linux except for two narrow
cases, and the child re-translates on demand (`src/translator/cache.c:1708-1741`). If we take the same
default on Windows, the child's W^X arena problem reduces to *allocate a fresh one*, and the hard case
(re-aliasing an inherited dual mapping) is not on the critical path for correctness.

### 5.4 Does our narrower problem admit a simpler solution?

**For case (a) — the isolation clone — yes, and dramatically so.** The child does not need the parent's
address space. It re-loads the guest itself (`src/core/lifecycle.c:76-89`). What it needs is the
*entry context* and the *engine's resource tables*. Both are serialisable. So case (a) can be a plain
`CreateProcessW` of our own image with a `--forked-child` argument, an inherited section carrying a
marshalled entry context, and an inherited handle set — i.e. **Cygwin's process-creation half without
Cygwin's memory-transfer half**. No fixed heap base, no DLL rebasing, no `longjmp`, no
`WriteProcessMemory`. This is by far the lowest-risk way to get the Windows host to the point where it
can run a guest at all.

The pieces that need care are the ones a re-exec does not inherit for free:
- `hl_engine_config` including `executable->image` (an in-memory ELF image, must go through the shared
  section or a temp file) and `rootfs`;
- `argc`/`argv`, `hl_options`;
- the `hl_engine_child_result` page, which is already an `HL_HOST_MEMORY_SHARED` anonymous mapping
  (`src/core/lifecycle.c:114-120`) → becomes an inherited section;
- the `hl_linux_abi` OFD table when `box != NULL`, which is already enumerated by
  `hl_linux_abi_fork_prepare` into a plan of records (`src/linux_abi/linux_abi.c:286`) → the plan
  becomes a wire format instead of inherited memory.

**For case (b) — the guest's `fork(2)` — no.** The guest may have an arbitrary address space, arbitrary
JIT state and an arbitrary instruction pointer. Nothing short of a real address-space clone reproduces
it, and Cygwin's technique costs a full copy of everything. `RtlCloneUserProcess` is the only viable
primitive on Windows, and if it does not work for us then case (b) is not implementable and guest `fork`
must be reported unsupported on Windows. **That is the question to answer first, before writing any
other Windows code**, because the answer determines whether the Windows host is a real host or a
restricted one.

**And the zygote idea does help — but only for case (a).** We can fork from a quiescent, single-threaded
moment before any JIT or guest thread exists, because case (a) *is* that moment already
(`src/core/lifecycle.c:130-137` runs before `hl_run_linux_guest`). It buys us the right to ignore every
"a peer thread held a lock" hazard on that path. It buys nothing for case (b), where the guest chooses
when to fork.

---

## 6. Recommended design sketch

Two paths, sequenced so that the risky one is answered by a probe before it is depended on.

### Phase 0 — Answer the `RtlCloneUserProcess` question with a probe (do this first)

A standalone ~200-line mingw-w64 program, in-tree under `tests/`, not linked against the engine. It must
establish, on this Windows 11 box and on whatever CI runner we intend to use:

1. `RtlCloneUserProcess(RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES, NULL, NULL, NULL, &info)` returns
   `STATUS_PROCESS_CLONED` (0x00000129) in the child and success in the parent.
2. The child observes identical private memory, including a 256 MB dirty region, and COW-diverges from
   the parent correctly.
3. A pagefile-backed section mapped twice (RW + RX aliases, i.e. our W^X arena shape) is inherited at
   both addresses, and writes through the child's RW alias are visible to the *parent* (confirming
   `ViewShare`, and therefore confirming that the arena needs the same privatisation Linux needs).
4. `malloc`/`free`/`printf` work in the child with **no** peer threads — then repeat with a peer thread
   spinning in `malloc`/`free` and record whether the child deadlocks. This is the UCRT lock question and
   it is the one that decides whether case (b) is viable at all.
5. `WaitForSingleObject` + `GetExitCodeProcess` on `info.ProcessHandle` behave.
6. Inheritable file/section/event handles land at the same numeric indices; a *non*-inheritable handle's
   index is a hole.
7. Console output from the child, with and without `FreeConsole` + `AttachConsole(ATTACH_PARENT_PROCESS)`.
8. Wall-clock cost of the clone, with a 16 MB and a 1 GB dirty parent, against
   `CreateProcessW`+`WaitForSingleObject` of a trivial image as the control.

Declare the API surface locally (`ntdll.dll`, `GetProcAddress` at startup); do not rely on any SDK
header. Record every result in this document's §7.

### Phase 1 — `spawn_cloned` / `spawn_prepared` by re-exec (no address-space cloning)

Fixed layout first. Add `src/host/windows/memory_layout.h` on Cygwin's model, and reserve the arenas at
process start with `VirtualAlloc2` + `MEM_ADDRESS_REQUIREMENTS` (or `MEM_RESERVE_PLACEHOLDER` where
sections will later be mapped in with `MapViewOfFile3(..., MEM_REPLACE_PLACEHOLDER, ...)`). Keep
`HL_LINUX_SNAPSHOT_BASE` as the guest arena and give the JIT arena and the engine's own large
allocations their own reserved ranges. Link the engine `/HIGHENTROPYVA:NO` equivalent
(`-Wl,--disable-high-entropy-va`) so images stay low, exactly as Cygwin does.

Then, in the parent:

1. `CreateFileMappingW(INVALID_HANDLE_VALUE, &sa /*bInheritHandle=TRUE*/, PAGE_READWRITE, ...)` for a
   **launch block**: a versioned, self-describing struct carrying the marshalled entry context, the
   `argv`/`envp`, the rootfs path, the guest image bytes (or a handle to them), the OFD plan records
   from `hl_linux_abi_fork_prepare`, and the handle values the child should expect.
2. Mark every handle the child needs inheritable (`SetHandleInformation(h, HANDLE_FLAG_INHERIT,
   HANDLE_FLAG_INHERIT)`), or better, build an explicit inherit list with
   `UpdateProcThreadAttribute(..., PROC_THREAD_ATTRIBUTE_HANDLE_LIST, ...)` on an
   `EXTENDED_STARTUPINFO_PRESENT` `STARTUPINFOEX` — this is strictly better than Cygwin's
   `bInheritHandles=TRUE`, which inherits *everything* marked inheritable, and eliminates a whole class
   of accidental leak.
3. `CreateProcessW(ourImagePath, L"hl-engine --forked-child <launchSectionHandleValue>", ..., TRUE,
   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, ...)`. Pass the launch-block handle value
   on the command line rather than in `lpReserved2`; `lpReserved2` works (it is how Cygwin does it) but
   is undocumented and we have no reason to need it.
4. `hl_host_process_services::wait` = `WaitForSingleObject` + `GetExitCodeProcess`;
   `terminate` = `TerminateProcess`. Map the exit code through the existing
   `hl_engine_child_result` shared page, which already carries the real status
   (`src/core/lifecycle.c:114-120`, `160-172`).

In the child: detect `--forked-child` before any engine init, open the launch section, rebuild the
tables, then call `hl_production_entry` exactly as the POSIX path does.

Failure modes are ordinary `hl_host_result` values. No `api_fatal`.

### Phase 2 — guest `fork(2)` via `RtlCloneUserProcess`, gated on Phase 0

1. **Introduce the seam.** `src/linux_abi/syscall/proc.c:1823` and `:2528` call `fork()` directly. Add
   `hl_host_fork()` (or extend `hl_host_process_services`) returning parent/child/error, implemented as
   `fork()` on POSIX and `RtlCloneUserProcess` on Windows. This is the only change outside
   `src/host/windows/`, and it is mechanical.
2. **Parent side, before the clone.** Everything `bound_fork_prepare` already does
   (`src/linux_abi/syscall/proc.c:336`), plus: mark the handles inheritable, and — if the Phase 0 probe
   says the UCRT locks are a hazard — quiesce the engine's own threads. Cygwin's answer here is
   `hold_everything`; ours would be the existing STW machinery.
3. **Child side.** `fork_child_hooks` (`src/linux_abi/syscall/proc.c:267-330`) is already the right
   shape and most of its entries carry over unchanged. The Windows-specific additions:
   - `FreeConsole()` + `AttachConsole(ATTACH_PARENT_PROCESS)` if we have a console;
   - re-create every host object that the clone shares rather than copies — starting with the JIT arena
     section, on the model of `hl_linux_memory_repair_code` (`src/host/linux/host.c:717-790`): create a
     fresh section, `NtUnmapViewOfSection` both inherited aliases, `MapViewOfFile3` the new one at the
     same two addresses, copy content only if preserving;
   - **default to `preserve = 0`**, matching the Linux default (`src/translator/cache.c:1719`), so the
     child simply starts with an empty cache and re-translates. Revisit only after it works.
   - reinitialise the host handle registry lock the same way Linux does — by overwriting it with a fresh
     initialiser rather than trying to acquire it (`src/host/linux/host.c:727-732`).
4. **The pshared futex table.** `src/linux_abi/thread.c:148-159` requires a region that is shared, not
   COW, across fork, with process-shared mutexes and condvars in it. On Windows: a pagefile-backed
   section mapped `ViewShare` gives the shared *memory*; the *primitives* inside it cannot be SRW locks
   or critical sections (they key on thread IDs, which differ — `Readme.md:244`). They must be either
   named kernel objects (mutex/event handles inherited by index) or a hand-rolled futex over
   `WaitOnAddress`. **`WaitOnAddress` across processes is not something I have established works** — it
   is documented as intra-process — so plan on inherited kernel event handles, which are known to work
   cross-process, at a cost of one or two handles per bucket. `FUTEX_NBUCKET` is **256**
   (`src/linux_abi/thread.c:31`), so that is 256–512 inherited handles for the *shared* table; the
   process-private table (`g_fbk_private`) needs nothing special because
   `futex_private_table_after_fork` already rebuilds it in place (`src/linux_abi/thread.c:193`).
   512 handles is affordable but it is 512 more entries in every inherit list — worth confirming against
   whatever handle-list mechanism Phase 1 settles on.

### Phase 3 — decide the fallback

If Phase 0 says `RtlCloneUserProcess` cannot carry case (b), the choice is between (i) a Cygwin-style
full memory transfer for guest fork, priced at linear-in-guest-footprint and therefore only tolerable for
small guests, and (ii) declaring guest `fork` unsupported on the Windows host. Given §3's numbers,
(ii) is more honest than (i). Do not build (i) speculatively.

---

## 7. What this document does *not* establish

Listed explicitly so nobody builds on a guess.

1. **Nothing in §5.3 has been run on this machine.** Every claim about `RtlCloneUserProcess` comes from
   the Hunt & Hackett guide and secondary sources. Phase 0 exists to replace all of it with measurement.
2. **Whether the UCRT's heap/stdio locks survive a clone taken while a peer thread holds one.**
   `RtlCloneUserProcess` takes the *ntdll* heap locks, not the CRT's. This is the highest-risk unknown
   and it decides Phase 2.
3. **Whether the mingw-w64/UCRT startup path loads any DLL lazily after `main`.** If it does, and the
   clone touches it, we hit the "loading DLLs in a clone deadlocks" failure.
4. **Whether `WaitOnAddress` functions across processes on shared memory.** Assumed no; not tested.
5. **The real default of Cygwin's `proc_retry`.** §2 argues from the source that it is 0 and that
   mechanism 3 is inert unless `$CYGWIN` sets it. Not confirmed by running Cygwin.
6. **A clean fork-only microbenchmark for Cygwin.** The §3 numbers include `exec` and teardown. The
   pure-fork cost is unmeasured; it will be lower, but the structural argument (eager copy of the whole
   dirty footprint, four context switches, a full loader run) puts a floor on it that is nowhere near
   a Linux `fork()`.
7. **Any per-fork failure rate for Cygwin.** §2 is qualitative because that is all the evidence supports.
8. **Whether `NtMapViewOfSectionEx` can specify `InheritDisposition`.** Cygwin's own declarations show
   `NtMapViewOfSection` takes a `SECTION_INHERIT` and `NtMapViewOfSectionEx` does not
   (`local_includes/ntdll.h:1467-1472`), so `ViewUnmap` appears to require the classic call. Whether
   that matters to us depends on whether we ever want a mapping to *vanish* in the clone (the Windows
   analogue of `MADV_DONTFORK`, which `dontfork_apply_child` implements today).

---

## 8. Sources

Primary — `github.com/cygwin/cygwin` @ `fa7b0cd` (2026-07-22), all under `winsup/`:
`cygwin/fork.cc`, `cygwin/dcrt0.cc`, `cygwin/sigproc.cc`, `cygwin/dll_init.cc`, `cygwin/forkable.cc`,
`cygwin/dtable.cc`, `cygwin/thread.cc`, `cygwin/cygtls.cc`, `cygwin/hookapi.cc`, `cygwin/environ.cc`,
`cygwin/pinfo.cc`, `cygwin/create_posix_thread.cc`, `cygwin/fhandler/base.cc`, `cygwin/mm/heap.cc`,
`cygwin/mm/cygheap.cc`, `cygwin/mm/mmap.cc`, `cygwin/cygwin.sc.in`, `cygwin/Makefile.am`,
`cygwin/local_includes/{child_info,memory_layout,sigproc,thread,dll_init,winsup,ntdll}.h`,
`doc/highlights.xml`, `doc/cygwinenv.xml`. Commits cited: `8a5d3952`, `ece7282f`, `023c107a`,
`a8c23e44`, `717c36c0`, `c0776fa7`, `2f9b8ff0`, `60675f1a`, `943433b0`.

Secondary:
- Hunt & Hackett, *The Definitive Guide To Process Cloning on Windows* —
  `github.com/huntandhackett/process-cloning` (`Readme.md`).
- Bill Demirkapi, *Abusing Windows' Implementation of Fork() for Stealthy Memory Operations*.
- `cygwin-apps` thread *"Optimising cygwin fork performance"*, Dec 2020 – Jan 2021.
- `cygwin` thread *"fast/native fork?"*, Jan 2018.
- Baumann, Appavoo, Krieger, Roscoe, *A fork() in the road*, HotOS 2019 — the conceptual argument that
  `fork` is a poor primitive, cited by both of the above.

Engine cross-references: `include/hl/host_services.h:160-207`, `:444-459`, `:588-597`;
`src/core/lifecycle.c:70-146`; `src/linux_abi/linux_abi.c:520-615`;
`src/linux_abi/syscall/proc.c:267-330`, `:336`, `:1823`, `:2528`; `src/translator/cache.c:1694-1778`;
`src/translator/arena.c:23-34`; `src/host/linux/host.c:655-790`, `:3475-3530`;
`src/linux_abi/thread.c:148-206`; `src/linux_abi/elf.c:800-830`;
`src/linux_abi/container/snapshot.{h,c}`; `src/linux_abi/fork.c:20-70`.
