# Prior art: the flinux filesystem and descriptor layer

`docs/windows/linux-abi-fd-lane.md` §2.1 identifies **Surface 3** — roughly 900 sites in `src/linux_abi`
where a guest descriptor number *is* a host descriptor number — as "the real Windows cost" and explicitly
leaves it unscoped. This document is the prior-art half of scoping it. flinux presents a Linux fd table to a
guest while every underlying object is a Windows `HANDLE`, which is the exact problem, solved once, in a
tree small enough to read completely.

`DOCS.md` is normative. This file is a record of prior art and a recommendation; it changes no behaviour.

### Licensing

**flinux is GPLv3. HL Engine is MIT. They are incompatible.** No flinux source may be copied into `src/`, and
this document does not reproduce flinux code beyond the short identifiers and one- or two-line fragments
needed to name a technique. Everything below describes *architecture, algorithms and constraints* — which are
not copyrightable — so that our implementation can be written independently. If you find yourself
transliterating a function from the citations here, stop.

### Provenance and method

Source read: [`wishstudio/flinux`](https://github.com/wishstudio/flinux) at `a041253` (*"Lower many vfs lock
requirements to use shared lock."*, 2016-03-29, last commit on master), cloned to the session scratchpad.
28 813 lines of C total; the descriptor/VFS layer is `src/syscall/vfs.c` (2 812), `src/fs/*` (~9 700) and
`src/str.c`. **All of it was read**, not sampled. Every `file:line` below is that tree.

Claims marked **measured** come from four probes written for this document
(`scratchpad/fsprobe{,2,3,4}.c`, `crtfd.c`), built with `C:\msys64\clang64\bin\clang.exe`,
`-target x86_64-w64-windows-gnu`, and run on Windows 11 Pro 10.0.26200, NTFS, non-elevated, Developer Mode
off. Where a Windows behaviour is asserted without that marker it was not measured and says so.

Companion documents: `docs/windows/prior-art-flinux.md` covers the same tree's fork, memory manager and DBT
and is *not* re-covered here. `docs/windows/linux-abi-fd-lane.md` covers Surfaces 1 and 2 (the `*at()`
family) and is the document this one extends.

---

## 1. The fd table

### 1.1 Shape

Three structures, all in `src/syscall/vfs.c` and `src/fs/file.h`:

| | | |
|---|---|---|
| `struct filed` | `vfs.c:62-66` | one fd slot: `struct file *fd` + `int cloexec`. Nothing else. |
| `struct vfs_data` | `vfs.c:74-82` | `SRWLOCK`, the `file_system *fs[4]` table, a mount mutex, `struct filed filed[MAX_FD_COUNT]`, `struct file *cwd`, `int umask`. |
| `struct file` | `file.h:87-93` | `const struct file_ops *op_vtable`, `int flags`, `uint32_t ref`, `SRWLOCK rw_lock`. |

`MAX_FD_COUNT` is **1024**, a compile-time constant (`vfs.h:35`); the table is a flat array, never grown.
`RLIMIT_NOFILE` is not honoured — the limit is structural.

The layering is exactly Linux's, and flinux gets the split right without commenting on it:

- **Per-descriptor state** is `cloexec`, and only `cloexec`. It lives in `struct filed`.
- **Per-open-file-description state** is `flags` (`O_APPEND`, `O_NONBLOCK`) and the file position. It lives
  in `struct file`, so it is shared by `dup`. The position itself is not a field: it is the Windows
  `HANDLE`'s own file pointer (`winfs_file.handle`, `winfs.c:37-45`), so `dup` shares it because the two
  descriptors share the `HANDLE`.
- **Lifetime** is a plain refcount, `InterlockedIncrement`/`Decrement` in `vfs_ref`/`vfs_release`
  (`vfs.c:190-200`). Reaching zero calls `op_vtable->close(f)`, which frees the object.

There is no separate OFD table. `struct file` *is* the OFD.

### 1.2 Allocation

`store_file_internal` (`vfs.c:441-451`) is a linear scan from index 0 for the first NULL slot, returning
`-EMFILE` on failure. That is the POSIX "lowest available descriptor" rule implemented literally, O(n) per
open, n = 1024. Every fd producer funnels through it or through `vfs_store_file`, its locking wrapper
(`vfs.c:453-459`) — `openat` (`vfs.c:1229`), `pipe2` (`:483`, `:491`), `eventfd2` (`:524`),
`epoll_create1` (`:2607`ff), `socket` (`socket.c:637`), `accept4` (`socket.c` conn path),
`inotify_init1` (`inotify.c:82`). **One allocator, one policy, no exceptions.**

Lookup is `vfs_get(fd)` (`vfs.c:214-224`): bounds-check, take the shared VFS lock, load, `vfs_ref`, release.
The reference the caller receives outlives the lock, so a concurrent `close` cannot free the object under an
in-flight `read`. `vfs_get_internal` (`vfs.c:203-211`) is the same thing without the lock, for callers that
already hold it.

Every descriptor-taking syscall in flinux has the identical five-line body: `vfs_get`, null-check → `EBADF`,
vtable-slot null-check → `EINVAL`, dispatch, `vfs_release`. `sys_read` is `vfs.c:594-613` and `sys_write` is
`vfs.c:615-634`; the other thirty are copies. This is worth noting precisely because it is boring: the
uniformity is what makes a heterogeneous fd table tractable.

### 1.3 `dup`, `dup2`, `dup3`

One function, `vfs_dup(fd, newfd, flags)` (`vfs.c:533-574`). `dup` passes `newfd = -1` and scans for a free
slot; `dup2`/`dup3` pass an explicit target, close it if occupied, and store. `dup3`'s `O_CLOEXEC` sets the
new slot's `cloexec`; `dup2`'s clears it (`:569`). The `struct file *` is shared, refcount incremented.

Two divergences from Linux, both real:

- `dup2(fd, fd)` returns `-EINVAL` (`vfs.c:559`). Linux returns `fd` unchanged when `fd` is valid, and
  `EBADF` when it is not. flinux never returns the no-op success.
- `fcntl(F_DUPFD, arg)` ignores `arg` — it is routed straight to `sys_dup` (`vfs.c:2215-2216`), so the
  "lowest fd ≥ arg" guarantee is silently dropped. `F_DUPFD_CLOEXEC` is not implemented at all
  (`vfs.c:2255`, `default:` → `EINVAL`).

### 1.4 `O_CLOEXEC` and exec

`vfs_reset()` (`vfs.c:320-330`) walks all 1024 slots and closes those with `cloexec` set. It is called from
`execve_initialize_routine` (`exec.c:403-410`) alongside `signal_reset`, `mm_reset`, `tls_reset`,
`dbt_reset`.

The important architectural fact is **flinux's `execve` does not create a process**. It re-initialises the
current one and loads the new ELF in place (`exec.c:390-397`). So "fd inheritance across exec" is not a
transport problem at all — the table is the same memory; only the CLOEXEC slots are dropped. Windows has no
`execve`, and flinux's answer is to not need one.

This is the same choice `src/linux_abi` already makes, and it means the hard descriptor-inheritance problem
on Windows is **fork**, not exec.

### 1.5 Fork

`vfs_fork(hProcess, dwProcessId)` (`vfs.c:364-388`) and its two epilogues (`vfs_afterfork_child`
`:390-415`, `vfs_afterfork_parent` `:417-439`) implement a three-phase bracket that is structurally the same
as our `hl_linux_fork_plan` (`include/hl/linux_abi.h:183-190`):

1. **Parent, before `ResumeThread`**: take the VFS lock shared; for each *distinct* `struct file`, call
   `op_vtable->fork(f, child_process, child_pid)` if present, otherwise take that file's `rw_lock` shared.
2. **Child**: re-derive `vfs` and `vfs_shared` from the static/shared arenas, re-init every lock, call
   `op_vtable->after_fork_child(f)`.
3. **Parent, after**: `op_vtable->after_fork_parent(f)` or release the lock taken in phase 1.

Two techniques here are worth stealing outright:

- **Deterministic lock order.** Phase 1 builds an index array and `qsort`s it by the *pointer value* of the
  underlying `struct file` (`cmpfiled`, `vfs.c:342-362`), then walks it skipping repeats. This both
  deduplicates aliased descriptors (`dup` targets) and makes the acquisition order total and identical in
  every process, which is the standard deadlock-freedom argument. Our fork plan deduplicates by OFD index,
  which achieves the same thing more cheaply because we *have* an OFD index; flinux has to sort because it
  does not.
- **Per-type transport, not per-type serialization.** The fd table itself survives fork because it lives in
  the `mm_static_alloc` arena (`vfs.c:285`, `mm.c:414`) which flinux's `mm_fork` copies into the child. Only
  the *host objects* need transport, and each type does its own: sockets `WSADuplicateSocketW` in the parent
  and `WSASocketW(0,0,0,&fork_info,0,0)` in the child (`socket.c:252-270`); the console maps a shared
  section into the child (`console.c:253-272`); winfs files and pipes do nothing at all, because their
  `HANDLE`s were created inheritable — `attr.Attributes = OBJ_INHERIT` (`winfs.c:1112`), decided by
  `bInherit` at `winfs.c:1223-1225`, and `SECURITY_ATTRIBUTES.bInheritHandle = TRUE` for the per-file
  pointer mutex (`winfs.c:1246-1249`) — and
  `CreateProcessW` is called with `bInheritHandles = TRUE` (`fork.c:171`). **A `HANDLE` value is numerically
  identical in parent and child when inherited**, which is why the child's copied table still points at
  valid objects.

That last point is the single most useful Windows fact in the whole tree, and it is why flinux can get away
with a copied fd table: inherited handle values are stable across `CreateProcess`, so a bitwise copy of a
table of `HANDLE`s is correct. `INTERNAL_O_NOINHERIT` (`common/fcntl.h:33`) exists to opt *out* for
transient internal opens.

### 1.6 What is missing from flinux's table

Stated plainly, because these are the places we should not copy: no `RLIMIT_NOFILE`; no `F_DUPFD` minimum;
no `F_DUPFD_CLOEXEC`; no `O_TMPFILE`; no honoured `O_NONBLOCK` at open time — `vfs_openat` logs an error for it and continues,
with the `return -L_EINVAL` commented out (`vfs.c:1174-1181`); `flock` logs and returns **0**
(`vfs.c:2807-2812`), i.e. it lies; `fallocate` returns `EOPNOTSUPP` (`vfs.c:2800-2805`); no `/proc/self/fd`;
and `epoll` stores raw fd *numbers* rather than object references (`epollfd.c:30-40`), so a
closed-and-reopened fd silently re-enters an epoll set — Linux registers the open file description.

---

## 2. The file abstraction, against `hl_host_file_services`

### 2.1 flinux's operation set

`struct file_ops` (`file.h:44-85`) is a single flat vtable, 35 slots, in five groups:

| group | slots |
|---|---|
| Polling | `get_poll_status`, `get_poll_handle` |
| Fork | `fork`, `after_fork_parent`, `after_fork_child` |
| General | `close`, `getpath`, `read`, `write`, `pread`, `pwrite`, `readlink`, `truncate`, `fsync`, `llseek`, `stat`, `utimens`, `getdents`, `ioctl`, `statfs` |
| Socket | `bind`, `connect`, `listen`, `accept4`, `getsockname`, `getpeername`, `sendto`, `recvfrom`, `shutdown`, `setsockopt`, `getsockopt`, `sendmsg`, `recvmsg`, `sendmmsg`, `recvmmsg` |

A second, much smaller vtable, `struct file_system` (`file.h:103-112`), carries the seven *namespace*
operations that take a path rather than an open file: `open`, `symlink`, `link`, `unlink`, `rename`,
`mkdir`, `rmdir`.

Unimplemented slots are NULL and the caller turns that into `EINVAL` or `EPERM`. `winfs_ops`
(`winfs.c:856-872`) fills 14 of 35. `epollfd_ops` (`epollfd.c:109-111`) fills **one** (`close`).

### 2.2 Direct comparison

`hl_host_file_services` (`include/hl/host_services.h:344-426`) has 41 callbacks. The two designs are not the
same kind of object, and the comparison is only useful once that is said explicitly:

> flinux's `file_ops` is a **Linux-object vtable**: one virtual dispatch that answers "what does this guest
> fd do when the guest calls `read`". Our `hl_host_file_services` is a **host-portability contract**: what a
> host OS must supply so that a Linux front end can be written once. The Linux-object vtable in our tree is
> a different type — `hl_linux_object_ops` (`src/linux_abi/object.h:14-34`, 12 slots) — and the host groups
> beside `file` (`stream`, `network`, `event`, `counter`, `watch`, `directory`, `transfer`) carry what
> flinux crams into the socket and polling halves of one struct.

So the honest comparison is **flinux's 35+7 against our 41 + 12 + six sibling groups.**

**Where flinux is richer:**

| flinux has | we have | why it matters |
|---|---|---|
| `ioctl` as a first-class per-object slot | nothing | Every `ioctl` in `src/linux_abi` is ambient. `TIOCGWINSZ`, `FIONBIO`, `FIONREAD`, `TCGETS` are per-object behaviours; flinux dispatches them through the vtable (`vfs.c:2003-2042`), with `FIOCLEX`/`FIONCLEX` intercepted at the VFS layer because they are fd-table operations, not object operations. That interception is exactly right and we have no place to put it. |
| `get_poll_status` **and** `get_poll_handle` as a pair | `readiness` + `wait_handle` in `hl_linux_object_ops` | Same idea, and we already have it — but flinux applies it to *every* file type including winfs files, whereas ours is only reachable for objects installed via `hl_linux_object_install`. |
| `getpath(f, buf)` returning the *Linux* path | `file->path` returning the **native** path | flinux's `winfs_getpath` (`winfs.c:341-402`) reverses the mount-point mapping and the character transform, so it returns `/home/x`, not `C:\...\x`. Ours returns a host path (`host_services.h:378-379`), which `linux-abi-fd-lane.md` §6.4 already flags as the most likely silent Windows-only defect. flinux's choice is better for the caller and worse for the host implementer; the reverse mapping is why `winfs_file` has to carry `mp_key` and `drive_letter` fields (`winfs.c:43-44`). |
| `statfs` per object | `filesystem_metadata` | equivalent. |
| A `file_system` vtable *per mount point* | one flat namespace group | flinux can mount `/proc` and `/dev` as real filesystems with their own `open`/`unlink`. We route virtual paths inside `src/linux_abi` instead. Both work; theirs is more uniform. |

**Where we are richer:**

- **Positional I/O that is actually positional.** We have `read_at`/`write_at`/`readv_at`/`writev_at` and a
  separate `append`/`appendv` with a documented atomicity contract (`host_services.h:350-355`). flinux's
  `winfs_pread` (`winfs.c:481-520`) has to take an interprocess mutex, save the file pointer, do the
  overlapped read, and restore the pointer — with a comment (`:463-480`) admitting the performance is
  "completely untested" and that the two-handle alternative was rejected. **This is a Windows problem we
  have already designed out**: `ReadFile` with an `OVERLAPPED` offset always updates the file pointer on a
  synchronous handle, so any design that shares one `HANDLE` between `read` and `pread` pays flinux's tax.
  Our OFD carries its own `offset` (`linux_abi.h:107`) and an `io_mutex` (`:120`), which is the same trick
  one layer up where it belongs.
- **Resolution.** `resolve_beneath` / `open_beneath` (`:387`, `:396`) have no flinux counterpart at all; see
  §4.3.
- **Times, permissions, owner, allocation, sync ranges, directory reads, private-file validation.** flinux
  has `utimens` and nothing else in this family.
- **Vectored I/O.** flinux emulates `readv`/`writev` by looping `read`/`write` per iovec at the syscall
  layer (`vfs.c:677-748`), which is not atomic and is visibly wrong for pipes and sockets.

**Where both are thin:** neither has extended attributes. flinux stubs all twelve xattr syscalls to
`ENOTSUP`/`0` (`vfs.c:2716-2798`). We have `src/linux_abi/xattr.c` but no host group, which
`linux-abi-fd-lane.md` §3 records as a container-blocking gap.

### 2.3 The one structural lesson

flinux's vtable mixes two concerns — host portability and Linux object behaviour — because it has exactly
one host. We separated them, and that separation is why our `epoll`, `eventfd`, `inotify` and `pipe`
implementations (`src/linux_abi/{epoll,eventfd,inotify,pipe}.c`) are already host-neutral and already
compile against the fake backend. **flinux does not suggest we should merge them.** It confirms the split is
load-bearing: the moment flinux needed an object that was not a Windows file (a socket), it had to bolt
fifteen socket slots onto the file vtable, and every non-socket `file_ops` in the tree carries fifteen NULL
pointers as a result.

---

## 3. The virtual filesystems, and how one table holds them

### 3.1 The mount table

Four filesystem drivers, `FS_WINFS`/`FS_DEVFS`/`FS_PROCFS`/`FS_SYSFS` (`vfs.c:68-72`), and a mount table of
64 entries (`vfs.c:73, 84-90`) that lives in **shared memory** so that forked processes agree on the
namespace. `vfs_shared_init` (`vfs.c:235-280`) builds it at first start:

- `/` → the process's own current directory at launch, resolved to an NT path with
  `GetFinalPathNameByHandleW` and then rewritten `\\?\C:\…` → `\??\C:\…` by poking index 1 (`vfs.c:267`).
  So flinux's rootfs is "wherever the exe was launched from", and it is a real directory tree on NTFS.
- `/a` … `/z` → `\??\A:\` … `\??\Z:\`, unconditionally, all 26 (`vfs.c:270-275`).
- `/dev`, `/proc`, `/sys` → the three virtual drivers (`vfs.c:276-278`).

Mount points are kept **sorted by POSIX path in descending order** (`vfs.c:131-166`) so that the first
prefix match in `find_mountpoint` (`vfs.c:991-1013`) is the longest one — `/home` is tested before `/`. That
is a neat way to get longest-prefix-match out of a linked list with no length comparison.

### 3.2 The virtual-file framework

`src/fs/virtual.{c,h}` is a small declarative framework, and it is the part of flinux most worth imitating.
A virtual filesystem is a **static table of descriptors**, one of five kinds (`virtual.h:24-29`):

| kind | backing | example |
|---|---|---|
| `DIRECTORY` | a nested entry table | `/proc`, `/dev` |
| `TEXT` | a `gettext(tag, buf)` callback producing the whole file at open | `/proc/meminfo`, `/proc/cpuinfo`, `/proc/<pid>/maps` |
| `PARAM` | typed get/set of an int/uint/raw value | `/proc/sys/vm/min_free_kbytes` |
| `CHAR` | `read`/`write` callbacks plus a `dev_t` | `/dev/null`, `/dev/zero`, `/dev/random` |
| `CUSTOM` | an `alloc()` returning a full `struct file` | `/dev/console`, `/dev/tty`, `/dev/dsp` |

Directory entries are either **static** (name + descriptor, `VIRTUALFS_ENTRY`) or **dynamic** (a
`begin_iter`/`iter`/`end_iter`/`open` quartet, `VIRTUALFS_ENTRY_DYNAMIC`, `virtual.h:64-66`). `/proc/<pid>`
is one dynamic entry whose `iter` walks live pids and whose `open` parses a decimal string
(`procfs.c:66-84`, `:280`). `/proc/self` is a *static* entry pointing at the same descriptor with tag 0
(`procfs.c:281`).

The whole of `/dev` is 18 lines (`devfs.c:28-41`). The whole of `/sys` is an empty table (`sysfs.c:28-34`) —
flinux mounts `/sys` and puts nothing in it.

This gives a clean answer to "how does one fd table hold heterogeneous objects": **it does not need to.** The
fd table holds `struct file *`. Heterogeneity is entirely inside the vtable, and the virtual framework means
most virtual files never define a vtable at all — they define a descriptor, and `virtualfs_text_alloc` /
`virtualfs_param_alloc` / `virtualfs_char_alloc` (`virtual.c:415`, `:556`, `:323`) build the `struct file`
around it with a shared vtable.

### 3.3 First-class file types, enumerated

| object | file | fd-table citizen? | notes |
|---|---|---|---|
| NTFS file/dir | `winfs.c` | yes | 14/35 slots |
| `/proc`, `/sys`, `/dev` nodes | `virtual.c`, `procfs.c`, `devfs.c`, `sysfs.c` | yes | 5 descriptor kinds |
| console / tty | `console.c` (1 727 lines) | yes | a full ANSI terminal emulator over the Win32 console, `console_ops` at `:1709`; `/dev/console` and `/dev/tty` are the same object |
| pipe | `pipe.c` | yes | a Win32 **named** pipe pair plus two manual-reset events driven by `FilePipeLocalInformation` polling (`pipe.c:44-96`) |
| socket | `socket.c` (1 655 lines) | yes | Winsock `SOCKET` + a `WSAEventSelect` event handle |
| eventfd | `eventfd.c` | yes | |
| epoll | `epollfd.c` | yes, degenerately | 1/35 slots; see below |
| inotify | `inotify.c` | yes, degenerately | `inotify_add_watch` returns 0 and does nothing (`inotify.c:100-104`) |
| `/dev/dsp` | `dsp.c` | yes | waveOut audio |

Two of these are informative failures:

- **epoll is not really an object.** `epoll_ctl` stores `(int fd, struct epoll_event)` pairs in a fixed array
  of 128 (`epollfd.c:30-40`), and `epoll_wait` converts the array into a `pollfd[]` and calls the same
  `vfs_ppoll` path (`epollfd.c:133-143`, `vfs.c:2684-2714`). So epoll is O(n) per wait, capped at 128
  descriptors, and holds fd numbers rather than references. `src/linux_abi/epoll.c` is already better than
  this; nothing to take.
- **AF_UNIX is emulated over loopback TCP.** `socket_bind` for `AF_UNIX` creates the socket path as a real
  NTFS file with `FILE_ATTRIBUTE_SYSTEM`, binds an `INADDR_LOOPBACK` port, and writes the port number into
  the file behind a `!<UNIX>\0379\0378` header (`socket.c` bind path, `winfs_write_special_file`
  `winfs.c:221-241`). `socket_connect` reads the file, parses the port, and connects to loopback. Abstract
  sockets (`sun_path[0] == 0`) are rejected. This is ingenious and also a security hole and a semantic
  divergence (credentials, `SCM_RIGHTS`, atomic bind-vs-unlink all break). Worth knowing the trick exists;
  not worth adopting.

### 3.4 Polling as the unifying operation

The reason a single table *can* hold these is `vfs_ppoll` (`vfs.c:2363-2498`). It asks each file for a
`HANDLE` via `get_poll_handle` and a level-triggered status via `get_poll_status`, then does one
`WaitForMultipleObjects` over the collected handles. Every object type therefore only has to answer "give me
a waitable Win32 handle and tell me your current readiness". Pipes synthesise their events from
`NtQueryInformationFile`; sockets use `WSAEventSelect`; the console uses its input handle; virtual files
return `POLLIN|POLLOUT` unconditionally (`virtual.c:32-35`).

`WaitForMultipleObjects` caps at `MAXIMUM_WAIT_OBJECTS` = 64. flinux does not handle exceeding it. Our
`hl_host_event_services` (`host_services.h:467-480`) is the right shape for this and does not have that cap
by construction, but a Windows backend will have to solve it (thread-per-64 fan-in, or IOCP) and this is the
one place where flinux is no help at all.

---

## 4. Path translation

This is the section with the most transferable content, because these are exactly the traps a `file` group
implementation walks into.

### 4.1 What flinux does, in order

`resolve_path` (`vfs.c:1019-1131`) works entirely on **Linux path strings**. It normalises `.` and `..`
lexically, and for each non-final component it calls the owning filesystem's `open(O_PATH|O_DIRECTORY)` to
learn whether that component is a symlink; if it is, it recurses on the target. `MAX_SYMLINK_LEVEL` is
**8** (`vfs.h:37`); Linux's is 40.

`resolve_pathat` (`vfs.c:1134-1147`) implements the entire `*at()` family by **turning the dirfd back into a
string**: `f->op_vtable->getpath(f, dirpath)`, then ordinary string resolution. There is no dirfd-relative
resolution anywhere in flinux.

Only at the leaf does `filename_to_nt_pathname` (`winfs.c:48-67`) produce a Windows name: the mount point's
stored NT prefix (`\??\C:\…`), a backslash, and then `utf8_to_utf16_filename` for the remainder.

### 4.2 The character transform

`utf8_to_utf16_filename` / `utf16_to_utf8_filename` (`str.c:316-349`, `:382-417`) drive a 128-entry table
(`str.c:101-135`) that maps each ASCII code point to its NT-filename form. The rule, stated rather than
copied:

- The seven characters legal in a Linux filename and illegal in an NTFS one — `"` `*` `:` `<` `>` `?` `|` —
  and **all of C0 (0x01–0x1F)** are mapped to the Unicode private-use code point `0xF000 | c`.
- `/` (0x2F) is mapped to `\` — the path separator translation happens in the same table pass, which is why
  a guest filename can never contain a literal `/`, correctly.
- The reverse pass maps `\` back to `/` and folds any `0xF000|c` back to `c` by testing whether the table
  entry for `c & 0x7F` equals the code point (`str.c:396-397`, `:411-412`).
- The comment at `str.c:94-99` states the scheme is deliberately **Cygwin-compatible**, which means a file
  created by flinux is visible with the right name in a Cygwin or MSYS2 shell and vice versa.

**Measured** (`fsprobe.c` §1): every one of the seven raw characters fails `CreateFileW` with
`ERROR_INVALID_NAME` (123); every one of the seven `U+F0xx` remappings succeeds, and
`GetFinalPathNameByHandleW` returns the private-use code point unchanged. A remapped backslash (`U+F05C`)
also succeeds, so even `a\b` as a Linux filename is representable. The scheme works, on current Windows,
exactly as described.

What the table does **not** cover, and neither does flinux:

- **NUL in a filename.** Legal in no filesystem, so not a real gap.
- **Bytes that are not valid UTF-8.** Linux filenames are byte strings; `utf8_read_increment`
  (`str.c:168-198`) returns −1 on a malformed sequence and the whole call fails with `ENOENT`. A guest that
  creates a Latin-1-named file cannot see it again. Cygwin solves this with a UTF-8-with-surrogate-escape
  scheme; flinux does not. **This is a gap we would inherit if we copied the table.**
- **Case.** flinux does nothing about case at all.

### 4.3 The traps, measured

| trap | measured behaviour | flinux | what we should do |
|---|---|---|---|
| **Illegal characters** | 7 chars + C0 rejected raw; `U+F000\|c` accepted and round-trips | the Cygwin transform | adopt the transform, **plus** a byte-escape for invalid UTF-8 that flinux lacks |
| **Reserved DOS names** | `CON`, `NUL`, `AUX`, `PRN`, `COM1`, `LPT1`, `CON.txt`, `CONOUT$` **all created successfully** as ordinary files — via the plain Win32 path *and* via `\\?\`. Only `NUL` behaved specially (`GetFinalPathNameByHandleW` → error 87). | nothing | **This trap is largely gone on Windows 11.** The historic advice to escape DOS device names is stale for file *creation*; it still matters for the `NUL`-shaped devices and for legacy tooling. Verify per Windows version before spending effort. |
| **Trailing dot / trailing space** | Win32 path silently **strips** them: creating `trail.` and `trail ` both produce `trail`. Through `\\?\` they are preserved (`trailx.` stays `trailx.`), but such a file is then **unreachable by any Win32 path** — reopening `trailx.` without the prefix fails `ERROR_FILE_NOT_FOUND`. | nothing — flinux uses `\??\` NT paths throughout, so it gets the preserving behaviour by accident | Use NT/`\\?\` paths throughout, as flinux does, so `foo.` and `foo` stay distinct. Accept that such files are invisible to Explorer. |
| **`MAX_PATH`** | Win32 `CreateDirectoryW` failed at total length 272 (deepest success 239). Through `\\?\`, **24 651** characters of nested path were created successfully. Single component: 255 accepted, **256 rejected** with `ERROR_INVALID_NAME`. | mostly right: NT `\??\` paths, and the working buffers are `WCHAR[PATH_MAX]` = 4096 (`winfs.c:876`, `956`, `1059`, `1100`; `vfs.h:33`). But the **mount-point prefix** is capped at `MAX_PATH` — `struct mount_point` stores `WCHAR win_path[MAX_PATH]` / `char mountpoint[MAX_PATH]` (`file.h:139,141`) and `vfs_shared_init` calls `process_exit(1,0)` if the launch directory exceeds it (`vfs.c:262-266`). So flinux tolerates a deep tree but refuses to start from a deep directory. | `\\?\`/`\??\` throughout **and** every buffer sized for `PATH_MAX` (4096) including the pinned-root prefix, not `MAX_PATH`. Component limit 255 matches Linux `NAME_MAX`; the total-path limit is not a practical constraint. |
| **Case sensitivity** | default NTFS directory is case-**in**sensitive: `CaseTest` opens as `casetest`, and `CREATE_NEW` of `CASETEST2` beside `casetest2` fails with `ERROR_FILE_EXISTS`. Setting `FileCaseSensitiveInfo` (class 23) with `FILE_CS_FLAG_CASE_SENSITIVE_DIR` **succeeded**, after which `Xy` and `xY` coexist in that directory. | nothing | Two honest options: (i) accept case-insensitivity and document it (what flinux, Cygwin and MSYS2 all do), or (ii) set the per-directory case-sensitivity flag on directories the engine creates inside a container rootfs. (ii) gives true Linux semantics, requires no privilege — **measured** — and is per-directory, so it must be applied at every `mkdir`. It also makes those trees hostile to Windows tools. Recommend (ii) for container rootfs trees, (i) for host-visible paths, and that the choice be a documented policy rather than an accident. |
| **Drive mapping** | — | `/a`…`/z` → `A:`…`Z:`, all 26 mounted unconditionally at init (`vfs.c:270-275`); `/` is the launch directory | The `/a`…`/z` convention is Cygwin's `/cygdrive` idea without the prefix. It is cheap and it makes `getcwd` answerable for any path. Worth adopting for bare (non-container) launches; a container launch should have a single rootfs and no drive mounts. |

---

## 5. Symlinks, hardlinks, and inode identity

### 5.1 Symlinks are a file format, not a reparse point

flinux stores a symlink as an ordinary NTFS file with `FILE_ATTRIBUTE_SYSTEM` set, whose contents are the
magic header `!<SYMLINK>\0379\0378` followed by the target path (`file.h:35-36`, `winfs_symlink`
`winfs.c:874-923`). Sockets use a second header, `!<UNIX>\0379\0378` (`file.h:37-38`).

Detection is a two-stage test tuned for cost:

1. `NtQueryInformationFile(FileAttributeTagInformation)` — is `FILE_ATTRIBUTE_SYSTEM` set? Almost always no,
   and the check is one metadata query (`winfs.c:1144-1154`).
2. Only then, read the first 11 bytes and `memcmp` the header (`winfs_read_symlink_unsafe`,
   `winfs.c:301-330`).

This is again the Cygwin scheme, and the reason for it is **measured**: `CreateSymbolicLinkW` failed with
`ERROR_PRIVILEGE_NOT_HELD` (1314) both plain and with `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, on a
normal non-elevated user account with Developer Mode off. **Native NTFS symlinks are not available to an
ordinary process by default in 2026.** Any design that assumes reparse points will fail on most user
machines.

Costs of the file-format approach, all visible in the tree:

- **Reading a symlink requires opening it for read.** `open_file` (`winfs.c:1096-1193`) has an ugly special
  case: if the caller asked for write-only access and the file turns out to have the system attribute, it
  must `ReOpenFile` with `GENERIC_READ` added just to check the header (`winfs.c:1157-1170`).
- **The file pointer is disturbed.** Every symlink probe moves the handle's position, so `winfs_stat` saves
  and restores it around the check under the fp mutex (`winfs.c:663-689`).
- **Directory enumeration becomes O(entries) opens.** See §7.
- **A guest can forge one.** Any file the guest writes starting with the magic bytes and carrying the system
  attribute becomes a symlink. flinux does not defend against this.
- **`O_NOFOLLOW` is fiddly.** `open_file` returns `1` to mean "this was a symlink, here is the target,
  re-resolve" and the caller loops (`vfs_openat:1190-1216`). With `O_NOFOLLOW` but not `O_PATH` it returns
  `ELOOP` (`winfs.c:1178-1183`), matching Linux.

### 5.2 Hardlinks

`winfs_link` (`winfs.c:925-952`) is `NtSetInformationFile(FileLinkInformation)` on the source handle with the
target NT path. Straightforward and correct. `sys_linkat` refuses `AT_EMPTY_PATH` (`vfs.c:1284-1288`) and
refuses to link anything that is not a winfs file (`vfs.c:1297-1301`).

### 5.3 `st_ino` / `st_dev`

flinux's answer is deliberately lossy and says so (`winfs.c:642-649`):

- `st_dev` is the **constant** `mkdev(8, 0)` for every winfs file — every file on every drive claims to be on
  `/dev/sda`. Virtual files use `mkdev(0, 1)` (`virtual.c:89`, `:240`, `:295`).
- `st_ino` is `nFileIndexHigh ^ nFileIndexLow` — the 64-bit NTFS file index **folded to 32 bits**, with a
  comment saying this is to "fix legacy applications" and that the full value (commented out at `:645`) may
  become an option.

Both are wrong and both matter, because the comment two lines above admits why: *"Programs (ld.so) may use
st_dev and st_ino to identity files so these must be unique for each file."* A constant `st_dev` plus a
32-bit `st_ino` gives a birthday collision at ~77 000 files.

**Measured** (`fsprobe2.c` §5b), on NTFS:

| property | result |
|---|---|
| `BY_HANDLE_FILE_INFORMATION.nFileIndex{High,Low}` across close/reopen | **stable** |
| across `MoveFileW` (rename) | **stable** |
| a hardlink to the same file | **same index**, `nNumberOfLinks` 2 |
| a `CopyFileW` copy | different index |
| `FILE_ID_INFO` (`GetFileInformationByHandleEx(FileIdInfo)`) | 128-bit `FileId` whose low 64 bits equal the `BY_HANDLE` index; 64-bit `VolumeSerialNumber` (measured `40CAAEB5CAAEA71E`) vs. the 32-bit `dwVolumeSerialNumber` (`CAAEA71E`) |

So for our contract:

- **`stable_object` ← the 64-bit NTFS file index** (`nFileIndexHigh:nFileIndexLow`, or the low 64 bits of
  `FILE_ID_INFO.FileId`). It is stable across reopen and rename and shared by hardlinks, which is the whole
  of what `st_ino` must guarantee. **Do not fold it to 32 bits.** `hl_host_file_metadata.stable_object` is
  already `uint64_t` (`host_services.h:263`), so we have room flinux did not use.
- **`stable_device` ← the volume serial number**, and it must be *per volume*, not a constant. Prefer
  `FILE_ID_INFO.VolumeSerialNumber` (64-bit) over `dwVolumeSerialNumber` (32-bit); it is measurably a wider
  value, not a zero-extension.
- **Caveat, not measured:** on ReFS the 128-bit `FileId` genuinely uses its high bits, and folding it into
  our 64-bit `stable_object` would be lossy in a way NTFS is not. This should be tested on a ReFS volume
  before we claim `stable_object` is exact on all Windows filesystems; the honest fallback is a hash with a
  documented collision domain.
- Neither `FILE_ID_INFO` nor `BY_HANDLE_FILE_INFORMATION` is available on all remote filesystems; SMB
  returns zeros in some configurations. Not measured here.

---

## 6. Permissions and ownership

flinux does not implement them. Stated as facts rather than a summary, because the pattern is instructive:

- `winfs_stat` (`winfs.c:650-653`) synthesises `st_mode` from exactly one bit: `FILE_ATTRIBUTE_READONLY` →
  `0555`, otherwise `0755`. Directory or regular is decided by `FILE_ATTRIBUTE_DIRECTORY`; symlink and
  socket by the magic-header probe.
- `st_uid` and `st_gid` are hardcoded `0` (`winfs.c:692-693`). Virtual files are `0644` or `0755` constants
  (`virtual.c:91`, `:242`, `:297`).
- `chmod`, `fchmod`, `fchmodat` log an error and **return 0** (`vfs.c:2296-2318`). So does `fchownat` and
  therefore `chown`, `lchown`, `fchown` (`vfs.c:2336-2361`).
- `umask` is stored (`vfs.c:2320-2325`) and never consulted.
- `faccessat` is "does the file exist" — it opens with `O_PATH` and discards the result (`vfs.c:2270-2288`).
  The `mode` argument is ignored entirely.
- `chroot` returns `ENOSYS` (`vfs.c:2327-2334`).

This works for a single-user desktop and would not survive our compat corpus. The relevant lesson is
**negative and useful**: returning success from `chmod` while doing nothing is what lets a package manager
or `./configure` run to completion, and it is a legitimate deliberate lie — but it must be a lie the *Linux
front* tells, backed by the virtual-ownership table (`src/linux_abi/container/owner.h`), not a lie the host
backend tells. `linux-abi-fd-lane.md` §3 already assigns `faccessat` to the Linux front for exactly this
reason; the same argument covers `chmod`/`chown` on Windows. Our `set_permissions`/`set_owner`
(`host_services.h:385`, `:401`) should map to whatever NTFS ACL manipulation is honest and return
`HL_STATUS_NOT_SUPPORTED` where it is not — never silent success.

One Windows-specific note flinux does supply: it caches the process token's SID once
(`get_user_sid`, `winfs.c:69-91`) rather than querying per call. Any ACL-based implementation of
`set_owner` will need that, and the query is not cheap.

---

## 7. Directory enumeration and `getdents64`

### 7.1 flinux's implementation

`winfs_getdents` (`winfs.c:730-819`) calls `NtQueryDirectoryFile` in a loop with
`FileIdFullDirectoryInformation` and a 32 KiB buffer, passing `RestartScan = winfile->restart_scan`, which is
set on open (`winfs.c:1252`) and again by `llseek(0, SEEK_SET)` (`winfs.c:625-629`) and cleared after the
first call (`:749`).

Per entry it maps `FILE_ATTRIBUTE_DIRECTORY` → `DT_DIR`, otherwise `DT_REG` — **except** that if
`FILE_ATTRIBUTE_SYSTEM` is set it opens the entry (`NtCreateFile` with `attr.RootDirectory` = the directory
handle, a relative open) purely to read the magic header and decide `DT_LNK` vs `DT_SOCK`
(`winfs.c:773-805`). In a directory of system-attributed files that is one extra open *per entry per scan*.
There is also a live bug there: the inner `int type` shadows the outer one (`:796`), so the classification is
computed and discarded and `DT_REG` is reported.

Sizing is done by halving: the NT buffer is `(count - size) / 2` bytes, justified by the comment
(`winfs.c:742-744`) that a UTF-16 character needs at most 4 UTF-8 bytes and
`sizeof(FILE_ID_FULL_DIR_INFORMATION)` exceeds `sizeof(struct linux_dirent64)`.

The virtual filesystems enumerate from their static tables, synthesising `.` and `..` at positions 0 and 1
and using the table index as the cursor (`virtual.c:108-201`).

### 7.2 `d_off` and the cursor

**`d_off` is hardcoded to 0** in both fill callbacks — `getdents_fill` (`vfs.c:1561`) and `getdents64_fill`
(`vfs.c:1589`), both with a bare `/* TODO */`. So `seekdir`/`telldir` to a previously returned offset does
not work in flinux at all. For the virtual filesystems the callback is passed `file->position`
(`virtual.c:161`, `:186`), but that value is written into `d_ino`, not `d_off`.

**Measured** (`fsprobe2.c` §9), using `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)`, which is the
Win32 face of `NtQueryDirectoryFile`:

- The cursor is **per-kernel-file-object and advances implicitly**. There is no cookie in the returned data
  and no API that accepts one.
- A handle produced by `DuplicateHandle` **shares** the cursor: after the original was exhausted, the
  duplicate immediately returned `ERROR_NO_MORE_FILES` (18). This is correct for Linux `dup` semantics and
  is a genuine convenience.
- Restart works: `FileIdBothDirectoryRestartInfo` rewinds. `SetFilePointer(dir, 0, FILE_BEGIN)` also returns
  success on a directory handle, which is what flinux keys `restart_scan` off.
- There is no seek-to-arbitrary-position. Windows offers `FileName` filtering, not offsets.

### 7.3 What this means for `read_directory`

`hl_host_file_services.read_directory` (`host_services.h:405-406`) is documented as consuming "complete
entries from the open directory's shared OFD cursor", and `hl_host_file_entry.next_offset`
(`host_services.h:330`) is exactly the `d_off` field. The measurement says a Windows backend **cannot
produce a meaningful `next_offset`**, and the choices are:

1. Return a synthetic monotonic counter — position-in-scan — which satisfies the common
   `while ((d = readdir(dir)))` loop and `telldir` immediately followed by `seekdir` to the *same* value with
   no intervening rewind, but breaks a `seekdir` to a stale value.
2. Return 0, as flinux does, and accept that `seekdir` is broken.
3. Buffer the whole directory at first read into the OFD's private state, making offsets exact and stable —
   at the cost of memory proportional to the directory and a snapshot that goes stale.

**Recommend (1)** with the divergence documented, and (3) available behind a flag if a corpus case demands
it. `linux-abi-fd-lane.md` §6.4 already flags this as a Windows-only regression risk; this section is the
measurement behind it.

### 7.4 Unlink-while-open

Not directory enumeration, but it belongs with it because both are where "Linux semantics on NTFS" bites.

flinux's `winfs_unlink` (`winfs.c:954-1009`) opens the victim with `DELETE`; if that fails with
`STATUS_SHARING_VIOLATION` it reopens permissively and **moves the file into the Recycle Bin** under a
generated name built from the file's `IndexNumber` and a hash of its path (`move_to_recycle_bin`,
`winfs.c:96-163`), so that the name disappears from its directory immediately even though the data cannot be
freed until the last handle closes.

**Measured** (`fsprobe3.c` §7c, `fsprobe4.c`), on Windows 11 Pro 10.0.26200:

| scenario | result |
|---|---|
| `DeleteFileW`, all existing handles opened with `FILE_SHARE_DELETE` | **succeeds**; the name is gone (`OPEN_EXISTING` → error 2) and can be immediately recreated. Existing handles keep working. This *is* Linux `unlink` semantics. |
| `DeleteFileW`, an existing handle lacks `FILE_SHARE_DELETE` | fails, `ERROR_SHARING_VIOLATION` (32). Name still visible. **This is flinux's recycle-bin case.** |
| `FileDispositionInfoEx` with `DELETE\|POSIX_SEMANTICS` on a **separate** handle, then close it | name gone (error 2), other handles keep reading and writing, `CREATE_NEW` of the same name succeeds |
| the same, but the deleting handle is left **open** | the name reads as delete-pending: reopen and `CREATE_NEW` both return `ERROR_ACCESS_DENIED` (5) |
| legacy `FileDispositionInfo(DeleteFile=TRUE)` | name remains occupied while any handle is open; `CREATE_NEW` → error 5 |

Conclusions, in order of importance:

1. **The recycle-bin trick is obsolete** — POSIX-semantics delete exists and works, provided the unlink is
   issued on a dedicated `DELETE` handle that is then closed.
2. **Everything depends on every opener passing `FILE_SHARE_DELETE`.** flinux does, unconditionally, in every
   `NtCreateFile`/`NtOpenFile` (`winfs.c:1131`, `:792`, `:893`). This is only enforceable if there is exactly
   one place that opens files — which is an argument for the typed seam, made from an unexpected direction.
3. `FILE_SHARE_READ|WRITE|DELETE` on every open is also what makes flinux's concurrent-process model work at
   all, and it is the opposite of the Win32 default.

---

## 8. What we should take — the Surface-3 recommendation

### 8.1 The finding that reframes the question

**We already have flinux's architecture, and ours is better factored.** Laid side by side:

| flinux | HL Engine | |
|---|---|---|
| `struct vfs_data` (`vfs.c:74-82`) | `hl_linux_abi` (`linux_abi.h:194-218`) | fd table + lock |
| `struct filed` (`vfs.c:62-66`) | `hl_linux_fd_entry` (`linux_abi.h:126-132`) | per-fd: object + `cloexec` / `descriptor_flags` |
| `struct file` (`file.h:87-93`) | `hl_linux_ofd_entry` (`linux_abi.h:103-124`) | OFD; ours additionally has an explicit `offset`, `generation`, `flock_token`, `io_mutex` |
| `struct file_ops` (`file.h:44-85`) | `hl_linux_object_ops` (`object.h:14-34`) | object vtable |
| `vfs_fork` / `after_fork_{parent,child}` | `hl_linux_fork_plan` (`linux_abi.h:183-190`) | three-phase fork bracket |
| `vfs_reset` (CLOEXEC on exec) | `hl_linux_fd_exec_all` (`linux_abi.h:269`) | |
| — | `hl_linux_fd_reserve_at` / `_cancel` / `_snapshot_get` | reservation and checkpoint, which flinux has no equivalent of |

`src/linux_abi/{pipe,epoll,eventfd,inotify}.c` are already installed as typed objects
(`hl_linux_object_install`, 5 call sites). Surface 3 is therefore **not** "we lack a design". It is "we built
the design and then ~900 sites went around it".

### 8.2 Measuring the population properly

The fd-lane doc's ~900 is "indicative only" and flagged as such. My own sweep (comment-stripped, member
accesses excluded, 71 descriptor-taking POSIX function names) over `src/linux_abi` gives **1 289 sites across
52 distinct POSIX functions**, led by `close` (377), `fcntl` (271), `open` (71), `fstat` (68), `lseek` (46).
Both numbers have false positives; the order of magnitude is right and the distribution is the useful part:

| population | files | sites | what it actually is |
|---|---|---|---|
| **A. Guest-fd sites** | `syscall/{fs,io,net,event,binding}.c` | ~441 | the real Surface 3 — a number that is simultaneously a guest fd and a host fd |
| **B. Engine-internal descriptors** | `checkpoint.c` (276), `fork.c` (33), `guest_copy.c` (23), `sentry.c` (19) | ~351 | host descriptors that never carry a guest fd number: checkpoint stream I/O, `SCM_RIGHTS` capture over a host `AF_UNIX` socket (`checkpoint.c:630-670`), fork plumbing |
| **C. Linux-only container features** | `container/netns.c` (216), `container/vfs.c` (103) | ~319 | netlink sockets, network namespaces, bind mounts — features with no Windows analogue at all |

These three need three different answers, and conflating them is how this work gets mis-sized in either
direction.

### 8.3 Option (a), the fd-emulation shim, and why it fails

The proposal is a Windows layer that hands out small integers backed by `HANDLE`s so the 1 289 sites compile
and run unchanged. Three measured objections:

**It partly exists already, and it is not good enough.** The UCRT/mingw CRT *is* an fd shim: `_open`, `_read`,
`_write`, `_close`, `_lseek`, `_dup`, `_dup2`, `_pipe`, `_get_osfhandle`, `_open_osfhandle`. **Measured**
(`crtfd.c`): `_open` returns 3 with 0/1/2 reserved for the standard streams; `_dup2` to fd 400 and 5 000
succeeds and to 100 000 fails; `_pipe` works. And then:

- `_fstat64` returns **`st_ino = 0` and `st_dev = 0`.** The CRT does not synthesize inode identity at all, so
  every `fstat` site would still need replacing.
- A Winsock `SOCKET` wrapped by `_open_osfhandle` produces an fd whose `_write` **fails**. Sockets and files
  can never share one descriptor namespace through the CRT — which is precisely the property a Linux fd
  table exists to provide.
- `O_NONBLOCK` is not defined; there is no `fcntl`, no `F_SETFD`, no `getdents`, no positional read that
  leaves the file pointer alone, no `mmap`.

So the shim would have to be written from scratch, and it would be a second descriptor table living
alongside `hl_linux_abi`.

**A second table is the dual-lane trap, again.** `linux-abi-fd-lane.md` §6.1 rejected `#if HL_HOST_POSIX`
for `resolve_beneath` with an argument that transfers verbatim: the shim path would be exercised in
production *only* on the host that has no reference implementation to differ against. Every divergence gets
found by a Windows user rather than by the compat corpus. §4.2 documents that the tree already pays this cost
once, in `jail_open_plan`, and that the typed lane there is "decoration except where it wins".

**The shim cannot supply the semantics anyway.** The measurements in §4, §5 and §7 are all cases where a
faithful answer requires a decision that a byte-level `read(fd, buf, n)` shim has no place to make:
`FILE_SHARE_DELETE` on every open (§7.4) is only enforceable from a single centralised open path; positional
reads that do not disturb the file pointer need the OFD-level `io_mutex` we already have and flinux had to
invent (`winfs.c:463-520`); `st_ino` needs a `FILE_ID_INFO` query the CRT does not make; `d_off` needs
per-OFD state (§7.3). A shim would let all 1 289 sites *keep* semantics Windows cannot supply, which is worse
than the sites failing to compile.

### 8.4 Recommendation

**Take (b) for population A, a bounded form of (c) for population B, and exclude population C.** Firmly, in
that order.

**A — route the ~441 guest-fd sites through `hl_linux_abi`.** This is the load-bearing work and there is no
cheaper correct alternative. It is not a rewrite: `hl_linux_read`/`write`/`close`/`dup`/`dup2`/`dup3`/
`fcntl`/`lseek`/`fstat`/`openat_reserved` all already exist (`linux_abi.h:248-334`) and are already
implemented against the typed host groups. The work is call-site rewriting plus deleting
`bound_shadow_reserve`'s shadow descriptor (`syscall/binding.c:986-1005`), which exists *only* to stop
ambient host code from claiming a guest fd number — i.e. it is a tax paid by Surface 3 and it disappears when
Surface 3 does. Sequence it after steps 1–6 of the fd-lane plan, on Linux, with the corpus green at each
step; a Windows compile should not be attempted until this lands, because `syscall/fs.c`, `container/vfs.c`,
`syscall/binding.c` and `container/vfs/resolve.c` are `#include`d into one unity TU
(`src/core/target/x86_64.c:118,690,721,751`) and cannot be ported incrementally.

**B — give `src/host/windows/` a private descriptor table, not exposed to `src/linux_abi`.** Checkpoint
streams, fork plumbing and the sentry use descriptors that are never guest-visible. Forcing them through
`hl_linux_abi` buys nothing and would put engine-internal I/O behind a table whose lock is on the guest
syscall hot path. A host-internal table — the same relationship `src/host/resolve.h`'s `hl_host_resolved_path`
already has to the public contract (`linux-abi-fd-lane.md` §4.2) — is legitimate, invisible above the seam,
and is the correct reading of "something else". Note separately that `checkpoint.c`'s `SCM_RIGHTS` capture
over a host `AF_UNIX` socket has no Windows form at all; checkpoint on Windows is a redesign, not a port, and
should be scoped on its own.

**C — do not compile `container/netns.c` on Windows.** Network namespaces, netlink and bind mounts are Linux
kernel features. ~319 sites disappear from the estimate the moment this is stated as policy rather than
discovered as breakage.

**The corrected size of Surface 3 is therefore ~441 sites, not ~900 and not ~1 289** — and roughly half of
those are `close`/`fcntl`, which are one-line mechanical substitutions once `hl_linux_close` and
`hl_linux_fcntl` are the only callers of the table.

### 8.5 The seven specific things worth taking from flinux

1. **Cygwin's `0xF000|c` filename transform** (§4.2), which is measured to work and gives interoperability
   with MSYS2 and Cygwin for free. Add the invalid-UTF-8 escape flinux lacks.
2. **`\??\`/`\\?\` NT paths everywhere, sized to `PATH_MAX` not `MAX_PATH`** (§4.3). flinux got the first
   half right and the second half wrong; do both.
3. **`FILE_SHARE_READ|WRITE|DELETE` on every open, without exception** (§7.4). This is what makes
   unlink-while-open work, and it is only enforceable from a single centralised open path.
4. **The symlink-as-a-file-format scheme** (§5.1), because native symlinks are measured to require a
   privilege ordinary users do not have. Keep the two-stage attribute-then-header probe; it is the reason
   the cost is bearable.
5. **The declarative virtual-filesystem table** (§3.2) — five descriptor kinds, static and dynamic entries.
   Our `/proc` synthesis is spread across `src/linux_abi`; this framework is 700 lines and makes `/dev` an
   18-line table.
6. **The deterministic fork lock order** (§1.5) and, more importantly, the observation that inherited
   `HANDLE` values are numerically identical in the child, which is what makes a copied descriptor table
   correct across `CreateProcess`.
7. **`get_poll_handle` + `get_poll_status` as the universal object interface** (§3.4). We have the same pair
   in `hl_linux_object_ops` (`readiness`, `wait_handle`); flinux's contribution is the demonstration that
   *every* object type, including plain files and virtual files, can answer it, which is what lets one
   `WaitForMultipleObjects` serve `poll`, `select` and `epoll` alike.

And one thing to take as a warning rather than a pattern: flinux's `resolve_pathat` converts a dirfd back
into a path string (`vfs.c:1134-1147`). It is simple, it is why flinux needs no `openat`, and it is
**TOCTOU-open by construction** — which is the property `resolve_beneath` exists to guarantee and which the
fd-lane doc (§7 risk 3) already identifies as unprotected by any current test. Do not let the Windows backend
regress into it.

---

## 9. Unknowns, stated

- **ReFS `FILE_ID_INFO`.** Not measured. The 128-bit `FileId` is documented to use its high bits on ReFS,
  which would make our 64-bit `stable_object` lossy in a way NTFS is not. One probe on a ReFS volume answers
  it.
- **Network filesystems.** `FILE_ID_INFO`, `nFileIndex`, `FileDispositionInfoEx` and the per-directory
  case-sensitivity flag were all measured on local NTFS only. SMB and network redirectors are known to differ
  and were not tested.
- **`FileDispositionInfoEx` availability floor.** It works on 10.0.26200. It requires Windows 10 1709+, which
  was not verified here; a backend targeting older Windows needs flinux's fallback.
- **Reserved DOS names.** Creating `CON`, `AUX`, `PRN`, `COM1` as ordinary files succeeded on this build,
  contradicting long-standing guidance. Whether this is a Windows 11 relaxation, a per-volume property, or an
  artefact of the temp directory was not determined. **Do not build on this without re-measuring on the
  minimum supported Windows version.**
- **`MAXIMUM_WAIT_OBJECTS`.** flinux's poll caps at 64 objects and does not handle exceeding it. What a
  Windows `hl_host_event_services` should do instead — thread fan-in, IOCP, or `RegisterWaitForSingleObject`
  — was not investigated and is the largest unexamined item in this lane.
- **The 441/351/319 split in §8.2** is derived from a regex sweep with known false positives, refined by
  spot-checks of `checkpoint.c:630-670` and `container/netns.c`. The three populations are real; the exact
  boundaries between them were not audited site by site.
- **Whether any compat-corpus case exercises `seekdir` to a stale offset.** If none does, §7.3's option (1)
  is free; if one does, option (3) becomes mandatory and the cost changes. Answerable by grepping the
  fixtures, which was not done.
