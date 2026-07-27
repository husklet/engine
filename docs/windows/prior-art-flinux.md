# Prior art: `wishstudio/flinux`

`docs/windows/prior-art-survey.md` §3.6 calls flinux "our twin" and "the blueprint". This document is the
close reading that claim needs: what flinux's fork actually does, line by line; what its 64 KiB-block memory
manager costs, measured on this box; how close its `*_fork(hProcess)` / `*_afterfork_parent()` decomposition
really is to `fork_child_hooks` in `src/linux_abi/syscall/proc.c`; and what it does not solve for us.

`DOCS.md` is normative. This file is the record for one piece of prior art.

Source read: [`wishstudio/flinux`](https://github.com/wishstudio/flinux) at `a041253` (*"Lower many vfs lock
requirements to use shared lock."*, **2016-03-29**, the last commit on master), cloned to the session
scratchpad. Every `file:line` below is that tree. Numbers marked **measured** were produced by probes in the
scratchpad (`exp_flinux{,2,3}.c`, clang 22.1.8, `x86_64-w64-windows-gnu`, `-O2`) on Windows 11 Pro
10.0.26200, x86-64 — the same host as the survey's §2.

**One correction to the survey up front.** §3.6 describes flinux as the engine "minus the JIT". That is
wrong: flinux ships a full dynamic binary translator in `src/dbt/` (~4 000 lines; `x86.c` is 2 110). The
README's first sentence is *"Foreign LINUX is a dynamic binary translator and a Linux system call interface
emulator"*. This matters in both directions — it means flinux faced a version of our translation-cache
problem, and it means the way it *dodged* that problem (§5) is itself the finding.

---

## 1. The fork mechanism, precisely

### 1.1 Shape

`fork_process()` (`src/syscall/fork.c:163-238`) is 75 lines and the whole of it is legible:

```c
CreateProcessW(filename, L"/?/fork", NULL, NULL, TRUE, CREATE_SUSPENDED, ...);   /* :171 */
tls_fork(hProcess);   vfs_fork(hProcess, dwProcessId);  mm_fork(hProcess);       /* :177-183 */
shared_fork(hProcess); heap_fork(hProcess); signal_fork(hProcess);               /* :186-192 */
process_fork(hProcess); exec_fork(hProcess);                                     /* :195-198 */
pid = process_init_child(dwProcessId, dwThreadId, hProcess);                     /* :201 */
NtWriteVirtualMemory(hProcess, &fork->context,    context,     sizeof ...);      /* :205 */
NtWriteVirtualMemory(hProcess, &fork->stack_base, &stack_base, sizeof ...);      /* :206 */
NtWriteVirtualMemory(hProcess, &fork->pid,        &pid,        sizeof ...);      /* :207 */
VirtualAllocEx(hProcess, stack_base, STACK_SIZE, MEM_RESERVE|MEM_COMMIT,
               PAGE_EXECUTE_READWRITE);                                          /* :214 */
NtWriteVirtualMemory(hProcess, context->esp, context->esp,
                     stack_base + STACK_SIZE - context->esp, NULL);              /* :215 */
ResumeThread(info.hThread);                                                      /* :217 */
vfs_afterfork_parent(); tls_afterfork_parent(); ... mm_afterfork_parent();       /* :221-228 */
```

**Child creation.** A plain `CreateProcessW` of `GetModuleFileNameW(NULL)` — the engine re-executing itself —
with `bInheritHandles = TRUE` and `CREATE_SUSPENDED`, and the command line set to the literal string
`"/?/fork"`. No `RtlCloneUserProcess`, no `NtCreateProcessEx`, no undocumented call anywhere in the fork path.
`fork_init()` (`fork.c:78-138`), called from `main()` at `main.c:118` *before* `mm_init()`, `strcmp`s
`GetCommandLineA()` against `"/?/fork"` and, on a match, jumps straight into `fork_child()` and never returns
(`fork.c:80-85`).

**The child never resumes a host C stack.** `fork_child()` (`fork.c:61-76`) runs on the *fresh* thread stack
`CreateProcess` gave it, runs nine re-initialisation hooks, then calls
`dbt_restore_fork_context(&fork->context)` (`x86.c:2060`), which jumps into a generated trampoline
(`dbt_gen_restore_fork_trampoline`, `x86.c:603-634`) that loads ECX/EDX/EBX/ESP/EBP/ESI/EDI from
`struct syscall_context`, pushes `context->eip`, zeroes EAX (fork's return value in the child) and jumps to
the DBT's indirect-dispatch stub. The parent's host call chain is *not* reconstructed and does not need to
be: flinux's engine/guest boundary is a register-file struct (`struct syscall_context`, `dbt/x86.h:29-44`),
not a C frame.

This is the same property `docs/windows/fork-model.md` §3.3 argues for our engine — the child's continuation
is "run the hooks, set the return register to 0, re-enter the dispatcher with a correct `struct cpu`".
**flinux is a working existence proof of exactly that argument.**

**The stack.** `STACK_SIZE` is 1 MiB (`syscall/process.h:30`). The guest stack is not in the block-managed
address space at all: `process_init()` allocates it with a raw `VirtualAlloc` (`process.c:182`), and
`mmap(MAP_STACK)` is forced onto `INTERNAL_MAP_VIRTUALALLOC` (`mm.c:1137-1143`) with the comment *"Windows
shows strange behaviour when the stack is on a shared section object … it sometimes crashes when returning
from a blocking system call"*. So the stack is reserved in the child with `VirtualAllocEx` and copied
eagerly with `NtWriteVirtualMemory` — but **only the live part**, from `context->esp` to the top
(`fork.c:215-216`). Same trick Cygwin uses.

**The handshake** is one-way and asynchronous. There is no rendezvous event, no ready flag, no retry loop.
The parent does all of the work into a *suspended* child and calls `ResumeThread` once. If any hook fails the
parent `TerminateProcess`es the child and returns `-1` (`fork.c:233-237`). The parent's `*_afterfork_parent()`
calls are pure lock releases (§4). The child, once resumed, is on its own.

### 1.2 What is shared, what is COW, what is copied eagerly

Four disjoint classes, and the classification is per `map_entry` (`mm.c:109-124`):

| Class | Marker | Treatment at fork |
|---|---|---|
| Ordinary guest memory (anon + file-backed private) | default | **Section handle inherited, view not mapped.** Parent's writable pages demoted to read-only (`mm.c:1083-1088`). COW resolved lazily in a VEH. |
| `MAP_SHARED` | `INTERNAL_MAP_SHARED` (`mm.c:1130-1136`) | **Genuinely shared.** Same section object in both processes, no protection demotion, no copy. `MAP_SHARED` also forces `MAP_POPULATE` and whole-block alignment. |
| Guest/engine stacks, the engine heap, the static-alloc arena, `startup` | `INTERNAL_MAP_VIRTUALALLOC` (`mm.h:56`) | **Copied eagerly**, `VirtualAllocEx` + `NtWriteVirtualMemory` per region, protections replayed from `VirtualQuery` (`mm.c:1023-1082`). |
| The `mm` bookkeeping itself | — | **Copied eagerly.** `NtWriteVirtualMemory(process, mm, mm, sizeof(struct mm_data))` (`mm.c:970`), plus the populated 64 KiB pages of the section-handle table (`mm.c:984-999`). |

The eager `mm_data` copy is not small on a 64-bit build. `SECTION_TABLE_COUNT` is `BLOCK_COUNT /
SECTION_HANDLE_PER_TABLE` = 2⁴⁸/2¹⁶ / (65536/8) = **524 288**, so `uint16_t
section_table_handle_count[SECTION_TABLE_COUNT]` (`mm.c:168`) is a flat **1 MiB**, written on every fork.
Measured `WriteProcessMemory` into a suspended child: **59 µs/MiB**, so ≈65 µs of unavoidable per-fork cost
from that field alone.

### 1.3 The lazy-mapping decision, in the author's words

`mm.c:1000-1015`, verbatim:

> Section mapping plus protection change is very time consuming. It takes about **8 msec for 50-60 sections
> (3-4M)** on my machine. This is too slow that even a `NtWriteVirtualMemory()` for such amount of data only
> takes about 4 msec. In most cases when `execve()` is invoked immediately after `fork()`, these regions are
> quickly discarded and the mapping time is wasted. To improve performance, we don't map any of the sections
> at all and leave the section objects in the child as `detached'.

8 ms / 55 sections ≈ **145 µs per 64 KiB block** on the author's 2015 machine. **Measured on this box in
2026: 2.03–2.23 µs per block** for `CreateFileMapping` + `MapViewOfFile`, flat from 1 024 to 65 536 blocks
(64 MiB → 4 GiB), plus 1.2–1.4 µs for a per-block `VirtualProtect`. That is a **~40× improvement in the
constant this design decision was built around**, and it is the single most important caveat on inheriting
flinux's conclusions: *the measurement that forced its laziest, most fragile machinery no longer holds.*

---

## 2. The 64-KiB-block design

### 2.1 Why

`mm.c:38-53` states it, and the wiki's *Memory manager* page repeats it: Windows `dwAllocationGranularity`
is 64 KiB, Linux `mmap` is 4 KiB. flinux therefore treats guest memory as an array of fixed 64 KiB **blocks**
(`BLOCK_SIZE`, `mm.h:31`), each backed by its **own** anonymous NT section object created with
`NtCreateSection(..., PAGE_EXECUTE_READWRITE, SEC_COMMIT)` and `OBJ_INHERIT` (`allocate_block`,
`mm.c:668-703`), and maps 4 KiB `PROT_*` onto per-page `VirtualProtect` *inside* the mapped block
(`mm_change_protection`, `mm.c:618-646`; `load_block_protection`, `mm.c:805-834`).

**The granularity constraint alone does not require a section per block** — one section with many 64 KiB
views would satisfy it. The per-block section is chosen for **fork**, and specifically so that COW ownership
can be decided per block by asking the *kernel* how many handles exist:

```c
NtQueryObject(handle, ObjectBasicInformation, &info, ...);   /* mm.c:775 */
if (info.HandleCount == 1) return 1;                          /* mm.c:781 — sole owner, write in place */
duplicate_section(block);                                     /* mm.c:789 — otherwise copy */
```

So: **allocation granularity forces block-structured memory; fork is what forces one section object per
block.** Both are load-bearing.

A second, subtler consequence: the *guest's* `PROT_EXEC` becomes free. Every block is created
`PAGE_EXECUTE_READWRITE` and merely `VirtualProtect`ed down, so a guest `mprotect(PROT_EXEC)` never needs a
new mapping. flinux pays for this by making all guest memory potentially executable.

### 2.2 What it costs — measured

| | Measured on this host |
|---|---|
| `CreateFileMapping` + `MapViewOfFile`, per 64 KiB block | **2.03–2.23 µs**, flat to 65 536 blocks |
| Same, 4 GiB of guest memory (65 536 blocks) | **137.5 ms** |
| VAD regions produced by 1 024 per-block views | **1 024** (one `VirtualAlloc` of the same bytes: **1**) |
| `NtQueryObject(ObjectBasicInformation)` | 0.213 µs/call |
| `VirtualProtect` on one block | 1.18–1.39 µs |
| 32 GiB `VirtualAlloc(MEM_RESERVE\|MEM_TOP_DOWN)` | succeeds, **2 µs** |

**Address-space consumption on a 64-bit host is the eye-catching number.** `mm_section_handle` is reserved
as one flat array indexed by block number (`mm.c:351`):
`VirtualAlloc(NULL, BLOCK_COUNT * sizeof(HANDLE), MEM_RESERVE|MEM_TOP_DOWN, ...)`. With
`ADDRESS_SPACE_HIGH = 0x0001'0000'0000'0000` (`mm.c:64`), `BLOCK_COUNT` = 2³² and the reservation is
**32 GiB of virtual address space**, committed 64 KiB at a time on first use
(`add_section_handle`, `mm.c:182-192`). It works — measured, 2 µs — but it is a direct 2⁻¹³ tax on the guest
address space, and it exists only because the design refuses any sparse structure.

**Bookkeeping cost** is a red-black tree of at most `MAX_MMAP_COUNT` = **1 024** `map_entry` records
(`mm.c:57`, `:165`). A guest that exceeds 1 024 VMAs gets `log_error("Map entry exhausted.")` and a NULL
(`mm.c:238-239`). Linux's default `vm.max_map_count` is 65 530. This is a hard ceiling, not a tuning knob.

**Fragmentation** is the per-block VAD: 1 024 distinct kernel VAD entries for 64 MiB of guest memory, versus
1 for the equivalent `VirtualAlloc`. `VirtualQuery`-based walks over guest memory become O(blocks); flinux's
own `mm_dump_windows_memory_mappings` (`mm.c:512-551`) walks to `0x00007FFFFFFF0000` one region at a time.

### 2.3 Interaction with 4 KiB guest expectations

This is where the design leaks, and flinux is candid about it.

- **Sub-block `mmap`/`munmap` cannot free address space.** `free_map_entry_blocks` (`mm.c:289-333`) only
  unmaps *whole* blocks that no neighbouring entry shares; a partially-used first/last block is merely
  `VirtualProtect`ed to `PAGE_NOACCESS` (`mm.c:305-320`) and its section stays alive. A guest that maps and
  unmaps many small regions leaks sections and blocks indefinitely.
- **`MAP_FIXED` at a non-64 KiB address is only *sometimes* legal.** `mmap_internal` accepts it for ordinary
  private mappings but rejects it outright for anything block-aligned — `MAP_SHARED` or
  `INTERNAL_MAP_VIRTUALALLOC` — with `ENOMEM` (`mm.c:1146-1172`), and rejects it again if it would collide
  with an existing block-aligned entry. The file header states the limitation plainly (`mm.c:49-52`): *"it
  seems impossible to implement MAP_FIXED with MAP_SHARED or MAP_PRIVATE on non 64kB aligned address."*
- **File-backed `mmap` is not a file mapping.** `map_entry_range` (`mm.c:601-616`) `pread`s the file content
  into anonymous section memory. `mmap` of a file is a *read*, `msync` would be a *write*. `mm.c:43-47`
  admits this: *"It's impossible to use Windows file mapping functions. We have to read/write file content
  manually … This may be slow."* There is no shared file mapping across processes at all.
- **COW granularity is the block, not the page.** One byte written into a shared block copies all 64 KiB.

`mm.h:90-101` carries the author's own verdict on the resulting layering:

> Since mm only accepts allocation granularity at PAGE_SIZE, there could be much space lost … We keep the
> initialization order consistent thus they will always get the same static address.
> **TODO: This scheme is really ugly, any better ideas?**

---

## 3. "Handle tables duplicated into the child *unmapped*, faulted in lazily by VEH"

The survey's phrasing is right but compresses three separate mechanisms. In full:

**(a) Inherited handle *values* are identical, so the table can be memcpy'd.**
Every section is created with `attr.Attributes = OBJ_INHERIT` (`mm.c:674`, `:712`) and the child is created
with `bInheritHandles = TRUE` (`fork.c:171`). NT handle inheritance preserves the numeric handle value in the
child, so the parent can copy the *raw* `HANDLE` array into the child's address space and every entry is
already a valid handle there. `mm_fork` does exactly that (`mm.c:977-999`): `VirtualAllocEx` a fresh
`BLOCK_COUNT * sizeof(HANDLE)` reservation in the child, patch the child's `mm_section_handle` pointer, then
`NtWriteVirtualMemory` each populated 64 KiB table page verbatim. No handle is duplicated, translated, or
re-opened.

> **Measured, because the whole scheme rests on it:** a parent created an inheritable pagefile-backed
> section (handle value `208`), spawned a child with `bInheritHandles=TRUE` passing *nothing* but the
> decimal number, and the child called `MapViewOfFile((HANDLE)208, …)` successfully and read the parent's
> cookie. Exit code 0. `exp_flinux.c`. Handle-value identity across inheritance is real.

**(b) The sections arrive *detached*: present in the table, not mapped.**
`mm_fork` deliberately performs no `NtMapViewOfSection` into the child (`mm.c:1000-1015`, quoted in §1.3).
The child's address space starts almost empty; every guest access is an access violation.

**(c) A single VEH resolves all three fault classes.**
`install_syscall_handler()` (`syscall.c:129-133`) is `AddVectoredExceptionHandler(TRUE, exception_handler)`,
and it is the **first** thing `fork_child()` does (`fork.c:63`), before any other hook. The handler
(`syscall.c:38-127`) reads `ExceptionInformation[0]` (0 read / 1 write / 8 DEP) and `[1]` (the faulting VA)
and calls `mm_handle_page_fault(addr, is_write)` (`mm.c:931-963`), which dispatches on two bits:

| Section handle present? | Write? | Path |
|---|---|---|
| no | — | `handle_on_demand_page_fault` (`mm.c:894`) — allocate a fresh block, populate from the file or zero-fill, apply protections |
| yes | no | `load_detached_block` (`mm.c:837`) — map the inherited section at the block address, then *drop write permission* (`load_block_protection(block, PROT_READ\|PROT_EXEC, …)`, `mm.c:847`) so the next write still faults |
| yes | yes | `handle_cow_page_fault` (`mm.c:857`) — `take_block_ownership` → `NtQueryObject` handle count → copy if shared → map → restore full protections |

The DEP case (`ExceptionInformation[0] == 8`) is handled by faulting in the *instruction* page, with a retry
one page further on (`syscall.c:47-57`) for an instruction straddling a block boundary.

`duplicate_section` (`mm.c:706-762`) is the copy itself, and its shape is dictated by a Windows constraint
recorded in its own comment (`mm.c:723-727`): the section is currently mapped **read-only** and *"write
protection can not be promoted afterwards"* on a view mapped without write access. So it must
`NtUnmapViewOfSection` the block, re-map the old section **somewhere else** as `PAGE_READWRITE`, create a
brand-new section, map *that* at the block's real address, `CopyMemory` 64 KiB, unmap the alias, and
`NtClose` the old handle.

### 3.1 Does it generalise? — measured

The mechanism generalises; **its performance does not, and the reason is the copy, not the handles.**

Breakdown of `duplicate_section`, 2 000 reps (`exp_flinux3.c`):

| Step | µs |
|---|---|
| `UnmapViewOfFile(old)` | 1.43 |
| `MapViewOfFile(alias, RW)` | 0.77 |
| `CreateFileMapping(new)` | 1.01 |
| `MapViewOfFile(new)` | 0.98 |
| **`CopyMemory(64 KiB)`** | **34.55** |
| `UnmapViewOfFile(alias)` | 3.96 |
| `CloseHandle(old)` | 2.90 |
| **total** | **45.59** |

Driven by a real VEH write fault end to end: **47.97 µs per 64 KiB block** (`exp_flinux2.c`). The four
address-space calls are cheap; the 64 KiB memcpy into a demand-zero destination is 76 % of the cost. **A
guest that dirties 256 MiB after forking pays ~196 ms of COW faults**, versus microseconds for a kernel COW
fork.

**There is a cheaper primitive flinux did not use, and it is worth recording.** NT sections support a
`PAGE_WRITECOPY` / `FILE_MAP_COPY` view: real kernel copy-on-write, 4 KiB granularity, no VEH, no handle
counting. Measured on this host:

- A `FILE_MAP_COPY` view of a pagefile-backed section **sees** the content written through a `FILE_MAP_WRITE`
  view (0xAA read back), and writes through it **stay private** (the RW view still reads 0xAA / 0x00).
- It works **cross-process** through an inherited handle: a child's `FILE_MAP_COPY` view saw the parent's
  RW-view write but not the parent's WRITECOPY-view write.
- An existing RW view can be **re-mapped `FILE_MAP_COPY` at the same VA with content intact**
  (`MapViewOfFileEx(sec, FILE_MAP_COPY, …, addr)` after `UnmapViewOfFile`).
- Cost: **1.243 µs per 4 KiB page** = 19.9 µs per 64 KiB equivalent, i.e. **2.4× faster than flinux's manual
  path at full-block granularity and ~39× faster when the guest touches one page of a block.** 256 MiB fully
  dirtied: **81.5 ms vs 196 ms**, and dramatically better for sparse dirtying.

**The catch, stated so nobody adopts this naively:** WRITECOPY-private pages are *not* in the section, so a
process cannot be forked *again* from them without first re-anchoring (create a new section, copy the
current view into it, re-map WRITECOPY over it — the same 34 µs/block flinux pays, but only for blocks
dirtied since the last fork). This is exactly why flinux's parent must keep writing *through* the section:
so the next child inherits its state. Given a fork-heavy guest, WRITECOPY is a strict improvement (it is
never worse than flinux's path and is much better when the *child* does the dirtying); given a
fork-once-then-run guest it is a large win. Either way it is an *adaptation*, not a drop-in.

---

## 4. The `*_fork(hProcess)` / `*_afterfork_parent()` decomposition, versus ours

### 4.1 Complete enumeration

Ten subsystems, of which **only three actually transfer state**:

| Subsystem | `*_fork(hProcess)` | `*_afterfork_parent()` | `*_afterfork_child()` | What it really does |
|---|---|---|---|---|
| `mm` | `mm.c:965` | `mm.c:1094` | `mm.c:1099` | **The whole address space.** Only real worker. |
| `tls` | `tls.c:74` | `tls.c:111` | `tls.c:92` | Snapshots the *calling thread's* Win32 TLS slots into shared static memory; child `TlsAlloc`s and restores them. |
| `vfs` | `vfs.c:364` | `vfs.c:417` | `vfs.c:390` | Iterates 1 024 fds, dispatches `f->op_vtable->fork`; otherwise just takes each file's lock. |
| `shared` | `shared.c:164` | `shared.c:218` | `shared.c:223` | `NtMapViewOfSection` of the global shared region and each live shared-heap pool **into the child** at the parent's address. |
| `console` | `fs/console.c:253` | — | `console_afterfork` `:274` | Maps the console shared section into the child, patches the child's `console` pointer. |
| `socket` (per-file) | `fs/socket.c:251` | `:262` | `:266` | `WSADuplicateSocketW(sock, child_pid, &fork_info)`; child `WSASocketW(FROM_PROTOCOL_INFO)`. |
| `heap` | `heap.c:77` | `heap.c:83` | `heap.c:88` | **Lock acquire / release only.** Data rides in the eagerly-copied static arena. |
| `signal` | `sig.c:468` | `sig.c:474` | `sig.c:462` | **`EnterCriticalSection` / `LeaveCriticalSection` only.** |
| `process` | `process.c:206` — `return 1;` | `process.c:211` — `{}` | `process.c:186` | Parent side is a **no-op**. |
| `exec` | `exec.c:488` | — | — | One `NtWriteVirtualMemory` of the `startup` pointer. |
| `flags` | — | `flags.c:31` | `flags.c:35` | — |

So the pattern is real but **six of the ten `*_fork` hooks are pure lock brackets**, and
`*_afterfork_parent()` is, in every case, *release the lock `*_fork` took*. It is a
lock-ordering-and-quiescence protocol wearing the clothes of a state-transfer protocol.

The state that *looks* transferred mostly is not: `heap`, `tls`, `signal`, `vfs`, `shared` all live in the
`mm_static_alloc` arena (`mm.h:100`, 3 × 64 KiB), which is `INTERNAL_MAP_VIRTUALALLOC` and therefore already
copied byte-for-byte by `mm_fork`. Each child hook simply re-derives its pointer by calling
`mm_static_alloc(sizeof …)` in the *same order* `init_subsystems()` used (`main.c:97-106`). That is the
scheme its own header calls "really ugly".

### 4.2 Honest comparison with our tree

The engine has **two** decompositions, and only one of them is the analogue.

**The real analogue is `hl_linux_abi_fork_prepare` / `_fork_parent` / `_fork_child`**
(`src/linux_abi/linux_abi.c:286`, `:442`, `:494`), driven by `bound_fork_prepare` /
`bound_fork_complete` (`src/linux_abi/syscall/proc.c:336`, `:427`) with peers
`hl_host_process_fd_private_fork_prepare`, `proc_fdvis_fork_prepare`, `bound_mapping_fork_prepare`,
`seq_ref_fork_prepare`. That is a genuine three-phase prepare/parent/child bracket over an enumerated,
per-descriptor state transfer — **structurally closer to flinux than the survey claimed**, and closer than
`fork_child_hooks` is.

**`fork_child_hooks` (`proc.c:267-324`) is *not* the analogue of `*_fork(hProcess)`.** It is the analogue of
flinux's `fork_child()` (`fork.c:61-76`) — the child-side-only list. And the correspondence there is one of
*shape*, not of work:

| | flinux `fork_child()` | engine `fork_child_hooks()` |
|---|---|---|
| Steps | 9 | 25 |
| Steps that **build** the child's address space | most of them (`mm_afterfork_child`, `shared_afterfork_child`, `tls_afterfork_child`, `dbt_init`) | **zero** — the host `fork(2)` already did it |
| Steps that **destroy or reset** inherited state | ~2 | ~20 (`fork-model.md` §3 counts it) |

**This is the honest verdict, and it inverts the survey's framing.** flinux's hooks exist because *nothing*
is inherited automatically and every subsystem must hand-build its half of the child. Our hooks exist because
*everything* is inherited automatically and most of it is wrong. They are mirror images, not the same shape.
`fork_child_hooks` is not a port target — it is a list of things a Windows child would mostly **not have to
do**, because it would never have inherited them.

Specific pairings that *do* transfer:

- **`mm_fork` ↔ nothing we have.** There is no engine code that replicates an address space into another
  process. This is the entire gap.
- **`tls_fork`/`tls_afterfork_child` ↔ `thread_after_fork` (`thread.c:2246`).** Superficially similar,
  opposite content. flinux *preserves* the calling thread's TLS by explicit save/restore. Ours *destroys*
  the inherited thread registry (`thread.c:2269-2277`: phantom entries poison `tgkill` routing and cost
  "~14 s PER compile child, measured"). flinux has no such problem — the child has no phantoms because it
  was never a copy.
- **`futex_private_table_after_fork` (`thread.c:193-209`) ↔ nothing.** flinux's futex table is a plain
  static (`futex.c:52-54`), never forked, so the child starts with an empty table — accidentally correct for
  private futexes. `futex.c:32` says `/* TODO: How to implement interprocess futex? */`. **flinux has no
  shared futex at all.** Our `g_fbk` shared-futex arena (`thread.c:165`) and the `futex_key` shared-object
  canonicalisation (`thread.c:244`) have no flinux counterpart. Our `MAP_SHARED` ledger arenas
  (`fork-model.md` §3.1 R2, seventeen of them) are entirely outside flinux's problem.
- **`jit_after_fork` (`cache.c:1694`) ↔ nothing.** See §5.

---

## 5. What flinux does not solve for us

### 5.1 It has a JIT — and it throws the cache away

`fork_child()` calls `dbt_init()` (`fork.c:72`) — the ordinary cold-start initialiser (`x86.c:774-791`).
There is **no `dbt_fork`, no `dbt_afterfork_parent`, no `dbt_afterfork_child`** anywhere in the tree. The
child re-allocates an 8 MiB code cache and an 8 MiB block table (`DBT_CACHE_SIZE`/`DBT_BLOCKS_TABLE_SIZE`,
`x86.c:480-481`) with raw `VirtualAlloc(..., PAGE_EXECUTE_READWRITE)` (`x86.c:765-769`) and re-translates
everything on demand.

Three things make that trivially safe for flinux and not for us:

1. **The cache is `__declspec(thread)` (`x86.c:550`) — per host thread, never shared.** Every guest thread
   gets its own 16 MiB. There is no cross-thread cache coherence problem, hence no cross-fork one.
2. **It is flat RWX** (`x86.c:768`). No dual RW/RX alias, so nothing analogous to `hl_arena_repair`, no
   VM_INHERIT_NONE hole, no re-coupling step. The whole class of bug `cache.c:1665-1677` documents cannot
   arise.
3. **It is not in the block-managed address space.** Raw `VirtualAlloc(MEM_TOP_DOWN)`, outside `mm`'s tree,
   never in a section, never inherited.

Our extra burden, precisely:

- **The dual-mapped W^X arena.** `cache.c:1665-1673` — the RX alias is `VM_INHERIT_NONE` and must be
  re-coupled in the child at the same VA, or the two views silently diverge. On Windows the same shape is
  available (survey §2.2 measured all three patterns working), but `hl_arena_repair` has no Windows body and
  the placeholder API (`VirtualAlloc2` + `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`) is what would implement
  it. flinux offers zero guidance here.
- **The `MAP_SHARED` ledger arenas.** Seventeen of them (`fork-model.md` §3.1 R2). flinux's only analogue
  is `shared_fork` (`shared.c:164-216`), which does the right thing — `NtMapViewOfSection(section,
  childProcess, &addr, …)` at the parent's address, into a suspended child — and that pattern *is* directly
  reusable, and is precisely `fork-model.md` §6.4's strategy D. But flinux maps *two* things this way (a
  global shared region and a shared-heap pool chain) and has no futex, no eventfd, no fdvis, no task-state,
  no cgroup accounting, no ptrace arena, no sigexit relay. The mechanism transfers; the scale does not.
- **The translation cache as inheritable state.** `jit_after_fork` already sets `preserve = 0` on Linux
  (`cache.c:1719`) — the child re-translates. So on this axis we are *already where flinux is*, and
  `fork-model.md` §3.2 N1 is right that this is the surprise: the hardest-sounding item is optional. What is
  **not** optional is that the child end up with a consistent arena at a usable VA, which on Windows means a
  working `hl_arena_repair`, which flinux never needed.

### 5.2 Threads

flinux **does** support threads. `sys_clone_imp` with `CLONE_THREAD` routes to `fork_thread`
(`fork.c:261-282`) → `CreateThread(..., CREATE_SUSPENDED)` → `fork_thread_callback` (`fork.c:240-259`), which
runs `dbt_init_thread()`, `process_thread_entry()`, `tls_set_thread_area()`, `dbt_update_tls()` and then
`dbt_restore_fork_context()` on the guest-supplied child stack. There is additionally a permanent dedicated
signal thread (`sig.c:438`), so **flinux is never single-threaded**, even for a `main(){fork();}` guest.

How threads interact with fork:

- **The child is always single-threaded**, by construction — it is a fresh `CreateProcess`. That is POSIX
  fork semantics for free, and it is the same property `RtlCloneUserProcess` gives (survey §3.1: only the
  calling thread survives).
- **The parent is not stopped.** There is no suspend-all, no `RtlCloneUserProcess`-style
  drain-the-thread-pool-and-take-the-loader-lock. Quiescence is attempted purely with reader locks:
  `mm_fork` takes `AcquireSRWLockShared(&mm->rw_lock)` (`mm.c:967`), `heap_fork` and `tls_fork` likewise,
  `signal_fork` takes a critical section, `vfs_fork` takes each file's lock — all released by the
  `*_afterfork_parent` calls at `fork.c:221-228`.
- **That is not sufficient, and the author knew it.** `mm_fork` copies the section-handle table into the
  child (`mm.c:977-999`) **before** demoting the parent's writable pages to read-only (`mm.c:1083-1088`). A
  peer thread writing guest memory between those two steps writes into a section the child already holds a
  handle to — the write leaks into the child. Nothing in the SRW-lock protocol prevents it, because guest
  memory writes do not take `mm->rw_lock`. The project's own wiki TODO list carries, under intermediate
  tasks: *"Fix data races. There might be many when forking in a multi-threaded program."* The last three
  commits before the project stopped include *"vfs: Fix fork deadlock when sharing sockets"* and *"Socket
  forking support (untested)"* (2016-02-26).

**Read against `fork-model.md` §9 risk 2, this is a warning, not a reassurance.** flinux's approach is not a
demonstration that user-mode fork is safe from a multithreaded parent; it is a demonstration that a
user-mode fork from a multithreaded parent is *racy in ways its own author flagged and never fixed*. Our
engine is multithreaded exactly where it matters (Go, npm, cargo — `cache.c:1697-1705` names the production
hang), and flinux offers no design for that. `RtlCloneUserProcess` at least *documents* what it drains and
which locks it takes, and ships `RtlUpdateClonedCriticalSection`/`RtlUpdateClonedSRWLock` for what it
cannot.

### 5.3 Other gaps

- **Self-modifying code.** `dbt_code_changed` (`x86.c:812-827`) is called only from `munmap_internal_unsafe`
  (`mm.c:1346`) when a `PROT_EXEC` entry is fully unmapped, and its response is to **flush the entire code
  cache**. There is no write-fault interception on translated guest code. The wiki TODO lists
  "self-modifying code support" under *hard* tasks. Our x86-64 transliterator's refusal posture and the
  interpreter's re-decode (survey §4.3) are both strictly stronger.
- **1 024 VMAs and 1 024 fds** (`mm.c:57`, `vfs.h:34`) are hard ceilings.
- **No shared/interprocess futex** (`futex.c:32`).
- **No `msync`, no shared file mappings** (`mm.c:43-47`).

---

## 6. Reliability and limits

**ASLR / address collision.** flinux has exactly one collision problem and one cure, both 64-bit-only.
`fork_init` (`fork.c:88-135`) checks whether `[0x400000, 0x400000+256 MiB)` — the default ET_EXEC base — is
free; if it is, it reserves it immediately; if it is already exactly that reservation, it recognises itself
as a re-launched child; **otherwise it spawns a suspended copy of itself, `VirtualAllocEx`es the reservation
into it, `ResumeThread`s and exits** (`fork.c:112-134`). `load_elf` releases the reservation just before
mapping the guest (`exec.c:200-203`). One retry, not a loop. This is the same forced pattern as PostgreSQL's
`pgwin32_ReserveSharedMemoryRegion` (survey §3.7) and Midipix's 32× retry (§3.5); three independent projects
converging on "reserve into a suspended child" is strong evidence it is platform-forced.

Note what is *absent*: flinux has **no** mechanism to guarantee its own image, heap, or the
`mm_section_handle` table land at the same address in the child. It does not need one for the image (per-boot
ASLR keeps the same EXE at the same base within a boot session, survey §2.5), and for the handle table it
sidesteps the problem entirely by `VirtualAllocEx`-ing a *fresh* reservation in the child and patching the
child's pointer (`mm.c:977-983`) rather than requiring the same address. That is a genuinely good idea and it
generalises.

**Correctness hazards visible in the source:**

- `handle_cow_page_fault` decides ownership by `NtQueryObject(...).HandleCount == 1` (`mm.c:781`). That is a
  *system-wide* handle count, not a live-peer count: an unrelated descendant, a `DuplicateHandle`, or a child
  that has exited but not been reaped inflates it and forces a spurious 64 KiB copy. It is also inherently
  racy against a concurrent fork.
- `mm_fork`'s eager `INTERNAL_MAP_VIRTUALALLOC` copy contains `// FIXME: How to handle this case?` for
  `PAGE_NOACCESS` pages, which it simply skips (`mm.c:1052-1057`) — a guest `PROT_NONE` guard page inside a
  VirtualAlloc region is silently not replicated.
- `mm_shutdown` (`mm.c:398-412`) loops `for (size_t i = 0; i < BLOCK_COUNT; i++)`. On a 64-bit build that is
  **2³² iterations at every process exit**. `mm_fork` similarly loops 524 288 times over
  `SECTION_TABLE_COUNT` per fork. Neither is plausible in a program that was actually run on x64.
- `shared_fork` returns `false` at `shared.c:184` on a path where `shared_afterfork_parent()` will still
  release a lock that was never taken.

**Why the project stopped.** Last commit 2016-03-29. The author's own TODO list (wiki) puts *"Make dbt dual
x86/x64 compatible and then work towards full x64 support"* under **hard tasks** — and the source agrees:
`src/dbt/x86.c` contains **zero** `#ifdef _WIN64`, uses `__writefsdword`, and `struct syscall_context`
(`dbt/x86.h:29-44`) is ten `DWORD`s. `exec.c:154-158` will only load `EM_X86_64` on a 64-bit build, but
nothing can translate it. **The 64-bit configuration in `flinux.vcxproj` was aspirational; the x86-64 guest
path was never finished**, which is also the most likely reading of closed issues #14 *"Crashes on Windows 7
64bit"* and #20 *"Crash on Win10 64-bit"*. Open issues at abandonment are compatibility, not architecture
(#83 gtk3, #96 quick start, #103 image site). No maintainer statement of abandonment was located — the
evidence is the commit log, the TODO list, and eight years of silence. **The project did not fail on fork;
it ran out of author while still 32-bit.**

---

## 7. Verdict

### 7.1 Adopt

1. **Inherited-handle-value identity as a state-transfer channel.** Measured, cheap, documented behaviour.
   Any table of kernel objects the child needs can be memcpy'd verbatim if every handle is inheritable and
   the child is `CreateProcess`ed with `bInheritHandles=TRUE`. This is a strictly better answer than
   PostgreSQL's decimal-handle-on-the-command-line (survey §3.7).
2. **`NtMapViewOfSection(section, hChild, &addr, …)` into a suspended child** (`shared.c:164-216`,
   `console.c:253-281`) as the way to place the `MAP_SHARED` ledger arenas. This is `fork-model.md` §6.4
   strategy D, already recommended there, and flinux is the working precedent. Use placeholders
   (`VirtualAlloc2`/`MapViewOfFile3`, survey §2.2) rather than reserve-and-free.
3. **Patch the child's pointer instead of demanding the same address**, where the structure is
   position-independent (`mm.c:977-983`). Cheap, and it removes an ASLR dependency.
4. **`WSADuplicateSocketW(sock, childPid, &info)` + `WSASocketW(FROM_PROTOCOL_INFO)`**
   (`socket.c:251-270`) for sockets. Same as PostgreSQL, same as libuv.
5. **Reserve the contested range into a suspended child and re-launch** (`fork.c:112-134`) if we ever need a
   fixed guest base.
6. **Arm the VEH before anything else in the child** (`fork.c:63`).

### 7.2 Adapt

7. **The COW mechanism, but with `PAGE_WRITECOPY` views instead of manual duplication.** §3.1 measured
   1.24 µs/4 KiB kernel COW against 48 µs/64 KiB manual, with correct cross-process semantics and in-place
   re-mapping. The re-anchoring cost at the *next* fork is the price and must be designed for.
8. **The prepare/parent/child bracket.** We already have the better version of it
   (`hl_linux_abi_fork_prepare`/`_parent`/`_child`, `linux_abi.c:286`/`:442`/`:494`). flinux confirms the
   shape works when the hooks must *build* the child rather than repair it.

### 7.3 Reject

9. **One NT section object per 64 KiB block, for the whole guest address space.** 1 024 VADs per 64 MiB, a
   32 GiB handle-table reservation, a 1 024-VMA ceiling, block-granular COW, no file mappings, no
   sub-block unmap. Its central justification — 145 µs per block-map — **is 2.03 µs on this host**, a 40×
   change that dissolves the argument for the laziest parts of the design.
10. **Per-thread private code caches** (`x86.c:550`). 16 MiB/thread is not affordable at our arena sizes.
11. **SRW reader locks as the fork quiescence protocol.** §5.2. The author's own TODO says it is racy.

### 7.4 Does flinux make our guest-fork problem look easier or harder than `RtlCloneUserProcess`?

**Harder — and it sharpens rather than weakens `fork-model.md`'s ranking.**

flinux is an existence proof that driver-free user-mode fork *ships*, and that is worth a great deal. But
the thing it proves works is a fork for a personality whose entire address space is *already* an explicitly
managed table of 64 KiB sections it created itself. That precondition is the design. We do not have it and
acquiring it would mean rewriting `src/linux_abi/syscall/mem.c`, the anon tracker, the logical-VMA snapshot
layer and `hl_gmap` onto a block-and-section allocator — and then accepting a 1 024-VMA-class ceiling, a
32 GiB reservation, block-granular COW at ~48 µs a block, no shared file mappings, and per-block VADs. And
after all that, flinux's mechanism still would not move the engine's own C heap, `struct cpu`, the dual-mapped
arena, or seventeen `MAP_SHARED` ledger arenas — every one of which `RtlCloneUserProcess` moves for free, at
the right VA, by construction.

`RtlCloneUserProcess` was **measured working on this box from a 10-thread parent at 2.90–3.46 ms**
(`fork-model.md` §5.1), which is the same order as flinux's unavoidable `CreateProcess` floor (survey §2.3:
1.54 ms bare, 5.05 ms round-trip) *before* flinux does any of its own work. **flinux is not cheaper, and it
is enormously more invasive.** Strategy A stays primary.

The two open questions in `fork-model.md` §5.4 / §9 are unchanged by this study, and flinux informs one of
each:

- **Do `MAP_SHARED` section views survive a clone as `ViewShare`?** flinux does not answer it — but
  `shared_fork` shows the fallback (map them explicitly into the child) is a two-line pattern, so a negative
  answer is survivable. **Still measure it first.**
- **Threads in a clone.** flinux offers nothing; its child is always single-threaded and its *parent*'s
  threading is a known race. This risk is untouched.

### 7.5 Versus checkpoint/restore

`fork-model.md` §6.2's fallback — checkpoint the parent, `CreateProcess` a fresh engine, restore — is the
**better** fallback than a flinux-style rebuild, on both axes the survey cares about.

**Complexity.** Checkpoint/restore is code that **exists and is tested**: 82/82 `checkpoint`, 34/34
`checkpoint-io`, 11/11 `ckpt-cross` (`docs/amd64-host.md` §1), and `checkpoint.c:5391` already re-forks a
whole process tree. It uses documented APIs only. A flinux-style path is a from-scratch memory manager
touching the most load-bearing subsystem in the tree, with a per-block section design we would then be stuck
with everywhere, not just at fork.

**Robustness.** Checkpoint/restore reconstructs into a *fresh* engine, so by construction it captures exactly
the guest-visible state and nothing else — which, per `fork-model.md` §3.3, is all a fork child needs. It has
one known defect (`g_gro` not repopulated, `amd64-host.md` §7) and no undocumented dependencies. flinux's
path has an unfixed multithreaded-fork race its author documented, a `HandleCount`-based ownership test that
is racy by construction, a `FIXME` on `PAGE_NOACCESS` replication, and 64-bit loops that prove the
configuration was never run.

**Cost.** Checkpoint/restore is ~55 ms per 256 MiB (`fork-model.md` §5.3) versus flinux's ~1.5 ms
`CreateProcess` + ~65 µs of `mm_data` + ~196 ms of lazy COW faults for the same 256 MiB *if the child dirties
it all*. So flinux is faster **only** for a child that touches little — which is the `fork`+`exec` case, and
exactly the case flinux's laziness was tuned for (`mm.c:1004-1006`). For a child that runs, the two converge,
and the checkpoint path is the one that is already tested.

**Conclusion: keep `fork-model.md`'s ranking.** Strategy A (`RtlCloneUserProcess`) primary, strategy B
(checkpoint/restore) as fallback and differential oracle, strategy D (explicit section mapping into the
child) for the `MAP_SHARED` arenas under either. flinux contributes six concrete, measured techniques to
§7.1 and one better COW primitive to §7.2 — and its most valuable contribution is the negative one: it shows
what a *complete* user-mode address-space replication costs when you build the whole memory manager around
it, and the answer is more than we should pay.

---

## 8. Unknowns

Stated rather than guessed:

- **flinux was never run.** No build, no execution, no measurement *of flinux*. It targets 32-bit MSVC/MASM
  (`stubs.asm` is x64-excluded in `flinux.vcxproj:158-166`) and does not build under mingw-clang. Every
  behavioural claim above is from reading source and from independent probes of the *primitives* it uses.
- **Whether the `PAGE_WRITECOPY` alternative (§3.1) holds under memory pressure, on a section under
  concurrent RW access from another process, or across a `CreateProcess` boundary with the section already
  partially privatised.** The probes covered the single-writer and simple cross-process cases only.
- **Whether the 2.03 µs/block figure holds on a machine with a large existing VAD tree.** Probes started
  from a clean process.
- **Why exactly the project stopped.** No maintainer statement was located; the inference in §6 is from the
  commit log and the wiki TODO list.
- **Whether flinux's x64 configuration ever executed a guest.** The absence of any `_WIN64` handling in
  `src/dbt/x86.c` and the 2³²-iteration loops say no; nothing states it.
