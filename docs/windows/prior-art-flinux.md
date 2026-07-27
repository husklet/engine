# Prior art: `wishstudio/flinux` — process creation and fork

`docs/windows/prior-art-survey.md` §3.6 calls flinux "our twin" and "the blueprint". This document is the
close reading that claim needs, scoped to **fork, process creation, and the fork/afterfork decomposition**.
Companion agents cover flinux's memory manager, its VFS, and its DBT/signal machinery; this file touches
those only where fork forces it.

`DOCS.md` is normative. This file is the record for one piece of prior art.

---

## 0. Licensing — read this first

**flinux is GPLv3+. This engine is MIT. They are incompatible for our purposes.**

- `scratchpad/flinux/LICENSE` is the GNU General Public License v3; `src/*.c` carry
  *"licensed under GPLv3+"* headers, applied wholesale in commit `ff0dbe7` (2015-01-20).
- `C:\Users\hutta\Desktop\engine\LICENSE` is MIT.

Therefore:

- **No flinux source may be copied into `src/`, ever.** Not a function, not a struct, not a table.
- **No wholesale reproduction of flinux source in `docs/`.** This document quotes only short comment
  fragments where the author's own words are the evidence, which is ordinary commentary use.
- **Everything below is a description of technique, algorithm, constraint and measurement.** Ideas,
  architectures, API-usage sequences and empirical facts about Windows are not copyrightable. Any
  implementation we write must be independently authored from an understanding of the approach — not
  transliterated from their expression. §3 is deliberately written as an algorithm specification precise
  enough to implement from, *without* reopening their tree.
- The measured numbers in this document come from **our own probes**
  (`scratchpad/exp_flinux{,2,3,4}.c`), not from flinux.

What we want from flinux is knowledge: what works on NT, what does not, what defeated them, where the
sharp edges are. That is worth more than their code would be.

---

## 1. Provenance and method

Source read: [`wishstudio/flinux`](https://github.com/wishstudio/flinux), full history (849 commits),
HEAD `a041253` *"Lower many vfs lock requirements to use shared lock."*, **2016-03-29** — the last commit on
master. Cloned to the session scratchpad and unshallowed for the archaeology in §9.

Numbers marked **measured** were produced on this box (Windows 11 Pro 10.0.26200, x86-64) by probes written
for this study: clang 22.1.8, target `x86_64-w64-windows-gnu`, `-O2`, in the session scratchpad. They are
measurements of *Windows primitives*, not of flinux.

**A correction to the survey, up front.** §3.6 describes flinux as the engine "minus the JIT". That is wrong:
flinux ships a full dynamic binary translator in `src/dbt/` (~4 000 lines; `x86.c` alone is 2 110), and the
README's first sentence says so. This matters in both directions — flinux faced a version of our
translation-cache-across-fork problem, and the way it *sidestepped* that problem is itself a finding (§10.1).

---

## 2. What flinux's fork is, in one paragraph

A guest `fork()` becomes: `CreateProcessW` of flinux's own image, suspended, with `bInheritHandles=TRUE` and
a magic command line; the parent then reaches into the suspended child with `NtWriteVirtualMemory`,
`VirtualAllocEx` and `NtMapViewOfSection` to construct the child's address space and kernel-object set; then
`ResumeThread` once. The child's `main()` detects the magic command line before doing anything else, runs a
child-side re-initialisation list, and jumps into a generated trampoline that reloads the guest register file
and re-enters translated code. Guest memory is **not** copied: each 64 KiB block of it is a separate NT
section object whose *handle* is inherited (handle values survive inheritance unchanged), so the parent
memcpy's the raw handle array into the child and maps **nothing**. The child's every access then faults, and
one vectored exception handler maps blocks in on demand and resolves copy-on-write by asking the kernel how
many handles a section has.

No undocumented API is used anywhere in the fork path. No `RtlCloneUserProcess`, no `NtCreateProcessEx`
clone mode, no driver.

---

## 3. The complete fork call path, as an algorithm

Written so it can be reimplemented from this description alone. Names in `code font` are flinux's, given so
a reader can navigate their tree if they must; the algorithm is described in our own terms.

### 3.1 Preconditions established at engine startup

Three things must already be true before any fork can work, and all three are startup decisions:

1. **Fork detection is the first statement in `main()`.** flinux calls `fork_init()` before its memory
   manager, before command-line parsing, before any subsystem allocates anything (`main.c:114-121`). It
   compares the *whole* command line against a fixed sentinel string (`"/?/fork"`); on a match it calls the
   child-side entry and **never returns**. This ordering is load-bearing: the child must not run any
   allocator before the parent's writes are interpreted, or the parent's carefully placed structures will
   collide with the child's own bookkeeping.
2. **Every kernel object the child will need is created inheritable.** flinux sets `OBJ_INHERIT` on section
   objects (`mm.c:674`, `:712`), on its named object directory and shared sections (`shared.c:91`, `:115`,
   `:154`, `:254`), on pipes and pipe events (`pipe.c:289-318`), and `bInheritHandle=TRUE` on eventfds,
   sockets and console handles (`eventfd.c:65`, `socket.c:529`, `console.c:187`). Files are opened
   inheritable through a parameter threaded down to the open path (`winfs.c:1112`).
   **This is a whole-backend constraint, not a fork-local one** — exactly what `fork-model.md` §9 flags as
   a runner-up risk. flinux paid it in full.
3. **Engine-private state lives in one of three places, each with its own fork story.** Plain C statics in
   the image (`_mm`, the fork mailbox, the DBT's global offsets, the futex table); a small bump-allocated
   "static arena"; or ordinary heap. See §3.4.

### 3.2 Parent side, step by step

Entered from the guest-syscall dispatcher with a pointer to the captured guest register file.

**P1. Create the child suspended.**
`CreateProcessW(GetModuleFileNameW(NULL), <sentinel command line>, inherit = TRUE, CREATE_SUSPENDED)`.
The child is the same image, so — given per-boot rather than per-process image ASLR — it loads at the same
base, which is what makes P4 legal. On failure, return `-1` immediately; nothing has been touched.

**P2. Run each subsystem's parent-side fork hook, in a fixed order,** passing the child's process handle.
flinux's order is TLS, VFS, MM, shared, heap, signal, process, exec. Two facts about this list matter more
than its contents:

- **Six of the eight hooks are pure lock acquisitions.** They take a reader lock (or a critical section) and
  return. Their matching `*_afterfork_parent()` releases it after `ResumeThread`. The list is a
  *quiescence protocol* that happens to be spelled as a state-transfer protocol. §9.2 shows this is exactly
  why it was introduced.
- **Any hook returning failure aborts the fork:** `TerminateProcess` the child, close its handles, return
  `-1`. There is no partial-fork recovery and no rollback of the parent-side protection changes already made
  (a real defect — see §8.4).

The three hooks that actually transfer state:

  - **TLS:** read the *calling thread's* Win32 TLS slots into a fixed array in the shared static arena, so
    the child can re-`TlsAlloc` and restore them. Peer threads' TLS is not saved — correct, because fork
    keeps one thread.
  - **VFS:** iterate the fd table; for each *distinct* open file (the table is sorted by file pointer first,
    so a dup'd file is visited once), dispatch a per-file-type fork operation if the type defines one, else
    just take the file's lock. Only sockets define one: `WSADuplicateSocketW(sock, childPid, &blob)` stores
    a protocol-info blob in the file object, and the child reconstructs with
    `WSASocketW(..., FROM_PROTOCOL_INFO)`. Everything else — real files, pipes, eventfds — relies purely on
    handle inheritance.
  - **MM:** §3.3.
  - **Shared:** `NtMapViewOfSection(section, hChild, &sameAddress, ...)` for the process-wide shared region
    and for each currently-mapped shared-heap pool. This is the "map it into the child at the parent's
    address" pattern, and it is `fork-model.md` §6.4 strategy D, working, in 2015.
  - **Console:** map the console's shared section into the child and patch the child's console pointer.

**P3. Allocate the child's guest pid — in the parent, before resuming.** flinux takes a cross-process named
mutant, allocates a slot in a shared process table, fills in win_pid / win_tid / tgid / pgid / ppid / sid,
registers the child in the parent's own child list for `wait4`, and releases the mutant. **The child never
allocates its own pid**; it is told what it is. Ordering constraint: this must happen before the child runs,
and it must be the parent that does it, because only the parent knows the parent–child relationship.

**P4. Write the fork mailbox into the child.** A single file-scope struct in the image (guest register file,
guest stack base, the assigned pid, and optionally the `CLONE_CHILD_SETTID` address) is written field by
field with `NtWriteVirtualMemory` at *the parent's own address of that struct*. Legal only because parent and
child are the same image at the same base. `CLONE_PARENT_SETTID` is satisfied by a plain store in the parent.

**P5. Materialise the guest stack eagerly.** `VirtualAllocEx(hChild, stackBase, STACK_SIZE, RESERVE|COMMIT,
PAGE_EXECUTE_READWRITE)` then `NtWriteVirtualMemory` of **only the live part** — from the guest stack pointer
in the captured register file to the top of the stack. This is both an optimisation and a correctness
requirement (§8.1).

**P6. `ResumeThread`, close the thread handle.** That is the entire handshake: **one-way, asynchronous, no
rendezvous event, no ready flag, no retry loop.**

**P7. Release everything P2 took,** in reverse-ish order, and return the child's guest pid.

### 3.3 The address-space step in detail

The parent's MM hook does four things and deliberately does not do a fifth.

1. **Copy the MM bookkeeping struct** into the child at its own address, verbatim
   (`NtWriteVirtualMemory(hChild, &mm, &mm, sizeof mm)`). It is a plain static in the image.
2. **Give the child a fresh reservation for the section-handle table** — `VirtualAllocEx(hChild, NULL, ...)`,
   letting Windows choose the address — and then **patch the child's pointer-to-table** with one more
   `NtWriteVirtualMemory`. *The table does not have to be at the same address in the child.* This is a small,
   generalisable idea and it removes an ASLR dependency; see §11.
3. **Copy the populated pages of the handle table.** The table is a flat array indexed by block number,
   committed lazily 64 KiB at a time and tracked by a per-64-KiB-page occupancy counter, so only occupied
   pages are `VirtualAllocEx(MEM_COMMIT)`'d and written into the child. The handle *values* are copied raw
   (§5).
4. **Walk the VMA tree once.** For each region:
   - **eagerly-copied region** (guest/engine stacks, the engine heap, the static arena, startup data —
     flagged at map time): `VirtualAllocEx` it in the child, then walk it with `VirtualQuery` and, per
     uniform-protection run, `NtWriteVirtualMemory` the bytes and `VirtualProtectEx` the protection.
   - **`MAP_SHARED` region**: do nothing at all. Both processes will map the same section object; that *is*
     the sharing.
   - **everything else (private guest memory)**: if writable, demote the parent's pages to non-writable so
     the parent's next write takes a COW fault too.
5. **It maps nothing into the child.** The sections arrive *detached*: present in the child's handle table,
   absent from its address space.

The author's stated reason for (5), which is the design's centre of gravity (`mm.c:1000-1015`):

> Section mapping plus protection change is very time consuming. It takes about **8 msec for 50-60 sections
> (3-4M)** on my machine … In most cases when `execve()` is invoked immediately after `fork()`, these regions
> are quickly discarded and the mapping time is wasted.

8 ms / 55 blocks ≈ **145 µs per 64 KiB block** on a 2015 machine. **Measured on this box in 2026: 2.03–2.23 µs**
per block (`CreateFileMapping` + `MapViewOfFile`), flat from 1 024 to 65 536 blocks, plus 1.18–1.39 µs for a
per-block `VirtualProtect`. **The constant this entire design decision was built around has improved ~40×.**
That is the single most important caveat on inheriting flinux's conclusions.

### 3.4 Where engine-private state lives, and how each kind survives

| Kind | Example | How it reaches the child |
|---|---|---|
| Plain static in the image | MM bookkeeping, fork mailbox | Explicit `NtWriteVirtualMemory` at the same address (same image base) |
| Plain static, *not* transferred | futex table, DBT global offsets | Left at its initial value; child re-derives |
| Static bump arena (3 × 64 KiB) | heap, TLS, signal, VFS, flags, shared descriptors | Rides in an eagerly-copied region; child re-derives its pointer by calling the bump allocator **in the same order** as startup |
| Cross-process shared section | process table, shared heap pools, console | Mapped into the child by the parent at the parent's address |
| Per-thread (`__declspec(thread)`) | the entire DBT state, current-thread pointer | Not transferred; rebuilt from scratch |

The bump-arena scheme is the load-bearing hack. Each subsystem's child-side hook re-calls
`arena_alloc(sizeof its struct)`, and because the child replays the *same allocation sequence*, it gets the
*same address*, which already contains the parent's bytes. The header comment on it is the author's own
verdict: *"TODO: This scheme is really ugly, any better ideas?"* (`mm.h:98`).

---

## 4. The child resume mechanism, in full

This is the part most worth understanding, because it is the part our `fork-model.md` §3.3 argument depends
on and flinux is a working proof of it.

**C1. `main()` runs normally** — the child is a fresh process with a fresh loader state, a fresh primary
thread and a fresh 1 MiB thread stack from `CreateProcess`. The CRT has initialised. `.data`/`.bss` hold
their *image* values except where the parent overwrote them.

**C2. The sentinel check fires** and control diverges into the child path, which never returns.

**C3. Arm the vectored exception handler first.** `AddVectoredExceptionHandler(TRUE, handler)` is literally
the first call (`fork.c:63`). It must be, because from step C4 onward every touch of guest memory is a fault
and there is nothing else to catch it.

**C4. Run the child-side re-initialisation list**, in the mirror order of the parent's hooks. Their content
falls into three classes:

- **Re-initialise a lock that was inherited in an unknown state.** Every SRW lock and critical section in
  the copied statics is re-initialised, unconditionally. (Note: the child's copies came from a parent that
  held them *shared* at the moment of the copy.)
- **Re-derive a pointer** by replaying the bump-arena allocation sequence.
- **Recreate a kernel object that cannot be inherited meaningfully.** The signal subsystem is the biggest:
  the child creates a *brand-new* signal pipe, IO completion port, semaphore, mutex **and a new signal
  thread** (`sig.c:423-441`, called from `signal_afterfork_child`). Nothing signal-related is inherited
  except the disposition table, which rode in the copied arena. The process subsystem re-creates its
  per-thread bookkeeping, duplicates a handle to its own thread, creates its wait event, and **adopts the
  pid the parent assigned** rather than allocating one.

**C5. Re-initialise the translator from scratch** — a cold `dbt_init()`, allocating a fresh code cache and
block table. See §10.1.

**C6. Satisfy `CLONE_CHILD_SETTID`** by storing the assigned pid at the guest address the parent recorded.
This happens *after* the memory hooks, so the store faults a block in through the VEH — which is fine, and
is the first exercise of the fault path.

**C7. Jump into a generated trampoline with a pointer to the fork mailbox's register-file field.**
The trampoline (generated at `dbt_init` time, so it is a fresh code-cache resident in the child):

1. saves the current host stack pointer into a TLS slot, as the "kernel stack" to return to on the next
   guest syscall;
2. loads the guest general-purpose registers **including the guest stack pointer** from the register-file
   struct;
3. pushes the saved guest program counter onto the now-guest stack;
4. zeroes the accumulator — this is `fork()`'s `0` return value in the child;
5. jumps to the translator's indirect-dispatch stub, which translates-or-finds the block at the pushed PC
   and enters it.

**The parent's host C call chain is never reconstructed, and never needs to be.** The engine/guest boundary
is a register-file struct, not a C frame. The child's host stack is a *different* stack at a *different*
address from the parent's, and nothing cares.

**This is exactly `fork-model.md` §3.3's claim, demonstrated in shipping code:** the child's continuation is
"run the hooks, set the return register to 0, re-enter the dispatcher with a correct register file". flinux
never had the option of resuming mid-C-stack — `CreateProcess` cannot do it — so it *had* to prove the
weaker requirement sufficient, and it did.

Corollary that transfers directly to us: **a `CreateProcess`-based guest fork is viable for our engine too,
because `struct cpu` plays the role of the register-file struct.** What it costs is not the resume — it is
everything in §3.2 P2–P5.

### 4.1 The `CLONE_THREAD` path, for contrast

Guest `clone(CLONE_THREAD)` does not create a process. flinux allocates a small info block, `CreateThread`s
suspended, allocates a *thread* pid slot in the shared table, fills in the register file with the guest's
requested child stack, records `CLONE_SETTLS`/`CLONE_CHILD_CLEARTID` data, copies the current GS selector,
and resumes. The new thread's entry function initialises per-thread logging, **a whole private translator
instance**, thread bookkeeping, TLS, and then enters the *same* register-file-restore trampoline with the
accumulator zeroed.

So `fork` and `clone(CLONE_THREAD)` converge on one resume primitive. That is a clean design and worth
copying.

One hard-won detail: before `CreateThread`, the guest's requested child stack is **explicitly populated**
(`mm_populate`, added by commit `b475f1e`, 2015-06-25, *"Populate child thread's stack beforehand"*). §8.1
explains why.

---

## 5. "Handle tables duplicated into the child *unmapped*, faulted in lazily by VEH"

The survey's phrase is correct but compresses three mechanisms.

### 5.1 Inherited handle *values* are identical — so a handle table can be memcpy'd

Sections are created `OBJ_INHERIT`; the child is created `bInheritHandles=TRUE`. NT inheritance preserves the
numeric handle value, so a raw copy of a `HANDLE[]` from parent to child produces an array of handles that
are already valid in the child. No `DuplicateHandle`, no translation table, no re-open.

> **Measured, because everything rests on it.** A parent created an inheritable pagefile-backed section
> (handle value `208`), spawned a child with `bInheritHandles=TRUE` passing **nothing** but the decimal
> number `208` on the command line, and the child's `MapViewOfFile((HANDLE)208, …)` succeeded and read back
> the parent's cookie. Exit code 0. `exp_flinux.c`.

`fork-model.md` §5.1 records that clone inheritance requires handles be marked inheritable. It does **not**
record that the *values* survive — and that is what turns "inheritable handles" from a per-object chore into
a **bulk transfer channel** for an entire table of kernel objects. Strictly better than PostgreSQL's
decimal-handle-on-the-command-line (survey §3.7).

### 5.2 Sections arrive detached

The parent maps nothing into the child (§3.3). The child's address space starts nearly empty; every guest
access is an access violation.

### 5.3 One VEH resolves three fault classes

The handler reads `ExceptionInformation[0]` (0 read / 1 write / 8 DEP) and `[1]` (faulting VA) and
dispatches on two bits — *is there a section handle for this block?* and *was it a write?*

| Handle present | Write | Action |
|---|---|---|
| no | — | **On-demand map.** Create a section for the block, map it, populate every VMA overlapping the block (file `pread` or zero-fill), apply each VMA's protection. |
| yes | no | **Attach a detached block.** Map the inherited section at the block's address, then *drop write permission* so the next write still faults. |
| yes | yes | **Resolve COW.** Ask the kernel `NtQueryObject(ObjectBasicInformation).HandleCount`; if 1, we are the sole owner — just restore full protections. Otherwise duplicate the block (below), then restore protections. |

The DEP class (`[0] == 8`) is handled by faulting in the *instruction* page, with a retry one page further
on for an instruction straddling a block boundary.

The handler doubles as the landing-pad mechanism for flinux's guest-pointer validators: if the faulting PC
lies inside one of three known code ranges (`mm_check_read`, `mm_check_read_string`, `mm_check_write`), the
handler rewrites the instruction pointer to that range's failure label and continues. That is a neat,
zero-cost `EFAULT` implementation and it is directly reusable.

### 5.4 Duplicating a block, and why it has the shape it has

To privatise a block the code must: unmap the block's view; map the *old* section somewhere else as a
writable alias; create a new section; map it at the block's real address; copy 64 KiB from alias to block;
unmap the alias; close the old handle.

The detour through a second alias exists because of an NT rule the author found the hard way and recorded in
a surviving comment: the block is currently mapped **not writable**, and *"write protection can not be
promoted afterwards"*. §8.2 pins the exact rule down by measurement.

**Cost, measured (`exp_flinux3.c`, 2 000 reps):**

| Step | µs |
|---|---|
| unmap old view | 1.43 |
| map writable alias | 0.77 |
| create new section | 1.01 |
| map new section | 0.98 |
| **copy 64 KiB** | **34.55** |
| unmap alias | 3.96 |
| close old handle | 2.90 |
| **total** | **45.59** |

Driven end-to-end by a real VEH write fault: **47.97 µs per 64 KiB block** (`exp_flinux2.c`). The four
address-space calls are cheap; the copy into a demand-zero destination is 76 % of it. **A child that dirties
256 MiB pays ~196 ms of COW faults.**

### 5.5 The primitive flinux left on the table

NT sections support `PAGE_WRITECOPY` / `FILE_MAP_COPY` views: **real kernel copy-on-write, 4 KiB
granularity, no VEH, no handle counting.** Measured on this host (`exp_flinux3.c`):

- A `FILE_MAP_COPY` view **sees** content written through a `FILE_MAP_WRITE` view of the same section
  (read back `0xAA`), and writes through it **stay private** (the RW view still reads the old byte).
- It works **cross-process** through an inherited handle: a child's `FILE_MAP_COPY` view saw the parent's
  RW-view write but not the parent's WRITECOPY-view write.
- An existing RW view can be **re-mapped `FILE_MAP_COPY` at the same VA with content intact**.
- **1.243 µs per 4 KiB page** = 19.9 µs per 64 KiB equivalent. 256 MiB fully dirtied: **81.5 ms vs 196 ms**
  — and vastly better when the guest touches one page of a block rather than all sixteen.

**The catch, so nobody adopts this naively.** WRITECOPY-private pages are *not in the section*, so a process
cannot be forked *again* from them without re-anchoring (create a new section, copy the current view into it,
re-map WRITECOPY over it — the same ~34 µs/block, but only for blocks dirtied since the last fork). This is
precisely why flinux's parent must keep writing *through* the section: so the next child inherits its state.
Net: WRITECOPY is never worse than the manual path and is much better whenever the child does the dirtying,
but it is an **adaptation with a re-anchoring obligation**, not a drop-in.

---

## 6. The `*_fork(hProcess)` / `*_afterfork_parent()` / `*_afterfork_child()` decomposition

### 6.1 Complete enumeration

| Subsystem | parent-side `*_fork` | `*_afterfork_parent` | `*_afterfork_child` | What it really does |
|---|---|---|---|---|
| mm | `mm.c:965` | `:1094` | `:1099` | **The address space.** The only real worker. |
| tls | `tls.c:74` | `:111` | `:92` | Save calling thread's TLS slots; child re-allocs and restores |
| vfs | `vfs.c:364` | `:417` | `:390` | Per-open-file dispatch; mostly just locks |
| shared | `shared.c:164` | `:218` | `:223` | Map shared region + live pools into the child |
| console | `fs/console.c:253` | — | `:274` | Map console section into child, patch child pointer |
| socket (per file) | `fs/socket.c:251` | `:262` | `:266` | `WSADuplicateSocketW` / `WSASocketW` |
| heap | `heap.c:77` | `:83` | `:88` | **Lock only.** Data rides in the copied arena |
| signal | `sig.c:468` | `:474` | `:462` | **Lock only** (parent); child builds an entirely new signal subsystem |
| process | `process.c:206` (`return 1;`) | `:211` (`{}`) | `:186` | Parent side is a **no-op** |
| exec | `exec.c:488` | — | — | One pointer write |
| flags | — | `flags.c:31` | `:35` | Pointer re-derivation only |

**Six of eleven parent-side hooks are pure lock brackets.** `*_afterfork_parent()` is, in every single case,
"release what `*_fork()` took". The shape is a quiescence protocol, and §9.2 confirms that is exactly what it
was introduced to be.

### 6.2 Honest comparison with our tree

The engine has **two** decompositions and only one of them is the analogue.

**The real analogue is `hl_linux_abi_fork_prepare` / `_fork_parent` / `_fork_child`**
(`src/linux_abi/linux_abi.c:286`, `:442`, `:494`), driven by `bound_fork_prepare` / `bound_fork_complete`
(`src/linux_abi/syscall/proc.c:336`, `:427`) with peers `hl_host_process_fd_private_fork_prepare`,
`proc_fdvis_fork_prepare`, `bound_mapping_fork_prepare`, `seq_ref_fork_prepare`. That is a genuine
three-phase prepare/parent/child bracket over an enumerated per-descriptor transfer —
**structurally closer to flinux than the survey claimed**, and closer than `fork_child_hooks` is.

**`fork_child_hooks` (`proc.c:267-324`) is *not* the analogue of `*_fork(hProcess)`.** It is the analogue of
flinux's child-side list. And there the correspondence is one of *shape*, not of work:

| | flinux child list | engine `fork_child_hooks()` |
|---|---|---|
| Steps | 9 | 25 |
| Steps that **build** the child's address space | most (`mm_afterfork_child`, `shared_afterfork_child`, `tls_afterfork_child`, `dbt_init`) | **zero** — the host `fork(2)` did it |
| Steps that **destroy or reset** inherited state | ~2 | ~20 (`fork-model.md` §3 counts it) |

**This inverts the survey's framing, and it is the honest verdict.** flinux's hooks exist because *nothing*
is inherited and every subsystem must hand-build its half of the child. Ours exist because *everything* is
inherited and most of it is wrong. They are mirror images. `fork_child_hooks` is not a port target — it is a
list of things a `CreateProcess`-based Windows child would mostly **not have to do**, because it never
inherited them in the first place.

Specific pairings:

- **`mm_fork` ↔ nothing we have.** No engine code replicates an address space into another process. This is
  the entire gap, and it is the whole cost of a flinux-shaped route.
- **TLS save/restore ↔ `thread_after_fork` (`thread.c:2246`).** Superficially similar, opposite content.
  flinux *preserves* the calling thread's TLS explicitly. We *destroy* the inherited thread registry
  (`thread.c:2269-2277`: phantom entries poison `tgkill` routing and cost "~14 s PER compile child,
  measured"). flinux has no phantoms because the child was never a copy — a genuine structural advantage of
  the `CreateProcess` route.
- **`futex_private_table_after_fork` (`thread.c:193-209`) ↔ nothing.** flinux's futex table is an untouched
  image static, so the child starts empty — accidentally correct for private futexes. `futex.c:32` reads
  `/* TODO: How to implement interprocess futex? */`: **flinux has no shared futex at all.** Our `g_fbk`
  shared arena (`thread.c:165`) and the `futex_key` shared-object canonicalisation (`thread.c:244`) have no
  counterpart, and neither do the other sixteen `MAP_SHARED` ledger arenas of `fork-model.md` §3.1 R2.
- **`jit_after_fork` (`cache.c:1694`) ↔ nothing.** §10.1.

---

## 7. Process creation outside fork

Worth recording because our port needs the same three shapes and flinux has all three:

- **Cold launch** is ordinary `main()`: parse args, init subsystems in a fixed order, `execve` the guest.
- **`execve` does not create a process.** It resets the memory manager (dropping all non-`NORESET` regions
  and their sections), resets TLS and the translator, and re-loads in place. This is why the fork-then-exec
  case is so cheap for flinux, and why the lazy design was tuned for it.
- **Self-relaunch for address-space repair** (§8.3) is a third shape: spawn a copy of self suspended,
  `VirtualAllocEx` a reservation into it, resume it, and exit. The original process exists only to prepare
  the successor's address space.

That third shape is worth naming because it generalises: **when you cannot fix your own address space, spawn
a successor and fix *its* address space before it runs.** It is the same primitive as PostgreSQL's
`pgwin32_ReserveSharedMemoryRegion` and it is available to us for any fixed-VA requirement.

---

## 8. Ordering constraints and failure modes — the hard-won knowledge

This section is the highest-value part of the study. Each item is something flinux got wrong first.

### 8.1 Kernel-mode writes into guest memory do not fault through the VEH

Three separate flinux changes are explained by one rule:

- `b475f1e` (2015-06-25) *"Populate child thread's stack beforehand"* — force-materialise the guest's child
  stack before `CreateThread`.
- `0a9e626` (2015-09-03) *"Directly copy stacks on fork instead of using CoW"*.
- `646123d` (2015-09-03) *"Use VirtualAlloc() to allocate MAP_STACK mmaps. This fixes a very mysterious crash
  on `git clone'."* The surviving code comment says: *"Windows shows strange behaviour when the stack is on a
  shared section object … it sometimes crashes when returning from a blocking system call."*

**Measured (`exp_flinux4.c`):**

| Operation targeting a `PAGE_NOACCESS` page, VEH installed | Result |
|---|---|
| user-mode store | **VEH fires**, repaired, continues |
| `ReadFile` writing into it | `ok=0 got=0`, **`ERROR_NOACCESS` (998)**, **VEH never fires** |
| `QueryPerformanceCounter` writing its out-param | **VEH fires** (written by user-mode code in kernelbase, not by the kernel) |

So the rule is precise: **a buffer written by the kernel on the caller's behalf yields an error status to the
caller; it does not trap into user-mode fault handling.** A demand-paged guest address space therefore needs
an explicit *"populate every syscall out-buffer before the call"* discipline, and any memory the OS itself
writes without our involvement — thread stacks above all — cannot be demand-paged at all.

**This applies to our port under every strategy**, including `RtlCloneUserProcess`, the moment we protect
guest pages for any reason (SMC tracking, `MADV_WIPEONFORK`, watchpoints). `fork-model.md` does not mention
it; the survey mentions it in passing at §4.3. It deserves to be a first-class constraint.

### 8.2 A view's granted access is a permanent ceiling on `VirtualProtect`

The author's comment says write protection *"can not be promoted afterwards"*. **Measured, and the true rule
is narrower and more useful:**

| View mapped with | Then | Result |
|---|---|---|
| `FILE_MAP_READ` | `VirtualProtect(PAGE_READWRITE)` | **FAIL**, `ERROR_INVALID_PARAMETER` (87) |
| `FILE_MAP_READ` | `VirtualProtect(PAGE_EXECUTE_READ)` | **FAIL** (87) |
| `FILE_MAP_ALL_ACCESS`, demoted to `PAGE_READONLY` | `VirtualProtect(PAGE_READWRITE)` | **OK** |
| `FILE_MAP_ALL_ACCESS\|FILE_MAP_EXECUTE`, demoted to `PAGE_NOACCESS` | `VirtualProtect(PAGE_EXECUTE_READWRITE)` | **OK** |

**You may freely demote and re-promote within the access the view was mapped with; you may never exceed it.**
That explains flinux's rule of always creating and mapping blocks `PAGE_EXECUTE_READWRITE` and only ever
demoting — and why duplicating a block needs a second, freshly-mapped writable alias of a view whose grant it
does not control.

**Direct consequence for us:** every section view the engine creates — JIT arena aliases, `MAP_SHARED` ledger
arenas, guest memory under strategy D — must be mapped with **the maximum access it will ever need**, even if
it starts read-only. Getting this wrong produces an `ERROR_INVALID_PARAMETER` from `VirtualProtect` much later,
far from the cause. `fork-model.md` §6.4 does not mention it.

### 8.3 Fixed absolute addresses were tried, shipped, and abandoned

Until 2015-04, flinux pinned **every** engine structure at a hard-coded virtual address: a documented memory
map with the MM struct, section-handle table, process data, VFS data, signal data, console data, TLS, startup
data, the fork mailbox, the kernel heap, the DBT block table and the DBT code cache each at its own constant.
Fork then just `VirtualAllocEx`'d each at the same address in the child.

Commit `a5864cd` (2015-04-15) deleted the entire scheme: *"Don't use fixed address allocation. This should
resolve issue 14."* Issue #14 is *"Crashes on Windows 7 64bit"* — an immediate `0xC0000005` at startup on
someone else's machine. The replacement is the bump arena + `MEM_TOP_DOWN` + patch-the-child's-pointer design
of §3.3–§3.4.

**This is direct field evidence for `fork-model.md` §9's ASLR concerns**, and it is a warning about strategy
B/D: any plan whose correctness depends on the engine's own structures landing at a chosen address will fail
on somebody's machine, for reasons involving third-party DLL placement that we cannot enumerate. flinux's
cure — *make the child's copy position-independent and patch the pointer* — is the right one wherever the
structure allows it.

The one address flinux could not make position-independent is the guest ET_EXEC base (`0x400000`), and its
handling is the self-relaunch of §7: check whether the range is free; if so reserve it; if it is already
exactly our own reservation, recognise ourselves as the relaunched successor; otherwise spawn a suspended
copy of self, reserve the range in *it*, resume it and exit. **One retry, not a loop.** Three independent
projects — flinux, PostgreSQL (100 retries), Midipix (32 retries) — converged on "reserve into a suspended
child", which is strong evidence the pattern is platform-forced rather than chosen.

### 8.4 Ordering hazards visible in the final code

- **The handle table is copied into the child *before* the parent's pages are demoted to read-only.** A peer
  thread writing guest memory between those two steps writes into a section the child already holds a handle
  to, and the write leaks into the child. The reader-lock protocol does not prevent it, because guest memory
  writes do not take the MM lock. See §8.5.
- **Failure after partial work leaves the parent damaged.** The abort path terminates the child but does not
  undo protection demotions already applied to the parent's own pages, nor the child's pid slot in the shared
  process table.
- **A `PAGE_NOACCESS` page inside an eagerly-copied region is silently skipped**, with an in-source
  `FIXME: How to handle this case?`. A guest `PROT_NONE` guard page inside such a region is not replicated.
- **COW ownership is decided by a system-wide handle count.** An unrelated descendant, a `DuplicateHandle`,
  or an exited-but-unreaped child inflates it and forces a spurious 64 KiB copy; and the test is inherently
  racy against a concurrent fork.
- **One shared-region hook has an unbalanced early return** that leaves its matching parent-side release
  unpaired.
- **Two loops are sized by the 64-bit block count.** Process shutdown iterates 2³² block slots, and the fork
  path iterates 2¹⁹ handle-table slots per fork. Neither is plausible in a program that was actually run on
  x64 (§11).

### 8.5 Fork and threads

flinux **does** support threads: `clone(CLONE_THREAD)` → `CreateThread` (§4.1), plus a permanent dedicated
signal thread created at startup. **flinux is therefore never single-threaded**, even for a
`main(){fork();}` guest.

- **The child is always single-threaded**, by construction — it is a fresh process. That is POSIX fork
  semantics for free, and it is the same property `RtlCloneUserProcess` gives (survey §3.1).
- **The parent is never stopped.** There is no suspend-all and no equivalent of `RtlCloneUserProcess`'s
  drain-the-thread-pool / take-the-loader-and-heap-locks step. Quiescence is attempted purely with reader
  locks and one critical section.
- **That is not sufficient, and the author knew it.** The project's own wiki TODO list carries, under
  intermediate tasks: *"Fix data races. There might be many when forking in a multi-threaded program."* The
  last commits before the project stopped include *"vfs: Fix fork deadlock when sharing sockets"* and
  *"Socket forking support (untested)"* (both 2016-02-26).

**Read against `fork-model.md` §9 risk 2, this is a warning, not a reassurance.** flinux is not evidence that
user-mode fork is safe from a multithreaded parent; it is evidence that it is *racy in ways its own author
flagged and never fixed*. Our engine is multithreaded exactly where it matters — `cache.c:1697-1705` names
the production hang. `RtlCloneUserProcess` at least documents what it drains and which locks it takes, and
ships `RtlUpdateClonedCriticalSection`/`RtlUpdateClonedSRWLock` for what it cannot.

---

## 9. Git archaeology: what was tried and abandoned

54 commits touch the fork file; 60+ touch the memory manager. The history records four arcs.

### 9.1 2014-07 → 2014-12: getting it to work at all

`fd1862b` (2014-07-31) *"Initial implementation of fork()"*; `130d421` *"vfs across fork() boundary. Still
buggy."*; then a burst in late December — `94800a2` *"Fix many bulding blocks for fork()"*, `1a8e33f` *"fork:
Make fork() work."*, `3d850f1` *"fork: Fix fork() and vfork()."*. The x64 work is contemporaneous and
explicitly fork-motivated: `992ad03` *"Initial x64 stuff"*, `15dab3f` *"Various fixes for x64 which allows a
successful fork()"*, `e48d1b6` *"Resolve the x64 ET_EXEC executable address space collision issue"* (§8.3).

### 9.2 2015-06 → 2015-07: the decomposition is born, and it is about locks

`15afd2d` (2015-06-17) *"mm: Add locks for multi-threading"*, then `de35193` (2015-07-06)
**"Threadsafe forking."** — the commit that creates the pattern the survey named. Before it there was a
single child-only `*_afterfork()` per subsystem plus one ad-hoc `tls_beforefork()`. `de35193` renames every
`*_afterfork()` to `*_afterfork_child()`, adds `*_fork(hProcess)` and `*_afterfork_parent()` for heap,
signal, process and TLS, and the *entire* body of each new pair is `AcquireSRWLockShared` / `ReleaseSRWLockShared`.

**So the `*_fork` / `*_afterfork_parent` decomposition was introduced as a lock-bracket protocol, not as a
state-transfer protocol.** Our `hl_linux_abi_fork_prepare`/`_parent`/`_child` triple was introduced to
transfer state. The names rhyme; the intent does not.

### 9.3 2015-06 → 2015-09: a steady retreat from COW for engine-owned memory

- `f9e7ee6` (06-25) *"Use explicit copying on fork instead of CoW on heap."*
- `7630ffe` (06-25) *"mm: Use copying on fork for static allocation area."*
- `0a9e626` (09-03) *"Directly copy stacks on fork instead of using CoW."*
- `6d90c4c` (09-03) renames the flag from "copy on fork" to "VirtualAlloc", finishing the retreat.
- `a7c0881` (09-19) *"Remove unused COPYONFORK logic."*

**Every piece of engine-owned memory was moved off COW and onto eager copying within three months.** COW
survived only for *guest* memory. The lesson generalises: user-mode COW is fine for large, sparsely-touched,
guest-owned regions and is not worth its complexity for small, dense, engine-owned ones.

### 9.4 2015-09-19: a one-day fork-latency campaign, and what it cost

Four commits, one afternoon:

| Commit | Message | Gain |
|---|---|---|
| `2602f9d` | *"Use native NtWriteVirtualMemory()."* | — (avoids the Win32 wrapper) |
| `981ae7f` | *"Cut half time of section mapping in mmap_fork()."* | **20 %** |
| `a8f03d9` | *"Defer CoW memory mapping."* | **30 %** |
| `b26a480` | *"Lower MAX_MMAP_COUNT from 65536 to 1024."* | **10 %** |

`a8f03d9` is the birth of the detached-block design. `981ae7f` is where block duplication acquired its
unmap/re-alias shape (§5.4), replacing an earlier `VirtualProtect`-based hack the author had marked
*"TODO: Find a better way"*.

**`b26a480` is the one worth staring at.** The guest's maximum number of memory regions was cut from 65 536
to **1 024** — below Linux's default `vm.max_map_count` of 65 530 — to buy 10 % of fork latency. It is a
direct trade of guest capability for fork speed, and it is forced by the design: the VMA table is a
fixed-size array inside a struct that fork copies wholesale, so **the size of any eagerly-copied engine
structure is a per-fork tax.** On a 64-bit build the residual tax is still ~1 MiB per fork from the block
occupancy counters alone (measured `WriteProcessMemory`: **59 µs/MiB**).

**This coupling exists under our strategy B (checkpoint/restore) too, and is worse there** — every byte of
engine state that must cross is per-fork latency. It is an argument for strategy A that
`fork-model.md` does not currently make.

### 9.5 One reverted experiment

`7d55738` *"dbt: Never tamper user stack."* (2015-01-29) and `86d57f6` reverting it **the same day**. The
translator's need to use the guest stack for its own trampolines could not be removed. Recorded because it is
the one place the history shows an attempt to decouple engine state from guest memory failing outright.

---

## 10. What flinux does not solve for us

### 10.1 It has a JIT — and it throws the cache away

The child runs a **cold** translator init. There is no `dbt_fork`, no `dbt_afterfork_parent`, no
`dbt_afterfork_child` anywhere in the tree. Three properties make that trivially safe for flinux and not for
us:

1. **The translator state is `__declspec(thread)` — per host thread, never shared** (`x86.c:550`). Every
   guest thread gets its own 8 MiB code cache and 8 MiB block table (`x86.c:480-481`, `:765-769`). No
   cross-thread cache coherence problem, hence no cross-fork one.
2. **It is flat RWX** (`PAGE_EXECUTE_READWRITE`, `x86.c:768`). No dual RW/RX alias, so nothing analogous to
   `hl_arena_repair`, no `VM_INHERIT_NONE` hole, no re-coupling step. The bug class documented at
   `cache.c:1665-1677` cannot arise.
3. **It is outside the block-managed address space** — raw `VirtualAlloc(MEM_TOP_DOWN)`, never in a section,
   never inherited.

Our extra burden, precisely:

- **The dual-mapped W^X arena.** `cache.c:1665-1673`: the RX alias is `VM_INHERIT_NONE` and must be
  re-coupled in the child at the same VA or the two views silently diverge. The Windows shape is available
  (survey §2.2 measured all three patterns working) but `hl_arena_repair` has no Windows body. flinux offers
  zero guidance.
  **Note the interaction with §8.2:** whichever section backs the arena, both aliases must be mapped with the
  maximum access they will ever need, or a later `VirtualProtect` fails.
- **The `MAP_SHARED` ledger arenas.** Seventeen (`fork-model.md` §3.1 R2). flinux's `shared_fork` /
  `console_fork` pattern — `NtMapViewOfSection` into a suspended child at the parent's address — *is*
  directly reusable and is strategy D working in 2015. But flinux maps two such things and has no futex, no
  eventfd, no fdvis, no task-state, no cgroup accounting, no ptrace arena, no sigexit relay. **The mechanism
  transfers; the scale does not.**
- **The translation cache as inheritable state.** `jit_after_fork` already sets `preserve = 0` on Linux
  (`cache.c:1719`), so on this axis we are *already where flinux is*, and `fork-model.md` §3.2 N1 is right
  that the hardest-sounding item is the optional one. What is not optional is that the child end up with a
  consistent arena at a usable VA — which needs a working `hl_arena_repair`, which flinux never needed.

### 10.2 Other gaps

- **Self-modifying code.** Code-cache invalidation is driven only from `munmap` of a `PROT_EXEC` region, and
  its response is to flush the *entire* cache. No write-fault interception on translated guest code. The
  wiki TODO lists SMC support under *hard* tasks. Our x86-64 transliterator's refusal posture and the
  interpreter's re-decode (survey §4.3) are both strictly stronger.
- **1 024 VMAs and 1 024 fds** are hard ceilings (§9.4).
- **No interprocess futex** (`futex.c:32`).
- **No `msync`, no shared file mappings** — file-backed `mmap` is `pread` into anonymous memory.
- **32 GiB of address space** is reserved for the section-handle table on a 64-bit build (block count 2³²
  × 8 bytes). Measured to succeed in 2 µs, but it is a direct tax on the guest address space.

---

## 11. Reliability, limits, and why the project stopped

**Last commit 2016-03-29.** Open issues at abandonment are compatibility, not architecture (#83 gtk3, #96
quick start, #103 image site). Closed crash issues #14 and #20 are both 64-bit.

**Fork was not what defeated it.** The author's own TODO list puts *"Make dbt dual x86/x64 compatible and then
work towards full x64 support"* under **hard tasks**, and the source agrees: the translator contains **zero**
`_WIN64` conditionals, uses 32-bit-only FS-relative intrinsics, and its guest register-file struct is ten
32-bit fields. The 64-bit configuration in the project file was aspirational. The 2³²-iteration shutdown loop
(§8.4) is proof the x64 build was never run to completion. **The project ran out of author while still
32-bit.** No maintainer statement of abandonment was located; this inference rests on the commit log, the
TODO list, and eight years of silence.

Nothing in the record says fork was abandoned, reverted, or found unworkable. What the record *does* say is
that fork was never made thread-safe (§8.5) and that its cost was fought for a whole afternoon in 2015 and
still cost a guest-visible capability (§9.4).

---

## 12. Verdict

### 12.1 Adopt

1. **Inherited-handle-value identity as a bulk transfer channel** (§5.1). Measured, cheap, and it turns a
   per-object chore into a table copy.
2. **`NtMapViewOfSection(section, hChild, &addr, …)` into a suspended child** for the `MAP_SHARED` ledger
   arenas (§3.2). This is `fork-model.md` §6.4 strategy D with a working precedent.
3. **Patch the child's pointer instead of demanding the same address** (§3.3 step 2), wherever the structure
   allows it. Removes ASLR exposure at essentially zero cost.
4. **`WSADuplicateSocketW` + `WSASocketW(FROM_PROTOCOL_INFO)`** for sockets. Same as PostgreSQL and libuv.
5. **Reserve-into-a-suspended-successor** for any fixed-VA requirement, and the "spawn a successor and fix
   *its* address space" shape generally (§7, §8.3).
6. **Arm the VEH before anything else in the child** (§4 C3).
7. **The fault-handler-as-`EFAULT`-landing-pad trick** (§5.3), which gives zero-cost guest-pointer validation.
8. **One register-file-restore trampoline shared by fork-child and thread-child** (§4.1).

### 12.2 Adapt

9. **COW via `PAGE_WRITECOPY` views instead of manual duplication** (§5.5): 1.24 µs/4 KiB kernel COW against
   48 µs/64 KiB manual, with correct cross-process semantics and in-place re-mapping. Design for the
   re-anchoring obligation at the next fork.
10. **The prepare/parent/child bracket.** We already have the better version. flinux confirms the shape holds
    when the hooks must *build* the child rather than repair it.

### 12.3 Reject

11. **One NT section per 64 KiB block for the whole guest address space.** 1 024 VADs per 64 MiB (measured),
    a 32 GiB handle-table reservation, a 1 024-VMA ceiling, block-granular COW, no file mappings, no
    sub-block unmap. Its central justification — 145 µs per block map — **is 2.03 µs on this host**.
12. **Per-thread private code caches.** 16 MiB/thread is not affordable at our arena sizes.
13. **Reader locks as the fork quiescence protocol** (§8.5). The author's own TODO says it is racy.
14. **Trading guest capability for fork latency** (§9.4). If an eagerly-copied structure is on the fork path,
    fix the copy, not the structure's size.

### 12.4 Easier or harder than `RtlCloneUserProcess`?

**Harder — and the study sharpens rather than weakens `fork-model.md`'s ranking.**

flinux proves driver-free user-mode fork ships. But what it proves works is a fork for a personality whose
entire address space is *already* an explicitly managed table of 64 KiB sections it created itself. That
precondition **is** the design. We do not have it, and acquiring it means rewriting
`src/linux_abi/syscall/mem.c`, the anon tracker, the logical-VMA snapshot layer and `hl_gmap` onto a
block-and-section allocator — then accepting a 1 024-VMA-class ceiling, a 32 GiB reservation, block-granular
COW at ~48 µs a block, no shared file mappings, and per-block VADs. And after all that, the mechanism still
would not move the engine's own C heap, `struct cpu`, the dual-mapped arena, or seventeen `MAP_SHARED`
ledger arenas — every one of which `RtlCloneUserProcess` moves for free, at the right VA, by construction.

`RtlCloneUserProcess` measured **2.90–3.46 ms from a 10-thread parent** on this box (`fork-model.md` §5.1),
which is the same order as flinux's unavoidable `CreateProcess` floor (survey §2.3: 1.54 ms bare, 5.05 ms
round-trip) *before* flinux does any of its own work. **flinux is not cheaper and is enormously more
invasive.** Strategy A stays primary.

flinux does inform the two open questions:

- **Do `MAP_SHARED` section views survive a clone as `ViewShare`?** flinux does not answer it — but its
  `shared_fork` shows the fallback (map them explicitly into the child) is a small, proven pattern, so a
  negative answer is survivable. **Still measure it first.**
- **Threads in a clone.** flinux offers nothing. Its child is always single-threaded and its *parent's*
  threading is a known race. This risk is untouched.

### 12.5 Versus checkpoint/restore

`fork-model.md` §6.2's fallback is the **better** fallback than a flinux-shaped rebuild, on both axes.

**Complexity.** Checkpoint/restore already **exists and is tested**: 82/82 `checkpoint`, 34/34
`checkpoint-io`, 11/11 `ckpt-cross` (`docs/amd64-host.md` §1); `checkpoint.c:5391` already re-forks a whole
process tree; documented APIs only. A flinux-shaped path is a from-scratch memory manager in the most
load-bearing subsystem in the tree, with a per-block section design we would then be stuck with everywhere,
not just at fork.

**Robustness.** Checkpoint/restore reconstructs into a *fresh* engine, so it captures exactly the
guest-visible state — which, per `fork-model.md` §3.3, is all a fork child needs. It has one known defect
(`g_gro`, `amd64-host.md` §7) and no undocumented dependencies. The flinux path carries an unfixed
multithreaded-fork race the author documented, a racy handle-count ownership test, a `FIXME` on
`PAGE_NOACCESS` replication, an unbalanced lock release, and 64-bit loops proving the configuration was never
run.

**Cost.** Checkpoint/restore is ~55 ms per 256 MiB (`fork-model.md` §5.3). flinux is ~1.5 ms `CreateProcess`
+ ~65 µs of eager bookkeeping + ~196 ms of lazy COW faults *if the child dirties all 256 MiB*. So flinux wins
**only** for a child that touches little — the `fork`+`exec` case, which is exactly what its laziness was
tuned for. For a child that runs, the two converge and the checkpoint path is the one already tested.

**Keep the ranking:** A primary, B fallback and differential oracle, D for the `MAP_SHARED` arenas under
either.

### 12.6 What `fork-model.md` has not considered

Seven items, in rough order of how much they would change the plan:

1. **`PAGE_WRITECOPY` views make strategy D lazy** (§5.5). `fork-model.md` §6.4 describes strategy D as
   placeholders + `WriteProcessMemory`, priced at ~55 ms/256 MiB. Mapping WRITECOPY views instead makes the
   kernel do the copying, per 4 KiB, at 1.24 µs, only for pages actually touched. **This materially improves
   the ranked cost of both B and D and should be measured properly before Phase 2.**
2. **A view's granted access permanently caps `VirtualProtect`** (§8.2, measured). Every section view we
   create — JIT aliases, ledger arenas, guest memory — must be mapped with the maximum access it will ever
   need. Not currently stated anywhere in our docs.
3. **Kernel-mode writes do not fault through the VEH** (§8.1, measured). Constrains any scheme that protects
   guest pages, under *all four* strategies, and dictates a populate-before-syscall discipline if we ever
   demand-page guest memory.
4. **Handle *values* survive inheritance** (§5.1, measured). `fork-model.md` §5.1 notes only that handles
   must be marked inheritable. Value identity permits bulk table transfer and is a cheaper C4 answer than
   per-descriptor `DuplicateHandle`.
5. **The eager-copy ↔ fork-latency coupling** (§9.4). Any engine structure that must cross the process
   boundary is a per-fork tax; flinux paid it by shrinking a guest-visible limit. This is a quantitative
   argument for A over B that §6 does not make.
6. **`fork_init`-before-everything ordering** (§3.1). A `CreateProcess`-based child must detect that it *is*
   a child before any allocator runs. If we ever implement strategy B, this is the first line of our `main`.
7. **The child's pid must be allocated by the parent** (§3.2 P3), under a cross-process lock, and pushed in.
   Relevant to `fork-model.md` §9's pid-map note: whatever pid mapping we adopt, the allocation point is in
   the parent, before resume, not in the child.

---

## 13. Unknowns

- **flinux was never built or run.** It targets 32-bit MSVC/MASM and does not build under mingw-clang. Every
  behavioural claim here comes from reading source and history; every number comes from independent probes of
  the *primitives* it uses.
- **Whether the `PAGE_WRITECOPY` result (§5.5) holds** under memory pressure, on a section under concurrent
  RW access from a third process, or across a `CreateProcess` boundary with the section already partially
  privatised. The probes covered single-writer and simple cross-process cases only.
- **Whether 2.03 µs/block holds on a process with a large existing VAD tree.** Probes started clean.
- **Whether the view-access ceiling (§8.2) applies identically to `NtMapViewOfSection` with an explicit
  `Win32Protect`** as it does to `MapViewOfFile`'s desired-access mask. Only the Win32 spelling was tested.
- **Why exactly the project stopped.** No maintainer statement located; §11 is inference.
- **Whether flinux's x64 configuration ever executed a guest.** The evidence says no; nothing states it.
