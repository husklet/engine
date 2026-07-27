# Prior art: flinux's socket layer, and what it costs us

Sockets and networking are the second-largest blocked surface in the Windows port: **862 diagnostics across
80 distinct symbols** in the production unity TU. The block has two independent causes that compound.

- `bound_route` returns **`-ENOSYS` for the entire socket family** — `src/linux_abi/syscall/binding.c:3811-3826`
  covers syscalls 200–212 (`bind` … `recvmsg`), and the same arm also swallows `epoll_ctl`/`epoll_pwait`
  (21/22) and `sendfile`/`vmsplice`/`splice`/`tee` (71/75/76/77). The rationale is one line at `binding.c:3824`:
  *"A bound slot is never a native descriptor. Unsupported fd operations cannot touch its shadow."* This is
  not a Windows-specific hole — a typed descriptor has no socket route **on any host**.
- `hl_host_network_services` (`include/hl/host_services.h:520-528`) has **six callbacks** — `socket`, `bind`,
  `connect`, `send`, `receive`, `close`. No `listen`, `accept`, `socketpair`, `shutdown`, `getsockname`,
  `getpeername`, `getsockopt`, `setsockopt`, `sendmsg`, `recvmsg`, or readiness. It is implemented on **Linux
  only** (`src/host/linux/host.c:3880-3882`), and no production code path dereferences it.

flinux ran a Linux socket surface on Winsock well enough that `wget`, `netcat`, `ping`, `mtr` and DNS worked.
This document is what it did, what it cost, what it refused, and — the part that matters — which of our two
possible paths to take.

`DOCS.md` is normative. This file is a record of prior art and a recommendation; it changes no behaviour.

### Licensing

**flinux is GPLv3. HL Engine is MIT. They are incompatible.** No flinux source may be copied into `src/`, and
this document does not reproduce flinux code beyond short identifiers and one- or two-line fragments needed to
name a technique. Everything below describes *architecture, algorithms, constants and constraints*, which are
not copyrightable, so that our implementation can be written independently. If you find yourself
transliterating a function from the citations here, stop.

### Provenance and method

Source read: [`wishstudio/flinux`](https://github.com/wishstudio/flinux) at `a041253` (last commit on master),
cloned to the session scratchpad. The socket layer is `src/fs/socket.c` (1 655 lines), `src/fs/socket.h` (22),
`src/common/socket.h`, `src/common/net.h`, `src/common/in.h`, plus the polling half in `src/syscall/vfs.c`
(`vfs_ppoll`, `vfs_pselect6`, the epoll syscalls) and `src/fs/epollfd.c` (160). **All of it was read**, not
sampled. Every flinux `file:line` below is that tree. Engine `file:line` citations were checked against the
working tree at the time of writing; four agents were mutating `src/` concurrently, so line numbers drift —
the claims, not the numbers, are what were verified.

Claims marked **measured** come from three probes written for this document
(`scratchpad/sockexp{,2,3}.c`, 41 distinct behaviours), built with `C:\msys64\clang64\bin\clang.exe`
22.1.8, target `x86_64-w64-windows-gnu`, linked `-lws2_32`, run on Windows 11 Pro 10.0.26200, Winsock 2.2,
non-elevated. Where a Windows behaviour is asserted without that marker it was not measured and says so.

Companion documents: `docs/windows/prior-art-flinux-fs.md` covers the same tree's fd table and VFS and is not
re-covered here; `docs/windows/host-services-map.md` §13 covers the *existing* six-callback network group and
this document **corrects two of its conclusions** (see §10.1).

---

## 1. Status of the finding

| | |
|---|---|
| flinux socket surface | 20 syscalls + `socketcall`; AF_INET / AF_INET6 / a fake AF_UNIX |
| Never implemented | `socketpair`, `recvmmsg`, SCM_RIGHTS/ancillary data, abstract AF_UNIX, `EPOLLET` |
| Structural techniques worth taking | 4 (§3.1, §6.2, §7.2, §9) |
| Defects found in flinux that we must not repeat | 7 (§8) |
| Windows behaviours measured for this document | 41 |
| Recommendation | **Extend `hl_host_network_services` to ABI 2**, 14 appended callbacks (§10) |

The single most consequential measurement is in §3.1 and it contradicts the premise this investigation was
given: **a Winsock `SOCKET` *is* a real kernel handle and `ReadFile`/`WriteFile` work on it.** That removes
the structural obstacle everyone expects and changes which design is correct.

---

## 2. The mapping, call by call

flinux's socket file is a `struct file` subclass (`socket.c:189-197`) carrying a `SOCKET`, an event `HANDLE`,
a mutex `HANDLE`, a `WSAPROTOCOL_INFOW` for fork, and a pointer to a small shared-memory record
(`socket.c:183-187`) holding `{af, type, events, connect_error}`. Every socket syscall is a thin
`DEFINE_SYSCALL` that validates guest memory, looks the fd up, and dispatches through `file_ops`
(`socket.c:1183-1208`, vtable declared at `src/fs/file.h:69-84`).

| Linux call | flinux → Winsock | Verdict |
|---|---|---|
| `socket` | `socket.c:584-641`. Translates domain and type, calls `socket()`, wires `WSAEventSelect`, allocates the shared record, stores into the fd table. | **emulated** — `AF_UNIX` is redirected to `AF_INET` (§6) |
| `bind` | `socket.c:643-724` → `bind()` | one-to-one for INET; **emulated** for UNIX |
| `listen` | `socket.c:811-824` → `listen()` | **one-to-one** |
| `accept`/`accept4` | `socket.c:826-897` → `accept()` in a retry loop, then builds a whole new `socket_file` and re-arms its own event | **emulated** (loop + re-arm) |
| `connect` | `socket.c:726-809` → `connect()`, with a `WSAEWOULDBLOCK` → `EINPROGRESS`-or-block fork at `:791-805` | **emulated** |
| `send`/`sendto` | `socket.c:286-314` → `sendto()`, wrapped in a wait-for-`FD_WRITE` loop | **emulated** |
| `recv`/`recvfrom` | `socket.c:361-390` → `recvfrom()`, wrapped in a wait-for-`FD_READ\|FD_CLOSE` loop | **emulated** |
| `sendmsg` | `socket.c:316-359` → `WSASendMsg()` with a hand-built `WSAMSG` | **emulated** |
| `recvmsg` | `socket.c:392-473`. For `SOCK_DGRAM`/`SOCK_RAW`, resolves `WSARecvMsg` through `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)`; **for stream sockets it degrades to a single-iovec `recvfrom`** (`:397-409`) | **emulated, lossy** |
| `sendmmsg` | `socket.c:1143-1181` — a loop over `sendmsg` with Linux's partial-success return convention | **emulated** |
| `recvmmsg` | vtable slot exists (`file.h:84`); **never implemented** | **refused** |
| `shutdown` | `socket.c:993-1014` → `shutdown()` with `SHUT_RD/WR/RDWR` → `SD_RECEIVE/SEND/BOTH` | **one-to-one** (the constants happen to coincide) |
| `getsockname` | `socket.c:899-951` → `getsockname()`, **plus a fabricated all-zero result when Winsock says `WSAEINVAL`** because an unbound socket is legal on Linux (`:910-936`) | **emulated** |
| `getpeername` | `socket.c:953-973` → `getpeername()` | **one-to-one** |
| `setsockopt`/`getsockopt` | `socket.c:1016-1123`. A hand-written translation switch covering exactly **eight** options | **emulated, tiny** |
| `socketpair` | Constant defined (`common/net.h:10`); `socketcall` case falls to `default` → `-EINVAL` (`socket.c:1649-1653`) | **refused** |
| `socketcall` | `socket.c:1587-1655`, with an argument-count table at `:1580-1585` | i386 multiplexer, n/a to us |

The sockopt table is the whole of it (`socket.c:1021-1080`): `IP_HDRINCL`; `SO_REUSEADDR`, `SO_ERROR`,
`SO_BROADCAST`, `SO_SNDBUF`, `SO_RCVBUF`, `SO_KEEPALIVE`, `SO_LINGER`; `TCP_NODELAY`. Everything else logs
*"Unhandled sockopt level %d, optname %d"* and returns `-EINVAL`. The commit log shows why that set and no
other: `9ddfea8` *"Add setsockopt SO_SNDBUF and SO_RCVBUF for ping"*, `f001e59` *"Add sockopt SO_REUSEADDR.
netcat works"*, `648e8aa` *"Add read() and write(). Support MSG_PEEK. wget works"*, `8716a07` *"Add recv() and
recvfrom(). DNS lookup works now"*. **The surface was grown application by application, not to a
specification.** That is the correct model for a hobby translator and the wrong model for us, because our
gate is a compat corpus, not a demo.

---

## 3. Structural mismatch 1: `SOCKET` versus file descriptor

### 3.1 The premise is false, and this is the most useful thing in this document

The expected obstacle is that a Winsock `SOCKET` is an opaque `UINT_PTR` from a separate namespace that
cannot be `read`/`write`n. **Measured, that is not true on modern Windows.** All of the following succeeded:

| Probe | Result |
|---|---|
| `GetHandleInformation((HANDLE)sock)` | returns 1, flags `0x1` — it is a real kernel handle |
| `ReadFile((HANDLE)sock, …)` on a default (overlapped) socket | succeeded, 2 bytes |
| `WriteFile((HANDLE)sock, …)` on a default socket | succeeded |
| `ReadFile`/`WriteFile` on a `WSASocketW(…, flags=0)` **non-overlapped** socket | both succeeded; the peer read the bytes back |
| `DuplicateHandle(…, (HANDLE)sock, …)` | succeeded, and `getsockopt(SO_TYPE)` on the duplicate returned 1 |
| `SetHandleInformation((HANDLE)sock, HANDLE_FLAG_INHERIT, …)` | succeeded, for AF_INET and AF_UNIX alike |

A Winsock2 socket is an `\Device\Afd` file object. It lives in the same handle table as files, pipes and
events, it duplicates like one, and it inherits like one. For our design this means a socket needs **no
separate namespace**: an `hl_linux_ofd_entry.host_handle` (`include/hl/linux_abi.h:103-124`) can hold a socket
directly on Windows exactly as it holds a file, and `hl_host_handle` is already a `uint64_t`
(`host_services.h:31`), wide enough for a `SOCKET` on Win64.

Two caveats, both real:

- `ReadFile` on an **overlapped** handle with a `NULL` `OVERLAPPED` is not a supported pattern; it worked in
  the probe but is a data race against any concurrent overlapped operation. If we ever use the file-handle
  path we must create sockets with `WSASocketW(…, dwFlags = 0)`. That trades away overlapped I/O. **We should
  not use it.** The value of this measurement is not "call `ReadFile` on sockets" — it is that the *handle
  table* and *duplication* stories are shared, so no parallel namespace is needed.
- `WaitForSingleObject((HANDLE)sock)` returned `WAIT_OBJECT_0` immediately in the probe. That is the AFD
  file object's I/O-completion signal, not a readability signal, and it is not documented as one. **It must
  not be used for readiness** (§7).

### 3.2 What flinux did instead, and why it still applies

flinux unified the namespace one level higher, at the VFS: `struct file` (`file.h:87-93`) is
`{const struct file_ops *op_vtable; int flags; uint32_t ref; SRWLOCK rw_lock;}` and the fd table stores
`struct file *`. A socket is a `struct file` whose vtable happens to implement `bind`/`connect`/`listen`; a
disk file is one whose vtable implements `llseek`/`getdents`. `read`/`write` on a socket land in
`socket_read`/`socket_write` (`socket.c:484-500`), which forward to the internal `recvfrom`/`sendto` helpers
under the socket's mutex. The Winsock `SOCKET` never escapes `struct socket_file`.

**We already have this shape and it is better than flinux's.** `hl_linux_object_ops`
(`src/linux_abi/object.h:14-34`) is the same idea with 12 slots, and `object_ops == NULL` is the discriminator
between "ordinary host file" and "typed object" (dispatch at `src/linux_abi/linux_abi.c:1371-1373`,
`:1467-1469`). flinux's vtable is 39 slots because it fused the file operations and the socket operations into
one table — `file.h:69-84` bolts fourteen socket entries onto the general file vtable, and every non-socket
file type carries fourteen NULL pointers whose only purpose is to produce `-ENOTSOCK` (`socket.c:1228-1231`
and eighteen identical sites). Our 12-slot vtable stays 12 slots; the socket-specific operations belong
*below* the object seam, in the host network group, addressed by `hl_host_handle`. See §10.3.

---

## 4. Structural mismatch 2: constants and `sockaddr` layout

### 4.1 What actually differs

Measured, Winsock 2.2 on Windows 11 versus Linux x86-64:

| Constant | Windows | Linux | |
|---|---|---|---|
| `AF_UNSPEC`, `AF_UNIX`, `AF_INET` | 0, 1, 2 | 0, 1, 2 | **identical** |
| `AF_INET6` | **23** | **10** | differs |
| `SOCK_STREAM/DGRAM/RAW/SEQPACKET` | 1, 2, 3, 5 | 1, 2, 3, 5 | **identical** |
| `IPPROTO_IP/TCP/UDP/IPV6` | 0, 6, 17, 41 | 0, 6, 17, 41 | **identical** |
| `SOL_SOCKET` | **0xFFFF** | **1** | differs |
| `SO_REUSEADDR` | **4** | **2** | differs |
| `SO_ACCEPTCONN` | **2** | **30** | differs |
| `SO_ERROR` | 0x1007 | 4 | differs |
| `SO_TYPE` | 0x1008 | 3 | differs |
| `SO_SNDBUF`/`SO_RCVBUF` | 0x1001/0x1002 | 7/8 | differs |
| `SO_KEEPALIVE` | 8 | 9 | differs |
| `SO_LINGER` | 0x80 | 13 | differs |
| `SO_RCVTIMEO`/`SO_SNDTIMEO` | 0x1006/0x1005 | 20/21 | differs |
| `TCP_NODELAY` | 1 | 1 | **identical** |
| `MSG_OOB`/`MSG_PEEK`/`MSG_DONTROUTE` | 1, 2, 4 | 1, 2, 4 | **identical** |
| `MSG_WAITALL` | **0x8** | **0x100** | differs |
| `MSG_DONTWAIT`, `MSG_NOSIGNAL`, `MSG_TRUNC`, `MSG_CMSG_CLOEXEC` | *do not exist* | 0x40, 0x4000, 0x20, 0x40000000 | absent |
| `SO_EXCLUSIVEADDRUSE` | 0xFFFFFFFB | *does not exist* | Windows-only |
| `SO_REUSEPORT`, `SO_PASSCRED`, `SO_PEERCRED`, `SO_BINDTODEVICE` | *do not exist* | 15, 16, 17, 25 | absent |
| `sizeof(struct linger)` | **4** (2 × `u_short`) | **8** (2 × `int`) | differs |
| `sizeof(sockaddr_in/in6/un/storage)` | 16 / 28 / 110 / 128 | 16 / 28 / 110 / 128 | **identical** |
| `sockaddr_in6` field offsets | 0, 2, 4, 8, 24 | 0, 2, 4, 8, 24 | **identical** |
| `sun_path` capacity | **108** | **108** | **identical** |
| `sizeof(WSACMSGHDR)` | 16 | 16 (`cmsghdr`) | **identical shape** |

The dangerous entries are the ones that differ **and remain valid**. `SO_ACCEPTCONN` is 2 on Windows and
Linux's `SO_REUSEADDR` is 2; Linux's `IP_TTL` is 2 (`common/in.h:4`). A guest value passed through numerically
does not fail — it silently selects a *different, valid* option. This is the single strongest argument for
never carrying a `(level, optname)` pair across our host seam; see §10.4.

### 4.2 Where flinux translated, and whether it got the guest ABI right

Four translators, all in `socket.c`:

- `translate_address_family` (`:57-69`) — a 4-case switch, Linux → Winsock.
- `translate_socket_addr_to_winsock` (`:119-146`) — for `AF_INET` it is a **length-checked `memcpy`**; for
  `AF_INET6` it is a memcpy plus a single 16-bit family rewrite; everything else is refused.
- `translate_socket_addr_to_linux` (`:149-158`) — rewrites `AF_INET6` back to 10 and nothing else.
- `socket_get_set_sockopt` (`:1016-1105`) — the eight-option switch.

The `memcpy` is legitimate and worth understanding rather than dismissing: `sockaddr_in` is byte-identical on
both platforms (measured), and `AF_INET == 2` on both, so no translation is required at all. `sockaddr_in6` is
also byte-identical (measured: family 0, port 2, flowinfo 4, addr 8, scope 24 on both), so only the family
word needs rewriting. **The whole `sockaddr` problem for INET reduces to one 16-bit field.** That is the
useful insight, and it survives into our design.

Did flinux get the *guest ABI* layouts right? For its own target, yes, and for a reason that does not
transfer. flinux is 32-bit x86 only, and it declares the guest structures itself in `common/socket.h`:
`sockaddr_un` as `{unsigned short sun_family; char sun_path[108];}` (correct, 110 bytes), `linux_linger` as
`{int l_onoff; int l_linger;}` (correct, 8 bytes), and `msghdr` as
`{void*; int; struct iovec*; size_t; void*; size_t; unsigned int;}`. On i386 that last one is exactly right
because every member is 4 bytes and there is no padding. **On x86-64 the same declaration also happens to
produce the correct 56-byte Linux `msghdr`** — the compiler inserts the four bytes of tail padding after
`msg_namelen` and after `msg_flags` that the kernel ABI specifies. flinux never had to think about it. We do:
we have two guest ISAs, and `struct msghdr`, `struct cmsghdr` and `struct mmsghdr` must be marshalled
explicitly from guest memory rather than cast, because the host struct is not the guest struct. flinux casts
guest pointers directly (`socket.c:1492-1510` passes the guest `struct msghdr *` straight into
`socket_sendmsg`) after an `mm_check_read_msghdr` bounds check (`:545-562`). We cannot do that; the
`hl_host_iovec` type (`host_services.h:329-332`, `{uint64_t address; uint64_t size;}`) already exists for
exactly this reason.

---

## 5. Structural mismatch 3: `WSAE*` versus `errno`

`translate_socket_error` (`socket.c:71-117`) is a flat 40-entry `switch` from `WSAE*` to `-L_E*`, with
`default:` logging *"Unhandled WSA error code"* and returning `-EIO`. That is the entire shape. Three
observations:

- The table is **not** injective and does not try to be: `WSAESOCKTNOSUPPORT` → `EPROTONOSUPPORT` (`:91`) and
  `WSAEHOSTDOWN` → `ETIMEDOUT` (`:109`) are deliberate merges. Winsock's socktype error genuinely has no
  distinct Linux spelling in flinux's set, and Linux does have `ESOCKTNOSUPPORT` (94), so `:91` is simply
  wrong; we should map it properly.
- Errors reach the table by *call site*: every Winsock call is followed by `WSAGetLastError()` and an explicit
  translation. There is no thread-local ambient errno. That is the right structure and matches our
  `hl_host_result` (`host_services.h:170-175`), which carries `{status, detail_domain, value, detail}` — the
  neutral status *and* the platform code, so nothing is lost.
- One place bypasses the table entirely, and it is a live bug: `SO_ERROR` is translated as an *option name*
  (`socket.c:1037`) and then read by the generic `getsockopt` path (`:1098`), so **the raw `WSAE*` value is
  copied into the guest's buffer**. Measured: after a failed non-blocking connect, `getsockopt(SO_ERROR)`
  yields **10061** (`WSAECONNREFUSED`). A Linux guest expects 111 (`ECONNREFUSED`). Any program that
  `strerror`s a pending socket error under flinux prints nonsense. `SO_ERROR` must be translated *as a value*,
  not routed as an option.

Also measured, and unlike Linux: **Windows' `SO_ERROR` is sticky.** Reading it twice returned 10061 both
times. Linux clears the pending error on read. A correct emulation must cache-and-clear in our own state.

---

## 6. Structural mismatch 4: `O_NONBLOCK` versus `FIONBIO`

### 6.1 Windows cannot be asked

Measured, exhaustively:

- `ioctlsocket(s, FIONBIO, &out)` does **not** read the flag back — `FIONBIO` is write-only, and the call
  *sets* the mode to whatever `out` happened to contain (the probe passed `0xdeadbeef` and the call returned
  success). Using it as a getter both returns garbage and corrupts the state.
- `WSAIoctl(s, FIONBIO, NULL, 0, &q, …)` fails with `WSAEFAULT` (10014).
- `getsockopt(SO_RCVTIMEO)` returns a millisecond timeout, which is a different concept.

There is no query. **The non-blocking bit must be shadowed in our own state**, which is fortunate because we
already do: `hl_linux_ofd_entry.status_flags` (`include/hl/linux_abi.h:103-124`) is documented as *"Open
status flags belong to the OFD; descriptor flags do not"*, which is exactly Linux's `O_NONBLOCK` ownership
rule, and `hl_linux_object_ops.set_status_flags` (`object.h:19`) is the setter. `F_GETFL` reads the shadow,
never the host.

flinux does the same thing — `socket_wait_event` (`socket.c:272-284`) branches on
`f->base_file.flags & O_NONBLOCK`, a VFS-level bit — but with a twist that matters enormously.

### 6.2 flinux's real technique: keep the socket permanently non-blocking

flinux calls `WSAEventSelect` on every socket at creation (`init_socket_event`, `socket.c:524-543`, arming
`FD_READ|FD_WRITE|FD_ACCEPT|FD_CONNECT|FD_CLOSE`). `WSAEventSelect` **implicitly and irreversibly puts the
socket into non-blocking mode** — measured: `recv` with no data on an event-selected socket returns
`WSAEWOULDBLOCK`, and `ioctlsocket(FIONBIO, 0)` on it fails with `WSAEINVAL` (10022) until
`WSAEventSelect(s, NULL, 0)` un-arms it first.

So every flinux socket is a *host* non-blocking socket regardless of what the guest asked for, and blocking
semantics are synthesised in a loop: try the operation; on `WSAEWOULDBLOCK`, if the guest wanted non-blocking
return `-EWOULDBLOCK`, otherwise wait on the socket's event handle via `signal_wait` and retry
(`socket.c:272-284`, and the same loop inlined at `:300-312`, `:346-357`, `:368-381`, `:448-459`, `:833-894`).
`signal_wait` (`src/syscall/sig.c:522-528`) is `WaitForMultipleObjects(count + 1, …)` with the extra slot
carrying the thread's signal event, so a blocking socket operation is interruptible by a Linux signal and
returns `-EINTR`. **This is the correct architecture and we should adopt it wholesale:** always non-blocking
underneath, blocking synthesised above, signal event always in the wait set.

Two measured consequences flinux did not handle:

- An `accept()`ed socket **inherits** the listener's non-blocking mode. Measured: `recv` on a freshly accepted
  socket with no data returns `WSAEWOULDBLOCK` even though nobody armed it. flinux calls `init_socket_event`
  on the accepted socket anyway (`socket.c:840`), so it lands in the right state by accident. A design that
  did *not* re-arm accepted sockets would silently produce non-blocking sockets that the guest believes are
  blocking.
- Un-arming is a two-step: `WSAEventSelect(s, NULL, 0)` then `ioctlsocket(FIONBIO, 0)`. If a guest ever needs
  a genuinely host-blocking socket (it does not, under this architecture), it costs two calls.

---

## 7. Structural mismatch 5: `MSG_NOSIGNAL`, `SO_REUSEADDR`, `shutdown`

### 7.1 `MSG_NOSIGNAL` is free; the rest of the flag word is not

Windows has no `SIGPIPE`. Measured: two consecutive `send`s to a closed peer both return `WSAECONNRESET`
(10054), with no signal-like mechanism anywhere. So `MSG_NOSIGNAL` translates to nothing — it should be
**masked off and discarded**, and `EPIPE` synthesised from `WSAECONNRESET`/`WSAESHUTDOWN` at the call site.
flinux defines `LINUX_MSG_NOSIGNAL` (`common/socket.h:79`) and never references it, which is accidentally
correct for the signal but not for the flag word.

Because the flag word is where flinux's worst live bug is. `socket_recvfrom_unsafe` handles `MSG_DONTWAIT` in
its wait loop and then **passes the raw guest `flags` straight to Winsock `recvfrom`** (`socket.c:372`).
Measured: `recv(s, buf, n, 0x40)` — Linux `MSG_DONTWAIT` — returns `WSAEOPNOTSUPP` (10045). So under flinux,
every `recv(…, MSG_DONTWAIT)` fails with `-EOPNOTSUPP` on a socket that has data waiting. Same for
`MSG_NOSIGNAL` (0x4000) on both `send` and `recv`, and for Linux `MSG_WAITALL` (0x100, versus Winsock's 0x8).
`MSG_PEEK` survives only because it is 2 on both. **Every flag must be explicitly mapped; there is no safe
pass-through.**

### 7.2 `SO_REUSEADDR`: the semantics genuinely invert

This is the one where naming coincidence hides a real difference, and the measurement settles it.

| Scenario | Linux | Windows, measured |
|---|---|---|
| Second `bind` to a **live listening** address, both with `SO_REUSEADDR` | `EADDRINUSE` | **succeeds (0)** — the second socket steals the port, and `listen` on it also succeeds |
| Second `bind` to a live listening address, **without** `SO_REUSEADDR` | `EADDRINUSE` | `WSAEADDRINUSE` (10048) |
| `bind` to a **`TIME_WAIT`** address, Windows default / `SO_REUSEADDR` / `SO_EXCLUSIVEADDRUSE` | default fails; `SO_REUSEADDR` succeeds | **all three succeed (0)** |
| `bind` over a holder that set `SO_EXCLUSIVEADDRUSE`, requester sets `SO_REUSEADDR` | n/a | `WSAEACCES` (10013) |

Reading the table: Windows' **default** already behaves like Linux's `SO_REUSEADDR = 1` (TIME_WAIT rebinding
permitted, live rebinding refused). Windows' `SO_REUSEADDR` is a *stronger* option with no Linux equivalent —
it is port hijacking, a well-known Windows security wart. Therefore the correct mapping is the opposite of the
obvious one:

- guest `SO_REUSEADDR = 1` → **do nothing**; the Windows default is already the requested behaviour.
- guest `SO_REUSEADDR = 0` → set Windows **`SO_EXCLUSIVEADDRUSE = 1`**, which restores refusal.
- **Never set Windows `SO_REUSEADDR`.**

flinux sets it directly (`socket.c:1034-1036`) with the comment *"SO_REUSEADDR: Current semantic is not
exactly correct."* Measured, "not exactly correct" means a flinux guest can steal another process's live
listening port, which no Linux program can do and none expects to be able to.

`SO_REUSEPORT` (Linux 15) has no Windows analogue at all and must return a typed unsupported status, not be
silently ignored — programs that set it expect load-balanced accept.

### 7.3 `shutdown` versus `closesocket`

The only genuinely one-to-one call in the surface. `SHUT_RD/WR/RDWR` are 0/1/2 and `SD_RECEIVE/SEND/BOTH` are
0/1/2, so flinux's mapping (`socket.c:993-1004`) is an identity it did not have to write. Measured:
`shutdown(SD_SEND)` on an AF_UNIX stream socket makes the peer's `recv` return **0**, which is exactly Linux's
EOF. Semantics match.

`closesocket` versus `CloseHandle` is the trap: a `SOCKET` must be closed with `closesocket`, and although §3.1
shows it is a real handle, `CloseHandle` skips the Winsock provider's teardown (including `SO_LINGER`
processing). Our OFD close path (`hl_linux_object_ops.close`, `object.h:33`) must know the object's kind. That
is already true of every other typed object, so it costs nothing new — but it means a socket **must** be a
typed object with its own `close`, not an ordinary opaque host file.

---

## 8. `AF_UNIX`: what flinux did, and whether it still matters

### 8.1 flinux's approach: TCP loopback with a filesystem rendezvous

flinux predates Windows AF_UNIX entirely. Its solution is visible in one line — `translate_address_family`
maps `LINUX_AF_UNIX` to **`AF_INET`** (`socket.c:62`). The rest is a rendezvous protocol:

1. `bind` on a socket whose recorded family is `AF_UNIX` (`socket.c:649-683`) creates the guest's `sun_path`
   as a **real file** via `vfs_openat(… O_CREAT|O_EXCL|O_WRONLY, INTERNAL_O_SPECIAL …)`, then binds the actual
   socket to `127.0.0.1:0` (ephemeral).
2. After the bind succeeds it calls `getsockname` to learn the assigned port and writes the decimal port
   number into that file behind a magic header, `WINFS_UNIX_HEADER` = `"!<UNIX>\x1a\x1b"`
   (`src/fs/file.h:37-38`, written by `winfs_write_special_file`, `src/fs/winfs.c:221`). The same mechanism
   backs flinux's symlink emulation (`WINFS_SYMLINK_HEADER`).
3. `connect` (`socket.c:731-782`) opens `sun_path`, reads the header, parses the port, and connects to
   `127.0.0.1:<port>`.
4. `accept` on such a socket reports the peer address as **unnamed** — it writes back only
   `sizeof(sun_family)` (`socket.c:872-877`) — because the real peer address is a meaningless loopback
   ephemeral port.

It is ingenious and it is unsound in ways that are worth naming, because each one is a requirement on our
implementation:

- The rendezvous file is not cleaned up, is not the socket, and any process on the machine can connect to the
  loopback port. AF_UNIX filesystem permissions do not apply. On a shared or multi-user host this is a
  local-privilege boundary that simply is not there.
- Abstract sockets are refused explicitly (`socket.c:662-666`, `:744-748`) with *"Abstract sockaddr not
  supported."*
- Datagram AF_UNIX becomes UDP loopback, which is unordered and lossy where AF_UNIX datagram is neither.
- `SOCK_SEQPACKET` becomes `SOCK_SEQPACKET` over `AF_INET`, which Winsock does not provide.
- The commit history — `ae1bf76` *"Initial AF_UNIX socket implementation"*, `7a3cfb3` *"Fix AF_UNIX bugs"*,
  `0c33ddf` *"Fix AF_UNIX accept() and connect()"* — is three commits of chasing a design that was leaky by
  construction.

### 8.2 Windows now has a real one, and it is measurably better — but incomplete

Measured, Windows 11 26200:

| | |
|---|---|
| `socket(AF_UNIX, SOCK_STREAM, 0)` | **ok** |
| `socket(AF_UNIX, SOCK_DGRAM, 0)` | `WSAEAFNOSUPPORT` (10047) |
| `socket(AF_UNIX, SOCK_SEQPACKET, 0)` | `WSAESOCKTNOSUPPORT` (10044) |
| `socket(AF_UNIX, SOCK_RDM/SOCK_RAW, 0)` | 10044 / 10047 |
| `socket(AF_UNIX, SOCK_STREAM, 6)` | `WSAEPROTONOSUPPORT` (10043) — protocol must be 0 |
| bind / listen / connect / accept / send / recv | **all ok**, end to end |
| `shutdown(SD_SEND)` → peer `recv` | **0** (EOF), correct |
| `getsockopt(SO_TYPE)` on AF_UNIX | ok, returns 1 |
| `getsockopt(SO_ACCEPTCONN)` on an AF_UNIX listener | ok, returns 1 |
| `DeleteFile` on a **live bound** `sun_path` | **succeeds** — the Linux unlink-after-bind idiom works |
| `WSAEventSelect` / `WSAPoll` / `select` on AF_UNIX | **all work** |
| `WSADuplicateSocketW` on AF_UNIX, then `WSASocketW` | **ok** — forkable |
| `bind` with a **forward-slash** path (`C:/Users/…/x.sock`) | **ok** — no separator conversion needed |
| `bind` with Linux-style trimmed `addrlen` (`2 + strlen + 1`) | **ok** |
| abstract namespace (`sun_path[0] == 0`) | see below |

Three findings that require handling:

**Abstract sockets are not supported, and fail *silently at bind*.** Measured: binding two different sockets to
the same abstract name `"\0hlabs1"` **both returned 0** — the name is ignored, and the socket is autobound to
an unnamed address. `listen` then succeeds, but `connect` to that name fails with `WSAEINVAL` (10022) and
`accept` never fires. Linux would have failed the second `bind` with `EADDRINUSE`. A guest using abstract
sockets — which is common; systemd, D-Bus and Chromium all do — gets a socket that appears to work through
`bind` and `listen` and then never receives a connection. **We must detect `sun_path[0] == 0` at our layer and
return a typed unsupported status immediately**, not forward it. `bind` with `addrlen == sizeof(sun_family)`
(Linux autobind) also silently succeeds and must be treated the same way.

**`addrlen` is not Linux-shaped for unnamed sockets.** Measured: `accept` and `getsockname` on a connected
client both return `addrlen = 110` — `sizeof(struct sockaddr_un)` — with an empty `sun_path`. Linux returns
`2` (`sizeof(sa_family_t)`) for an unnamed AF_UNIX socket. `getsockname` on a *listener* correctly returned
`53` = `2 + strlen(path) + 1`, which is Linux-shaped. We must normalise the unnamed case ourselves; programs
do test `addrlen == sizeof(sa_family_t)` to detect unnamed peers.

**`sun_path` is 108 bytes on both, and that is a real constraint.** The guest supplies a *guest* path such as
`/tmp/foo.sock`; the container VFS maps it to a host path such as
`C:\Users\hutta\AppData\Local\Temp\hl\tmp\foo.sock`. The translated path can exceed 108 bytes where the guest
path did not. `hl_host_network_address.local_path` (`host_services.h:120`) is also 108 bytes, so the seam
inherits the limit. The mitigation is a short, stable host root for the container's `/tmp` plus a hashed
fallback name; this needs a decision before implementation, not after.

**Verdict on flinux's approach: superseded for stream, but still the template for datagram.** For
`SOCK_STREAM` we use the real thing. For `SOCK_DGRAM` and `SOCK_SEQPACKET`, which Windows lacks, flinux's
"emulate over a different transport with a side-channel rendezvous" idea is still the only option — except
that we should emulate over **AF_UNIX stream with an explicit length-prefix framing layer**, not over UDP
loopback. Stream AF_UNIX gives us the reliability and ordering that UDP loopback threw away, and framing over
it reproduces datagram and seqpacket boundaries exactly. See §10.5 for why the `pair` callback makes this
safe.

---

## 9. `SCM_RIGHTS` and ancillary data

**flinux did not support it, at all, and did not know it did not.** There is no `struct cmsghdr` in the tree,
no `SCM_RIGHTS` constant, no `CMSG_*` macro. What `socket_sendmsg_unsafe` does (`socket.c:341-342`) is assign
the guest's `msg_control`/`msg_controllen` **directly into `WSAMSG.Control`** and hand it to `WSASendMsg`. A
guest sending an `SCM_RIGHTS` cmsg therefore hands a Linux-formatted control message to Winsock, which
interprets `Control` as `IPPROTO_IP`/`IPPROTO_IPV6` packet-info ancillary data. On the receive side
(`socket.c:465`) `msg_controllen` is set from whatever Winsock reports and the contents are passed through
uninspected. This is not "unsupported"; it is silent misinterpretation of a guest buffer.

Measured on the Windows side:

- **`SCM_RIGHTS` is not defined by any Windows header.** Nor is `SO_PASSCRED`.
- `WSA_CMSG_FIRSTHDR` and friends *do* exist, and `sizeof(WSACMSGHDR)` is 16, the same shape as Linux's
  `struct cmsghdr` on x86-64. The framing exists; only `IPPROTO_IP`/`IPPROTO_IPV6` types are defined for it.
- `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)` resolves both `WSASendMsg` and `WSARecvMsg` on an **AF_UNIX
  stream** socket (both returned 0), but `WSASendMsg` with a control buffer on that socket returns
  `WSAEINVAL`. There is no fd transfer over any Windows socket.

Windows' analogue is `DuplicateHandle(source_process, handle, target_process, …)`, which is a fundamentally
different model: the *sender* must hold a `PROCESS_DUP_HANDLE`-capable handle to the *receiver*, the transfer
is synchronous and out-of-band, and the receiving process learns the numeric value through some other channel.
It is a push, not a queued in-band payload.

### 9.1 What this means for `checkpoint.c`

This matters more to us than to any other consumer. `ckpt_restore_socket_queue_load`
(`src/linux_abi/checkpoint.c:4585+`) re-materialises a socket's receive queue after restore by building **one**
`SCM_RIGHTS` cmsg carrying up to `253 * 4 = 1012` descriptors — the bound is literal at
`src/linux_abi/checkpoint.c:4635` (`int combo[253 * 4];`) and `:4827`
(`unsigned char control[CMSG_SPACE(253 * 4 * sizeof(int))];`) — and issuing a single `sendmsg`
(`:4829-4844`). The four-per-right factor is engine-private marker fds: an eventfd contributes its writer plus
a metadata marker, a timerfd a marker, an epoll a kqueue placeholder plus a marker, and every right gets an
OFD-identity marker. Capture is the mirror (`ckpt_capture_socket_queue`, `:583-683`), which rejects any
ancillary type that is not `SCM_RIGHTS` (`:626-634`).

**Ruling: this must not be ported through the socket layer, and it must not be attempted through the network
group.** The reason is that it is not really socket ancillary data — it is a *self-to-self handle-table
transfer* that happens to use the kernel socket buffer as a queue because on Linux that is free. Both ends are
ours. On Windows the same intent is expressed by `DuplicateHandle` into the target process plus a private
side-channel carrying the ordinals and markers, which is exactly the shape the existing broker already has
(`include/hl/checkpoint_stream.h:12` hands the broker socket to the first engine process by `SCM_RIGHTS`
today). Modelling it as socket ancillary data on Windows would mean inventing a cmsg encoding, framing it over
AF_UNIX stream, and reimplementing the kernel's queue — for a payload that never crosses a trust boundary.

The corollary for the network group: **guest-visible `SCM_RIGHTS` is a separate, later, and much smaller
problem.** A guest passing fds over its own AF_UNIX socket is a real Linux behaviour we will eventually need,
and it is implementable — both endpoints are inside our engine, so we can intercept the cmsg, translate the
guest fd numbers to OFD identities, frame them in our own encoding over the AF_UNIX stream, and reconstitute
them on receive. But it requires the receiving side to be *us*, which means it only works between two engine
processes, and that limitation must be stated in the contract rather than discovered. Phase 3 (§10.7).

---

## 10. Readiness: does flinux fit our wakeup-bus model?

### 10.1 Our model, restated

`src/linux_abi/epoll.c` discards host readiness and re-derives it. The proof is two lines: `epoll_notify`
(`epoll.c:50-54`) does `(void)token;` and just kicks the single `epoll->wake` pollset, and
`hl_linux_epoll_wait` (`epoll.c:427-428`) names the `hl_host_event_record` it passes to `event->wait`
**`ignored`**. Truth comes from `epoll_sample` (`epoll.c:354-399`), which walks every watch and calls
`hl_linux_object_ready` (`src/linux_abi/linux_abi.c:879-882`), which calls the *object's own*
`readiness(context, interests)`. Edge-triggering is synthesised above the object
(`epoll.c:380-384`: `transition = ready & ~watch->previous`). Hence the ruling recorded at
`docs/windows/host-services-map.md:413` — `WaitForMultipleObjects` over a registered handle set, IOCP
withdrawn.

### 10.2 flinux's model is the same model

`vfs_ppoll` (`src/syscall/vfs.c:2363-2497`) is a two-phase loop over a **two-function** interface:
`get_poll_status(f)` returns current readiness as `LINUX_POLL*` bits, and `get_poll_handle(f, &events)`
returns a Win32 `HANDLE` to wait on plus the events it can signal (`file.h:47-48`). The loop:

1. Scan every fd calling `get_poll_status`. If anything is ready, record it and skip the wait entirely.
2. Otherwise collect the `HANDLE`s and `signal_wait` on them (`vfs.c:2436`) — `WaitForMultipleObjects` plus the
   signal event.
3. On wake, **re-query `get_poll_status` for the woken fd** and only report it if it is genuinely ready;
   otherwise recompute the remaining timeout and loop. The comment at `vfs.c:2456-2458` states the reason
   outright: *"Some file descriptors (console, socket) may be not readable even if it is signaled / Query the
   current state again to make sure."*

That is a wakeup bus with re-derivation, arrived at independently, for the same reason. The socket's two
implementations are `socket_get_poll_status` (`socket.c:230-242`) and `socket_get_poll_handle`
(`socket.c:244-249`), which returns the `WSAEventSelect` event handle. epoll is layered on top: `epollfd.c`
stores a flat array of `{fd, epoll_event}`, `epoll_pwait` (`vfs.c:2684-2710`) marshals it into a `pollfd`
array and calls `vfs_ppoll`, and converts the result back. There is no persistent host registration at all.

**So flinux fits our model and confirms it.** Two of its measurements strengthen ours:

- `WSAEventSelect` events are **edge-triggered and consumed by `WSAEnumNetworkEvents`**. Measured: after a
  peer send, the first `WSAEnumNetworkEvents` reports `FD_READ`; a second call **with the byte still unread**
  reports `0`. A level-triggered `poll()` built on the raw event would hang. `socket_update_events_unsafe`
  (`socket.c:202-228`) exists solely to fix this: it accumulates the edges into a sticky bitmask in shared
  memory with `InterlockedOr`, and the read/write paths clear the relevant bit with `InterlockedAnd` when
  they observe `WSAEWOULDBLOCK` (`:311`, `:371`, `:452`, `:893`). That is an entire hand-rolled
  level-triggering layer, and it is a state machine we do **not** need, because our `readiness()` re-derives.
- Re-derivation is cheap and unambiguous. Measured on an AF_UNIX stream socket:

  | state | `FIONREAD` | `recv(1, MSG_PEEK)` | correct answer |
  |---|---|---|---|
  | empty | 0 | `-1` / `WSAEWOULDBLOCK` | not ready |
  | data | 2 | 1 | `POLLIN` |
  | peer `shutdown(SD_SEND)` | 0 | **0** | `POLLIN \| POLLHUP` |
  | peer closed | 0 | **0** | `POLLIN \| POLLHUP` |

  `FIONREAD` alone cannot distinguish "empty" from "EOF" — both report 0. A one-byte `recv(MSG_PEEK)` can, and
  does not consume. For writability there is no `FIONWRITE`; measured, `select(writefds)` and
  `WSAPoll(POLLWRNORM)` both correctly report 0 on a socket whose send buffer is full (131 072 bytes on
  AF_UNIX loopback), and — unlike Linux — `send(s, buf, 0, 0)` on a full socket returns `WSAEWOULDBLOCK`
  rather than 0, so a zero-length send is a valid probe for stream sockets. It is **not** valid for datagram
  sockets, where it transmits an empty datagram; use `select` there.

### 10.3 Corrections to `host-services-map.md` §13

Two conclusions in that section should be revised in light of the measurements here. It is not this
document's place to edit it, so they are recorded:

- *"The most mechanical group. Winsock2 is a near-clone of BSD sockets."* — the API shapes match; the
  **constant values, option semantics and flag words do not**, and the group as specified cannot express
  `listen`/`accept` at all. The `send`/`receive` rows correctly flag the flag-mapping hazard; §7.1 measures
  the failure mode (`WSAEOPNOTSUPP`, not a silent misread).
- *"`SOCKET` and `HANDLE` are different types with different closers, so the handle …"* — the closers differ
  (`closesocket`, not `CloseHandle`), but §3.1 measures that they are the same *kind* of object: same handle
  table, `DuplicateHandle`-able, inheritable. Only the close path needs to discriminate.

One further constraint, measured and unchanged: `MAXIMUM_WAIT_OBJECTS` is **64**, and Winsock's `FD_SETSIZE`
defaults to 64. Sockets impose no *new* ceiling — pipes and eventfds already impose it — but a pollset larger
than 63 needs chunking or a waiter tree either way.

### 10.4 Where flinux's readiness fails

Three defects our design avoids by construction, worth naming so nobody reintroduces them:

- `vfs_ppoll` returns **at most one** ready fd when it has to block (`vfs.c:2478-2483` fills one `revents` and
  `break`s). Only the non-blocking pre-scan reports several. A `poll()` on 50 sockets with 10 ready wakes,
  reports 1, and the guest loops. Correct but pathologically slow, and `epoll_wait`'s `maxevents` becomes
  decorative.
- `EPOLLET` is **refused outright** with `-EINVAL` (`vfs.c:2638-2642`, *"Edge triggered epoll is not
  supported."*). Our `epoll_sample` synthesises it (`epoll.c:380-384`), so we are ahead here.
- `epollfd_ctl_add` (`epollfd.c:51-68`) writes `epollfd->fds[epollfd->fd_count]` with **no bound check**
  against `MAX_EPOLLFD_COUNT` (128, `epollfd.c:28`). The 129th `EPOLL_CTL_ADD` writes past the array.

---

## 11. What flinux never made work

From the tree, the vtable, and the commit log:

| Gap | Evidence |
|---|---|
| `socketpair` | no `DEFINE_SYSCALL`; `socketcall` case 8 falls to `default` → `-EINVAL` (`socket.c:1649-1653`) |
| `recvmmsg` | vtable slot `file.h:84`, never assigned in `socket_ops` (`socket.c:1183-1208`) |
| SCM_RIGHTS / any ancillary data | §9 — control buffer forwarded verbatim to `WSAMSG.Control` |
| Abstract AF_UNIX | refused, `socket.c:662-666`, `:744-748` |
| `MSG_EOR`, `MSG_OOB`, `MSG_ERRQUEUE` | `/* TODO */` at `socket.c:471` |
| `MSG_WAITALL` for stream `recvmsg` | `/* TODO: MSG_WAITALL */` at `socket.c:402-405`; the stream path discards `msg_flags` (`:407`) and reads **only `msg_iov[0]`** (`:408`) — a multi-iovec stream `recvmsg` silently drops every buffer after the first |
| Netlink, packet sockets, raw ICMP | `translate_address_family` (`:57-69`) has four cases |
| `EPOLLET` | `-EINVAL`, `vfs.c:2638-2642` |
| `SO_PEERCRED`, `SO_PASSCRED`, `SO_REUSEPORT`, `SO_BINDTODEVICE`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_TYPE`, `SO_ACCEPTCONN`, `SO_OOBINLINE`, `SO_DOMAIN`, `SO_PROTOCOL` | not in the eight-option switch → `-EINVAL` |
| Datagram truncation semantics | not addressed. Measured: Linux `recv` of 4 bytes from a 10-byte datagram returns 4 and discards the rest silently (and returns 10 with `MSG_TRUNC`); Windows returns `-1`/`WSAEMSGSIZE` (10040) **and discards the remainder**, and `MSG_PARTIAL` does not change it. A guest that reads short datagrams sees an error where Linux sees success. |
| `sendfile`/`splice` to a socket | absent; also `-ENOSYS` in our tree (`binding.c:3806-3810`) |

Seven live defects were found while reading, all cited above: the `SO_ERROR` pass-through (§5), the raw
`MSG_*` pass-through (§7.1), the `SO_REUSEADDR` inversion (§7.2), the single-iovec stream `recvmsg`, the
`epollfd` overflow (§10.4), the one-fd-per-`poll` limit (§10.4), and one more: `socket_get_set_sockopt`'s
`switch` has **no `break` after the `LINUX_SOL_IP` arm** (`socket.c:1021-1029`), so a `SOL_IP` option falls
through into the `SOL_SOCKET` arm, which overwrites `level` and matches by optname. Linux `IP_TTL` is 2
(`common/in.h:4`) and Linux `SO_REUSEADDR` is 2, so `setsockopt(SOL_IP, IP_TTL, …)` becomes
`setsockopt(SOL_SOCKET, SO_REUSEADDR, …)` — cross-level aliasing, exactly the failure mode §4.1 predicts, in
the code that was supposed to prevent it. Also `getsockopt(SO_LINGER)` passes an **uninitialised** `optlen` by
pointer (`socket.c:1058-1059`); `struct linger` is 4 bytes on Windows and 8 on Linux, so the size matters.

---

## 12. Recommendation

### 12.1 The choice

**Extend `hl_host_network_services` to ABI 2. Do not build a Windows-specific socket front.** Firmly.

Five reasons, in order of weight.

**1. The typed lane has no socket path on any host, so this is not a Windows tax.** `binding.c:3811-3826`
returns `-ENOSYS` for 200–212 regardless of platform. Whatever we build for Windows we are building for
everyone; the only question is whether it lands above or below the seam. There is no "keep it Linux-front"
option that leaves Linux working and Windows broken — Linux is *also* broken here, it just has a second lane
that hides it.

**2. That second lane cannot be extended to Windows even in principle.** `src/linux_abi/syscall/net.c` (2 823
lines, `#include`d into the target TU at `src/linux_abi/syscall/dispatch.c:205`) and
`src/linux_abi/container/netns.c` (4 606 lines) call `socket(2)`/`bind(2)`/`recvmsg(2)` directly and index
per-fd state by host fd integer. `netns.c:263-265` states the assumption in its own words: *"hl uses host fds
directly as guest fds, so the fd integers in an `SCM_RIGHTS` payload need no remap."* A Winsock `SOCKET` is
not a small dense integer and is not the guest's fd number. A Windows front would be a **second** 3 000-line
socket emulator with a different fd model, used on one platform, sharing no tests with the first.

**3. macOS is in the same hole and gets fixed for free.** `src/host/macos/host.c` has zero occurrences of
`network` and its capability mask (`:4808-4812`) omits `HL_HOST_CAP_NETWORK`. Extending the group and
implementing it on Linux, macOS, Windows and fake gives three platforms a tested socket path where today one
platform has an untested six-callback stub.

**4. The existing group has exactly one consumer and it is a unit test** (`tests/unit/linux.c:963-1042`).
Nothing in `src/` dereferences `services->network`. That is unusually good news: **we may tighten the
semantics of the existing six callbacks without a compatibility break**, which we must, because
`hl_linux_network_send`/`_receive` (`src/host/linux/host.c:3496-3515`) pass the guest's `flags` word through
raw as a native `int` — the same defect §7.1 measures in flinux, waiting to happen.

**5. The engine's own plan already says so.** `docs/windows/surface3-plan.md:185` scopes A-socket as *"~14 new
`hl_host_network_services` callbacks + a typed socket object + `hl_linux_socket*`"* and `:310` as
*"`HL_HOST_NETWORK_ABI` 1 → 2"*. This document independently reaches the same shape and supplies the
signatures and the measurements behind them.

### 12.2 The mechanism, per the contract's own rule

`DOCS.md:316-317`: *"A backend advertises only complete groups. Optional appended callbacks are detected using
the group size. Missing optional behavior returns a typed unsupported status without side effects."* The
worked precedent is the memory group: `HL_HOST_MEMORY_ABI_MIN 6u` with the comment at
`include/hl/host_services.h:9-13` (*"an ABI 6 provider ends at `repair_signal_page`; the address-keyed
operations appended in ABI 7 are absent rather than NULL"*), `hl_memory_prefix_size()` returning
`offsetof(hl_host_memory_services, unmap_address)` (`src/core/host_services.c:14-17`), and the two-tier check
at `:29-41`.

Network today uses the **strict** form (`src/core/host_services.c:100-104`): exact `abi` match via
`hl_valid_group`, `size >= sizeof(*network)`, all six callbacks non-NULL. The bump mirrors memory exactly:

```c
#define HL_HOST_NETWORK_ABI     2u
#define HL_HOST_NETWORK_ABI_MIN 1u
/* Bytes an ABI 1 network group is required to carry: everything through close. */
static size_t hl_network_prefix_size(void) { return offsetof(hl_host_network_services, listen); }
```

with the validator accepting `abi` in `[MIN, ABI]`, demanding `size >= hl_network_prefix_size()` and the six
original callbacks always, and demanding the appended fourteen only when `abi >= HL_HOST_NETWORK_ABI`. Note
`hl_valid_group`'s check is `header[1] >= size` (`src/core/host_services.c:9-12`) — group size, not
`hl_has_field` — so the prefix form must be written explicitly, as memory's is.

### 12.3 Callbacks to add

Fourteen, appended in this order. `hl_host_result` (`host_services.h:170-175`) already carries
`{status, detail_domain, value, detail}`, so a new handle rides in `value` and a platform error code rides in
`detail` without leaking into the status.

```c
    /* --- appended at HL_HOST_NETWORK_ABI 2 --- */
    hl_host_result (*listen)(void *context, hl_host_handle socket, uint32_t backlog);
    hl_host_result (*accept)(void *context, hl_host_handle socket, hl_host_network_address *peer, uint32_t flags);
    hl_host_result (*pair)(void *context, uint32_t family, uint32_t type, uint32_t protocol, hl_host_handle ends[2]);
    hl_host_result (*shutdown)(void *context, hl_host_handle socket, uint32_t direction);
    hl_host_result (*local_address)(void *context, hl_host_handle socket, hl_host_network_address *address);
    hl_host_result (*peer_address)(void *context, hl_host_handle socket, hl_host_network_address *address);
    hl_host_result (*get_option)(void *context, hl_host_handle socket, uint32_t option, hl_host_bytes value);
    hl_host_result (*set_option)(void *context, hl_host_handle socket, uint32_t option, hl_host_const_bytes value);
    hl_host_result (*send_message)(void *context, hl_host_handle socket,
                                   const hl_host_network_message *message, uint32_t flags);
    hl_host_result (*receive_message)(void *context, hl_host_handle socket,
                                      hl_host_network_message *message, uint32_t flags);
    hl_host_result (*readiness)(void *context, hl_host_handle socket, uint32_t interests);
    hl_host_result (*wait_handle)(void *context, hl_host_handle socket);
    hl_host_result (*set_status_flags)(void *context, hl_host_handle socket, uint32_t flags);
    hl_host_result (*duplicate)(void *context, hl_host_handle socket);
```

One new public type, carrying no C library object and no native descriptor, per `DOCS.md:107-109`:

```c
typedef struct hl_host_network_message {
    hl_host_network_address *address;  /* NULL when unaddressed */
    const hl_host_iovec *buffers;      /* guest-address vectors; hl_host_iovec already exists */
    uint32_t buffer_count;
    uint8_t *control;                  /* engine-neutral ancillary encoding, never a raw cmsghdr */
    uint32_t control_size;             /* in: capacity. out: bytes produced */
    uint32_t flags;                    /* out: HL_HOST_MSG_TRUNCATED / _CONTROL_TRUNCATED / _END_OF_RECORD */
} hl_host_network_message;
```

Plus neutral constant families: `HL_HOST_MSG_*` (peek, dontwait, waitall, oob, truncate — **not** the Linux
values, and never passed through), `HL_HOST_SHUTDOWN_{READ,WRITE,BOTH}`, `HL_HOST_SOCKOPT_*`, and two
extensions to the existing enums, `HL_HOST_NETWORK_SEQPACKET = 3` and `HL_HOST_NETWORK_RAW = 4`
(`host_services.h:113`).

### 12.4 The one design decision that matters: options are a flat neutral enum

`get_option`/`set_option` take a **single `uint32_t option`, not a `(level, optname)` pair.** This is not
stylistic. §4.1 measures that Windows `SO_ACCEPTCONN` is 2, Linux `SO_REUSEADDR` is 2 and Linux `IP_TTL` is 2;
§11 shows flinux shipping a live cross-level aliasing bug in the very function meant to prevent it. A flat
`HL_HOST_SOCKOPT_REUSE_ADDRESS` / `_ERROR` / `_TYPE` / `_ACCEPT_CONNECTIONS` / `_SEND_BUFFER` / `_LINGER` / …
enum makes pass-through *impossible to express*, so the bug cannot be written.

Three rules ride with it:

- **Values are neutral too.** Scalar options carry a `uint32_t`; `LINGER` carries `{uint32_t on; uint32_t seconds;}`
  (Windows' `struct linger` is 4 bytes, Linux's is 8 — measured); timeouts carry `uint64_t` nanoseconds
  (Windows takes a `DWORD` of milliseconds, Linux a `struct timeval`).
- **`HL_HOST_SOCKOPT_ERROR` returns an `hl_status`, never a platform code**, with the platform code in
  `hl_host_result.detail`. This is the flinux `SO_ERROR` defect (§5), designed out.
- **`HL_HOST_SOCKOPT_REUSE_ADDRESS` is defined by behaviour, not by name**: "permit rebinding an address in
  `TIME_WAIT`; still refuse a live bind". The Windows backend implements `1` as a no-op and `0` as
  `SO_EXCLUSIVEADDRUSE = 1` (§7.2, measured). A backend that cannot honour an option returns
  `HL_STATUS_NOT_SUPPORTED` — `SO_REUSEPORT` on Windows, `SO_PASSCRED` everywhere but Linux.

### 12.5 Why `pair` is its own callback and not `socket`+`bind`+`connect`

Because a socketpair is entirely private: both ends are ours, nobody else can observe the wire. That makes it
the **only** place we can legitimately insert a framing layer. Windows has no `AF_UNIX` `SOCK_DGRAM`
(measured, `WSAEAFNOSUPPORT`) and no `SOCK_SEQPACKET` (`WSAESOCKTNOSUPPORT`), yet
`socketpair(AF_UNIX, SOCK_DGRAM|SOCK_SEQPACKET, 0)` is extremely common in Linux userland. With a `pair`
callback the Windows backend implements it over an AF_UNIX **stream** pair with a 4-byte length prefix,
reproducing message boundaries exactly, and no external party can ever see the prefix. If instead the guest
composed the pair from `socket`+`bind`+`listen`+`connect`+`accept`, the resulting socket could later be handed
to something outside our control and the framing would leak.

The same trick does **not** rescue a *named* `AF_UNIX` datagram socket, where a third party may connect.
Those must return `HL_STATUS_NOT_SUPPORTED` on Windows, and that is the contract working as designed
(`DOCS.md:316-317`).

### 12.6 What else has to change (this is not just the header)

| Change | Where | Note |
|---|---|---|
| Group bump + 14 signatures + 2 types + ~30 constants | `include/hl/host_services.h` | `HL_HOST_NETWORK_ABI 1 → 2`, `_ABI_MIN 1` |
| Prefix validator | `src/core/host_services.c:100-104` | mirror memory's `:29-41` |
| A typed socket object | new `src/linux_abi/socket.c`, `socket.h` | `HL_LINUX_OBJECT_SOCKET`, a 5th `hl_linux_object_ops` table alongside pipe/eventfd/inotify/epoll. **The vtable does not grow** — `read`/`write`/`readiness`/`wait_handle`/`subscribe`/`clone`/`close` forward to the network group by `hl_host_handle`; socket-only operations are reached by `hl_linux_socket_*` entry points, not vtable slots |
| Guest ABI marshalling | same file | `sockaddr_in/in6/un`, `msghdr`/`cmsghdr`/`mmsghdr`, both guest ISAs. Cannot be a cast (§4.2) |
| Constant translation tables | same file | guest `AF_*`/`SOCK_*`/`MSG_*`/`SOL_*`/`SO_*`/`IPPROTO_*` → `HL_HOST_*`. Table-driven, exhaustive, unknown → `EINVAL` |
| Replace the `-ENOSYS` arm | `src/linux_abi/syscall/binding.c:3811-3826` | route 198/199/200–212 to `hl_linux_socket_*`; leave 21/22/71/75/76/77 alone |
| Linux backend | `src/host/linux/host.c` after `:3515` | 14 implementations; **also fix the raw `flags` pass-through at `:3496-3515`** |
| macOS backend | `src/host/macos/host.c` | whole group new, 20 callbacks; `SO_NOSIGPIPE` instead of `MSG_NOSIGNAL`; add `HL_HOST_CAP_NETWORK` to `:4808-4812` |
| Windows backend | `src/host/windows/host.c` | 20 callbacks over Winsock; `WSAStartup` once; `WSAEventSelect` per socket for `wait_handle`; `FIONREAD`+`recv(MSG_PEEK)` / `select` for `readiness`; ~50-entry `WSAE*` table; AF_UNIX datagram framing; abstract-namespace rejection; add `HL_HOST_CAP_NETWORK` to `:315-316` |
| Fake backend | `src/host/fake/host.c` | in-memory loopback so the compat corpus runs hostless |
| Tests | `tests/unit/`, a compat manifest | replace `tests/unit/linux.c:963-1042` |

### 12.7 Cost, and phasing

Line counts are estimates from the surface areas above, not measurements; they are stated so they can be
checked against reality later.

| Phase | Content | New/changed | Confidence |
|---|---|---|---|
| **1** | Header + validator + typed socket object + `binding.c` routing + `socket`/`bind`/`listen`/`accept`/`connect`/`send`/`receive`/`close`/`shutdown`/`local_address`/`peer_address`/`readiness`/`wait_handle`/`set_status_flags`/`duplicate`; AF_INET, AF_INET6, AF_UNIX **stream**; four backends | ~3 000 | medium |
| **2** | `get_option`/`set_option` and the neutral option table; `pair` incl. Windows datagram framing; `send_message`/`receive_message` **without** ancillary data; `sendmmsg`/`recvmmsg` above the seam | ~1 500 | medium |
| **3** | Guest-visible ancillary data (`SCM_RIGHTS` between two engine processes only, §9.1); `sendfile`/`splice` to a socket | ~800 | **low** |

Phase 1 is the one that unblocks: it covers `socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`/
`shutdown`/`getsockname`/`getpeername`/`poll`/`epoll` participation, which is where the large majority of the
862 diagnostics live. Phase 2 removes the long tail of `getsockopt`/`setsockopt` and the `msg` family. Phase 3
should not be started until a compat manifest demonstrates a guest that needs it.

Total ≈ **5 300 lines across 9 files and 4 backends**, versus a Windows-front alternative that is ≥ 3 000
lines used on one platform, tested by nothing that exists, and permanently divergent from `net.c`. The
extension is not cheaper in the first phase; it is cheaper by the second, and it is the only one of the two
that leaves macOS with a socket.

### 12.8 What was not measured, and what could still surprise us

Stated explicitly, because the estimate above depends on these:

- **Fork.** `WSADuplicateSocketW` → `WSASocketW` works for AF_INET and AF_UNIX (measured, same process), and
  `SetHandleInformation(HANDLE_FLAG_INHERIT)` succeeds on both. Whether a socket survives our
  `RtlCloneUserProcess` path (`docs/windows/experiment-rtlclone.md`) was **not** tested. flinux's own history
  is a warning: `a2229ff` *"Socket forking support (untested)"* followed by `7874a4f` *"vfs: Fix fork deadlock
  when sharing sockets"* — its `socket_fork` (`socket.c:251-256`) takes an exclusive `SRWLOCK` in the parent
  and releases it only in `after_fork_parent`, which is the deadlock it later had to fix.
- **Blocking `accept`/`connect` cancellation.** Interrupting an in-flight Winsock operation for signal
  delivery was not tested. The architecture (§6.2, always non-blocking, `WaitForMultipleObjects` with the
  signal event) is designed to avoid needing it, but the first `EINTR` compat case will prove it.
- **`WSAPoll` on a failed connect.** The historical Winsock defect is that `WSAPoll` never reports
  `POLLWRNORM` for a failed connect. Measured on this build it returned `revents = 0x13`
  (`POLLERR|POLLHUP|POLLWRNORM`), i.e. the defect appears fixed here. Do not rely on that across Windows
  versions; `FD_CONNECT` with `iErrorCode[FD_CONNECT_BIT]` (measured: 10061) and `SO_ERROR` (measured: also
  10061) both report it reliably.
- **AF_UNIX path length.** §8.2 identifies the 108-byte squeeze after guest→host path translation. Whether
  the container's real `/tmp` mapping fits was not measured and needs deciding before Phase 1.
- **Performance.** Nothing here was benchmarked. `readiness` re-derivation costs a `FIONREAD` plus possibly a
  one-byte `recv(MSG_PEEK)` per fd per `epoll_wait` wake, which is the price of the wakeup-bus model and was
  accepted at `docs/windows/host-services-map.md:413`. On a 63-fd pollset that is up to 126 syscalls per wake.
  If a compat case shows this dominating, the mitigation is a per-socket sticky-readiness cache maintained by
  the `WSAEventSelect` edges — which is precisely what flinux's `socket_update_events_unsafe`
  (`socket.c:202-228`) is, so the fallback design is already proven to work.
