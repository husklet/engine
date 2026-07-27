# Porting the `hl-engine` crate's Rust source off Unix

`docs/windows/rust-crate.md` covers how the crate is *built* — the frozen archive, the MSVC-vs-GNU ABI
question, the packaging budget. This document covers the problem that sits underneath it and is the larger
of the two: **the crate's own Rust source does not compile for any Windows target**, and some of it will
not merely fail to compile but will silently change meaning.

Scope and design only. Nothing here is implemented, and no file under `pkgs/rust/` was modified.

`DOCS.md` is normative. Every number below was measured on this branch on a Windows 11 Pro 26200 box with
`rustc 1.97.1 (x86_64-pc-windows-msvc)` and VS 2022 Build Tools; where something was not measured it says
so. The sibling document could not run any of this — it had no Rust toolchain — so several of its ranked
open questions are settled here, one of them against its prediction.

---

## 1. Status

| | |
|---|---|
| `cargo check --target x86_64-pc-windows-msvc` with the `compile_error!` gate removed | **52 errors, 12 files** — measured, §3 |
| Lines of `pkgs/rust/src` that fork per platform | **~250 of 11,977**, concentrated in 12 functions — §7 |
| C headers that must change | **2 of 12** (`activation.h`, `core/checkpoint_channel.h`); ~10 signatures — §4 |
| `hl_host_services` (686 lines, 17 groups) | **no change** — already descriptor-free by construction, §4.1 |
| AF_UNIX `SOCK_STREAM` on this box | **works** — measured, §5.1 |
| AF_UNIX `SOCK_DGRAM` / `SOCK_SEQPACKET` | **do not exist** — `WSAEAFNOSUPPORT` / `WSAESOCKTNOSUPPORT`, measured |
| Rust code that needs AF_UNIX *datagrams* | **none.** The datagram is entirely C-side; Rust holds a token — §5.2 |
| Rust code that passes descriptors as ancillary data | **none.** All `SCM_RIGHTS` is C-side — §5.2 |
| First milestone at which `cargo build --target x86_64-pc-windows-msvc` succeeds | **M3**, with no archive and no Windows host backend — §9 |

The single most important finding is **not** in the error list. It is §6: three guest-path validations
compile cleanly on Windows and return different answers, with no diagnostic and no test that would catch
it.

---

## 2. What was measured

All six probes are self-contained single-file Rust programs of under 100 lines each, using no dependencies
beyond raw `ws2_32`/`kernel32` externs; each is described precisely enough below to be rewritten in a few
minutes. They were run out-of-tree and nothing was committed.

| # | Question | Result |
|---|---|---|
| P1 | Which AF_UNIX socket types does the Windows kernel support? | `SOCK_STREAM` **OK**; `SOCK_DGRAM` **WSAEAFNOSUPPORT (10047)**; `SOCK_SEQPACKET` **WSAESOCKTNOSUPPORT (10044)**. `sizeof(SOCKET) == 8` |
| P2 | Can AF_UNIX stream stand in for `socketpair()`, with timeouts, as an inheritable handle? | bind/listen/connect/accept **OK**; `SO_RCVTIMEO(200 ms)` returned `WSAETIMEDOUT` after **205.9 ms**; `GetHandleInformation(SOCKET)` **succeeded**, `SetHandleInformation(HANDLE_FLAG_INHERIT)` **succeeded**. A real file appears at the bind path |
| P3 | Do `ffi.rs`'s four raw libc externs resolve at link time on windows-msvc? | **All four fail.** `LNK2019: unresolved external symbol` for `pipe`, `fcntl`, `kill`, **and `close`** |
| P4 | Does rustc honour `static:+whole-archive` on windows-msvc, and does a `.CRT$XCU` constructor survive out of a static `.lib`? | **Yes to both.** An unreferenced constructor in an `lib.exe`-produced `.lib` **ran before `main()`**. The counterfactual (`static=`, no modifier) linked, ran `main()`, and **silently dropped the constructor** |
| P5 | Do `std::path` guest-path validations keep their meaning on Windows? | **No.** `Path::new("/etc/passwd").is_absolute()` is **`false`**; `Path::new(r"a\b")` yields **two** `Normal` components. §6 |
| P6 | Does a missing archive break `cargo build`/`cargo check` of the *library*? | Only when the link directives are emitted. No directives → rlib builds fine. Directives + missing archive → **both `build` and `check` fail** with `could not find native static library` |

**P4 settles the sibling document's open questions 1 and 2 affirmatively**, at rustc 1.97.1. It also
demonstrates the silent-failure mode on Windows rather than predicting it: without `+whole-archive` there
is no link error, no warning, and no runtime diagnostic — the program simply runs unactivated. *Unverified:*
that this holds at the declared MSRV, `rust-version = "1.81"`.

P4 turned up one thing the sibling document did not anticipate. A `.lib` compiled with `cl /MT` (the
default) produces `LINK : warning LNK4098: defaultlib 'LIBCMT' conflicts with use of other libs`, because
rustc on windows-msvc defaults to `crt-static=false` and links the *dynamic* UCRT. Rebuilding the archive
with `/MD` removes it. **The Windows archive must be built against the dynamic UCRT**, or every downstream
`cargo build` carries a linker warning and a real risk of two CRT copies with two heaps.

P6 confirms that `build.rs`'s "supported host, archive absent → warn and emit *no* link directives" branch
is load-bearing on Windows for exactly the reason it is on Unix. Preserve it verbatim for the Windows row
or the Windows lint lane is red from the first commit.

---

## 3. The compile errors, measured

With `src/lib.rs`'s `compile_error!` gate removed and `build.rs` stubbed:

| error | count | what it is |
|---|---|---|
| `E0599` no method / no associated fn | 23 | `as_bytes`, `from_vec`, `mode`, `as_raw_fd`, `from_raw_fd`, `from_mode`, `is_socket` |
| `E0433` cannot find `unix` in `os` | 23 | the `std::os::unix::*` paths themselves |
| `E0432` unresolved import `std::os::fd` | 2 | `ffi.rs:5`, `engine.rs:17` |
| `E0425` cannot find function | 2 | `machine.rs:220,243` — `stop_signal`/`continue_signal` have no Windows arm |
| `E0308` mismatched types | 1 | `ffi.rs:123` — `interrupt_signal()` has no Windows arm, so its body is `()` |
| `E0004` non-exhaustive patterns | 1 | `control.rs:24` — `Signal::User1`/`User2` have no Windows arm |
| **total** | **52** | |

Per file:

| file | errors |
|---|---|
| `src/wire.rs` | 12 |
| `src/ffi.rs` | 11 |
| `src/engine/launch.rs` | 7 |
| `src/protocol/service/namespace.rs` | 6 |
| `src/checkpoint_stream.rs` | 3 |
| `src/projection.rs` | 3 |
| `src/configfile.rs` | 2 |
| `src/machine.rs` | 2 |
| `src/engine/validation.rs` | 2 |
| `src/engine.rs` | 2 |
| `src/control.rs` | 1 |
| `src/runtime/transport.rs` | 1 |

Two things this count does **not** include, and both matter for staging:

- **Link errors.** `cargo check` does not link, so `ffi.rs`'s four raw externs pass it and fail later (P3).
- **Test code.** `cargo check` does not build `#[cfg(test)]` modules or `tests/`. `wire.rs:645`,
  `projection.rs:191-253`, `runtime/transport.rs:268-410` and the whole of `tests/` are additional.

The last four rows of the error table (`E0425`, `E0308`, `E0004`, 4 errors in 3 files) are a distinct and
reassuring category: they are *existing two-arm `#[cfg]`s with no `else`*. The crate already forks on host
OS in three places and the compiler names all of them. That is the pattern §7 argues against extending.

---

## 4. The FFI boundary: does the C API have to change?

**Yes, but the surface is ~10 signatures in 2 headers, and it is much smaller than "the C-ABI descriptor
model".** Most of the C API is already Windows-clean, deliberately.

### 4.1 What needs nothing

**`include/hl/host_services.h` — no change.** This is 686 lines and 17 service groups, and it is
descriptor-free by construction. `hl_host_handle` is `uint64_t` (`:27`) with `HL_HOST_HANDLE_INVALID = 0`
(`:30`), and the header states the intent at three separate sites:

- `:179` — "the handle is the only token accepted by protect/release and **must not expose a native fd**"
- `:384` — "Apply guest ownership after creation **without exposing a native descriptor**"
- `:538` — "Host-owned message channels transfer object identity, **never native descriptor numbers**"

The one POSIX escape hatch, `hl_host_posix_attachment_services` (`:654`), is *optional*: it is gated on
`HL_HOST_CAP_POSIX_ATTACHMENT` (bit 17) and a Windows backend simply never advertises it. This layer was
designed for a non-POSIX host and it holds up. The engine core needs nothing from this work.

**`include/hl/engine.h` — no change.** `hl_engine_fd_binding` carries `hl_host_handle host_handle`
(`:50`), not an fd. `hl_engine_executable` likewise (`:57`). `hl_engine_config` has no descriptor fields.
`hl_engine_guest_fd_limit()` (`:19`) names descriptors but returns a *bound*, not one.

**`include/hl/config.h` — no change.** 111 lines, zero descriptor fields, and the header says why at `:13`:
"The channel itself is a descriptor activation hands the engine; nothing here names a location."

**`include/hl/checkpoint_stream.h` — no change to the wire format.** The request/reply structs are
fixed-width little-endian records with explicit `name_size`/`length` framing; the crate's Rust decoder
(`checkpoint_stream.rs:816-843`) reproduces them byte-for-byte. Nothing in the protocol is descriptor-typed.
Only the header's *topology* comment (`:12-21`) describes a mechanism that has no Windows analogue — see
§5.3.

### 4.2 What must change

Everything descriptor-typed that the crate touches lives in two places:

**`include/hl/activation.h`, 98 lines.** Six descriptor-typed parameters across the three functions the
crate actually calls:

| site | parameter |
|---|---|
| `activation.h:18-20` | `hl_activation_stdio { int32_t input, output, error }` |
| `activation.h:66` | `hl_activation_start_with_channels(..., int32_t transport, int32_t checkpoint, int32_t trigger, ...)` |
| `activation.h:67` | `..., int32_t *out_master, ...` |
| `activation.h:68` | `hl_terminal_resize(int32_t master, ...)` |

**`src/core/checkpoint_channel.h:49-55`**, the embedder half:
`hl_ckpt_broker_pair(int*, int*)`, `hl_ckpt_broker_accept(int, int, uint64_t*)`,
`hl_ckpt_trigger_create(int*, void**)`, `hl_ckpt_trigger_destroy(void*, int)`.

That is the whole cross-language surface. Everything else in the crate's `unsafe extern "C"` block
(`ffi.rs:62-112`) is already portable: opaque `*mut hl_activation_process`, `hl_process_domain`
(`uint64_t[2]`), `hl_activation_process_info`, `hl_engine_exit`, `hl_terminal_size`.

### 4.3 Is `int32_t` actually too narrow? — probably not, and this changes the size of the work

The sibling document says: "Windows `SOCKET` is `UINT_PTR` — 64-bit — so `i32` is not merely the wrong name
for it, it is the wrong width." The first half is right about the C *type*. The second half is very likely
wrong about the *values*, and the distinction matters because it decides whether the C ABI must change at
all for width reasons.

Windows kernel handle values are documented to be 32-bit significant — that is the contract that lets a
64-bit process pass a handle to a 32-bit one and back (MSDN, "Interprocess Communication Between 32-bit and
64-bit Applications"). Winsock `SOCKET`s are ordinary kernel handles here: P2 called `GetHandleInformation`
on one and it succeeded, returning flags. So `int32_t` would in practice *carry* both a `HANDLE` and a
`SOCKET`, sign-extended on the way back out.

*Unverified:* that this holds for every object class a Windows host backend ends up using. It is a
documented contract about handle values, not a type guarantee, and nothing in the toolchain enforces it.

Two things are wrong with `int32_t` regardless of width, and they are the real reasons to change it:

1. **The `-1` sentinel is overloaded on Windows.** `activation.h:14-17` says "-1 inherits the application's
   stream". On Windows `(HANDLE)-1` is both `INVALID_HANDLE_VALUE` **and** the pseudo-handle returned by
   `GetCurrentProcess()`. A parameter that means "not supplied" cannot also be a legal handle value.
   `HL_HOST_HANDLE_INVALID` is already `0`, and `0` is the correct "no object" value for nearly every Win32
   object class.
2. **Consistency with the layer directly below it.** `hl_host_handle` is `uint64_t` for precisely this
   reason, and `activation.h` is now the only header in `include/hl/` that still speaks in native fds.

**Recommendation.** Introduce `typedef uint64_t hl_activation_descriptor;` with
`HL_ACTIVATION_DESCRIPTOR_NONE = 0` and use it at all ten sites. On Unix it holds an `int` fd widened; on
Windows a `HANDLE` or `SOCKET`. This is a mechanical change, but it is an **unversioned ABI change to a
prebuilt binary**, and there is no `HL_ABI_HEADER` on `hl_activation_start_with_channels` the way there is
on `hl_launch_config`. What protects it is the mechanism `docs/windows/rust-crate.md` §2.3 describes: the
source manifest hashes `include/hl/*.h`, so touching `activation.h` invalidates every committed archive's
stamp and `check_crate_archives.sh` goes red until they are regenerated. That gate is adequate; rely on it
deliberately rather than by luck, and do not skip the stamp loop.

### 4.4 So: Rust-only task, or cross-language?

**Cross-language, but the two halves decouple cleanly and the crate goes first.**

The crate can reach *compiles and links* with no C change at all, by defining
`sys::RawDescriptor = c_int` on both platforms and truncating Windows handles into it. Everything in §3
and §6 is then addressable in Rust alone. What the crate *cannot* do alone is **run a guest**: that needs
the Windows host backend, the descriptor-type agreement of §4.3, and the topology decision of §5.3.

That is the useful scoping answer. The Rust work is not blocked on the C decision; only the last milestone
is.

---

## 5. AF_UNIX

### 5.1 What Windows actually has

Measured (P1, P2) rather than taken from documentation:

- `AF_UNIX` + `SOCK_STREAM` — **works**. bind, listen, connect, accept, send, recv all succeeded.
- `AF_UNIX` + `SOCK_DGRAM` — **`WSAEAFNOSUPPORT` (10047)**. Not "unimplemented"; the address family is
  rejected outright for this socket type.
- `AF_UNIX` + `SOCK_SEQPACKET` — **`WSAESOCKTNOSUPPORT` (10044)**.
- `SO_RCVTIMEO` works: a 200 ms timeout returned `WSAETIMEDOUT` after 205.9 ms.
- The `SOCKET` is a real kernel handle: `GetHandleInformation` succeeded and
  `SetHandleInformation(HANDLE_FLAG_INHERIT)` succeeded, so it participates in `CreateProcessW` handle
  inheritance and in `DuplicateHandle`.

Three further properties, all consequential:

- **There is no `socketpair()` in Winsock.** A pair is bind → listen → connect → accept over a **real
  filesystem path**. P2 confirmed a file appears at the path. There is no abstract namespace and no
  autobind, so the rendezvous is visible in the filesystem for the duration of the connect.
- **There is no ancillary data.** Windows AF_UNIX carries no `cmsghdr` and no `SCM_RIGHTS` equivalent. Handle
  transfer on Windows is `DuplicateHandle` with a handle to the *target process* — a different model, and
  one that runs in the opposite direction (see §5.3).
- **Winsock sockets are created inheritable.** P2 read back `flags = 0x1` (`HANDLE_FLAG_INHERIT`) on a
  socket nobody had marked. This inverts the Unix hygiene problem. `checkpoint_channel.c:176` carefully sets
  `FD_CLOEXEC` on both broker ends, with a comment recording that without it "the guest descriptor scan sees
  two anonymous sockets it cannot account for and refuses to checkpoint at all". On Windows the equivalent
  default leaks *every* socket in the process into any child created with `bInheritHandles = TRUE`. The
  Windows backend must either use `WSASocketW(..., WSA_FLAG_NO_HANDLE_INHERIT)`, or clear the flag
  explicitly, or — best — pass an explicit allow-list with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.

### 5.2 What `checkpoint_stream.rs` actually needs — much less than the API suggests

This is the finding that shrinks the problem, and it took reading the C side to see it.

The Rust crate **never sends or receives ancillary data, and never reads or writes a datagram.** Every
`SCM_RIGHTS` call in this codebase is in C (`src/host/fork_wire.c`, `src/host/{linux,macos}/host.c`,
`src/linux_abi/checkpoint.c`, `src/linux_abi/container/netns.c`). The two Rust-visible AF_UNIX roles are:

**The broker (`ffi.rs:276-286`).** `broker_pair()` calls `hl_ckpt_broker_pair`, which does
`socketpair(AF_UNIX, SOCK_DGRAM)` in C (`checkpoint_channel.c:171`), and wraps the parent fd as a
`UnixDatagram`. That `UnixDatagram` is then used for exactly one thing: `broker.as_raw_fd()` at
`ffi.rs:296`, handing the number straight back to `hl_ckpt_broker_accept`, which does the `poll` and the
`recvmsg` in C. **Rust never calls a single method on it.** It is an owning fd wrapper wearing a datagram
socket's type.

So all four `UnixDatagram` uses can become `OwnedFd` **today, on Unix, with zero behaviour change**. That is
a pure simplification available before any Windows work starts, and it deletes the entire "Rust needs
AF_UNIX datagrams" problem.

**The channel (`checkpoint_stream.rs:503`, `:881`; `ffi.rs:292-304`).** `hl_ckpt_broker_accept` returns a
descriptor that C received over `SCM_RIGHTS`; Rust wraps it as a `UnixStream` and uses only
`read_exact` / `write_all` / `flush`. Semantically it needs:

| property | needed? | why |
|---|---|---|
| reliable, ordered, bidirectional byte stream | **yes** | strict request/response, strictly serial (`checkpoint_stream.h:17-21`) |
| message boundaries | **no** | the protocol frames itself: 48-byte request header with explicit `name_size`+`length`, 32-byte reply header with explicit `length` |
| ancillary data / handle passing | **no** | never used from Rust |
| read/write timeouts | **no** | blocking; channel close is the normal end (`checkpoint_stream.rs:508`) |
| independent duplication | **no** | one thread per channel |

The third Rust AF_UNIX role is separate and has different requirements — **the provider transport**
(`runtime/transport.rs`, `engine/launch.rs:177`). It needs `pair()`, `try_clone()`,
`set_read_timeout`/`set_write_timeout`, and one end passed to the child. Also no ancillary data.

### 5.3 Recommendation

**For the two Rust-side byte channels: AF_UNIX `SOCK_STREAM`.** Not named pipes, not loopback TCP.

- It is the only option that preserves both the semantics *and* the security model. The channel grants
  provider authority and carries checkpoint image bytes; it must be scoped by filesystem permissions, not
  reachable from the network stack. Loopback TCP would give `std::net::TcpStream` for free — timeouts,
  `try_clone`, identical code on both platforms — and is rejected for exactly that reason: any local process
  can connect during the accept window, and the endpoint is visible in `netstat`.
- `SO_RCVTIMEO`/`SO_SNDTIMEO` give `runtime/transport.rs`'s timeouts directly (measured, P2). Named pipes do
  not: a synchronous pipe handle has no read timeout, and getting one means overlapped I/O plus
  `CancelIoEx`, or the deprecated `PIPE_NOWAIT`. That is real work for no benefit.
- The framing code, the protocol codec and the digest logic stay byte-identical across platforms. Only the
  constructor forks.

Cost, stated plainly: `std` does not expose AF_UNIX on Windows, so this is ~150 lines of raw `ws2_32` in
`sys/windows.rs` — `socket`/`bind`/`listen`/`connect`/`accept`/`send`/`recv`/`setsockopt`/`closesocket`
behind a `Read + Write` newtype over `OwnedSocket`. P2 is a working skeleton of it. Reach for named pipes
only if the filesystem-path rendezvous proves untenable; `\\.\pipe\` needs no path and is the fallback.

*Unverified:* whether a bound AF_UNIX socket file can be unlinked while in use on Windows (the Unix
bind-then-unlink trick). If not, the pair constructor must delete the path after `accept` and tolerate a
crash leaving a stale file — which the existing `hl-projection-*` / `hl-engine-config-*` temp files already
have to.

**For the broker topology: invert it. This is the change that has no cheap alternative.**

The Unix design is: each engine process creates its channel and *sends the descriptor up* to the embedder
over an inherited datagram socket. On Windows that direction is the hard one. `DuplicateHandle` requires the
sender to hold a handle to the *target* process with `PROCESS_DUP_HANDLE` rights. A grandchild engine
process — and `checkpoint_stream.rs:10-11` is explicit that every guest process is a further `fork()` — has
no handle to the embedder and cannot get one without the embedder's PID plus a right it must be granted.

The workable shape is to **make the broker a listener**. Instead of the child creating a channel and passing
the handle up, the child *connects* to an endpoint the embedder is already listening on, whose name travels
down in the launch config. Then no handle crosses a process boundary at all: the connection *is* the
channel, and `hl_ckpt_broker_accept` becomes a real `accept()`.

Two things make this a small change rather than a redesign:

- The protocol already tolerates it. `hl_ckpt_hello` carries `host_pid`, and `checkpoint_stream.h:74`
  already says it is "diagnostic only; **the server keys on the channel, not on this**". The server assigns
  its own id (`checkpoint_stream.rs:792`). Nothing depends on the descriptor having arrived by `SCM_RIGHTS`.
- The Rust change is confined to `ffi.rs`: `broker_pair()` becomes `broker_listen() -> (OwnedListener,
  CString)`, and the endpoint name goes into the launch config instead of a descriptor into activation.
  `checkpoint_stream.rs`'s 900 lines are untouched.

What it gives up, and this should be a deliberate decision rather than a discovery: the socketpair model has
**no rendezvous to hijack**. A filesystem endpoint does. The Unix backend should therefore keep
`socketpair` + `SCM_RIGHTS`; this is a per-host constructor, not a protocol change. Unifying them later
would be a simplification, but it would weaken Unix, and it should not be done as a side effect of the
Windows work.

**The trigger page needs no design work.** `hl_ckpt_trigger_create` (`checkpoint_channel.c:227-248`) is a
4-byte anonymous shared mapping — `memfd_create` on Linux, `shm_open`+`shm_unlink` on macOS. The Windows
form is `CreateFileMappingW(INVALID_HANDLE_VALUE, ..., 4, NULL)` + `MapViewOfFile`, which is a nameless
section object: closer to `memfd_create` than the macOS arm is. Same shape, same lifetime, and the section
handle is inheritable and duplicable. Only the descriptor type is at issue.

---

## 6. The hazard that is not a compile error

Three validations compile cleanly on Windows and return different answers. Measured (P5):

```
"/etc/passwd"   is_absolute=false  all-Normal=false  ["RootDir", Normal("etc"), Normal("passwd")]
"a\b"           is_absolute=false  all-Normal=true   [Normal("a"), Normal("b")]
"C:\windows"    is_absolute=true   all-Normal=false  [Prefix(Disk), RootDir, Normal("windows")]
```

**1. `wire.rs::file_owners` stops rejecting guest-absolute paths.** The guard is

```rust
if path.is_absolute() || bytes.is_empty() || ... { return Err(...) }
```

`Path::new("/etc/passwd").is_absolute()` is `false` on Windows — absoluteness there requires a *prefix*, and
a bare `RootDir` is not one. So `/etc/passwd` is accepted as a file-owners key on Windows and rejected on
Linux, and it is then written into the config pool as an absolute path where the engine expects a
normalized relative one. Silent, and no existing test can see it.

**2. The `Component::Normal` check changes what a backslash means.** `wire.rs` requires every component to
be `Normal`. On Linux `a\b` is one filename containing a backslash — one `Normal` component, accepted. On
Windows it is *two* components, also accepted. The check passes on both hosts and means two different
things, and the bytes handed to the guest differ in meaning from the bytes the caller wrote. The same
reasoning applies to `engine/validation/extension.rs`'s path checks.

**3. `projection.rs::host_path` produces mixed separators.** `Path::new("/etc/x").strip_prefix("/")` works
(the `RootDir` component strips), but `root.join("etc/x")` yields `C:\tmp\proj\etc/x`. Win32 accepts both
separators so it *functions*, and it will break the first string comparison anyone writes against it.

**Recommendation: stop using `std::path` for guest paths.** A guest path is a Linux path — always
`/`-separated, always an arbitrary byte string — and `Path` on Windows is a Win32 path parser. They are
different types that happen to share a name. Introduce an explicit `GuestPath` validator that checks
`/`-rooted-ness, rejects `\` and any drive or UNC prefix, and classifies `.`/`..` itself, and use it at
every site that today asks `Path` a question about a guest path. Do this **on Unix first**, where every
existing test can confirm the new rejections change nothing.

This is the item most likely to be got wrong by a mechanical port, because a mechanical port fixes what the
compiler complains about and this produces no complaint.

---

## 7. The OS-string problem is a policy question, not an API question

23 of the 52 errors are `as_bytes` / `from_vec` (§3). The reflex fix is two `#[cfg]` helpers. That is wrong,
and it is worth being precise about why.

These bytes go into `hl_launch_config`'s string pool and are consumed by the **Linux guest** as argv,
environment records, mount paths and file-owner keys. A Linux path is an arbitrary byte string. On Unix the
host `OsStr` is also an arbitrary byte string, so `as_bytes` is a lossless identity and the question never
arises. On Windows `OsStr` is WTF-16 — potentially *ill-formed* UTF-16 — and there is no total, lossless map
from WTF-16 to bytes that also agrees with the Unix behaviour on ASCII. The crate has to pick a policy.

**Recommendation: UTF-8, and reject what will not encode.**

- Encode with `OsStr::to_str()`, which is `Some` exactly when the WTF-16 is well-formed UTF-16, and return
  `Error::InvalidConfig` otherwise. This is total, lossless for every string a user can type, and identical
  to the Unix path for all valid UTF-8.
- Decode with `String::from_utf8` → `OsString`, erroring on invalid UTF-8.
- **Do not use `to_string_lossy()`.** It substitutes U+FFFD and hands the guest a path that is not the path
  the caller named. `docs/amd64-host.md` §7 has the phrase for this: it "would be a plausible-looking lie".
  A rejection is a known unknown; a substituted path is a bug in a guest nobody will trace back here.

This slots into the crate's existing habit — it already rejects embedded NUL (`wire.rs:4-13`) and newline
and tab in the same helpers — so the new failure mode is one the API already documents.

The decode direction needs a total answer for a different reason: `decode_namespace_install`
(`protocol/service/namespace.rs:83`) is on the *receive* path from the engine, not just a round-trip of the
crate's own output. "A projected path that is not valid UTF-8 is a protocol error" is the right rule there,
and it matches the surrounding code, which already returns `linux(22, ...)` for a malformed transaction.

---

## 8. Proposed module structure

The crate should not grow 52 `#[cfg]`s. It already has three two-arm `#[cfg]`s with no `else`
(`control.rs:30-36`, `machine.rs:385-399`, `ffi.rs:125-132`) and all three are in the error list — the
pattern does not scale to a third host and it fails loudly at exactly the wrong moment.

**Model on `std` itself.** `std` does not sprinkle `#[cfg(unix)]` through `std::fs`; it has one platform
module selected by a single `cfg_if!`, exporting an identical set of names, and everything above it names
only `crate::sys::X`. Do the same:

```
pkgs/rust/src/sys/mod.rs        -- the ONLY #[cfg(unix)] / #[cfg(windows)] in the crate
pkgs/rust/src/sys/unix.rs       -- today's code, moved verbatim
pkgs/rust/src/sys/windows.rs
pkgs/rust/src/sys/guest_path.rs -- shared; §6. Not a platform fork
pkgs/rust/src/sys/os_str.rs     -- policy, §7. One fork, two functions
```

The surface, derived from the 52 errors plus the four link failures — twelve items:

```rust
pub(crate) type  RawDescriptor;                                     // c_int | truncated HANDLE/SOCKET
pub(crate) struct OwnedDescriptor;                                  // closes on drop; .raw()
pub(crate) struct Stream;                                           // Read + Write + try_clone
                                                                    //   + set_read_timeout/set_write_timeout
pub(crate) fn stream_pair()        -> io::Result<(Stream, Stream)>; // socketpair | bind/listen/connect/accept
pub(crate) fn adopt_stream(RawDescriptor) -> Stream;                // the accepted checkpoint channel
pub(crate) fn pipe_pair()          -> io::Result<(File, File)>;     // pipe+FD_CLOEXEC | CreatePipe
pub(crate) fn adopt_file(RawDescriptor)   -> File;                  // the pty master
pub(crate) fn file_raw(&File)      -> RawDescriptor;
pub(crate) fn null_stdio(read: bool) -> io::Result<File>;           // /dev/null | NUL
pub(crate) fn secure_random(&mut [u8]) -> io::Result<()>;           // /dev/urandom | BCryptGenRandom
pub(crate) fn create_private_file(&Path) -> io::Result<File>;       // mode(0o600) | owner-only DACL
pub(crate) fn symlink(&Path, &Path) -> io::Result<()>;              // symlink | symlink_file/symlink_dir
pub(crate) fn set_mode(&Path, u32) -> io::Result<()>;               // PermissionsExt | read-only bit, or no-op
pub(crate) fn is_socket(&Metadata) -> bool;                         // FileTypeExt | false
pub(crate) fn signal_process(pid: u64, sig: i32) -> io::Result<()>; // kill | see below
pub(crate) mod signal { HANGUP INTERRUPT QUIT KILL TERMINATE USER1 USER2 STOP CONTINUE INTERRUPT_ENGINE }
```

**What is genuinely shared: essentially everything.** Of 11,977 lines, the platform fork is these twelve
functions plus the two policy modules — call it 250 lines, and the Windows half is dominated by the ~150
lines of `ws2_32` from §5.3. Unchanged and untouched: all of `api/`, `spec/`, `provider/`, `service/`,
`protocol/` (bar the codec's string calls), `config.rs`, `container.rs`, `command.rs`, `result.rs`,
`error.rs`, `engine/lowering.rs`, `engine/validation*` (bar `is_socket`), `wire.rs`'s entire offset table and
layout logic, `runtime/transport.rs`'s framing and handshake, and all 900 lines of
`checkpoint_stream.rs`'s state machine, staging, claim arbitration and digest. The protocol logic never had
a platform dependency; it was only ever *reached through* one.

Three items in that list deserve a note rather than an implementation:

- **`signal_process`.** `child.rs:74` sends a numbered signal to a host pid, and `machine.rs:123-128` fans
  `interrupt_signal()` across a process domain. There is no Windows `kill(2)`. The honest arm is not an
  emulation — it is to route these through the C engine, which already owns `hl_activation_kill` and
  `hl_engine_request(HL_ENGINE_REQUEST_SIGNAL)` (`engine.h:31-35`). **`ffi.rs`'s `kill` extern should be
  deleted on every platform**, not conditionally compiled: sending a raw host signal from Rust bypasses an
  abstraction the C API already provides. That is a portability fix that improves the Unix build too.
- **`set_mode` / `create_private_file`.** `configfile.rs:24`'s `mode(0o600)` and `projection.rs:110,124`'s
  `from_mode` are not decoration. The config file holds the launch configuration; the projection holds the
  guest's namespace contents. The Windows arm must be a real owner-only DACL, not a no-op and not the
  read-only attribute. This is the one item on the list where a lazy implementation is a security
  regression rather than a functionality gap.
- **`symlink`.** `projection.rs:128`. `symlink_file`/`symlink_dir` on Windows need either Developer Mode
  (`SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`) or `SeCreateSymbolicLinkPrivilege`. Under "compiles like
  any other rust crate", a projection containing a symlink must fail with a *diagnosable* error naming
  Developer Mode — not an opaque `ERROR_PRIVILEGE_NOT_HELD`.

---

## 9. Tests

Measured across `pkgs/rust/tests/`: 104 `#[test]` functions in 15 files.

| file | tests | guest-launch sites | Unix-API hits | verdict |
|---|---|---|---|---|
| `spec.rs` | 46 | 48 | 44 (8× `UnixListener`, 4× `symlink`) | needs a working engine **and** a Unix port |
| `alpine.rs` | 13 | 18 | 6 | needs the engine + `assets/alpine/` (excluded from the published crate) |
| `e2e.rs` | 7 | 17 | 0 | needs a working engine |
| `control.rs` | 6 | 10 | 0 | needs a working engine |
| `terminal.rs` | 5 | 5 | 10 | needs the engine; `open_files()` (`:31-37`) is a host `#[cfg]` with no Windows arm |
| `checkpoint_store.rs` | 3 | 7 | 0 | needs a working engine |
| `checkpoint_terminal.rs` | 3 | 4 | 2 | needs a working engine |
| `network.rs` | 3 | 3 | 0 | needs the engine; `/tmp/.hl-bridge-*` is a host path (`network.rs:49,199`) |
| `container.rs` | 1 | 1 | 0 | needs a working engine |
| `packaged_archive.rs` | 2 | 1 | 0 | **split** — see below |
| `policy.rs` | 9 | 0 | 1 (a `/tmp` string literal in a config value) | **portable now** |
| `providers.rs` | 2 | 0 | 0 | **portable now** |
| `contracts.rs` | 2 | 0 | 0 | **portable now** |
| `api.rs` | 1 | 0 | 0 | **portable now** |
| `traits.rs` | 1 | 0 | 0 | **portable now** |

**15 tests can pass on Windows before any host backend exists** — `policy`, `providers`, `contracts`,
`api`, `traits`. They validate specification lowering, provider contracts and re-export surface, and they
never enter the engine. They do have to *link*, so they need an archive; a stub suffices.

**`packaged_archive.rs` splits, and the split is worth taking deliberately.** Its two tests are usually
described as one thing and are not:

- `crate_launch_abi_matches_the_c_header` reads `include/hl/config.h` and compares
  `HL_CONFIG_ABI` to `hl_engine::launch_abi()`. It launches nothing and uses no Unix API. **It can pass on
  Windows from the first milestone**, and it is the cheap half of the staleness gate.
- `committed_archive_launches_a_guest_on_both_backends` launches `testdata/exit42-{aarch64,x86_64}` and is
  the only check that can see an ABI mismatch, because that is a runtime rejection. It cannot pass until a
  Windows engine exists. Its two fixtures are ELF binaries and stay ELF: the *guest* is Linux on every host.

Two general notes:

- The four Unix-heavy test files are excluded from the published crate already (`Cargo.toml`'s `include`
  omits `tests/`, `testdata/`, `assets/alpine/`), so this is a repository concern, not a consumer one.
  `#![cfg(unix)]` at the top of `spec.rs`, `alpine.rs`, `terminal.rs` and `checkpoint_terminal.rs` is the
  honest interim state — but note that `spec.rs` is 46 of the 104 tests, so `#![cfg(unix)]` there is a large
  coverage cliff that must be recorded as owed work, not treated as done.
- `tests/support/checkpoint_env.rs` is worth keeping in mind as a model: it decides at run time, by
  performing a real capture, whether the checkpoint class can run *here*, and self-skips with a visible
  reason otherwise. That pattern — probe, don't guess, and make the skip loud — is exactly right for the
  Windows lane and should be reused rather than reinvented as a `--skip` list.

---

## 10. Staging

Ordered so that Linux and macOS compile and pass at every step. Each milestone is independently
reviewable and independently revertible.

**M0 — delete work rather than port it.** No `#[cfg]`, no new files, no behaviour change on any host.

1. `ffi.rs`: `broker_pair()` returns `OwnedFd`, not `UnixDatagram` (§5.2). Deletes all four
   `UnixDatagram` uses.
2. `ffi.rs`: delete the `kill` extern and route `child.rs:74` / `machine.rs:123-128` through the C API
   (§8).
3. `ffi.rs`: delete the `close` extern in favour of `OwnedFd`'s own drop.

Reviewable as "did any behaviour move" and the answer should be no. This alone removes 5 of `ffi.rs`'s 11
errors and one of the four link failures.

**M1 — introduce `src/sys/`, Unix only.** `#![cfg(unix)]` on the module; today's code moved in verbatim;
every call site rewritten to `crate::sys::…`. Zero semantic change, entirely mechanical, and it is where the
*shape* lands with no Windows risk. `cargo test` on Linux/macOS must be unchanged, including the ignored set.

**M2 — the two policy modules, on Unix, where they are testable.**

- `sys::os_str` (§7) — `Result`-returning encode/decode; on Unix infallible in practice.
- `sys::guest_path` (§6) — explicit `/`-rooted validation replacing `Path::is_absolute` and
  `Component::Normal` at the guest-path sites in `wire.rs`, `projection.rs` and
  `engine/validation/extension.rs`.

Doing this on Unix first is the point: the new rejections can be shown to change no existing test before
the platform where they matter exists. This is the milestone that repays the most, because §6 is the class
of defect the compiler will never mention.

**M3 — `sys/windows.rs`, plus the `lib.rs` cfg arm and the `build.rs` row.**
`cargo check --target x86_64-pc-windows-msvc` is green.

**`cargo build --target x86_64-pc-windows-msvc` also succeeds at M3, with no archive in the tree** —
measured (P6). A library crate with no link directives produces its rlib without resolving anything, and
`build.rs`'s archive-absent branch emits no directives. That branch must therefore cover the Windows row,
or M3 regresses to "fails until an archive exists". This is the answer to "which milestone first yields a
successful build": **M3, and it needs neither a Windows host backend nor a Windows archive.**

**M4 — a stub archive.** `hl_engine_*` / `hl_activation_*` / `hl_ckpt_*` returning
`HL_STATUS_UNSUPPORTED`, `/MD`, built by the Windows lane. Test binaries link.
`cargo test --test contracts --test policy --test traits --test api --test providers` is green — 15 tests —
plus `packaged_archive::crate_launch_abi_matches_the_c_header`. `rust.fmt` and `rust.clippy` are green.
This is the first milestone that vouches for anything on Windows.

**M5 — the C-side agreement (§4.3, §5.3).** `hl_activation_descriptor`, the `0` sentinel, and the
broker-as-listener topology. Needs the host-backend owner. Regenerates every committed archive, which the
source-manifest stamp already forces.

**M6 — the real archive and `packaged_archive::committed_archive_launches_a_guest_on_both_backends` on
Windows.** Only here does the crate genuinely work, and only here should Windows enter
`HL_CI_COMPAT_HOSTS`. `docs/windows/rust-crate.md` §7 records the trap: declaring the token alone turns I20
off, so the shards must land in the same commit.

M0–M4 are Rust-only and unblocked today. M5 is the coupling.

---

## 11. Open questions

Ranked. The first three can each change the plan.

1. **Does `static:+whole-archive` work at the declared MSRV, `rust-version = "1.81"`?** P4 proves it at
   1.97.1, including that the `.CRT$XCU` constructor survives and that its absence is silent. The MSRV is a
   different question and one `rustup toolchain install 1.81` answers.
2. **Is `int32_t` genuinely sufficient to carry every handle class the Windows backend will use** (§4.3)?
   If yes, the C ABI change is a sentinel and a rename rather than a width change, and M5 shrinks. If no,
   M5 is mandatory before anything runs.
3. **Can the broker become a listener** (§5.3), and does the host-backend owner agree? This is the one
   decision that cannot be made from the crate side, and both sides are blocked on it.
4. **Can a bound AF_UNIX socket file be unlinked while in use on Windows?** Decides whether the pair
   constructor leaves stale files behind after a crash.
5. **What does `set_mode` mean on Windows** (§8)? A real owner-only DACL is the answer; the question is who
   writes it, since a wrong answer here is a security regression rather than a missing feature.
6. **What is the coverage cost of `#![cfg(unix)]` on `spec.rs`?** 46 of 104 tests. Enumerating which of
   them are Unix by *subject* and which merely by *mechanism* would likely recover a substantial fraction,
   and that audit was not done here.
7. **Does `network.rs`'s `/tmp/.hl-bridge-*` rendezvous have a Windows form at all**, or is virtual
   networking simply unsupported on a Windows host for now? The latter is a legitimate answer; it just has
   to be a stated one, surfaced through `EngineCapabilities` rather than discovered at run time.
