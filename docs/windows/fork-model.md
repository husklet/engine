# The process/fork model on a Windows host

One document for the hardest blocker of the Windows port: what the engine actually needs from `fork()`,
which of those needs are the Linux guest ABI's and which are only the engine's own launch convenience, what
Win32/NT can and cannot supply, and what a staged port loses at each stage.

`DOCS.md` is normative. This file is the record for one decision.

Numbers marked **measured** were produced on this box (Windows 11 Pro, build 10.0.26200, x86-64). Everything
else is marked as reasoning or as unverified. Line numbers drift; every claim below was checked against the
tree at the time of writing.

Companion documents, written in parallel and cross-referenced rather than duplicated here:
`docs/windows/prior-art-cygwin-fork.md`, `docs/windows/prior-art-cygwin-threads-signals.md`,
`docs/windows/prior-art-survey.md`.

---

## 1. Status

Nothing is built. `src/host/windows/` contains one file, `README.md`, and it says so. `flake.nix:49-53`
carries `windows = { supported = false; }`. There is no CMake arm, no toolchain file, no CI lane, and no
`mingw` string anywhere in the tree. `include/hl/base.h:7` is the only Windows-aware line of C.

So this document is a design, not a report on an implementation. What it does contain that is not design is
§5: a set of measurements of the NT cloning primitives taken on this machine, because the load-bearing
question — *does `RtlCloneUserProcess` actually work on modern Windows from a multithreaded parent* — is
answerable in an afternoon and was not worth taking on trust.

---

## 2. Three unrelated things are called "fork" in this tree

Separating them is the single most useful thing in this document, because they have **different
requirements, different costs, and different solutions**, and conflating them makes the port look
impossible when only one third of it is hard.

### 2.1 Engine launch — `spawn_cloned` / `spawn_prepared`

`include/hl/host_services.h:444-459`:

```c
/* Run an already-loaded entry in an isolated clone of the current process. */
hl_host_result (*spawn_cloned)(void *context, hl_host_process_entry entry, void *entry_context);
...
/* Consume a fork bracket previously acquired through sync.fork_prepare. */
hl_host_result (*spawn_prepared)(void *context, hl_host_process_entry entry, void *entry_context);
```

Two production call sites, and only two:

- `src/core/lifecycle.c:133` — `spawn_cloned`, taken when `box == NULL`.
- `src/linux_abi/linux_abi.c:606` — `spawn_prepared`, inside `hl_linux_abi_spawn`, taken when `box != NULL`.

Both run `hl_production_entry` (`lifecycle.c:76`), which calls `hl_run_linux_guest` — **a full cold load and
run**. The child inherits nothing warm. It inherits four things it uses:

1. the `hl_engine_config` / `hl_options` / `hl_linux_abi *box` the parent built, as ordinary C pointers;
2. the shared child-result page mapped at `lifecycle.c:116-118` with `HL_HOST_MEMORY_SHARED`;
3. the parent's fd table (for `box != NULL`, an explicitly cloned one — see below);
4. a waitable process identity for `process->wait` / `terminate` / `close`.

`spawn_prepared` differs from `spawn_cloned` only in that `hl_linux_abi_spawn` has already run
`hl_linux_abi_fork_prepare` (`linux_abi.c:286`), which snapshots every live OFD, pins it, calls
`files->clone_for_fork` per handle, and re-validates a bijection under lock before committing
(`linux_abi.c:381-412`). **That is an explicit, per-descriptor handle transfer that already exists.** It does
not rely on fork's implicit inheritance at all; fork merely happens to be underneath it.

**This use of fork is a launch mechanism, not a semantic.** It needs a fresh process, a shared page, a
waitable handle, and a way to move a fixed, enumerable set of descriptors. `CreateProcess` +
`DuplicateHandle` + a named/inherited pagefile section supplies all four, arguably more cleanly than fork
does. **This is not a blocker.**

### 2.2 Launch acceleration — the resident forkserver

`src/linux_abi/fork.c`. Its own header comment states the purpose: amortise "the irreducible per-process
`posix_spawn` + dyld + codesign-validation floor of the engine ITSELF (opt8 measured ~2 ms of a ~3-5 ms
launch)". A resident parent pays engine init and optional pre-translation once, then `fork()`s a COW worker
per launch (`fork.c:561`, runner at `fork.c:219`).

**It is a pure optimisation with no correctness content.** Removing it costs launch latency and nothing else.
Its only test coverage is `compat.process-forkserver-{aarch64,x86_64}`, two ctest cases registered at
`cmake/Phase3Compat.cmake:576-594` **only when `CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"`** — the Linux lane
never runs them.

Note that this component does the *opposite* of a zygote: it forks after maximal warm-up, deliberately, so
that the child inherits a hot arena. See §7.

### 2.3 Guest `fork()` / `clone()` / `vfork()` — the real one

`src/linux_abi/syscall/proc.c:1787` (`case 220`, clone) and `proc.c:2496` (`case 435`, clone3). The host
primitive is a **plain libc `fork()`** at `proc.c:1823` and `proc.c:2528`. Not `posix_spawn`, not
`host->process->spawn_*`. x86-64 guests never reach a `fork`/`vfork` case at all: the translator rewrites
syscalls 57 and 58 into `clone` at `src/translator/guest/x86_64/legacy.c:408-422`.

`src/linux_abi/thread.c:150-153` states the design assumption in one sentence:

> `hl's fork() is a real host fork(): the child inherits the identical guest address space, so a
> shared-memory futex word resolves to the SAME host address in parent and child`

**This is the only one of the three that the Linux guest ABI requires.** It is irreducible: a Linux program
that calls `fork()` is promised a child with a copy-on-write duplicate of its memory, a duplicate fd table
sharing file offsets, and independent divergence thereafter. There is no way to decline that and still claim
a Linux ABI.

---

## 3. Inventory: what the child actually needs

Read out of `syscall/proc.c:267-324` (`fork_child_hooks`), `translator/cache.c:1694`, `thread.c:2246`,
`core/lifecycle.c`, and the arena allocation sites.

The most useful way to read `fork_child_hooks` is by counting. It has **25 steps. Roughly twenty of them
destroy or rebuild inherited state.** Fork gives the engine four things it wants and about twenty it has to
undo. A Windows child-creation primitive only has to deliver the four.

### 3.1 Required — the guest ABI is wrong without it

**R1. The guest address space, copy-on-write, at identical host virtual addresses.**
Not merely "a copy" — *at the same VAs*. Three independent reasons:

- The futex bucket table is hashed **by the host VA of the guest word** (`thread.c:148-157`). A
  fork-inherited `MAP_SHARED` anonymous page must land at the same VA in both processes or a `FUTEX_WAKE` in
  one never matches a `FUTEX_WAIT` in the other. The engine already had to add a shared-object key
  (`futex_key`, `thread.c:244`) for *file-backed* `MAP_SHARED`, and the comment at `thread.c:216-220`
  records the observed failure when the key was wrong: a command-buffer flush that never woke its peer.
- Every arena in R2 is reached through a pointer global (`g_fbk`, `g_eventfd_count`, `g_poslk`, `g_shkey`,
  …). Those globals are copied verbatim into the child. If the arena is not at that address in the child,
  the child dereferences a wild pointer.
- `struct cpu` — the guest register file, and the checkpoint format — is a host heap or stack object the
  child continues to use.

**R2. The `MAP_SHARED` arenas must be the *same physical pages* at the *same VA* in parent and child.**
Every one of these is created once, before any guest fork, through
`hl_linux_shared_create` → `map_anonymous(..., HL_HOST_MEMORY_SHARED)` → `MAP_SHARED|MAP_ANONYMOUS`
(`src/host/linux/host.c:528`):

| Arena | Site | Guest-visible thing it implements |
|---|---|---|
| shared futex buckets `g_fbk` | `thread.c:165` | cross-process `FUTEX_WAKE`/`WAIT`, pshared semaphores |
| eventfd counters + nonblock flags | `container/vfs.c:333` | `eventfd(2)` across fork |
| fdvis | `container/vfs.c:492` | `/proc/<pid>/fd` identity |
| task-state | `container/vfs.c:870` | the `R`/`S` char in `/proc/<pid>/stat` |
| memfd registry | `container/vfs.c:1348` | `memfd_create` |
| seq-ref / udp-ref | `container/netns.c:1679`, `:1682` | `SOCK_SEQPACKET` and AF_UNIX endpoint refcounts |
| acct / cgroup slots | `container/state.c:136` | `pids.current`, `memory.current` |
| sigexit relay | `signal.c:311` | `WIFSIGNALED` reconstruction in `wait4` (`proc.c:2405-2408`) |
| ptrace arena | `syscall/ptrace.c:157` | `ptrace(2)` |
| filemap events | `thread.c:600` | — |
| sentry ring | `sentry.c:1647` | sentry mode |
| POSIX record locks + flock broker `g_poslk` | `syscall/helpers.c:283` | `fcntl` locks, `flock` |
| owner table | `container/owner.h:84` | namespace object ownership |
| fdcache resolver epoch | `fdcache.c:196` | — |
| engine-private fd registry | `src/host/private.c:149` | engine fds hidden from the guest |
| provider demux ring | `src/core/provider/demux.c:319` | — |
| engine child-result record | `src/core/lifecycle.c:116` | the child's exit code/signal |

`futex_private_table_after_fork` (`thread.c:193-209`) is explicit about the split: it rebuilds the *private*
table in place and leaves the shared one alone, because "the shared table must retain its cross-process
waiters".

**R3. The host descriptor table, with shared file offsets.**
Linux fork duplicates fd numbers onto the same open file descriptions. The engine depends on both halves of
that in guest-visible ways: `flock_broker_after_fork` (`helpers.c:398`) adds the child as a holder on each
active broker record *because* `flock` is OFD-scoped and therefore inherited; `poslk_after_fork`
(`helpers.c:276`) zeroes `g_mypid`/`g_i_locked` *because* POSIX record locks are explicitly **not**
inherited. The vfork rendezvous pipe (`proc.c:1814-1830`) and the CLONE_PIDFD fd (`proc.c:1863-1875`) are
also plain inherited descriptors.

**R4. The engine's C heap and all globals, byte-identical, at identical addresses.**
This is implied by R1 but deserves separate billing, because it is what makes "re-exec and replay" hard: the
engine's own state is a pointer graph — logical-VMA snapshots, the anon tracker, `g_gmap`/`g_gna`, the
fdcache, `struct loaded`, `hl_options` — with no serializer other than checkpoint/restore (see §6.2).

**R5. Signal dispositions and the guest signal state**, minus the resets `fork_child_hooks` performs.

### 3.2 Not required — convenient, or actively unwanted

**N1. The JIT code arena. This is the surprise, and it is load-bearing.**
`jit_after_fork` (`translator/cache.c:1694`) sets `preserve = 0` on a Linux host **by default**
(`cache.c:1719`), with a comment saying a Linux fork child *must not* resume the parent's copied
translations — "a second fork after any completed child otherwise executes a corrupted libc return path and
trips `__stack_chk_fail`". Preservation survives only for the fork-server runner (`g_fsrv_preserve`,
`cache.c:1723`) and single-threaded x86-with-pcache (`cache.c:1737`). On macOS it is
`preserve = !g_threaded || !g_dualmap` (`cache.c:1740`).

So on the host family the Windows port most resembles, **the child already throws the parent's translations
away and re-translates on demand.** The dual-mapped W^X arena, the RX alias, `hl_arena_repair`, the block
map, the IBTC — none of it has to survive child creation. What *does* have to happen is that the child ends
up with a consistent arena at a usable VA and cleared block maps, which is exactly what
`hl_arena_repair(..., 0)` + `map_clear()` + `ibtc_clear_lazy()` + `pend_reset()` (`cache.c:1752-1772`) do.
The hardest-sounding item on the list is the one that is already optional.

**N2. The parent's thread registry — actively harmful.** `thread_after_fork` (`thread.c:2246-2289`) memsets
`g_threg` and reinstalls a single entry. The comment at `thread.c:2269-2277` names the damage phantom
entries cause: poisoned `tgkill`/`tkill` routing (Go's async preemption spinning a goroutine at 100%) and a
`~10 s` busy-wait ceiling per execve teardown, "(~14 s PER compile child, measured)".

**N3. Every engine mutex.** `jit_after_fork` re-inits `g_jit_lock` and `g_cache_lock`; `thread_after_fork`
re-inits `g_threg_m`, `g_filemap_lock`, `g_filemap_replay_lock`, `g_shkey_m`; `sysv_after_fork`,
`eventfd_after_fork`, `anon_after_fork`, `event.c:715` each re-init theirs. `cache.c:1697-1705` names the
bug this exists for: "**This is THE go/npm/cargo build hang**". The child wants these locks *reset*, not
inherited — so a primitive that cannot faithfully copy lock state is not thereby disqualified.

**N4. `kqueue`/epoll/timerfd/inotify objects.** `kqueue_rebuild_after_fork` (`syscall/event.c:623`) already
rebuilds them at the same fd numbers on macOS, re-arming timerfd deadlines and re-registering inotify
vnodes. **A working reconstruction path for these already exists and is tested.**

**N5. Path/metadata caches and `DIR*` handles.** Dropped (`proc.c:304-305`).

**N6. The forkserver's warm image and prewarmed arena.** Speed only.

**N7. The private futex table's contents, POSIX timers, dnotify registrations, `mlock` locks, semadj,
`MADV_WIPEONFORK` ranges, `MADV_DONTFORK` ranges.** All reset or re-derived by hooks 14–25.

### 3.3 The one requirement that is easy to over-state

`fork()` returns twice into the middle of the engine's own C call chain, several frames deep inside the
dispatcher's syscall handler. It is tempting to conclude that any strategy which cannot resume a host thread
mid-C-stack is disqualified.

**It is not that strong.** The child's continuation is: run `fork_child_hooks(c)`, set `G_RET(c) = 0`, and
return into the dispatcher loop. Nothing guest-observable depends on the *host* frames between `run_guest`
and the syscall case. A strategy that can re-enter `run_guest(c)` fresh, with a correct `struct cpu` and
correct memory, is observationally equivalent. That materially raises the ceiling for §6.2.

What is *not* recoverable that way is the C stack of **peer guest threads**, but those must be destroyed
anyway (N2) — Linux `fork()` gives the child exactly one thread.

---

## 4. Evaluation criteria

Applied uniformly in §5 and §6.

| | |
|---|---|
| **C1** | Reproduces a guest-visible `fork()`: COW duplicate of guest memory, divergent thereafter |
| **C2** | Identical host VAs for guest memory, the engine heap, and the arena globals (R1, R4) |
| **C3** | The `MAP_SHARED` arenas stay *shared*, not copied (R2) |
| **C4** | Inherited descriptors with shared file offsets (R3) |
| **C5** | The child can subsequently create host threads — needed for guest `clone(CLONE_THREAD)` after a fork (`thread.c:2676`) |
| **C6** | Cost per child |
| **C7** | Robustness: documented vs undocumented API, breadth of field use |

---

## 5. Measurements of the NT cloning primitives on this box

No C toolchain is installed here, so the probe is C# compiled with
`C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe` (`/unsafe /platform:x64`) and P/Invoke.
The child path is pre-JITted and pre-warmed in the parent, allocates nothing managed, and reports through
raw `WriteFile` on an unbuffered `FILE_FLAG_WRITE_THROUGH` handle opened before the clone.

**Caveat, stated up front: the parent is a CLR process.** That makes it a *more* hostile host than the
engine will be, so a success generalises well and a failure generalises poorly. Where a failure is reported
below, the mechanism was isolated further rather than left as "it crashed".

Windows 11 Pro, build **10.0.26200**, x86-64.

### 5.1 `RtlCloneUserProcess` works, and the clone runs real code

| Observation | Value |
|---|---|
| `RtlCloneUserProcess(flags, NULL, NULL, NULL, &info)` status in parent | `STATUS_SUCCESS` |
| status in the clone | `0x00000129` = `STATUS_PROCESS_CLONED` |
| parent thread count at clone time | **10** (4 explicit native threads + CLR) |
| clone latency, parent side | **2.90 – 3.46 ms** over 9 runs |
| clone executes user code after the call | **yes** — reported its own pid/tid, matching the parent's `RTL_USER_PROCESS_INFORMATION` |
| `CreateFileW` in the clone | **succeeded**, handle `0x10` |
| clone termination | `NtTerminateProcess(NtCurrentProcess(), 0x5A)`; parent's `GetExitCodeProcess` read back `0x5A` |
| 4 M-iteration arithmetic loop in the clone | completed, result written |

So C1/C2 are satisfied in the strong form, **including from a 10-thread parent**, which is the case the
literature is most pessimistic about.

**Handle inheritance is not automatic.** With `flags = 0` the clone ran but produced no log output at all,
because the evidence handle was not inherited. With `RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES` (`0x2`) and
`SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)` the same handle worked in the clone.
**C4 therefore requires that every descriptor the engine intends the child to see be created inheritable.**
That is a real, pervasive constraint on the whole Windows file backend, not a fork-local one.

### 5.2 Thread creation in a clone: the one hard failure, and its exact mechanism

| Attempt in the clone | Result |
|---|---|
| `kernel32!CreateThread` | returns a handle (`0x14`) and a TID, then the **process dies with `0xC0000005`** before the caller's next log line. 4/4 runs. |
| `NtCreateThreadEx(..., CreateFlags = 0)` | `STATUS_SUCCESS`, handle `0x14`, then **`0xC0000005`**. |
| `NtCreateThreadEx(..., CreateFlags = 0x2)` — `THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH` | `STATUS_SUCCESS`; the thread **ran to completion**; `WaitForSingleObject` returned `WAIT_OBJECT_0`; the clone then finished its remaining work and exited `0x5A`. |

That is a mechanism, not a symptom: **the access violation is in the DLL thread-attach path**
(`LdrpInitializeThread` → `DLL_THREAD_ATTACH` → per-module `DllMain` and TLS callbacks), and skipping it
makes thread creation work. It is consistent with the documented fact that some section views are mapped
`ViewUnmap` rather than `ViewShare` during a clone — CSR shared memory, the CSR port heap and the GDI shared
handle table are *absent* in a clone — so a module's thread-attach that touches `BaseStaticServerData` finds
a hole.

**Which module faults was not identified**, and one of the loaded modules here is the CLR. A mingw-w64
engine loads far fewer DLLs with far simpler thread-attach handlers, so plain `CreateThread` may well work
there. **This must be re-measured with a native mingw binary before any plan depends on either answer.**

The escape hatch has its own price: `SKIP_THREAD_ATTACH` also skips **TLS callbacks**, so `__thread`
variables are not initialised in such a thread. The engine uses thread-locals structurally — `g_fbk_active`
(`thread.c:160`), the interpreter's fault pad, the per-thread `struct cpu` binding. A thread created that
way would need the engine to initialise its own TLS explicitly. Filed as a known risk, not a solved problem.

### 5.3 Cross-process memory transfer bandwidth

Measured against a live child process on the same box, 256 MiB in 64 MiB chunks:

| Operation | Throughput | Per-4 KiB latency |
|---|---|---|
| `WriteProcessMemory` | **4713 MiB/s** (256 MiB in 54.3 ms) | **1.98 µs** |
| `ReadProcessMemory` | **8986 MiB/s** (256 MiB in 28.5 ms) | — |

These bound §6.2 and §6.4. A 256 MiB guest image costs ~55 ms to push eagerly. That is three orders of
magnitude worse than a COW fork and it is the reason eager copy is a fallback rather than a plan.

### 5.4 What was *not* measured, and matters

- **Whether a pagefile-backed section view survives a clone as `ViewShare`.** This is R2/C3 and it is the
  single most important open question for strategy A. `NtMapViewOfSection`'s `InheritDisposition` decides
  it; `MapViewOfFile`/`MapViewOfFile3` do not expose the parameter, so it must be verified empirically or
  the arenas mapped through `NtMapViewOfSection` directly. If shared views come out `ViewUnmap` or
  privately copied, every cross-process guest primitive in the R2 table breaks silently — futex wakes lost,
  `eventfd` counters diverging, `flock` brokers disagreeing. **Measure this first.**
- Behaviour under a debugger, under EDR, and inside a Job object with
  `JOB_OBJECT_LIMIT_ACTIVE_PROCESS`. The literature flags all three.
- Console handle behaviour (`FreeConsole` + `AttachConsole(ATTACH_PARENT_PROCESS)` is the documented
  reattachment dance).
- Whether the clone can `LoadLibrary`. It is documented not to; the probe run that tested it died in the
  earlier `CreateThread` stage, so this tree has no measurement of its own.
- **Nothing here was measured with a native mingw-w64 binary.** Every number in §5 is from a CLR host.

---

## 6. The candidate strategies

### 6.1 A — `RtlCloneUserProcess` / `NtCreateUserProcess`

| | |
|---|---|
| C1 | ✔ measured |
| C2 | ✔ by construction (COW replica of the whole address space) |
| C3 | **unknown** — §5.4, the deciding question |
| C4 | ✔ with `INHERIT_HANDLES` and universally inheritable handles (measured) |
| C5 | ✖ as-is; ✔ with `NtCreateThreadEx(SKIP_THREAD_ATTACH)` and engine-managed TLS (measured) |
| C6 | ~3.0–3.5 ms measured, vs. a Linux fork's tens-to-hundreds of µs |
| C7 | undocumented, but shipped since Vista and reached indirectly by `RtlCreateProcessReflection`, which Windows Error Reporting itself uses |

The decisive property is the one nothing else has: **the clone continues *the calling thread*, at the
calling thread's exact continuation.** Guest `fork()` is issued by the guest thread executing inside
`run_block`, and that is precisely the thread that must survive into the child. Every other strategy has to
reconstruct that.

Known constraints, from the literature and consistent with §5: only the calling thread is cloned (other
threads' stacks and TEBs are copied but nothing runs on them — which is what Linux fork does anyway, and
what `thread_after_fork` already assumes); the clone cannot load new DLLs; win32k/GDI is unavailable;
`RtlCloneUserProcess` without `NO_SYNCHRONIZE` drains the thread pool and takes the loader, PEB, TLS/FLS and
heap locks before cloning, which is the right default for us.

### 6.2 B — fresh process + explicit state transfer, via the existing checkpoint serializer

The obvious form of this ("copy `.data`, `.bss`, heap and stack into a suspended child") is Cygwin's, and
`docs/windows/prior-art-cygwin-fork.md` covers what it costs and how it fails. The interesting form is
specific to this engine and is not in the prior-art documents:

**The engine already has a complete, tested guest-state serializer: checkpoint/restore.** `amd64-host.md`
§1 records `checkpoint` 82/82, `checkpoint-io` 34/34, `ckpt-cross` 11/11, and `src/linux_abi/checkpoint.c`
already re-forks a whole process tree at `checkpoint.c:5391` while populating `g_pidmap`
(`container/pidmap.c`). Restore reconstructs into a *fresh* engine, so by construction it captures
everything guest-visible — which, per §3.3, is all a fork child needs.

So guest fork can be implemented as: checkpoint the parent → `CreateProcess` a fresh engine →
restore → diverge.

| | |
|---|---|
| C1 | ✔ semantically |
| C2 | ✔ — restore already places guest regions at fixed addresses |
| C3 | ✖ — restore rebuilds arenas rather than sharing them; every cross-process primitive in R2 would need explicit re-attachment to inherited sections |
| C4 | needs `DuplicateHandle` per descriptor. The machinery exists in shape: `hl_linux_abi_fork_prepare` (`linux_abi.c:286`) already enumerates and clones the OFD table |
| C5 | ✔ — an ordinary process with an ordinary loader state |
| C6 | dominated by the guest image: **~55 ms per 256 MiB** at the §5.3 rate, plus process creation. ~250× a fork |
| C7 | ✔ fully documented APIs only |

The cost is disqualifying for `forkchurn`, `dbserver`, and any shell-heavy guest, and `checkpoint.c` also
carries a known defect (`amd64-host.md` §7: restore does not repopulate `g_gro`, so `.rodata` comes back
writable). But it is *correct*, it needs no undocumented API, and it doubles as the differential oracle for
strategy A. **That combination makes it the right fallback and the wrong primary.**

### 6.3 C — a single-threaded zygote, cloned before threads/arenas/handles exist

Answered directly, because it is the crux: **this solves 2.1 and 2.2 completely and does nothing at all for
2.3.**

For launch (§2.1) and launch acceleration (§2.2) it is exactly right, and it is what the Windows port should
do: keep a pool of pre-spawned idle engine processes, hand one a request over the existing `HLF3` protocol
(`fork.c:35-45`, codec in `src/linux_abi/fork_codec.c`), and let it run the guest cold. The protocol already
carries argv, envp, cwd and three descriptors; on Windows the descriptors become `DuplicateHandle` targets
instead of `SCM_RIGHTS`. That is a straightforward rewrite of `src/host/fork_wire.c` and *no* change to
`fork_codec.c`, which is pure byte-slinging with no POSIX in it.

For guest fork it does not help, and the code says why:

- The fork happens at an arbitrary point in guest execution (`proc.c:1823`) with an arbitrary guest heap,
  arbitrary mappings and an arbitrary fd table. The child must be a copy of *that* state. A zygote is by
  definition a copy of an *early* state. Hoisting is not available.
- The parent is frequently **not** single-threaded at fork time. `spawn_thread` (`thread.c:2676`) creates
  host threads for guest `clone(CLONE_THREAD)`; `thread_after_fork` (`thread.c:2246`) exists precisely
  because peers may be live; `jit_after_fork`'s preserve decision branches on `g_threaded`
  (`cache.c:1740`); and `cache.c:1697-1705` names the exact production failure caused by a peer holding
  `g_jit_lock` at the instant of fork. The quiescence precondition is *known to be violated* by the
  workloads that matter (Go, npm, cargo).
- A pre-forked *pool* of blank clones could be kept and then filled with state — but that is no longer a
  zygote, it is strategy B with the process-creation cost hidden, and the transfer cost of §5.3 is what
  dominates anyway.

The useful residue of the idea is narrower and real: **do the `RtlCloneUserProcess` call from the guest
thread that issued the fork, and make that the only thread the child needs.** That is not a zygote, but it
is the same instinct — minimise what the new process must be true of — and strategy A already has it.

### 6.4 D — eager address-space copy via `NtCreateSection` / `MapViewOfFile3` + `WriteProcessMemory`

A cheaper-looking variant of B: reserve placeholders in the child with `VirtualAlloc2(hChild, …,
MEM_RESERVE_PLACEHOLDER)`, then `MapViewOfFile3(section, hChild, addr, …, MEM_REPLACE_PLACEHOLDER)` — both
of which take a target process handle — and push private pages with `WriteProcessMemory`.

This is genuinely useful for **R2**: the `MAP_SHARED` arenas are the ideal case for it. They are created
once, before any guest fork, at addresses the engine chooses; mapping the same section into a child at the
same VA is exactly what `MapViewOfFile3` with a placeholder is for. **Under any strategy, this is probably
how the arenas should be handled**, rather than relying on clone inheritance disposition (§5.4).

As a way to move the *guest* address space it fails on cost (§5.3) and on the same C3/C4 problems as B.
`MEM_REPLACE_PLACEHOLDER` also refuses image sections, so a guest ELF mapped as an image cannot be moved
this way — though the engine loads guest ELFs itself into anonymous memory, so that restriction may not
bind.

### 6.5 Ranking

| | C1 | C2 | C3 | C4 | C5 | C6 |
|---|---|---|---|---|---|---|
| **A** clone | ✔ | ✔ | ? | ✔* | ✔* | 3 ms |
| **B** checkpoint→fresh process | ✔ | ✔ | ✖ | ✔* | ✔ | ~55 ms/256 MiB |
| **C** zygote | ✖ (launch only) | — | — | — | ✔ | ~1 ms |
| **D** eager section copy | ✔ | ✔ | ✔ | ✔* | ✔ | ~55 ms/256 MiB |

`*` = requires work described above (universal handle inheritability; engine-managed TLS).

---

## 7. Recommendation

**Primary: split the problem along §2 and use a different mechanism for each.**

- **§2.1 engine launch** → `CreateProcess` of the engine image + `DuplicateHandle` for the descriptor set
  `hl_linux_abi_fork_prepare` already enumerates + an inherited pagefile section for the child-result
  record. No clone, no undocumented API. `spawn_cloned` and `spawn_prepared` become genuinely different
  functions on Windows instead of two names for `fork()`.
- **§2.2 launch acceleration** → a zygote pool (strategy C), reusing `fork_codec.c` unchanged and rewriting
  `fork_wire.c`'s transport.
- **§2.3 guest fork** → **strategy A, `RtlCloneUserProcess`**, called from the guest thread issuing the
  fork, with the R2 arenas placed by strategy D so their sharing does not depend on clone inheritance
  disposition.

**Fallback: strategy B** — checkpoint the parent, spawn a fresh engine, restore. Correct, documented-API-only,
~250× slower, and useful regardless as the differential oracle for A.

### 7.1 Staged order

**Phase 0 — no guest fork.** Host backend, launch via `CreateProcess`, `map_anonymous` on sections,
`hl_host_sync_services` on SRW locks with the `fork_prepare`/`fork_parent`/`fork_child` bracket reduced to
no-ops. Guest `clone`/`clone3` without `CLONE_THREAD` returns `ENOSYS`. **The `CLONE_THREAD` path is
untouched and must work from day one** — it is a separate branch taken before any fork machinery
(`proc.c:1789-1792`, `proc.c:2512-2515`) and it is what `tests/compat/threads` (57 cases, 1 fork-dependent)
exercises.

**Phase 1 — verify the C3 question of §5.4 with a native mingw binary**, and re-measure §5.2 there. These
two measurements decide whether the rest of the plan stands. Do them before writing the clone path.

**Phase 2 — `RtlCloneUserProcess` guest fork**, arenas via strategy D, handles universally inheritable,
`fork_child_hooks` ported hook by hook. `kqueue_rebuild_after_fork` (`event.c:623`) is the model: it already
proves the epoll/timerfd/inotify family is reconstructible.

**Phase 3 — checkpoint-restore fork** as fallback and oracle: run the compat process suite under both and
diff, exactly as `ckpt-cross` does for the two backends.

**Phase 4 — zygote pool** for launch latency. Pure performance; last.

---

## 8. Blast radius

Counted per fixture source across all 24 manifests: a case is fork-dependent if its source calls `fork`,
`vfork`, `clone`/`clone3`, `posix_spawn*`, or `forkpty`. There are **no** uses of `system`, `popen` or
`daemon` anywhere in `tests/` — the daemonise cases hand-roll double-fork plus `setsid`.

**245 of 1600 compat cases = 15.3%.** (`amd64-host.md` reports 3036 *legs* because the runner registers each
case per guest ISA.)

| Manifest | Cases | Fork-dependent |
|---|---|---|
| `process` | 80 | **62 (77.5%)** |
| `ipc` | 126 | 48 |
| `network` | 87 | 25 |
| `signals` | 71 | 24 |
| `memory` | 111 | 18 |
| `posix` | 98 | 18 |
| `filesystem` | 89 | 11 |
| `syscall` | 87 | 9 |
| `core/syscall` | 57 | 8 |
| `procfs` | 57 | 8 |
| `syscall_edges` | 52 | 5 |
| `completeness` | 184 | 3 |
| `core/workload` | 21 | 2 |
| `core/abi`, `core/regress`, `threads`, `time` | 146 | 1 each |
| `abi` + `abi/corpus` + `libc` + `isolation` + `isa/*` | **334** | **0** |

Plus `tests/soak` 1/18, and the two Darwin-only `compat.process-forkserver-*` ctest cases, which are 100%
forkserver-dependent and would simply not be registered on Windows.

**Phase 0 therefore ships at most 1355/1600 = 84.7%,** with `tests/compat/process` gutted (18 survivors:
`futex-operations`, `pi-and-robust-futex`, `process-limits`, `ltp_procmisc`, `sysinfo`, `nonpie-dladdr`,
`sched-attr`, `pidfd-getfd-dup`, `prctl-*`, `capget-version`, `times-contract`, `getcpu-sysinfo-shape`,
`setpgid-errno`, `credentials-shape`, `sched-setscheduler-errno`, `exec-symlink-entry`).

### 8.1 There is no cheap subset of fork

The tempting phase-1.5 is "support only fork-immediately-followed-by-exec", implemented as a plain
`CreateProcess` of a fresh engine on the new image. Two facts kill it as a *general* win and one keeps it
alive as a small one:

- glibc's plain `fork()` does **not** set `CLONE_VM`, so the common `fork` + `execve` idiom carries no
  signal that an exec is coming. It cannot be distinguished from a fork that diverges.
- Conversely `posix_spawn`, `popen` and `vfork` **do** arrive as `clone(CLONE_VM|CLONE_VFORK, child_stack)`
  — `proc.c:1846-1851` documents exactly this and the reason the child must run on `a1` — and
  `CLONE_VM` *semantically means "share the address space"*, which on Windows is trivially satisfiable by
  running the child on a **host thread in the same process** and splitting into a real process only at
  `execve` (`proc.c:1971`). The existing vfork pipe rendezvous (`proc.c:1813-1886`, released at
  `proc.c:2134`) is already the synchronisation this needs.
- But that covers only three `process` fixtures plus scattered libc-internal uses. **Call it 1–2% of the
  corpus, not 15%.**

So: the corpus is recovered by strategy A working, or not at all. Plan accordingly.

### 8.2 Non-compat suites

Also lost in phase 0, from the same inventory: `tests/unit/test_linux_fork.c`, `test_fork_wire.c`,
`test_eventfd_fork.c`, `test_child.c`, `test_process.c`; 13 of 33 `tests/e2e` files (all
`checkpoint_*` tree/threads/pipe/socketpair/eventfd/timerfd/cycle/io_recovery, plus `fd_binding.c`,
`guest_descendant.c`, `guest_domain.c`); four `tests/integration` files; `tests/perf/ops.c`. Note also that
`tools/matrix_runner.c:848` and `tools/compat_runner.c:16` are themselves `fork()`-based — **the harness
that runs all 1600 cases has to be ported before a single case can be measured on Windows.**

---

## 9. Risks

Ranked.

1. **The `MAP_SHARED` arenas may not survive a clone as shared pages** (§5.4). If they come out privately
   copied, the failures are *silent and non-local*: a lost futex wake, an `eventfd` counter that diverges,
   two `flock` brokers that disagree, `/proc/<pid>/stat` showing a stale state char. `thread.c:216-220`
   records what a wrong futex key already cost once. Mitigation: map every arena through
   `NtCreateSection` + `NtMapViewOfSection` with an explicit `ViewShare` disposition, or via strategy D
   into the child after the clone, rather than trusting inheritance.
2. **Host threads in a clone.** `CreateThread` faulted 4/4 here and `SKIP_THREAD_ATTACH` fixed it, but that
   flag also skips TLS callbacks, and the engine's `__thread` usage is structural. Without a resolution, a
   guest that forks and *then* creates threads — every JVM, Go and Node workload — is unsupported. This is
   the difference between "fork works" and "fork works for shell scripts".
3. **`RtlCloneUserProcess` is undocumented, and Microsoft Research has published that fork "has long
   outlived its usefulness and is now a liability."** A servicing change is possible and there is no
   contract. The concrete mitigations are that strategy B exists, that phase 3 makes it a *tested*
   fallback rather than a paper one, and that the two must be diffable case-for-case.

Runner-up risks worth writing down now: universal handle inheritability is a constraint on the entire
Windows file backend, not a fork-local one, and getting it wrong leaks handles into every child;
`hl_host_process_open` (`src/host/process.h`) is declared `int hl_host_process_open(pid_t pid)` returning a
pidfd-shaped descriptor and has no Win32 analogue at that signature; and the guest pid **is** the host pid
today except for the container init (`container/state.c:294-302`), so Windows PIDs — which are neither small
nor monotonic — flow straight into guest-visible `getpid`, `/proc`, and `kill` unless a pid map is made
mandatory. `container/pidmap.c` exists but is empty on a normal launch (`proc.c:2283`).

---

## 10. What will not work, stated plainly

- **A single mechanism for all three uses of fork.** Trying to write one `spawn_cloned` that also serves
  guest `fork()` is what makes this look intractable.
- **A zygote for guest fork.** §6.3.
- **Preserving the parent's JIT arena in the child.** Not needed on Linux today (`cache.c:1719`) and it
  should not be attempted on Windows first.
- **`LoadLibrary` inside a clone**, and therefore any lazily-loaded dependency reached only after a guest
  fork. The engine must have every DLL it will ever need resident before the first clone.
- **Windows GUI/GDI anything in a clone.** Irrelevant here, but it means the clone cannot pop a dialog,
  cannot use win32k, and — untested — may not be able to do some console operations without the
  `FreeConsole`/`AttachConsole` dance.
- **Marking the Windows host "Supported" on the strength of this document.** It contains no line of shipped
  code. The first two things to do are the two measurements in Phase 1.
