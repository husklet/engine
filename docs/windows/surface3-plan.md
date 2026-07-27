# Surface 3 — the ambient-descriptor surface

Scoping and migration plan for the ~1.4k sites in the portable layer that operate on a raw POSIX
descriptor. **Nothing here has been implemented, and no source file was edited to produce it.** Every
count was re-derived from the tree at `feat/windows-amd64`; where a claim is inference rather than
reading, it says so.

`DOCS.md` is normative. `docs/windows/linux-abi-fd-lane.md` §2.1 named this surface and deferred it;
this file is the answer. It **corrects** that document's §2.1 estimate (§1.2 below) and its §7 sizing
remark.

> These `docs/windows/*.md` files are working scratchpad for coordinating the port. They are not
> shipped documentation and **no source file may reference them.**

> Line references were read from the working tree while other port work was in flight (the memory
> group gained `HL_HOST_MEMORY_ABI` 7 during this survey). Line numbers in `include/hl/host_services.h`
> and `src/linux_abi/container/netns.c` may have drifted; the symbol names have not.

---

## 1. Counts, re-derived

### 1.1 Method

The earlier sweep's first attempt counted identifier *occurrences*, which in this tree are dominated by
comments and `switch` labels. I re-ran it with a real tokenizer rather than a regex:

- Strip `/* */` and `//` comments and the contents of string/char literals by scanning the file as a
  character stream (not by regex), preserving line numbers.
- Require the identifier be immediately followed by `(`.
- Reject a preceding `[A-Za-z0-9_]` (kills `proc_dir_try_open(`), a preceding `.` or `>` (kills
  `ops->read(`, `file->close(`), and a `#define NAME(` prefix (kills macro definitions).
- 122 candidate function names covering byte I/O, descriptor lifecycle, the `*at` family, sockets,
  readiness/event, directory enumeration, termios and mapping.

The delta between occurrences and calls is large and confirms the earlier warning, though my numbers
for the specific example differ: in `syscall/fs.c`, `openat` appears **24** times and is **called 4**;
`close` appears 55 times and is called 25; `fcntl` appears 22 times and is called 21. (The prior doc
cited 86 occurrences / 3 calls for `openat`; I cannot reproduce 86 with a word-boundary match on that
file and I do not know what it counted.)

Spot-checked for false positives on the risky identifiers — `bind`, `open`, `isatty`, `mkstemp`,
`shm_open` — by reading every hit. All were genuine calls. There are no macro or static-function
shadows of any POSIX name in `src/`. I believe the count is accurate to within a few sites; it is
certainly not accurate to a single site and should not be quoted as if it were.

### 1.2 The numbers

**1,453 sites across 81 distinct functions in 31 files** in `src/linux_abi`, against the earlier
**1,289 across 52**. The gap is almost entirely my wider function list — the earlier sweep appears not
to have counted `kevent`/`kqueue` (36+10), the termios family (`isatty` 15, `tcgetattr`/`tcsetattr`
12, `tcgetpgrp`/`tcsetpgrp` 6), `mkstemp` (18), `shm_open` (7), and the `opendir`/`readdir`/`seekdir`
cursor family (42). Those all belong in Surface 3: every one takes or returns a descriptor.

| Population | Earlier | **Mine** | Files |
|---|---|---|---|
| **A** — guest fds | ~441 | **522** | `syscall/{fs,io,net,event,binding}.c` |
| **B** — engine-internal | ~351 | **393** | `checkpoint.c` 304, `fork.c` 33, `sentry.c` 19, `thread.c` 18, others |
| **C** — Linux-only | ~319 | **403** | `container/{netns,vfs}.c` 352, `vfs/{resolve,overlay}.c` 39, `state.c` 10 |
| **unclassified** | — | **135** | `syscall/{helpers,guest_copy,sysv,proc,mem,rare,dispatch,inotify,aio}.c`, `eventfd.c` |

The earlier triage's three populations sum to 1,111, not 1,289 — **178 sites were counted but never
assigned to a population.** My unclassified 135 is the same hole: `syscall/helpers.c` (28) holds the
epoll shadow tables and the `RLIMIT_NOFILE` gate, `syscall/proc.c` (19) holds the execve CLOEXEC
sweep, `syscall/mem.c` (17) passes the guest's fd straight into `mmap`. These are population A by
behaviour — they operate on guest fd numbers — and should be folded into it. **Working population A
is therefore ~600, not ~441.**

`checkpoint.c` is 304 by my count, not 276.

### 1.3 The count is the wrong metric

The call sites are the visible part. The load-bearing part is that **the guest fd number is an index
into 127 static arrays**:

```
 47  src/linux_abi/container/vfs.c        g_fdpath[HL_NFD][192], g_eventfd_peer, g_pipe_identity,
 45  src/linux_abi/container/netns.c      g_sock_*, g_tcp_*, g_udp_*, g_ipopt_*, g_ofd_id, …
 14  src/linux_abi/syscall/helpers.c      g_ep_*, g_flock_type, g_lk*
  6  src/linux_abi/syscall/dispatch.c     g_ep_provider_generations, g_mqfd_*
  4  src/linux_abi/syscall/io.c           g_lease, g_fsig, g_dn_mask, g_dn_sig
  4  src/linux_abi/syscall/event.c        g_ep_prime, g_ep_wake_armed, g_ep_member
  3  src/linux_abi/syscall/fs.c           g_ptm_term[HL_NFD], g_ptm_win[HL_NFD], g_ptm_*set
  2  src/linux_abi/checkpoint.c           views[HL_NFD], desired_pipe[HL_NFD]
  1  each: signal.c (g_sigfd_slot), container/state.c (g_fd_cport)
```

`HL_NFD` is 65536 (`container/state.c:18`). `fd_reset_emul` (`syscall/fs.c:343-498`, **155 lines**) is
one function that clears all of them at index `fd`; it is called from `close`, `dup3`, execve and
checkpoint restore. A migration that substitutes calls but leaves these tables in place has moved
nothing: the tables *are* the descriptor model.

`container/vfs.c:235` `g_ovldir[1024][192]` and `:239` `g_opath[1024]` are indexed by fd but sized
**1024**, not `HL_NFD`, and the guards are inconsistent (`io.c:464,603` and `fs.c:3605` test
`fd < 1024`; nine sites in `fs.c` test `fd < HL_NFD`). `fs.c:427-437` documents the memory corruption
this already caused via `close_range`. This is a live bug independent of the port and it is worth
fixing first regardless of what Surface 3 does.

---

## 2. Does "the API already exists" survive contact?

**Partly. It is true for files and false for everything else.** Read `include/hl/linux_abi.h`,
`src/linux_abi/object.h`, `src/linux_abi/linux_abi.c`, and the `bound_route` dispatcher in
`syscall/binding.c:2129-3830`.

### 2.1 What exists and works

`hl_linux_abi` is a real descriptor/OFD table with Linux semantics: lowest-free allocation
(`hl_linux_find_fd`, `linux_abi.c:186`), shared OFD offsets, `FD_CLOEXEC` per descriptor, status flags
per OFD, `flock_token` identity, and a fork bracket (`fork_prepare`/`fork_parent`/`fork_child`).
Public operations cover: `read`/`write`/`pread64`/`pwrite64`/`readv`/`writev`/`preadv`/`pwritev`,
`lseek`, `fstat`, `ftruncate`, `fsync`/`fdatasync`/`sync_range`/`sync_filesystem`, `openat`
(+`_reserved`, +`_handle_reserved`), `file_adopt_reserved`, `close`, `dup`/`dup2`/`dup3`, `fcntl`,
`map_file`.

`hl_linux_fcntl` (`linux_abi.c:1896`) implements exactly `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`,
`F_DUPFD`, `F_DUPFD_CLOEXEC`. Measured against population A's 107 `fcntl` sites: **101 use only those
six commands.** Seven use `F_SETSIG`, `F_SETPIPE_SZ`/`F_GETPIPE_SZ`, `F_PUNCHHOLE`, `F_NOTIFY`,
`F_ADD_SEALS`/`F_GET_SEALS`. So the fcntl claim holds at 94%.

There is also a typed *object* layer (`object.h`) — `read`/`write`/`status`/`set_status_flags`/
`readiness`/`wait_handle`/`subscribe`/`retire`/`clone`/`close` — and **four fully-built, unit-tested
providers over it**: `epoll.c`, `eventfd.c`, `pipe.c`, `inotify.c`, all in `LINUX_ABI_SOURCES`
(`CMakeLists.txt:188-189`) with tests (`tests/unit/test_epoll.c`, `test_ckptinoq.c`).

### 2.2 What does not exist

**Only `inotify` of those four providers is reachable from a guest syscall.** `bound_route` wires
`nr == 26` to `hl_linux_inotify_create_at` (`binding.c:2187`). `hl_linux_pipe_create`,
`hl_linux_eventfd_create` and `hl_linux_epoll_create/control/wait` have **no caller outside
`tests/unit`**. They are finished, tested, dead code. This is the single largest piece of good news in
this document and §4 builds on it.

**Sockets are absent at every layer.** `bound_route` ends (`binding.c:3804-3826`) with:

```c
    case 20: return 0; /* epoll_create1 */
    case 21: case 22:            /* epoll_ctl, epoll_pwait */
    case 71: case 75: case 76: case 77:   /* sendfile, vmsplice, splice, tee */
    case 200 ... 212:            /* bind…recvmsg, the whole socket family */
        result = -ENOSYS;
```

A typed descriptor cannot be a socket today. `hl_host_network_services`
(`include/hl/host_services.h:520-528`, ABI 1) has six callbacks — `socket`, `bind`, `connect`, `send`,
`receive`, `close` — with **no** `listen`, `accept`, `socketpair`, `sendto`/`recvfrom`,
`sendmsg`/`recvmsg`, `getsockopt`/`setsockopt`, `getsockname`/`getpeername`, `shutdown`. It is
implemented **only on Linux** (`src/host/linux/host.c:3778`); macOS, fake and Windows do not advertise
it. `hl_host_network_address` (`:115-121`) has a 108-byte `local_path`, so AF_UNIX is at least
expressible.

**Also missing entirely:** `ioctl` and the termios family (no typed equivalent anywhere — the typed
lane at `binding.c:3415-3496` reaches termios by calling `tcgetattr(native_fd, …)` on the
*same-numbered native shadow*, which is exactly the thing Windows cannot supply); directory
enumeration with a Linux `d_off` cursor; `flock`; `fallocate`; `fchmod`/`fchown`/`fchdir`; the `*at`
metadata family (Surface 2's job); and any path-from-descriptor operation (§5.3).

**Readiness is polling-only for ordinary files.** `hl_linux_object_poll` (`linux_abi.c:884`) resolves
a typed object's `readiness` when one exists, falls back to `stream->readiness` for an opaque host
file, and otherwise **assumes ready** for whatever the caller asked. It has no blocking wait for
non-object descriptors — it spins on `clock->sleep_until`. That is adequate for regular files (Linux
regular files are always ready) and inadequate for a socket or a pipe.

### 2.3 Verdict

"The API already exists" is **true for the descriptor table, the file byte-I/O path and the flag
model — roughly 60% of population A by site count — and false for sockets, readiness, termios/ioctl
and directory cursors.** Restating it without that split would under-size the work by a factor of
three, because the missing half needs new host-service groups on four backends, not call-site
rewriting.

---

## 3. Population A, broken into migration units

522 sites in the five named files (~600 with the unclassified files folded in). Partitioned by *what
the site needs*, which is what decides whether it is a substitution or a design:

| Unit | Sites | Needs | API status |
|---|---|---|---|
| **A-lifecycle** — `close` 97, `dup`/`dup2`/`dup3` 10 | **107** | `hl_linux_close`, `hl_linux_dup*` | **exists** |
| **A-flags** — `fcntl` | **107** | `hl_linux_fcntl` | **exists** (101/107 commands) |
| **A-byteio** — `read`/`write`/`pread`/`pwrite`/`*v` 43, `lseek` 12, `fstat` 31, `ftruncate` 7, `fsync`/`fallocate`/`futimens`/`fchdir` 4, `mmap` 1 | **98** | `hl_linux_read`…`hl_linux_fstat`, `hl_linux_map_file` | **exists** |
| **A-socket** — `net.c` 61 + 3 elsewhere | **64** | ~14 new `hl_host_network_services` callbacks + a typed socket object + `hl_linux_socket*` | **greenfield** |
| **A-readiness** — `kevent` 28, `kqueue` 5, `poll`/`ppoll`/`pselect` 11, `pipe` 3, inotify 3 (wired) | **49** | typed `epoll`/`eventfd`/`pipe` providers **already built**; blocking wait for non-object fds | **half-built** |
| **A-tty** — `ioctl` 14, `isatty` 5, `tcgetattr`/`tcsetattr` 9, `tc*pgrp` 4 | **32** | new host group or Linux-front emulation over `metadata` | **greenfield** |
| **A-open** — `open` 13, `mkstemp` 1 | **14** | `hl_linux_openat` + Surface 2's `jail_resolve` | exists, blocked on Surface 2 |
| **A-dirent** — `seekdir` 4, `telldir` 2, `rewinddir`/`fdopendir`/`readdir`/`closedir` 4 | **10** | `read_directory` + a per-OFD logical cursor | partial |
| **A-\*at** | **41** | **Surface 2's** work, not this document's | see `linux-abi-fd-lane.md` |
| **A-fdpath** — `hl_native_fd_path` in `fs.c` 19 + `io.c` 2 | **21** | see §5.3 | **no portable answer** |

### 3.1 "Roughly half are one-line `close`/`fcntl` substitutions" — confirmed, and misleading

**Confirmed as arithmetic.** A-lifecycle + A-flags = **214 of 522 = 41%**; adding the trivial members
of A-byteio (`read`/`write`/`lseek`/`fstat`) reaches **~49%**. The prior estimate is right.

**Misleading as a schedule input**, for a reason that is the crux of this plan: *a `close(fd)` →
`hl_linux_close(g_linux_box, fd)` substitution is only correct if `fd` is in the typed table*, and
today a descriptor is in that table only if some *producer* put it there. The producers are `open`,
`openat`, `socket`, `socketpair`, `accept`, `pipe`, `eventfd`, `epoll_create`, `timerfd_create`,
`signalfd`, `memfd_create`, `dup` — and all but `inotify_init1` and embedder-supplied bindings still
return a raw host fd. **The consumer substitutions are not independently landable. They are the tail
of each producer's migration, not units of their own.**

So the honest unit boundary is **per fd-producing family**, and the 214 "one-line" sites distribute
across those families as their cleanup. This does not make the work a month instead of a week; it
makes it *neither*, because the schedule is set by A-socket, A-readiness and A-tty — the three units
with no API — plus the 127 side tables, and not by the 214 easy lines.

### 3.2 The shadow is the invariant, not a wart

`bound_shadow_reserve` (`binding.c:986-1005`) is described in the earlier doc as a tax. It is more than
that: it is the mechanism that keeps the two descriptor namespaces **disjoint in number**.
`bound_handle_reserve` (`:1157-1183`) takes the lowest free *native* fd via
`fcntl(g_bound_sentinel, F_DUPFD_CLOEXEC, minimum)`, then tries `hl_linux_fd_reserve_at` on that same
index, and retries upward until a number is free in **both** spaces. `bound_snapshot`
(`binding.c:955`) is then a reliable oracle for "is this guest fd typed?".

Consequence for ordering: **you cannot delete the shadow until the last ambient producer is gone**, and
until then every new typed producer must go through `bound_handle_reserve`. That is good news — it
means new producers reuse proven machinery — but it means the shadow is the *last* thing removed, not
an early cleanup.

---

## 4. Ordering, and the first unit

### 4.1 The build constraint that dominates ordering

`syscall/{fs,io,net,event,binding,helpers,proc,rare,…}.c` are **not** compiled separately. `dispatch.c`
`#include`s them (`dispatch.c:192-636`) and is itself `#include`d into `src/core/target/x86_64.c:721`,
alongside `container/vfs.c` (`:690`), `container/netns.c` (`:691`) and `checkpoint.c` (`:752`).
`LINUX_ABI_SOURCES` (`CMakeLists.txt:183-194`) contains only the portable half.

Therefore **there is no partial Windows compile of population A.** The unity TU either compiles or it
does not. `cmake/Phase2Production.cmake:77-81` already provides the right instrument for this — the
`EXCLUDE_FROM_ALL` `win_unity_probe_x86_64` OBJECT target, compile-only, so `ninja -k 0` enumerates
every missing symbol in one pass. **Every step below should be measured by the delta in that target's
diagnostic count**, which is a better progress metric than the site count.

### 4.2 Dependency order

```
  A-lifecycle ─┐
  A-flags     ─┼── all blocked on: at least one typed PRODUCER per family
  A-byteio    ─┘

  A-readiness ── blocked on typed pipe/eventfd (built) ── unblocks A-socket's poll story
  A-socket    ── blocked on new host network group ───── unblocks netns.c's 45 tables (pop C)
  A-tty       ── independent; blocked on nothing but a design decision
  A-dirent    ── independent
  A-open      ── blocked on Surface 2's jail_resolve
  A-*at       ── Surface 2
  A-fdpath    ── blocked on A-open and on a path-syntax decision (§5.3)
  shadow removal ── blocked on ALL producers
```

Independent and startable today: **A-readiness (eventfd, pipe), A-tty, A-dirent.**
Everything else has a predecessor.

### 4.3 The first unit: **`eventfd2` (syscall 19) → `hl_linux_eventfd_create_at`**

Named specifically, and here is why it beats the alternatives.

- **The provider is finished and tested.** `src/linux_abi/eventfd.c` (13.5 kB) implements the full
  `hl_linux_object_ops` over `hl_host_counter_services`, including `readiness` and `subscribe`.
- **The host group exists on every non-Windows backend.** `counter` is implemented in
  `src/host/{linux,macos,fake}/host.c`. On Windows it is an event object plus an interlocked 64-bit
  counter — the smallest possible new host group, and a useful first proof that a non-file group can
  be built there.
- **The wiring template is two lines away from where it goes.** `bound_route` already has exactly this
  shape for inotify at `binding.c:2133-2198` (`if (nr == 26 && g_linux_box != NULL) { … reserve shadow
  … hl_linux_inotify_create_at … }`). The eventfd arm is the same ~40 lines against
  `hl_linux_eventfd_create_at`.
- **It measurably deletes state, not just calls.** Today an eventfd is a *pipe pair* plus five side
  tables: `g_eventfd_peer`, `g_eventfd_cslot`, `g_eventfd_gnb`, `g_eventfd_sema`, `g_eventfd_refs`
  (`vfs.c:311,323,346,378,384`), a fork-shared counter arena `g_eventfd_count`, plus
  `eventfd_peer_owner`/`eventfd_peer_vacate` (`io.c:8-28`) and
  `eventfd_poll_writable_fixup` (`event.c:759`). Migrating it removes **five of the 127 fd-indexed
  arrays**, one O(65536) linear scan executed on every `exec_fd_is_engine` call (`proc.c:143`), and a
  readiness bug workaround.
- **Blast radius is small and enumerable.** The consumers are `event.c` case 19, `aio.c:117-128`
  (io_setup eventfd notification), `fs.c:337-357` (the CLOEXEC-after-exec cleanup),
  `netns.c:1020,1140-1146` (SCM_RIGHTS marker), `checkpoint.c:887,1106-1112,3063-3066,3251`. That is
  ~30 sites in six files, all listed.
- **Linux stays green by construction**, because the arm is gated on `g_linux_box != NULL` and sits
  beside the identical inotify arm that already ships.

The two costs, stated plainly: the checkpoint image gains a typed-eventfd record (`checkpoint.c:3251`
currently restores by handing back a raw peer fd), and the fork-shared counter arena's semantics have
to be reproduced by `counter`'s `clone_for_fork` path. Neither is speculative — both are visible in
the files named.

**Rejected alternatives and why.** `pipe2` has the same provider maturity but far higher blast radius
(`g_pipe_identity`, O_DIRECT packet mode, `F_SETPIPE_SZ`, splice/tee/vmsplice, SIGPIPE, and every
shell pipeline in the compat corpus). `epoll` cannot go first: `hl_linux_epoll_control` can only watch
descriptors that are already typed. A pure `close`/`fcntl` sweep cannot go first for §3.1's reason.

### 4.4 After that

2. **`pipe2` → `hl_linux_pipe_create`** (provider built; `hl_host_stream_services` present on three
   backends). Unblocks a large share of A-readiness and is the prerequisite for a typed `epoll`.
3. **`epoll_create1`/`epoll_ctl`/`epoll_pwait` → `hl_linux_epoll_*`**, once enough watched objects are
   typed. Deletes 14 `helpers.c` tables, 4 `event.c` tables and `g_ep_member`'s fd² bitmap space.
4. **A-tty**, in parallel with the above — independent of everything, and the decision is *where* it
   lives (a new host group vs. Linux-front emulation over `metadata` + a per-OFD termios record), not
   *whether*.
5. **A-socket** — the largest unit and the one to design before starting. It needs
   `HL_HOST_NETWORK_ABI` 1 → 2 with ~14 appended callbacks, implementations on four backends, and a
   typed socket object. It also unblocks population C: 42 of `netns.c`'s 112 file-static globals are
   read by `fs.c`/`net.c`/`event.c`/`binding.c`/`vfs.c`/`checkpoint.c`, so **"do not compile
   `netns.c`" is not free** — see §7.
6. **A-open / A-\*at / A-fdpath** — sequenced behind Surface 2.
7. **Shadow removal** — last.

---

## 5. Traps

The two already known are real. `bound_shadow_reserve` is §3.2 and is better understood as an
invariant than a wart. `file->path` returning `C:\…` is §5.3 and is worse than described. The rest:

### 5.1 The guest fd number is a persisted identity

- **Checkpoint on-disk format.** `struct ckpt_fd` (`checkpoint.c:183-190`) carries `int32_t gfd` as
  the **primary key of a saved descriptor**, and restore re-materializes by `dup2(source, r.gfd)` onto
  that literal number at **18 sites** (`checkpoint.c:2812, 2970, 3104, 3149, 3208, 3246, 3283, 3352,
  3373, 3410, 3433, 3451, 3471, 3483, 3502, 3527, 3539, 3561`). Any typed replacement must support
  "install at exactly this index" — which `hl_linux_fd_install_at`/`hl_linux_fd_reserve_at` do, so
  this is tractable, but the format is versioned by it.
- **`checkpoint.c:1160-1166`** falls back to `(uint64_t)(unsigned)(fd + 1)` as the OFD identity when
  no stable id exists. The fd number *is* the identity.
- **`g_fdvis`** (`container/vfs.c:429-543`) is a **cross-process shared-memory** hash table keyed on
  `fdvis_key(pid, fd) = (pid << 32) | (fd + 1)`. The guest fd number is a globally meaningful
  identifier across engine processes, not a process-local index.
- **`/proc/net/*` inode synthesis.** `netns.c:2236-2239` emits `ino = 100000UL + fd`; `vfs.c:6086-6093`
  does the same for `/proc/net/unix` and prints `%016x` of the fd as the "Num" column;
  `vfs.c:3919` builds `pipe:[<fd>]` / `anon_inode:[<fd>]` link targets. A guest that correlates
  `/proc/net/tcp` inodes against `/proc/self/fd` symlinks (`ss`, `lsof`, `netstat`) depends on this
  coincidence.
- **SCM_RIGHTS is a raw integer copy.** `netns.c:263-265` states it: *"hl uses host fds directly as
  guest fds, so the fd integers in an SCM_RIGHTS payload need no remap."*

### 5.2 fd numbers are compared, ranged over, and assumed lowest-first

- **`nofile_gate(int r)`** (`syscall/helpers.c:27-36`) is the **entire `RLIMIT_NOFILE` emulation**, and
  it is a pure number comparison: if the host allocator returned a number `>= guest_nofile_cur()`, it
  closes it and returns `EMFILE`. It assumes the host allocates lowest-free. Nine call sites
  (`fs.c:3187,3365,3433,3529,3569`, `io.c:1845`, `net.c:537,606,1053`). Under a typed table the guest
  limit and the host limit stop being the same quantity and this must be rewritten, not substituted.
- **`engine_fd_vacate` / `engine_fd_reloc` / `engine_fd_vacate_range` / `eventfd_peer_vacate` /
  `exec_fd_is_engine` / `engine_fd_hoist` / `g_bound_sentinel`** — a whole subsystem
  (`io.c:8-28,181-191,306-335`, `proc.c:139-149`, `vfs.c:1533-1537`, `binding.c:12,33-56`) that exists
  *only* because engine-private host fds share the guest's number space. It is dead weight after the
  migration and load-bearing during it. `binding.c:3583-3596` performs three separate number-space
  operations (`fd_reset_emul`, `engine_fd_vacate`, `bound_shadow_dup2`) for one logical `dup3`.
- **`close_range`** (`syscall/rare.c:165-233`) clamps to `sysconf(_SC_OPEN_MAX)` and iterates the raw
  numeric range.
- **Full-space scans** `for (fd = 0; fd < HL_NFD; fd++)`: `checkpoint.c:3062,3076`;
  `netns.c:1746,1794,1800,1806,1812,2236`; `vfs.c:1057,1211,1226,3303,6086`; `event.c:628,698`;
  `io.c:328`; `proc.c:162`.
- **`fcntl(fd, F_GETFD)` as an "is this fd open?" probe** — `vfs.c:3268,3305,6088`, `event.c:632,701`,
  `proc.c:165,236`, `rare.c:222`, `io.c:1878,1896`, `checkpoint.c:1345-1348,1425-1428`. Under a typed
  table this probe answers about the **wrong namespace**, silently. It will not fail loudly; it will
  report "closed" for every typed descriptor. This is my candidate for the most dangerous single
  pattern in the migration.
- **`fs.c:517-521` `at_dirfd_check()`** justifies validating a guest `dirfd` against the real host fd
  table in a comment. Same failure mode.
- **`helpers.c:1310-1312`** stores `g_ep_owner[watched_fd] = epoll_fd + 1` — *one fd number as the
  identity of another fd* — and compares it for equality after fork (`event.c:698-712`).
- **`event.c:130-148`** `ep_native_watch { int32_t epoll; int32_t descriptor; int32_t
  logical_descriptor; }`, searched linearly by number pair over 16384 entries.
- **`io.c:111`** `kevent(oldfd, g_ep_chg[oldfd], …)` — **the guest's epoll fd number is used directly
  as the host kqueue descriptor**, and `event.c:628-640` rebuilds it with `dup2(kq, fd)` onto the guest
  number. `hl_native_kqueue_relocate` exists (`checkpoint.c:3290`) precisely because the shim keys its
  queue by descriptor number.
- **`binding.c:1464,1552-1555,1673,1736`** — the `struct pollfd` array in the typed poll path is
  **indexed by fd number** (`native[fd] = (struct pollfd){.fd = (int)fd, …}`).
- **`sentry.c:2478`** caps at `FD_SETSIZE/8 == 128` for `select`.
- **`vfs.c:1525-1536`** documents that `g_root_fd` landing on fd 3 "shift[s] every guest fd allocation
  up by one," breaking guests that expect a pipe on the by-convention-lowest fd 3. Guest-visible fd
  *numbering* is already an observed compatibility surface.

### 5.3 Path-from-descriptor is a category, not a single trap

`hl_native_fd_path` (`src/host/native_compat.h:34` for macOS `F_GETPATH`, `:498` for Linux
`/proc/self/fd`) has **29 call sites** in `src/linux_abi` — 19 in `fs.c` alone, plus `vfs.c` 3,
`proc.c` 3, `io.c` 2, `dispatch.c` 1, `vfs/overlay.c` 1. Its typed counterpart is `file->path`
(`host_services.h:417`), reached through `logical_fd_path` (`container/route.c:18-30`), which has
exactly **one** caller today (`route.c:78`).

On Windows `file->path` returns a native `C:\…` string. Every one of those 29 sites feeds the result
into Linux-path logic — `atpath`, `confine`, `jail_match`, `hl_fdcache_*` keys, `strcmp` against
`g_rootfs_canon`. This is not "one known trap"; it is a 29-site unit with no portable answer yet, and
it needs a decision recorded *before* A-open starts: either `file->path` gains a documented
guest-path-syntax contract, or `src/linux_abi` gets a host-path→guest-path normalizer at the seam.

Related: `hl_fdcache_fd_setpath(int fd, …)` / `hl_fdcache_fd_evict(int fd)` (`fdcache.h:54-56`) index
`fd_paths[fd_capacity][192]` by fd number — and `fdcache.c` **is** in `LINUX_ABI_SOURCES`, i.e. this
dense-fd assumption has leaked into the separately compiled portable library.

### 5.4 `/proc/<pid>/fd` is materialized from the host fd table

`proc_fd_dir_pid_open` (`vfs.c:3858-3971`) builds a **real temp directory of `N → target` symlinks** by
iterating `hl_host_process_fds(host, …)` — the host's own fd table — and only *overlaying* typed
entries from `proc_fdvis_list`. `vfs.c:3281-3283` says it outright: *"The guest fd numbers ARE the host
fd numbers here, so this process's open fds are exactly the guest's."* `proc_fdinfo_dir_open`
(`vfs.c:3294-3318`) probes `fcntl(fd, F_GETFD)` across all 65536. Once guest fds have no host number,
the host enumeration contributes **nothing** and this directory must be rebuilt from the typed table.
`sentry.c:537,611,1251,1263,1496` maintains a *second, parallel* virtual-fd table with its own
`/proc/self/fd` handling — useful precedent, and a second place to update.

### 5.5 Guest fds handed to host APIs that require a real fd

`mmap(…, fd, …)`: `mem.c:790,804`, `sysv.c:386,441,962`, `logical_vma.c:56`,
`checkpoint.c:443,2507,2513`. `fdopendir`: `fs.c:3683` (`fdopendir(dup(fd))`), `vfs.c:3234`.
`sendfile`/`vmsplice`: `io.c:1650,1704`. `/proc/self/fd/%d` **paths built from a guest fd number**:
`state.c:421`, `fs.c:1862-1864` (O_TMPFILE `linkat` materialization), `proc.c:1985`
(`execve("/proc/self/fd/N")`), `sentry.c:1251,1263`.

---

## 6. Population B — not separable

**The claim does not survive.** A host-private descriptor table inside `src/host/windows/` cannot
absorb `checkpoint.c`, for four independent reasons.

1. **The descriptors are guest descriptors, not engine descriptors.** Roughly 80% of the sites are in
   `ckpt_scan_fds` (`checkpoint.c:1250-1598`) and `ckpt_restore_fds_dir` (`:3032-3593`), which issue
   `fcntl(fd, F_GETFL)`, `lseek(fd, 0, SEEK_CUR)`, `fstat(fd)`, `isatty(fd)` **directly against the
   guest fd number** (`:1280-1281, 1293-1294, 1309, 1321, 1438-1439, 1510-1511, 1532-1534,
   1554-1557, 1560`). `ckpt_capture_pipe` (`:513-547`) and `ckpt_capture_signalfd` (`:549-581`)
   *drain the guest's live descriptor*. The genuinely engine-private cluster — the activation-trigger
   `mmap` (`:442-446`), the `mkstemp` staging files (`:2316-2384, 2772-2822`), the sink `fsync`
   (`:463-470`) — is roughly **30 of ~300 sites**, and even that leaks: `:2811-2817` ends in
   `dup2(restored, record->gfd)`, forcing the private fd onto a guest number.

2. **`checkpoint.c` is not a translation unit.** It is `#include`d at `src/core/target/x86_64.c:752`
   and `aarch64.c:331`, and is absent from `LINUX_ABI_SOURCES`. It shares file-static namespace with
   `container/state.c`, `container/vfs.c`, `container/netns.c`, `eventfd.c` and `epoll.c`, and reads
   their statics directly (`g_eventfd_peer`, `g_pipe_identity`, `g_sock_*`, `g_linux_box`). It cannot
   be moved to `src/host/windows/` in any sense — it is not host code, it is guest-state
   serialization. It also `#include`s `src/host/file.h` and `src/host/system.h` directly (`:54-55`)
   and calls `hl_host_process_fd_private_adopt`/`_remove` at ~30 sites plus
   `hl_host_process_fd_read`/`_peers`/`_interrupt` — none of which `src/host/windows/` implements.

3. **`SCM_RIGHTS` replay has no Win32 analogue at any layer.** `ckpt_restore_socket_queue_load`
   (`:4585-4882`) rebuilds a `combo[253*4]` array of real fds interleaved with engine metadata marker
   fds and issues one `sendmsg(peer->fd, …)` with `cmsg_type = SCM_RIGHTS` (`:4838-4843`) carrying up
   to 1012 descriptors. The purpose is to **re-enqueue descriptors into a kernel socket receive
   buffer** so the guest sees byte-for-byte and rights-for-rights what it had at capture. Windows
   sockets cannot hold a handle in a receive buffer. Note the *capture* side (`:596-672`) drains a
   **guest-created** `AF_UNIX` socketpair carrying arbitrary guest descriptors — this is not an
   engine-to-engine control channel, so a private table would not own those fds anyway.
   (`src/linux_abi/fork.c:40,515` also uses SCM_RIGHTS, but only to hand stdio to the forkserver
   between two engine processes — that one *is* small and *is* replaceable by
   `hl_host_transfer_services`, which already exists with an attachment model and is implemented on
   Linux, macOS and fake.)

4. **Cross-process object distribution is `fork()` + number-preserving `dup2`.** `ckpt_restore_tree`
   (`:5469-5578`) creates every shared kernel object in the init process, then `fork()`s
   (`:5391`); each child `dup2`s the inherited seed onto the guest's exact recorded number. Windows
   has no `fork`, and `CreateProcess` handle inheritance does not preserve handle *values* the way an
   inherited fd table preserves fd *numbers*.

**Revised disposition for B.** The separable part is `fork.c`'s stdio hand-off (33 sites → replaceable
by `transfer`), `sentry.c` (19, already has its own virtual-fd table), `thread.c` (18) and the ~30
genuinely private sites in `checkpoint.c`. The other ~270 checkpoint sites are population A wearing a
different hat: they are guest-descriptor operations, and they migrate when population A migrates, not
before. **Checkpoint/restore should be treated as out of scope for a first Windows milestone** and
gated off, rather than ported.

There is a positive corollary: `hl_host_transfer_services` (`host_services.h:578-593`) is exactly the
right primitive for descriptor passing — *"transfer object identity, never native descriptor
numbers"* — and it is already implemented on three backends. Whoever designs A-socket's AF_UNIX story
should build on it rather than on `SCM_RIGHTS`.

---

## 7. Population C is not free either

The disposition "do not compile `container/{netns,vfs}.c`" understates the coupling. `netns.c` defines
**112 file-static globals**; **42 of them are read from `syscall/fs.c`, `net.c`, `event.c`,
`binding.c`, `container/vfs.c` and `checkpoint.c`** — `g_sock_stream`, `g_sock_fam`, `g_sock_pair_peer`,
`g_lo_port`, `g_so_error`, `g_tcp_*`, `g_udp_*`, `g_ofd_id`, `g_dns_sock`, and more. Because everything
is one unity TU, "do not compile" means "delete the `#include` at `x86_64.c:691` and stub 42 symbols
that population A code reads." That is a real unit and it should be sized, not assumed.

`container/vfs.c` is worse: it owns 47 of the 127 fd-indexed tables, `g_fdvis`, the `/proc/<pid>/fd`
synthesis and `g_root_fd`. It is not optional at all — Surface 2 already depends on it booting.

---

## 8. Unknowns, stated

- **Whether A-tty belongs in a host group or the Linux front.** I have not designed it. `isatty` and
  `TCGETS`/`TCSETS` on a Windows console are answerable via `GetConsoleMode`, but `tcsetpgrp` and
  `TIOCSCTTY` have no meaning there and the fallback behaviour is a product decision.
- **Whether the compat corpus covers fd-number-sensitive guests.** `vfs.c:1525-1536` says a guest
  expecting a pipe on fd 3 was a real observed failure. Whether the corpus *contains* such a case is
  unverified, and it determines whether the typed table's lowest-free allocator is enough.
- **The eventfd fork-shared counter arena.** I read that `g_eventfd_count` is a shared arena bound
  once per process and that `hl_host_counter_services` has `duplicate` and a `readiness`/`subscribe`
  pair, but I did **not** verify that `counter`'s fork semantics reproduce the arena's. This is the
  one thing that could turn the recommended first unit from a week into three. It should be checked
  before the unit is committed to.
- **Whether `hl_linux_object_poll`'s spin-and-sleep is acceptable for pipes.** It has no blocking wait
  for non-object descriptors. Typed pipes get `subscribe`, so they may be fine; I did not measure.
- **My 1,453 figure has an unknown error bar.** The tokenizer is sound and I read every hit on the
  five riskiest identifiers, but I did not read all 1,453. Treat the *distribution* as reliable and
  the *total* as ±3%.
- **`syscall/sysv.c` (21 sites, `shm_open`/`mmap`) and `syscall/guest_copy.c` (22)** were counted but
  not investigated. They are in my "unclassified 135" and someone should place them.
