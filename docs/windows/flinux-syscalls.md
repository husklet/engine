# flinux's syscall residue: `/proc`, SysV IPC, futex, identity, clocks

**Licensing.** Foreign Linux (flinux, `wishstudio/flinux`) is **GPLv3**. HL Engine is **MIT**. The licences are
incompatible in the direction that matters: GPLv3 code may not be copied into `src/`. Nothing in this document
is a transliteration. What follows describes *techniques, data shapes, constraints and measured consequences* —
which are not copyrightable — with `file:line` citations so a reader can check the claim against flinux's tree
without importing it. Short quotations are limited to comments and log strings that name a design decision.
**Any implementation must be written independently against the Linux ABI, not against flinux's source.**

flinux commit under study: the tree cloned to the session scratchpad, 20,655 lines of C across `src/`.
Engine line numbers were checked against `feat/windows-amd64` on 2026-07-27 and will drift.

---

## 0. The verdict up front

The obvious expectation for this survey — "flinux implemented `/proc` and SysV IPC on Windows, mine it" — is
wrong in a way that changes the work. **flinux is prior art for the Windows *mechanism*, not for the Linux
*surface*.** On the surface, the engine is already far ahead of it:

| Area | flinux | HL Engine today |
|---|---|---|
| `/proc` entries | **9 files**, no symlinks, no `self/exe`, no `self/fd`, no `self/status` | ~60 own-process leaves, peer-process registry, magic links, masking policy, `/proc/net/**` |
| SysV IPC | **none** — `shmget`/`semget`/`msgget` and x86 `ipc()` are all fatal | all 12 syscalls, `sysv.c` 1,983 lines, namespaced registry, `SEM_UNDO`, `*id64_ds` marshalling |
| `futex` | WAIT/WAKE/REQUEUE/CMP_REQUEUE only; **no interprocess futex**; no BITSET, no WAKE_OP, no PI | 13 ops incl. PI, BITSET, WAKE_OP, requeue-PI; robust lists; cross-process bucket table; shared-object keys |
| identity syscalls | mostly constant fictions; `times` and (on x86-64) `gettid` are **fatal** | all implemented, `capget`/`capset` with real ABI-version negotiation |
| clocks | `CLOCK_REALTIME`/`MONOTONIC` only; `PROCESS_CPUTIME_ID` → `EINVAL` | 12 Linux clock ids + pid-encoded CPU clocks; clock group already ships on Windows |

So the residue is **not "implement these"**. It is: *these are implemented, correctly, against POSIX directly,
and the Windows unity TU cannot compile them.* Measured on this branch, across the seven files that own this
residue:

> **89 distinct POSIX symbols, 591 call sites.**
> `sysv.c` 57 sites / 12 symbols · `thread.c` 149 / 31 · `proc.c` 109 / 40 · `rare.c` 36 / 14 ·
> `time.c` 57 / 14 · `container/vfs.c` 168 / 33 · `host/linux/system.c` 15 / 7.
> `src/linux_abi/syscall/misc.c`: **0**.

That last line is the whole recommendation in one number. `misc.c` already implements `uname`, `sysinfo`,
`getrandom`, `sethostname` and `setdomainname` with zero POSIX calls, behind a callback context
(`src/linux_abi/syscall/misc.h:31`), populated at `src/linux_abi/syscall/dispatch.c:835-862`. It is the only
host-neutral handler in the family and it is the shape the other six files need. The port is not new
emulation; it is **moving 89 symbols behind either an existing typed group or a `misc.c`-style callback
context**, plus exactly one piece of genuinely new machinery (§6).

Where flinux is worth mining is the Windows side of three specific problems, and on two of the three its answer
is *"they didn't"* — which is itself the finding, because it tells you what it cost them.

---

## 1. What was measured

Nine Windows-side facts were measured, not reasoned. Source in the session scratchpad
(`exp/wexp.c`, `wexp2.c`, `wexp3.c`, `wexp4.c`), built with the verified toolchain
(`C:/msys64/clang64/bin/clang.exe`, `x86_64-w64-windows-gnu`, `-lsynchronization`), run on Windows 11 Pro
26200. Each is cited by tag below.

| Tag | Measurement | Result |
|---|---|---|
| **M1** | `WaitOnAddress` in process B on a page shared with process A; A writes the word and calls `WakeByAddressAll` | **No wake.** Waiter ran its full 3,000 ms timeout with the new value visible in memory. `WaitOnAddress` is process-local. |
| **M2** | Is `WaitOnAddress` alertable? `QueueUserAPC` to a thread parked in `WaitOnAddress(…, 1500)` | **Not alertable.** APC queued successfully, ran full 1,505 ms, APC never executed — not even at thread exit. |
| **M3** | `WakeByAddressAll` on an address whose value is **unchanged** | **Wakes.** Returned `TRUE` after 404.7 ms with the word still `0`. A full EINTR simulation delivered `-EINTR` in 407.5 ms against a signal raised at 400 ms. |
| **M4** | Round-trip cost, same process | `WaitOnAddress`/`WakeByAddressSingle` **0.549 µs**; auto-reset event + `WaitForSingleObject` **7.326 µs**. 13.3× |
| **M5** | Round-trip cost, cross-process, named event + shared word | **3.548 µs** round-trip = **1.774 µs per one-way wake**. `OpenEventA` by name **1.136 µs**; `SetEvent` on a cached handle **0.229 µs**. |
| **M6** | Named section lifetime | Pure refcount. Closing the last section **handle** destroys the name (`ERROR_FILE_NOT_FOUND`) **even while a view is still mapped**. A second process holding a handle keeps it alive; when that process exits, the name dies. |
| **M7** | Wait-timeout quantisation at default timer resolution | ~15.6 ms for **all** of `Sleep`, `WaitForSingleObject`, `WaitOnAddress`, `NtWaitForSingleObject`. With `NtSetTimerResolution(0.5 ms)`: 100 µs → 0.298 ms, 500 µs → 0.894 ms, 1 ms → 1.427 ms, 5 ms → 4.893 ms. |
| **M8** | Private NT object namespace | `NtCreateDirectoryObject` under `\BaseNamedObjects\<name>` **succeeds**; naming an event with that directory as `RootDirectory` succeeds. An explicit `\Sessions\0\BaseNamedObjects\…` path **fails** (`STATUS_OBJECT_PATH_NOT_FOUND`) outside session 0. |
| **M9** | Clock sources | QPC = 10 MHz exactly (100 ns). `GetSystemTimePreciseAsFileTime` resolves sub-µs (17 × 100 ns across `Sleep(0)`); `GetSystemTimeAsFileTime` resolves **0** — coarse. `QueryUnbiasedInterruptTime` excludes suspend; `GetTickCount64` includes it. |
| **M10** | Engine residue | 89 distinct POSIX symbols / 591 call sites across seven files (§0). |

One thing could **not** be measured: the full Windows unity-TU diagnostic count. `ninja -k 0
win_unity_probe_x86_64` in `build-unity` halts at `src/core/target/x86_64.c:48` on a missing `<sys/socket.h>`
and produces one diagnostic, not thousands — the probe cannot enumerate past the first missing *header*. The
"3,726 diagnostics" figure quoted in the task brief is therefore not reproducible from a clean tree without
header shims already in place; M10 is the substitute, and it is attributable per file.

---

## 2. `/proc`

### 2.1 What flinux synthesised, and from what

flinux mounts a whole `procfs` as a *virtual filesystem* with a static descriptor tree
(`src/fs/procfs.c:276-291`). The complete inventory is nine files. There is nothing else.

| Path | Source | Verdict |
|---|---|---|
| `/proc/<pid>/maps`, `/proc/self/maps` | flinux's **own VMA red-black tree**, walked in `mm_get_maps` (`src/syscall/mm.c:565-598`) | **Real** for the address ranges and permissions; **fictional** for every numeric column |
| `/proc/<pid>/stat`, `/proc/self/stat` | `process_get_stat` (`src/syscall/process.c:595-683`) | **Almost entirely fiction** — see below |
| `/proc/<pid>/mounts`, `/proc/mounts` | literal `"none / ntfs\n"` (`procfs.c:41-46`) | Fiction, one line |
| `/proc/stat` | `GetSystemTimes` for cpu times; boot time from `NtQuerySystemInformation(SystemTimeOfDayInformation)` (`procfs.c:110-146`) | **Half real.** `cpu` and `btime` real; `intr`, `ctxt`, `processes`, `swap` hard zero |
| `/proc/cpuinfo` | **`CPUID`**, leaves 0, 1, 4, `0x80000002-4`, `0x80000008` (`procfs.c:148-225`) | **Real** — the most faithful entry they have. Single processor only (`/* TODO: Support more than one processors */`, `procfs.c:113`) |
| `/proc/meminfo` | `GlobalMemoryStatusEx` (`procfs.c:236-256`) | **Real** for Total/Free/Swap; `High*`/`Low*` are aliases or zero |
| `/proc/uptime` | `GetTickCount64` + `GetSystemTimes` idle (`procfs.c:258-274`) | **Real** |
| `/proc/loadavg` | all zeros (`procfs.c:227-234`) | Fiction |
| `/proc/sys/vm/min_free_kbytes` | constant `4096` (`procfs.c:86-99`) | Fiction. **This is the entirety of `/proc/sys`.** |

`/sys` is mounted and **empty** — `sysfs.c:27-33` is a directory descriptor with a single `END` entry.

`/proc/<pid>/stat` deserves its own line because it is the clearest illustration of how far a plausible
fiction can be pushed. Of ~52 fields, flinux fills six with real data (pid, ppid, pgid, sid, and two
constants) and hard-codes the rest to zero, including `utime`, `stime`, `vsize`, `rss`, `num_threads`, and
every signal mask. The `comm` field is the string literal **`"hello"`** (`process.c:598`). Anything that
parses `/proc/self/stat` for memory or CPU accounting — `ps`, `top`, most language runtimes' RSS probes —
reads structurally valid, semantically empty data. That is a worse failure mode than `ENOENT`, because the
caller cannot detect it.

### 2.2 Absent, and consequential

`self/exe`, `self/fd`, `self/status`, `self/cmdline`, `self/environ`, `self/task`, `self/auxv`, `self/limits`,
`self/smaps`, `self/mountinfo`; `/proc/version`, `/proc/filesystems`, `/proc/devices`, `/proc/sys/kernel/**`,
`/proc/sys/fs/**`, `/proc/net/**`.

`self/exe` and `self/fd` are absent for a **structural** reason, not an oversight: flinux's virtualfs has
exactly five node types — `DIRECTORY`, `CUSTOM`, `CHAR`, `TEXT`, `PARAM` (`src/fs/virtual.h:24-29`). **There
is no symlink type.** No `/proc` symlink can exist, and neither can `/dev/stdin`, `/dev/stdout`, `/dev/stderr`
or `/dev/fd` (`src/fs/devfs.c:28-40` lists only `dsp`, `null`, `zero`, `random`, `urandom`, `console`, `tty`).
Nothing in the tree calls `GetModuleFileName` for guest purposes — the only two uses are in `fork.c` for
re-launching the emulator itself (`src/syscall/fork.c:115,166`).

What a guest visibly cannot do: busybox cannot re-exec its own applets via `/proc/self/exe`; anything that
enumerates open descriptors via `/proc/self/fd` (shell `exec` cleanup, `close_range` fallbacks, Java's
`ProcessBuilder`) sees `ENOENT`; anything that reads `/proc/self/status` for `VmRSS`, `SigQ` or the capability
sets gets nothing.

### 2.3 Two techniques worth taking

**(a) Snapshot-on-open with `st_size == 0`.** `virtualfs_text_alloc` (`src/fs/virtual.c:415-428`) calls the
node's `gettext` into a **64 KiB stack buffer**, then heap-allocates exactly `len + 1` and keeps that snapshot
for the file's lifetime; reads and `lseek` serve the snapshot. `virtualfs_text_stat` reports `st_size = 0`.
That last part is not a shortcut — real Linux `/proc` also reports zero size for these files, so the fiction
is *faithful*, and any guest that `fstat`s before reading already copes. The snapshot semantic is also correct:
Linux `/proc/<pid>/stat` is generated once per `open`, not per `read`.

The 64 KiB stack buffer, however, is a defect we must not copy. `mm_get_maps` (`mm.c:565`) writes into it with
**no bounds check** while iterating an unbounded VMA tree. A guest with a few thousand mappings — routine for
a JVM or a Go binary — overflows the emulator's stack. The right shape is a growable heap buffer with an
explicit cap and a truncation policy.

**(b) Foreign-pid `/proc` by RPC to the target's own emulator.** This is flinux's one genuinely clever idea
here. Only the target process knows its own VMA table, so `/proc/<other-pid>/maps` cannot be answered locally.
`process_query_pid` (`process.c:700-720`) looks the pid up in a shared process table, retrieves the target's
signal-pipe write handle and a per-target query mutex, and `signal_query` (`sig.c:557-595`) does:
`OpenProcess(PROCESS_DUP_HANDLE)` → `DuplicateHandle` the pipe and mutex **out of the target** → take the
mutex (serialising concurrent queriers on one pipe) → write a `SIGNAL_PACKET_QUERY` → read back a length-
prefixed blob. The target's signal thread services it in its own address space (`sig.c:241-253`) by calling
the same `process_query` the local path uses.

Two flaws are visible in their own comments: `/* TODO: Avoid blocking when the other end died */`
(`sig.c:251`) and a `__debugbreak()` on `WAIT_ABANDONED` because a querier that crashed mid-query leaves
unread bytes in the pipe with no recovery (`sig.c:569-575`). Both are inherent to *"reply on the same pipe
you deliver signals on"*; a dedicated request/response channel with a deadline avoids both.

### 2.4 What this means for us

The engine's `/proc` is synthesised in `src/linux_abi/container/vfs.c`, dispatched from `proc_open`
(`vfs.c:5479`), and is a superset of flinux's by an order of magnitude. The Windows problem is not content —
it is **two host dependencies**:

1. `proc_leaf_dir_open` (`vfs.c:4528-4565`) materialises a **real temporary directory** with `mkdtemp` and
   populates `exe`/`cwd`/`root` with real `symlink()` calls, so `readdir` on `/proc/self` works. Neither
   `mkdtemp` nor `symlink` exists usefully on Windows (NTFS symlinks require privilege or Developer Mode).
   flinux offers nothing here — it has no symlinks at all — so this needs an engine-side answer: serve the
   directory from the synthetic table (`vfs.c:4533-4537` already *is* that table) rather than from a real
   directory on disk.
2. `src/host/linux/system.c` answers peer-process and system-wide queries by **reading the host's own
   `/proc`** — `/proc/meminfo` (`:32`), `/proc/stat` (`:53,80`), `/proc/<pid>/stat` (`:105`),
   `/proc/<pid>/fd` (`:166,199`), `/proc/self/exe` (`:235`). This is precisely the set flinux answered from
   Win32, and its answers transfer directly: `GlobalMemoryStatusEx`, `GetSystemTimes`,
   `NtQuerySystemInformation(SystemTimeOfDayInformation)`, `GetTickCount64`, `CPUID`.

---

## 3. SysV IPC

### 3.1 flinux implemented none of it

This is unambiguous, and the syscall tables are the proof. `syscall_table_x64.h` line *N* is syscall *N*
(`syscall_dispatch.c:33-42` supplies syscall 0 separately). Lines **29, 30, 31** (`shmget`, `shmat`,
`shmctl`) and lines **64-71** (`semget`, `semctl`, `semop`, `shmdt`, `msgget`, `msgsnd`, `msgrcv`, `msgctl`)
are all `SYSCALL(unimplemented)`. On x86, `syscall_table_x86.h:117` — the `ipc()` multiplexer that carries the
entire family on 32-bit — is also `unimplemented`. Grepping the tree for `shmget`, `shmat`, `semget`, `msgget`
or `System V` returns nothing but a comment on `CLONE_SYSVSEM` (`common/sched.h:14`) and a licence header.

`unimplemented` is not `ENOSYS`. `sys_unimplemented_imp` (`syscall_dispatch.c:63-68`) logs
`"FATAL: Unimplemented syscall"`, executes `__debugbreak()`, and calls `process_exit(1, 0)`. **Any guest that
touches SysV IPC dies immediately, with no errno the guest can handle.** That design choice — fatal rather
than `ENOSYS` — recurs across their whole unimplemented set and is worth naming as an anti-pattern: it makes
a graceful-degradation path impossible for every caller.

### 3.2 But they built exactly the substrate SysV needs

Their process table is a SysV-shm-shaped problem solved for pids, and the shape is directly reusable:

- **A per-session NT object directory.** `shared_create_object_directory` (`src/shared.c:78-99`) builds
  `\BaseNamedObjects\flinux-<session-id>` with `NtCreateDirectoryObject` and `OBJ_INHERIT | OBJ_OPENIF`,
  where the session id is a command-line flag. Every named object the emulator creates — sections, mutants —
  is created with that directory as `OBJECT_ATTRIBUTES.RootDirectory` (`shared.c:115`, `:154`, `:254`;
  `process.c:78`). Two independent flinux "systems" on one Windows machine therefore have **disjoint
  namespaces**, which is precisely the containment property a SysV `key_t` namespace needs. **Measured (M8):
  this works today** — `NtCreateDirectoryObject` under `\BaseNamedObjects\` succeeds and events named inside
  it resolve. Note the detail: the *relative* path is required. An absolute `\Sessions\0\BaseNamedObjects\…`
  fails outside session 0 (`STATUS_OBJECT_PATH_NOT_FOUND`); `\BaseNamedObjects\` resolves per-session
  automatically via the caller's `\Sessions\<n>\` view.
- **A fixed-size shared slot table with a named mutant as its lock.** `shared_alloc` (`shared.c:231-243`) is a
  bump allocator over one large named section; the process table is `struct process_info processes[4096]`
  inside it (`process_info.h:69-70`, `process.c:44-48`), and allocation is a round-robin linear scan for a
  `PROCESS_NOTEXIST` slot under `NtCreateMutant`-based mutual exclusion (`process.c:123-140`, `:74-85`). Their
  own comment concedes the lock is too coarse: *"It's better to have a lightweight interprocess RW lock.
  Windows only provides an intraprocess one."* (`process_info.h:91-92`).
- **Named per-object sections.** `map_shared_heap_pool` (`shared.c:245-297`) creates `shared_heap_pool_<id>`
  sections inside the session directory on demand and maps them lazily per process.

A SysV shm registry is that table with `{key, id, size, perms, nattch, ctime, creator}` instead of
`{pid, ppid, pgid, sid}`, plus one named section per segment named by id inside the same directory. flinux
did not build it; it built everything underneath it.

### 3.3 The lifetime problem, measured

System V shared memory has a lifetime Windows does not offer. A segment created by `shmget` survives the exit
of its creator, survives `shmdt` by every attacher, and is destroyed **only** by an explicit
`shmctl(IPC_RMID)` (and even then only once `nattch` reaches zero). Windows sections are refcounted.

**Measured (M6), and it is worse than the usual summary suggests:**

- Creating a named section, mapping a view, then closing the **section handle** while the view is still mapped
  → `OpenFileMappingA` by that name fails with `ERROR_FILE_NOT_FOUND`. The memory remains valid through the
  live view, but **the name is already gone**. Name lookup is bound to open *handles*, not to mapped views.
- A second process holding a handle keeps the name alive; when it exits, the name dies.

So there is no configuration of plain named sections that yields IPC_RMID semantics. Three options, and only
two are sound:

1. **A session broker holds one handle per live segment.** Something must own a handle for the segment's whole
   logical lifetime. That is a real process (or the container-init process the engine already models). Correct,
   and it composes with the per-session object directory; costs a broker.
2. **File-backed sections in a session-scoped directory.** `CreateFileMappingW` over a real file under the
   session's private directory; the *file* carries the lifetime, `IPC_RMID` deletes it. Correct without a
   broker, at the cost of touching the filesystem. This is the closest analogue to what the engine already
   does on Linux, where `sysv.c` uses `shm_open` — itself a file under `/dev/shm`.
3. ~~Rely on the refcount~~ — silently converts `IPC_RMID` into "last close wins", which is a semantic change
   `DOCS.md:63-64` item 7 forbids ("No successful API call silently ignores a requested option, limit,
   payload, or operation").

### 3.4 Is `hl_host_shared_memory_services` enough to build SysV shm on?

**No, not as written.** The group (`include/hl/host_services.h:530-538`) is:

```c
typedef struct hl_host_shared_memory_services {
    HL_ABI_HEADER;
    /* create returns a reopen identity in detail; it remains valid while the source handle is live. */
    hl_host_result (*create)(void *context, uint64_t size, uint32_t flags);
    /* open duplicates a live identity into an independently resizable and closeable handle. */
    hl_host_result (*open)(void *context, uint64_t identity, uint32_t flags);
    hl_host_result (*resize)(void *context, hl_host_handle object, uint64_t size);
    hl_host_result (*close)(void *context, hl_host_handle object);
} hl_host_shared_memory_services;
```

Three gaps, in descending order of severity.

1. **The identity contract encodes exactly the Windows lifetime, which is the wrong lifetime.** *"it remains
   valid while the source handle is live"* is `DuplicateHandle` semantics, and M6 shows that is precisely what
   SysV shm is not. A segment must be reopenable by **id** after its creator has exited. The group has no
   name/key concept at all — `open` takes a `uint64_t identity` that is a live-handle token, not a namespace
   key. There is no way to express `shmget(key, …, IPC_CREAT)` through it.
2. **`resize` cannot shrink.** `docs/windows/host-services-map.md:611-618` already establishes this:
   `NtExtendSection` grows only, so shrink must return `HL_STATUS_NOT_SUPPORTED`. SysV shm segments are fixed
   size after creation, so this happens not to bite `shmget` — but it does bite any `ftruncate`-shaped use.
3. **The group has zero consumers.** The only references to `services->shared_memory->*` anywhere in `src/`
   are the four NULL checks in `hl_host_services_validate` (`src/core/host_services.c:107-108`). `sysv.c`
   uses `shm_open`/`ftruncate`/`mmap` directly; `memfd_create` goes to the real syscall or `mkstemp`
   (`src/linux_abi/syscall/rare.c:105-158`); `/dev/shm` is a real host directory
   (`src/linux_abi/container/shm.c:16-30`). Porting the group to Windows as it stands buys nothing, because
   nothing would call it.

The group is a good fit for *anonymous* shared memory with a handle-scoped lifetime — which is what the engine's
own fork-shared arenas want. It is not a fit for a keyed, creator-outliving namespace. See §8.2 for the
recommended signatures.

---

## 4. Process and identity syscalls

### 4.1 Honest, fictional, and fatal

| Syscall | flinux | Class |
|---|---|---|
| `getpid` | real slot in the shared process table (`process.c:487-491`) | **Honest** |
| `getppid`, `getpgid`, `getsid`, `getpgrp` | read the shared table (`process.c:498-572`) | **Honest** (but see `setpgid`) |
| `gettid` | returns `process->pid` (`process.c:556-560`) — the *process* id, not a thread id | Fiction; **and unreachable on x86-64**: `syscall_table_x64.h:186` is `unimplemented` while `syscall_table_x86.h:224` wires it. On x86-64, `gettid()` is fatal. |
| `setpgid` | `return 0` without writing the table (`process.c:505-509`) | Fiction, and **incoherent with `getpgid`**, which reads the table |
| `setsid` | `log_error("setsid() not implemented."); return 0;` (`process.c:722-727`) | Fiction |
| `getuid`/`geteuid`/`getgid`/`getegid` | constant `0` (`process.c:729-757`) | Fiction — every guest is root |
| `setuid`/`setgid`/`setres[ug]id` | `return 0`, ignored (`process.c:759-795`) | Fiction |
| `getresuid`/`getresgid` | write `0,0,0` | Fiction, self-consistent |
| `getgroups` | `return 0` (empty set) | Fiction |
| `prctl` | `log_error("prctl() not implemented."); return 0;` (`process.c:976-981`) | **Fiction that lies** — returns success for every option |
| `capget`/`capset` | same shape, `return 0` **without writing the output buffer** (`process.c:983-995`) | **Fiction that lies and leaves the caller's buffer uninitialised** |
| `sched_yield` | `SwitchToThread()` (`process.c:1018-1023`) | **Honest** |
| `sched_getaffinity` | hard-codes a **1-CPU mask** (`process.c:1025-1053`) | Deliberate fiction — their comment says ffmpeg uses it to size its thread pool and *"Since we does not support multithreading at the time, we just report back one bit"* |
| `getrlimit`/`setrlimit`/`prlimit64` | only `RLIMIT_STACK`, `RLIMIT_NPROC`, `RLIMIT_NOFILE`; **every other resource returns `EINVAL`**; any `setrlimit` returns `EINVAL` (`process.c:887-946`) | Partial, honest about the gap |
| `getrusage` | zeroes the struct then falls into `default: return -EINVAL` for **every** value of `who` (`process.c:948-960`) | Broken — unreachable success path |
| `getpriority`/`setpriority` | `"Fake returning 0"` (`process.c:962-974`) | Fiction |
| `sysinfo` | `GlobalMemoryStatusEx` + `GetTickCount64`; `loads[]` zero, `procs` constant `100` (`process.c:862-885`) | **Mostly honest** |
| `uname` | `Linux` / `ForeignLinux` / `3.15.0` / `x86_64`\|`i686` (`process.c:815-832`) | Fiction, correct shape |
| `times` | **absent entirely.** No `sys_times` in the tree; `syscall_table_x64.h:100` is `unimplemented` | **Fatal** |
| `set_tid_address` | returns the **Win32 thread id**, logs `"clear_child_tid not supported"` (`process.c:1055-1060`) | **Fatal by consequence** — see §4.2 |
| `arch_prctl` | `ARCH_SET_FS`/`GET_FS`/`SET_GS`/`GET_GS` all return `EINVAL` (`src/syscall/tls.c:178-202`) | **Fatal by consequence** — see §4.2 |

### 4.2 Two fictions that cost them observably

**`set_tid_address` never stores the pointer.** `struct thread` has a `clear_tid` field
(`process_info.h:49-50`) and `thread_exit` faithfully implements the `CLONE_CHILD_CLEARTID` protocol — zero
the word, `futex_wake(clear_tid, 1)` (`process.c:451-461`). But `sys_set_tid_address` never writes
`current_thread->clear_tid`; it just returns a Win32 tid. The field is therefore always `NULL` and the wake
never fires. **`pthread_join` blocks forever**, because glibc's join waits on exactly that futex word. A
correct-looking exit path, disconnected from its only producer.

**`arch_prctl(ARCH_SET_FS)` returns `EINVAL`.** This is the single reason flinux's 64-bit mode could not run
glibc at all. `__libc_setup_tls` calls it before anything else; on failure, static glibc aborts before `main`.
flinux's TLS emulation is built on `set_thread_area` and Win32 TLS slots reachable through `fs:`/`gs:` offsets
(`tls.c:36-57`, `:160-176`) — a 32-bit design with no 64-bit counterpart. The x86-64 syscall table's other
gaps (`times`, `gettid`) point the same way: the 64-bit port was a partial retrofit. For us this matters
because `arch_prctl` sits in *this* residue, not the translator's, and it is item #1 on the shortest path (§9).

**`sched_getaffinity`'s 1-CPU fiction** is the interesting case of a fiction that *worked*. It is wrong, and
it was chosen deliberately to suppress guest multithreading while their threading was immature. It is a
legitimate technique — return a constrained truth rather than a lie — but it must be a *decision*, not a
default. The engine's equivalent (`src/linux_abi/syscall/proc.c:863`) already reports the real online set.

### 4.3 The auxiliary vector

flinux pushes nine auxv entries (`src/syscall/exec.c:81-100`): `AT_FLAGS`, `AT_SECURE`, `AT_RANDOM`,
`AT_PAGESZ`, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_BASE`. Absent: `AT_CLKTCK`, `AT_HWCAP`,
`AT_HWCAP2`, `AT_EXECFN`, `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`, `AT_SYSINFO_EHDR` (no vDSO).

`AT_RANDOM` points at **16 uninitialised stack bytes** — `char *random_bytes = ALLOC(16);` with the comment
`/* TODO: Fill in real content */` (`exec.c:81-82`). glibc derives the stack canary and pointer guard from
those bytes. On a deterministic startup path they are likely identical across runs, which silently defeats
both mitigations. A guest program cannot observe this; a security reviewer must.

---

## 5. Randomness, time and clocks

The engine's clock story is already better than flinux's and already ships on Windows
(`src/host/windows/clock.c:195-205` advertises all eight callbacks of `hl_host_clock_services`), and the
Linux-front mapping covers 12 clock ids plus pid-encoded CPU clocks (`src/linux_abi/syscall/time.c:369-384`).
So the only useful output here is **where flinux's answers differed observably from Linux**, as a checklist for
our own Windows clock provider.

| Behaviour | flinux | Observable difference from Linux |
|---|---|---|
| `getrandom` | `RtlGenRandom` (`src/fs/random.c:30-38`); `/dev/random` and `/dev/urandom` the same (`:40-53`) | Ignores `GRND_NONBLOCK`/`GRND_RANDOM`; **on failure returns `0`, not `-EAGAIN`/`-EFAULT`** — a caller reading the return as "bytes produced" silently gets an unfilled buffer |
| `CLOCK_REALTIME` | `GetSystemTimePreciseAsFileTime` via a Win7 shim (`src/syscall/timer.c:85-88`) | Correct. **M9 confirms it matters**: the Precise variant resolves sub-µs; plain `GetSystemTimeAsFileTime` resolved **0** across a `Sleep(0)` — it is the ~15.6 ms coarse clock and is only correct for `CLOCK_REALTIME_COARSE` |
| `CLOCK_MONOTONIC`, `_COARSE`, `_RAW` | all three collapse to `QueryPerformanceCounter/Frequency` (`timer.c:91-101`) | Epoch is boot, which is close enough. But `_RAW` on Linux is NTP-unadjusted and `MONOTONIC` is adjusted; collapsing them hides slew. Neither excludes suspend the way Linux `CLOCK_MONOTONIC` does — **M9**: `QueryUnbiasedInterruptTime` is the primitive that excludes suspend; `GetTickCount64` includes it and is the `CLOCK_BOOTTIME` analogue |
| `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID` | **`-EINVAL`** (`timer.c:103-104`) | glibc's `clock()` uses `CLOCK_PROCESS_CPUTIME_ID`; combined with `times` being fatal (§4.1), **flinux has no working CPU-time source at all**. `GetProcessTimes`/`GetThreadTimes` supply it trivially; they simply never wired it |
| `clock_getres` for monotonic | computes `(uint64_t)(1.0 / freq)` — integer-truncates to **0** for any freq > 1 Hz, then the `if (ns == 0)` branch reports 1 ns (`timer.c:128-140`) | Right answer by accident. M9: QPC is 10 MHz here, so the honest answer is 100 ns |
| `nanosleep` | `NtDelayExecution` with a relative 100 ns interval (`timer.c:66-75`); **`rem` is never written** | A sleep interrupted by a signal cannot be resumed correctly. And **M7**: at default timer resolution every wait primitive quantises to ~15.6 ms, so `nanosleep(1ms)` sleeps ~15 ms. flinux never calls `NtSetTimerResolution` or `timeBeginPeriod` — grepping the tree returns nothing |
| `setitimer`, `alarm`, `timer_create`/`settime`/`gettime`/`getoverrun`/`delete` | every one logs `"not implemented"` and **returns 0** (`timer.c:148-188`, `sig.c:599-604`) | The worst class: the guest is told the timer was armed and it never fires. `sleep(3)` implemented via `alarm` hangs |

**M7 is the actionable one for us.** The 15.6 ms quantisation is system-wide, not specific to any primitive —
`Sleep`, `WaitForSingleObject`, `WaitOnAddress` and `NtWaitForSingleObject` all showed ~15 ms for a 2 ms
request. Requesting `NtSetTimerResolution(0.5 ms)` brought a 100 µs wait to 0.298 ms and a 5 ms wait to
4.893 ms. Any `sleep_until`, `clock_nanosleep`, `ppoll` timeout or `FUTEX_WAIT` timeout shorter than ~15 ms is
otherwise wrong by an order of magnitude. The cost is a system-wide timer-resolution bump with power
implications, so it should be **scoped** — raised while the guest has a short-deadline wait outstanding, and
dropped otherwise — not held for the process lifetime.

---

## 6. `futex`

### 6.1 What flinux did

`src/syscall/futex.c`, 262 lines. A 256-bucket hash table (`FUTEX_HASH_BUCKETS`, `:34`), each bucket a
spin-locked intrusive list of wait blocks (`:36-54`). The hash is `addr % 256` — their own comment:
`/* TODO: Improve this silly hash function */` (`:84`). Locking is a hand-rolled
`InterlockedCompareExchange` spin with `YieldProcessor()` (`:56-65`), with two-bucket ordered acquisition to
avoid deadlock on requeue (`:67-80`).

The blocking primitive is **one auto-reset event per thread**, allocated at thread creation and stored in the
thread control block (`process_info.h:47-49`; created at `process.c:179`, `:199`, `:223`). `futex_wait`
(`:88-132`) takes the bucket lock, re-checks `*addr == val`, links a stack-allocated wait block, drops the
lock, and blocks in `signal_wait(1, &current_thread->wait_event, timeout)`. `futex_wake_requeue`
(`:134-205`) walks the bucket, and for each match either `NtSetEvent`s the waiter's event or rewrites the wait
block's address (requeue), moving the node between buckets when the two addresses hash differently.

`signal_wait` (`sig.c:519-530`) is what makes this interruptible: it appends the calling thread's
**`sigevent`** to the handle array and calls `WaitForMultipleObjects`, returning a distinguished
`WAIT_INTERRUPTED` when the signal event fires. That is how `FUTEX_WAIT` yields `EINTR`.

**Ops supported: `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`. Everything else returns
`-ENOSYS`** (`futex.c:249-252`). Absent: `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, `FUTEX_WAKE_OP`, the entire
PI family, `FUTEX_WAIT_REQUEUE_PI`. `set_robust_list` accepts the call, logs `"set_robust_list() not
supported."`, and returns 0 (`:255-262`) — the robust list is never walked at thread exit, so a mutex held by
a thread that dies is never marked `OWNER_DIED` and never woken.

Three defects visible in the code:

- **A lost wakeup on the timeout/interrupt path.** After a non-success return, `futex_wait` re-takes the bucket
  lock and calls `WaitForSingleObject(current_thread->wait_event, 0)` to decide whether it was concurrently
  woken (`:118`). The event is auto-reset, so that probe **consumes** the wakeup — and the function then
  returns `-EINTR` or `-ETIMEDOUT` anyway. The waiter was woken, the token is gone, and the caller is told it
  timed out. glibc's `pthread_cond_timedwait` re-checks its predicate and survives; a semaphore-shaped
  protocol loses a post.
- **Timeout conversion is lossy and can overflow.** `timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000`
  into a `DWORD` (`:228`) truncates sub-millisecond timeouts to zero and wraps past ~49.7 days.
  `FUTEX_CLOCK_REALTIME` is masked off by `FUTEX_CMD_MASK` and silently ignored, so a `FUTEX_WAIT_BITSET`-style
  absolute deadline would be treated as relative — moot only because that op returns `ENOSYS`.
- **The `FUTEX_CMP_REQUEUE` failure path returns the wrong errno namespace and unbalances the bucket locks.**
  The comparison itself is right — `*addr != *requeue_val` with `requeue_val == &val3` (`:150`, dispatched at
  `:246`) is Linux's `*uaddr != val3`. But the failure return is `-EAGAIN` (`:155`), the host CRT constant,
  where every other return in the file uses the `-L_*` Linux constants (`-L_EINTR` `:124`, `-L_ETIMEDOUT`
  `:129`, `-L_ENOSYS` `:251`). And that path unlocks **both** buckets (`:152-154`) even when
  `bucket == bucket2`, in which case only one lock was ever taken (`:141-142`) — benign here only because
  `unlock_bucket` is a bare store, and inconsistent with the success path, which correctly guards the second
  unlock (`:202`).

### 6.2 There is no interprocess futex — confirmed, and what it cost

**Confirmed.** Line 32 of the file is the comment `/* TODO: How to implement interprocess futex? */`, and the
table itself is a plain file-scope static: `struct futex_data { … } static _futex;` (`futex.c:49-52`). It lives
in ordinary process-private memory, not in the shared region `shared_alloc` hands out. Keys are raw virtual
addresses in the calling process (`futex_hash((size_t)addr)`, `:82-86`), so even the *key space* is
process-local: two processes mapping the same `MAP_SHARED` page at different addresses would hash to different
buckets even if the table were shared.

What it cost them, as a list of things a guest visibly cannot do:

- `pthread_mutex`/`pthread_cond`/`pthread_rwlock` with `PTHREAD_PROCESS_SHARED` never wake across processes —
  the waiter blocks until its timeout, or forever.
- POSIX semaphores in shared memory (`sem_init(pshared=1)`, `sem_open`) are broken in the same way. That is the
  standard cross-process synchronisation idiom.
- Anything that synchronises a parent and a forked child through a `MAP_SHARED` word: process pools,
  double-fork daemon handshakes, `posix_spawn`'s error channel in some libcs.
- Combined with §4.2, `pthread_join` is also broken *within* a process — so flinux's threading story was thin
  at both ends.

For us the exposure is concrete and named: **17 `MAP_SHARED` ledger arenas** and an interprocess futex the
engine already implements. `src/linux_abi/thread.c:148-186` allocates the 256-bucket table through
`hl_linux_memory_create(..., HL_HOST_MEMORY_SHARED|PRIVATE, ...)` and initialises `pthread_mutex`/`pthread_cond`
with `PTHREAD_PROCESS_SHARED`, before any guest fork, so every forked process inherits the same physical
buckets. `thread.c:211-323` adds the harder half: a registry that canonicalises a word inside a file-backed
`MAP_SHARED` region to a hash of `(st_dev, st_ino, file offset)`, so a `FUTEX_WAKE` through one mapping reaches
a `FUTEX_WAIT` through a different mapping of the same object at a different address. flinux has no equivalent
of either. **Everything flinux lacked here, we already have — expressed in POSIX.**

### 6.3 Which Windows primitive, measured

The naive answer is `WaitOnAddress`/`WakeByAddressSingle`, and the naive answer is half right. Four
measurements decide it.

**M1 — `WaitOnAddress` does not work across processes.** Two processes mapping the same named section: the
child parks in `WaitOnAddress` on the shared word, the parent writes the word and calls `WakeByAddressAll` on
*its own* mapping of the same physical page. The child ran the full 3,000 ms timeout and then observed the new
value. The wake never crossed. `WaitOnAddress` is keyed on a virtual address within a process; it is not a
cross-process primitive, whatever the shared backing. **An interprocess futex cannot be built on it.**

**M2 — `WaitOnAddress` is not alertable.** `QueueUserAPC` to a thread parked in `WaitOnAddress(…, 1500)`
returned success, the wait ran its full 1,505 ms, and the APC never executed — not even at thread exit.
So there is no APC-based `EINTR`. This is exactly why flinux used `WaitForMultipleObjects` over
`{wait_event, sigevent}`.

**M3 — but `WakeByAddressAll` wakes a waiter whose value is unchanged.** The waiter returned `TRUE` after
404.7 ms with the word still at its undesired value. This is the documented spurious-wake allowance, and it is
load-bearing: it means a signal-delivery path can interrupt a `WaitOnAddress`-based `FUTEX_WAIT` by publishing
the wait address in the thread's control block and calling `WakeByAddressAll` on it. A full simulation —
signal thread sets a pending flag, then wakes the published address; waiter loops on
`{pending? → EINTR, word changed? → 0, else WaitOnAddress}` — delivered `-EINTR` in **407.5 ms** against a
signal raised at 400 ms. **`EINTR` without an event object.**

**M4/M5 — the price.** Same-process round-trip: `WaitOnAddress` **0.549 µs** vs auto-reset event **7.326 µs**
— flinux paid **13×** for interruptibility it could have had for free via M3. Cross-process, named event plus
shared word: **3.548 µs** round-trip, **1.774 µs per one-way wake**; `SetEvent` on a cached handle is 0.229 µs
while `OpenEventA` by name is 1.136 µs, so a shared-futex implementation **must cache handles** rather than
open by name per wake.

That yields a two-tier design, and the tiering falls out of the ABI rather than being imposed:

- **`FUTEX_PRIVATE_FLAG` (and every futex the engine already routes to `g_fbk_private`, `thread.c:2002`) →
  `WaitOnAddress`/`WakeByAddressSingle`/`WakeByAddressAll`.** 0.549 µs, interruptible via M3, no kernel object
  per address, no namespace. This is the common case by a wide margin: glibc marks every
  `PTHREAD_PROCESS_PRIVATE` mutex, condvar and rwlock private.
- **Shared futexes (`g_fbk`, and every key resolved through the shared-object registry) → a fixed array of
  named auto-reset events in the per-session NT object directory, one per bucket, plus the existing shared
  bucket table for waiter bookkeeping.** 1.774 µs per wake. The events are opened once per process and cached.
  Bucket-granular wakes mean spurious wakeups, which `FUTEX_WAIT` explicitly permits — and which
  `src/linux_abi/thread.c:30` already documents the engine relying on: *"FUTEX_WAIT may return 0 spuriously
  (per spec); the guest re-checks the word and re-waits."*

Two constraints on any implementation. `WaitOnAddress` accepts sizes 1, 2, 4 and 8 only — size 3 returned
`ERROR_INVALID_PARAMETER` (M-measured alongside M7); Linux futexes are always 4 bytes, so this is not binding
but must be asserted. And timeouts: **M7**, `WaitOnAddress`'s `DWORD` millisecond timeout quantises to ~15.6 ms
at default timer resolution, so a `FUTEX_WAIT` with a 1 ms deadline oversleeps by 15×. The fix is a scoped
`NtSetTimerResolution` request while a short-deadline futex wait is outstanding.

### 6.4 What this actually requires of us

The engine's futex is complete at the Linux front. The gap is one level down, and the survey named it
precisely: **`hl_host_sync_services` has no wait/wake primitive.**

```c
typedef struct hl_host_sync_services {
    HL_ABI_HEADER;
    hl_host_result (*mutex_create)(void *context);
    hl_host_result (*mutex_lock)(void *context, hl_host_handle mutex);
    hl_host_result (*mutex_unlock)(void *context, hl_host_handle mutex);
    hl_host_result (*mutex_close)(void *context, hl_host_handle mutex);
    hl_host_result (*fork_prepare)(void *context);
    hl_host_result (*fork_parent)(void *context);
    hl_host_result (*fork_child)(void *context);
} hl_host_sync_services;                       /* include/hl/host_services.h:626-635 */
```

`src/linux_abi/thread.c` calls `pthread_mutex_lock`, `pthread_cond_wait`, `pthread_cond_timedwait`,
`pthread_cond_broadcast`, `pthread_mutexattr_setpshared` and `pthread_condattr_setpshared` **directly** — 149
POSIX call sites across 31 symbols (M10). The only host-service call in the entire futex path is one
`hl_linux_memory_create` for the bucket table. This is the single largest structural gap in the whole residue,
and it is the one place where the answer is "new host API", not "route it through an existing group".

---

## 7. What flinux refused outright, and what it cost a guest

Their `not implemented` / `not supported` log strings are the honest inventory — 230 `TODO`/`FIXME`/
`not implemented` markers across `src/`. The ones in this residue, with the guest-visible consequence:

| Refused | Site | A guest program visibly cannot… |
|---|---|---|
| **All SysV IPC** (fatal, not `ENOSYS`) | `syscall_table_x64.h:29-31,64-71`; `_x86.h:117` | run PostgreSQL, Oracle clients, X11's MIT-SHM, or any `ipcs`-shaped tool. The process is killed, not given an errno |
| **Interprocess futex** | `futex.c:32` | use process-shared mutexes, condvars or semaphores (§6.2) |
| `FUTEX_WAIT_BITSET`/`WAKE_BITSET`/`WAKE_OP`/PI | `futex.c:249-252` | use modern glibc `pthread_cond_timedwait` (BITSET + `FUTEX_CLOCK_REALTIME`), `pthread_cond_broadcast`'s `WAKE_OP` fast path, or `PTHREAD_PRIO_INHERIT` mutexes |
| `set_robust_list` (accepted, ignored) | `futex.c:255-262` | recover a mutex whose owner died — the lock is held forever, no `EOWNERDEAD` |
| **`kill` to any other process** | `sig.c:503-518`, `"signal_kill: Killing other processes are not supported."` | ^C a pipeline, `kill %1`, or have a shell reap and signal its children. This alone forecloses job control |
| `tgkill`, `alarm`, `setitimer`, all POSIX timers | `sig.c:622-626`, `sig.c:599-604`, `timer.c:148-188` | use `sleep()` where libc implements it via `alarm`; use any interval timer. All return **success** and never fire |
| `sigaltstack` | `sig.c:714-718` | handle `SIGSEGV` on an alternate stack — i.e. stack-overflow recovery, and Go's runtime |
| `arch_prctl(ARCH_SET_FS)` | `tls.c:178-202` | run **any** 64-bit glibc program (§4.2) |
| `times`, x86-64 `gettid` | `syscall_table_x64.h:100,186` | call `times()` or `gettid()` — fatal |
| `mremap`, `msync` | `mm.c:1667`, `mm.c:1556` | grow a realloc'd large allocation in place; flush a shared file mapping |
| `rusage` on `wait4` | `process.c:299-300` | get child CPU time from `wait4`; `time(1)` reports zeros |
| `/proc/self/exe`, `/proc/self/fd`, any symlink | `virtual.h:24-29` (no symlink node type) | re-exec itself; enumerate its own descriptors (§2.2) |
| `chroot`, namespaces | `chroot` wired but the tree has no namespace concept | isolate anything |

The pattern worth naming: **flinux's failure mode is silent success far more often than it is an error.**
`prctl`, `capget`, `capset`, `setsid`, `setpgid`, `alarm`, `setitimer`, all six POSIX timer calls,
`getpriority`, `setpriority` and `set_robust_list` all return `0`. Only `getrusage`, `setrlimit` and the
unsupported futex ops return an errno. `DOCS.md:63-64` item 7 — *"No successful API call silently ignores a
requested option, limit, payload, or operation"* — is the rule that forbids exactly this, and it is the single
most valuable thing the engine has that flinux did not.

---

## 8. Recommendations

Four dispositions, applied per area. The engine's own convention governs which is legitimate: a group is
advertised only when every callback is real (`src/host/windows/README.md:6-8`); a missing capability is a
cleared bit, an impossible *request* within a live callback is `HL_STATUS_NOT_SUPPORTED` with a discriminant in
`detail`, and a NULL callback in an advertised group is `HL_STATUS_ABI_MISMATCH` — a bug, not an absence
(`src/core/host_services.c:23-157`).

### 8.1 `/proc` → **Linux-front over existing primitives; no new host API**

`vfs.c`'s synthesis is already host-neutral in content. Two host dependencies to break:

1. **Delete the `mkdtemp`/`symlink` materialisation** at `vfs.c:4528-4565`. `/proc/self` readdir should be
   served from the static table that already exists two lines below it (`vfs.c:4533-4537`) plus the three
   magic links, exactly as `proc_open` (`vfs.c:5479`) already serves reads. This is a simplification on Linux
   too, not a Windows special case, and it removes a temp directory per process.
2. **Route `src/host/linux/system.c`'s five `/proc` reads through a callback context**, in the shape of
   `hl_linux_misc_dispatch` (`src/linux_abi/syscall/misc.h:31`). The context already carries
   `host_memory_total`, `host_memory_free`, `uptime_seconds`, `process_count` and `loads`
   (`dispatch.c:835-862`) — extend it rather than inventing a group:

   ```c
   /* additions to hl_linux_misc_context — same style as the existing members */
   uint64_t (*boot_time_unix)(void *context);                  /* NtQuerySystemInformation(SystemTimeOfDayInformation) */
   int      (*cpu_times)(void *context, uint64_t *user_ns,     /* GetSystemTimes                                        */
                         uint64_t *system_ns, uint64_t *idle_ns);
   int      (*cpu_topology)(void *context, uint32_t *online,   /* GetLogicalProcessorInformationEx                      */
                            uint32_t *possible);
   int      (*self_exe_path)(void *context, char *out, size_t cap); /* QueryFullProcessImageNameW + path translation    */
   ```

   flinux's Win32 sources for the first three are directly reusable as *facts about Windows*
   (`procfs.c:110-146`, `:236-274`), and `/proc/cpuinfo` from `CPUID` (`procfs.c:148-225`) is the right shape
   for a host with no `/proc/cpuinfo` of its own.

**Do not** add an `hl_host_procfs_services` group. `/proc` content is Linux semantics and belongs in
`linux_abi`; only the four host facts above cross the seam.

### 8.2 SysV IPC → **extend `hl_host_shared_memory_services`; do not build on it as-is**

The group cannot express `shmget` (§3.4). Two ways forward, and the first is preferred:

**(a) Add a keyed, creator-outliving namespace to the group.** Two callbacks and one flag:

```c
/* appended to hl_host_shared_memory_services (ABI 2); absent-not-NULL on an ABI 1 provider */

/* Create or open by name within a per-instance namespace. The object outlives every handle
 * until unlink_named, which is the SysV IPC_RMID lifetime. name is UTF-8, NUL-terminated,
 * <= HL_HOST_SHM_NAME_MAX. flags: HL_HOST_SHM_CREATE | HL_HOST_SHM_EXCLUSIVE.
 * value = handle; detail = the reopen identity, as create. */
hl_host_result (*create_named)(void *context, const char *name, uint64_t size, uint32_t flags);

/* Detach the name. Existing handles and mappings stay valid; new opens fail with
 * HL_STATUS_NOT_FOUND. Returns HL_STATUS_NOT_FOUND if the name was already unlinked. */
hl_host_result (*unlink_named)(void *context, const char *name);
```

Linux implements both with `shm_open`/`shm_unlink` — which is what `sysv.c` already calls, so the Linux
provider is a move, not a rewrite. Windows implements them per §3.3 option (2): a section over a real file in
a per-instance directory, which is the only shape that satisfies M6 without a broker. The namespace scoping
should reuse flinux's per-session object-directory idea (`shared.c:78-99`) for the *event/mutant* half and a
per-instance filesystem directory for the *section* half.

**(b) If (a) is judged too large:** implement `sysv.c`'s backing store Linux-front over
`hl_host_file_services` + the memory group's `MAP_SHARED` path, treating a segment as a file in a container
directory. No new host API; more Linux-side code; identical lifetime semantics. This is strictly a scheduling
choice, not a correctness one.

The **robust cross-process spinlock** in `sysv.c` (`hl_ipc_lock`/`hl_ipc_unlock`, `sysv.c:343,363`) needs the
same shared-futex machinery as §8.4 — it is not a separate problem. flinux's answer to the same need was a
named `NtCreateMutant` (`process.c:74-85`), which is correct but coarse and gives `WAIT_ABANDONED` recovery for
free; that is worth having as the fallback if the shared futex slips.

### 8.3 Identity, `prctl`, caps, sched, rlimits → **extend the `misc.c` callback context**

`src/linux_abi/syscall/misc.c` has **zero POSIX calls** (M10) and already owns `uname`, `sysinfo`,
`getrandom`, `sethostname`, `setdomainname`. `proc.c` (109 sites / 40 symbols) and `rare.c` (36 / 14) are the
same *kind* of code with the seam not yet cut. Extend the same context rather than adding a host group:

```c
/* additions to hl_linux_misc_context */
uint64_t (*host_pid)(void *context);                                   /* GetCurrentProcessId          */
uint64_t (*host_tid)(void *context);                                   /* GetCurrentThreadId           */
int      (*thread_cpu_times)(void *context, uint64_t *user_ns,         /* GetThreadTimes               */
                             uint64_t *system_ns);
int      (*process_cpu_times)(void *context, uint64_t *user_ns,        /* GetProcessTimes              */
                              uint64_t *system_ns,
                              uint64_t *child_user_ns, uint64_t *child_system_ns);
int      (*yield)(void *context);                                      /* SwitchToThread               */
int      (*affinity_get)(void *context, uint64_t *mask, size_t words); /* GetProcessAffinityMask       */
int      (*affinity_set)(void *context, const uint64_t *mask, size_t words);
```

`process_cpu_times` closes `times` (canonical 153, `time.c:574`) and `getrusage` (165, `proc.c:1371`) at once —
both are pure derivations of the same four numbers, and both are among the syscalls flinux left fatal or
broken. The uid/gid family, `getgroups`, `capget`/`capset`, `prctl` and the rlimit store are **already**
container state in `proc.c`, not host state; they need no callback at all, only the removal of the incidental
POSIX calls around them. `setsid`/`setpgid`/`getpgid`/`getsid` are the exception — they currently delegate to
the real host (`rare.c:822`, `proc.c:1229`) and on Windows must become pure container-table operations, which
is *more* correct than the Linux path, not less. flinux's incoherence here (`setpgid` writes nothing,
`getpgid` reads the table — §4.1) is the failure to avoid.

**Declare a typed absence for:** `sched_setscheduler`/`sched_setparam`/`sched_rr_get_interval` (real-time
policies have no Windows analogue that preserves the guarantee); `setpriority` with a real `PRIO_USER` target.
Return the errno Linux returns for a permission failure rather than pretending success — the opposite of
flinux's `"Fake returning 0"` (`process.c:962-974`).

### 8.4 `futex` → **new callbacks on `hl_host_sync_services`** (the only genuinely new host API)

This is the one area where routing through an existing group is not possible: the contract has no wait/wake
primitive anywhere (§6.4). Proposed, shaped to the two tiers M1–M5 established:

```c
/* appended to hl_host_sync_services (ABI 3); absent-not-NULL on an ABI 2 provider.
 *
 * A "parking spot" is identified by (scope, key). PRIVATE keys are process-local and may be
 * virtual addresses; SHARED keys are values the caller has already canonicalised across
 * processes (the engine's shared-object registry, thread.c:211-323) and must be usable by a
 * process that never saw the address.
 *
 * Spurious wakes are permitted and expected: HL_STATUS_OK from wait means only "re-check".
 * That is the futex contract (thread.c:30) and it is what lets a Windows provider wake at
 * bucket granularity. */

enum { HL_HOST_PARK_PRIVATE = 0, HL_HOST_PARK_SHARED = 1 };

/* Block until woken, until deadline_ns (absolute, host monotonic; HL_HOST_DEADLINE_INFINITE
 * to block), or until interrupted. compare_size is 4 or 8; the provider re-checks
 * *(uintN*)address == expected under its own park lock and returns HL_STATUS_WOULD_BLOCK
 * without sleeping if it differs -- this is the futex WAIT race, and only the provider can
 * close it.
 *   HL_STATUS_OK          woken or spurious -- caller re-checks
 *   HL_STATUS_WOULD_BLOCK value already differed (guest sees EAGAIN)
 *   HL_STATUS_TIMED_OUT   deadline reached (guest sees ETIMEDOUT)
 *   HL_STATUS_INTERRUPTED interrupt_park was called for this thread (guest sees EINTR) */
hl_host_result (*park)(void *context, uint32_t scope, uint64_t key, const void *address,
                       uint64_t expected, uint32_t compare_size, uint64_t deadline_ns);

/* Wake at most `count` parked waiters on (scope, key). value = number definitely woken;
 * a provider that can only wake at bucket granularity reports what it can prove and the
 * caller's bookkeeping does exact FUTEX_WAKE(n) selection, as thread.c:81 already does. */
hl_host_result (*unpark)(void *context, uint32_t scope, uint64_t key, uint32_t count);

/* Interrupt one thread's outstanding park, for signal delivery. Idempotent; safe from a
 * signal-delivery context; must not allocate or log. Returns HL_STATUS_OK whether or not
 * the thread was parked. */
hl_host_result (*interrupt_park)(void *context, uint64_t host_tid);
```

Provider mapping:

- **Linux** — `SYS_futex` for both scopes, or the existing `pthread_cond` table verbatim. Cheap, and it lets
  the migration land without changing Linux behaviour.
- **Windows** — `PRIVATE`: `WaitOnAddress` / `WakeByAddressSingle` / `WakeByAddressAll`, 0.549 µs (M4);
  `interrupt_park` sets a per-thread pending flag then `WakeByAddressAll`s the address the thread published,
  which M3 proves wakes a waiter whose value is unchanged and which delivered `EINTR` in 407.5 ms against a
  400 ms signal. `SHARED`: a fixed array of named auto-reset events in a per-instance NT object directory
  (M8 confirms `\BaseNamedObjects\<name>` works), handles cached per process (M5: 0.229 µs cached `SetEvent`
  vs 1.136 µs `OpenEventA`), 1.774 µs per wake. Deadlines under ~15 ms require a scoped
  `NtSetTimerResolution` (M7).
- **macOS** — `__ulock_wait`/`__ulock_wake` or the existing pthread path.

`compare_size` and the provider-side re-check are the non-obvious part and must not be dropped: the
compare-and-park has to be atomic with respect to the park queue, and on Windows only `WaitOnAddress` itself
can do that. Handing the provider a pre-checked "just sleep" call reintroduces the lost-wakeup race that
flinux's own timeout path exhibits (§6.1).

### 8.5 Time, clocks, randomness → **already routed; two fidelity fixes**

`hl_host_clock_services` covers all of it and `src/host/windows/clock.c` already ships. Two items from §5 that
the Windows provider should be checked against, both measured:

- `realtime_ns` must use `GetSystemTimePreciseAsFileTime`, not `GetSystemTimeAsFileTime` (M9: the latter
  resolved **0** across a `Sleep(0)`; it is the coarse clock).
- `sleep_until` and every deadline path must scope a `NtSetTimerResolution` request for sub-15 ms deadlines
  (M7), and must document that it does so.

`getrandom` is already in `misc.c` with exact Linux flag validation; `RtlGenRandom` (`SystemFunction036`) is
the right primitive, as flinux found (`fs/random.c:30-38`) — but the callback must distinguish failure from
zero bytes, which flinux does not.

---

## 9. The shortest path

Ordered by what a guest *actually executes*, not by syscall number. Tier 0 is measured from the engine's own
fixture; tiers 1 and 2 are derived from the documented startup paths of glibc/musl and busybox and are marked
accordingly.

### Tier 0 — the nolibc guest (**measured**)

`tests/compat/isa/x86_64/hello_x86` is a freestanding binary. Disassembly shows exactly two syscalls:
`mov $0x1,%eax; syscall` (`write`) and `mov $0xe7,%eax; syscall` (`exit_group`, 231).

**From this residue it needs: nothing.** Not one item. It needs `write` and a process teardown. If this does
not run, the problem is the translator, the loader or the fd lane — not this document's surface.

### Tier 1 — a **static glibc** hello-world

*Derived from glibc's `csu`/`libc-tls` startup path, not traced on this branch — see §10.*

This is the first bar that touches the residue, and it is short. In execution order:

1. **`arch_prctl(ARCH_SET_FS)` — canonical 158.** `__libc_setup_tls` calls it before anything else; on failure
   static glibc aborts before `main`. This is the syscall that ended flinux's 64-bit ambitions
   (`tls.c:178-202`). It is item one.
2. **A non-fatal `ENOSYS` tail.** Already correct (`dispatch.c:877`) — named only because flinux's fatal
   `unimplemented` (`syscall_dispatch.c:63-68`) is the alternative and it makes every subsequent item
   unreachable. `rseq` (334), `set_robust_list` (99) and `futex_waitv` (449) must all reach it cleanly.
3. **`set_tid_address` (96)** — must return the real tid **and store the pointer**. flinux returned a tid and
   dropped the pointer, and `pthread_join` never returned (§4.2).
4. **`brk` (214) and `mmap`/`mprotect`** — not this residue's, but the gate before it.
5. **`prlimit64` (261) / `getrlimit` (163)** for `RLIMIT_STACK` and `RLIMIT_NOFILE`. Already implemented
   (`rare.c:1312`, `proc.c:2443`); needs only the POSIX calls around it removed.
6. **`getpid` (172), `gettid` (178), `getuid`/`geteuid`/`getgid`/`getegid` (174-177)** — container-table reads,
   no host call. Note flinux made `gettid` **fatal on x86-64** by table omission; check the number map.
7. **`uname` (160)** — already host-neutral in `misc.c`. Zero work.
8. **`exit_group` (94)** and the `futex_robust_exit` teardown it triggers (`proc.c:735`).
9. **`clock_gettime` (113)** for `CLOCK_REALTIME`/`CLOCK_MONOTONIC` — already ships on Windows.
10. **A complete `auxv`.** Not a syscall, but it is where flinux was quietly wrong: `AT_RANDOM` pointing at
    uninitialised stack (`exec.c:81-82`) silently defeats glibc's stack canary. Fill `AT_RANDOM` from
    `RtlGenRandom`, and supply `AT_CLKTCK`, `AT_HWCAP`, `AT_EXECFN`, `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`
    which flinux omits entirely.

**Not needed at tier 1:** `futex` (single-threaded and uncontended — glibc takes the futex path only on
contention), any `/proc` entry, any SysV IPC, `times`, `getrusage`, `sched_*`, `capget`/`capset`, `prctl`
beyond `PR_SET_NAME` being harmless. **Ten items, of which items 1, 3 and 10 are the only ones where a wrong
answer is silent rather than loud.**

### Tier 2 — a **busybox `ash` shell** (*derived from busybox's applet and job-control paths, not measured*)

Everything from tier 1, plus — in rough order of how quickly the shell hits them:

1. **`/proc/self/exe` as a working magic link.** busybox re-execs itself to dispatch applets. flinux could not
   provide this at all because its virtualfs has no symlink node (`virtual.h:24-29`); the engine already has
   magic links (`vfs.c:4557-4565`) but they are materialised with a real `symlink()` — see §8.1.
2. **`setsid` (157), `setpgid` (154), `getpgid` (155), `getsid` (156)** as **coherent container-table
   operations.** This is the tier-2 item most likely to be got wrong, because the flinux failure mode is
   available by accident: stub the setters to `0` while the getters read a table, and job control fails in a
   way that looks like a shell bug (§4.1).
3. **`kill` (129) / `tgkill` (131) to other processes.** flinux refused outright — `"signal_kill: Killing other
   processes are not supported."` (`sig.c:503-518`) — and that single refusal forecloses ^C on a pipeline,
   `kill %1`, and every form of job control. Signals are another agent's surface; it is listed here because
   **it is the hard gate between "runs a program" and "runs a shell"**, and no amount of `/proc` or SysV work
   substitutes for it.
4. **`wait4` (260) with a real `rusage`,** for the `times` builtin. flinux logged `"rusage not supported"`
   (`process.c:299-300`) and returned zeros.
5. **`times` (153) and `getrusage` (165)** — one callback (§8.3) closes both. flinux had `times` **fatal**.
6. **`nanosleep` (101) / `clock_nanosleep` (115)** for `sleep`, with M7's timer-resolution caveat: without it,
   `sleep 0.01` sleeps 15 ms.
7. **`getgroups` (158), `capget`/`capset` (90/91), `prctl` (167)** — container state, already implemented;
   the requirement is only that they do not silently lie. flinux's `capget` returns success **without writing
   the caller's buffer** (`process.c:983-988`).
8. **`sched_getaffinity` (123)** reporting the real online set, for `nproc`.
9. **`/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`, `/proc/stat`, `/proc/self/fd`, `/proc/<pid>/stat`** —
   for the `free`, `nproc`, `uptime` and `ps` applets. Content already exists in `vfs.c`; the four host facts
   in §8.1 are what is missing, and flinux's Win32 sources for three of them are directly reusable.
10. **`futex` private only.** `ash` is single-threaded, but a glibc-linked busybox still initialises the
    pthread machinery and takes the futex path on any contention. `WaitOnAddress` alone suffices — **the
    shared tier is not needed to run a shell.**

**Not needed at tier 2:** SysV IPC (nothing in busybox's default applet set uses it), the shared/interprocess
futex, `/proc/self/maps`, PI futexes, robust lists, `sched_setscheduler`.

### Tier 3 — where the rest becomes load-bearing

Stated only so the ordering above is not mistaken for a claim that the rest is optional.

- **Any threaded program** (a JVM, anything linking `-lpthread` and actually creating threads): full private
  futex incl. `FUTEX_WAIT_BITSET` + `FUTEX_CLOCK_REALTIME` (modern `pthread_cond_timedwait`) and `FUTEX_WAKE_OP`
  (`pthread_cond_broadcast`), plus `set_robust_list` actually walked. The engine implements all of these
  (`thread.c:1996-2177`); only the host primitive is missing.
- **Anything that forks and shares memory** — process pools, PostgreSQL, `sem_open` users: the **shared**
  futex tier (§8.4) and SysV shm (§8.2). This is where flinux's twin refusals finally bind, and where the
  engine's 17 `MAP_SHARED` ledger arenas and shared-object futex keys become load-bearing.
- **Go**: `sigaltstack` (flinux refused), `sched_getaffinity`, `/proc/self/maps`, `membarrier`, `rseq`.
- **A JVM or any allocator that sizes itself off the machine**: `/proc/meminfo` and `sysinfo` agreeing,
  `/proc/self/statm`, `/proc/sys/vm/overcommit_memory`.

---

## 10. Unknowns, and what would settle them

- **The unity-TU diagnostic count could not be reproduced.** `ninja -k 0 win_unity_probe_x86_64` halts at the
  first missing header (`src/core/target/x86_64.c:48`, `<sys/socket.h>`) with one diagnostic. M10's 89
  symbols / 591 sites is a substitute measured by symbol, not by diagnostic, and the two will not agree
  numerically. Settling it requires the header shims to land first; until then, per-file symbol counts are the
  honest progress metric.
- **The tier-1 and tier-2 syscall sets are derived, not traced.** Tier 0 was read out of the fixture binary.
  Tiers 1 and 2 come from glibc's and busybox's documented startup and job-control paths. A single `strace -f`
  of a static hello-world and of `busybox ash -c 'echo hi | cat'` on the Linux host would convert both to
  measurements, and should be done before the list is used to sequence work.
- **`WaitOnAddress`'s timeout clock is unidentified.** M7 establishes the quantisation but not whether the
  underlying deadline is interrupt-time (suspend-excluding) or system-time. `FUTEX_WAIT` is `CLOCK_MONOTONIC`
  by default and `CLOCK_REALTIME` under `FUTEX_CLOCK_REALTIME`; if `WaitOnAddress` tracks a different clock,
  long deadlines across a machine sleep will be wrong. A suspend/resume test would settle it; that could not
  be run here.
- **The shared-futex bucket count is unmeasured.** §8.4 proposes a fixed array of named events sized to the
  existing 256 buckets (`thread.c:31`), which means 256 kernel objects per engine instance and bucket-granular
  spurious wakes. Whether 256 is the right trade between object count and spurious-wake storms under a real
  process-shared workload has not been measured; `tests/compat/process/futex_xproc.c` and
  `futex_shared_key.c` are the fixtures that would show it.
- **`hl_host_shared_memory_services` has no consumer**, so the proposed `create_named`/`unlink_named` extension
  (§8.2) is designed against `sysv.c`'s needs on paper. It should be validated against `/dev/shm`
  (`container/shm.c`) and `memfd_create` (`rare.c:105-158`) before the ABI is bumped, since all three want the
  same namespace and only one of them should define it.
- **flinux's cross-process `/proc` RPC (§2.3) was not benchmarked.** Its cost is one `OpenProcess`, two
  `DuplicateHandle`s, a mutex acquisition and a pipe round-trip per query. Whether that is acceptable for
  `ps`-shaped workloads that query dozens of pids is unknown; the alternative — a shared arena each process
  publishes its own summary into — trades freshness for cost and was not explored by them.
