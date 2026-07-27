# The Windows host

The record for bringing HL Engine to a native Windows host. `docs/amd64-host.md` is the model for
this work and its §2.1 is the reason it is tractable: "host platform" is two axes, and they are added
independently. Windows is the **host-OS** axis. The host-CPU axis (x86-64) was solved on
`feat/amd64-linux-host`, which this branch is based on — without it there would be no way to execute
either guest ISA here, because both production frontends emit ARM64 directly.

**Status: a Linux guest runs on Windows, and 769 of 1,471 active x86-64 compatibility cases pass
(52.3%), golden-compared.** No `excluded-windows` disposition has been used to reach that number —
every remaining failure is a named defect, not a hidden one.

```
$ hl-engine-windows-x86_64.exe build-guest-fixtures/compat/isa/x86_64/hello_x86 | od -c
0000000   h   i  \n        rc=42      (nolibc static)
$ hl-engine-windows-x86_64.exe build-guest-fixtures/compat/isa/x86_64/gw
glibc-min ok               rc=0       (dynamically linked glibc)
```

| | |
| --- | --- |
| Archives, runner, production engine | build and link |
| Host services | `memory` `clock` `log` `sync` `file` `process` complete; `terminal` added, Windows arm pending |
| Guest fixtures | 3,233 cross-built from WSL, digest-gated, reproducible |
| CI | `Windows-x86_64` declared, `ctest -L unit` 6/6, `gate.windows-imports` live |
| Rust crate | checks clean on all three targets; no Windows archive yet, so link-dependent tests are gated |
| Linux and macOS | unregressed throughout — flag order byte-identical, compat suites 100% both columns |

Largest remaining blocks, by cases unblocked: IPC 117 · sockets 76 · filesystem 68 · memfd and
file-backed mmap 59 · guest `fork()` 56 · epoll/inotify/timerfd/pidfd wiring 51 · PTY and termios ~15.

## The documents

These are working material for coordinating the port, not shipped documentation, and **code must
never reference them**. Several record measurements that later work corrected; where they disagree
with a commit message, the commit is newer.

| Document | Owns |
| --- | --- |
| [`toolchain.md`](toolchain.md) | The verified build environment and the viability gate. **Measured.** |
| [`linux-regression.md`](linux-regression.md) | How "Linux is unregressed" is proved, and the script that proves it |
| [`build-system.md`](build-system.md) | CMake, milestones M0–M8, what compiles when |
| [`host-services-map.md`](host-services-map.md) | Every callback of all 15 groups → Win32/NT |
| [`fork-model.md`](fork-model.md) | The process model. **Measured** NT primitives |
| [`experiment-rtlclone.md`](experiment-rtlclone.md) | `RtlCloneUserProcess` against the engine's real process shape |
| [`signals-and-faults.md`](signals-and-faults.md) | POSIX signals → VEH; all four fault mechanisms measured |
| [`linux-abi-fd-lane.md`](linux-abi-fd-lane.md) | The boot blocker, and why it was a gate rather than a dependency |
| [`surface3-plan.md`](surface3-plan.md) | The ambient-fd surface: 127 fd-indexed tables, not a call count |
| [`testing-and-ci.md`](testing-and-ci.md) | Lane staging, and the three ways this matrix could report green |
| [`rust-crate.md`](rust-crate.md) | Crate build model, the MSVC-vs-GNU ABI question |
| [`rust-unix-port.md`](rust-unix-port.md) | The crate's Unix-only source dependencies |
| [`prior-art-cygwin-fork.md`](prior-art-cygwin-fork.md) | How Cygwin actually forks |
| [`prior-art-cygwin-threads-signals.md`](prior-art-cygwin-threads-signals.md) | Cygwin threads, signals, `select` |
| [`prior-art-flinux.md`](prior-art-flinux.md) | flinux's fork, and seven facts the fork model had not considered |
| [`prior-art-flinux-memory.md`](prior-art-flinux-memory.md) | Their 64-KiB block design, and why placeholders beat it |
| [`prior-art-flinux-fs.md`](prior-art-flinux-fs.md) | Their fd table over HANDLEs; the path traps, measured |
| [`prior-art-flinux-dbt.md`](prior-art-flinux-dbt.md) | Their translator exists for one segment register |
| [`flinux-tty.md`](flinux-tty.md) | The tty block: VT flags delete their emulator, ConPTY is the wrong tool |
| [`flinux-sockets.md`](flinux-sockets.md) | Sockets over Winsock; identical constants are the danger |
| [`flinux-syscalls.md`](flinux-syscalls.md) | The syscall residue, and the shortest path to a running guest |
| [`prior-art-survey.md`](prior-art-survey.md) | WSL1, MSYS2, Interix, PostgreSQL, JIT-on-Windows, containers |

**flinux is GPLv3 and this engine is MIT.** Those documents extract techniques and measurements;
no flinux code has been copied, and none may be.

## 1. What is settled

**The toolchain is mingw-w64 clang, and it is verified rather than assumed.**
`tools/windows/toolchain_probe.c` compiles the three GNU constructs the tree cannot give up
(`__attribute__((constructor))` ×11, file-scope `__asm__` ×3, `__uint128_t` ×5 files) under the engine's
own warning set, and proves the two Win32 mechanisms the port lives on: VEH fault-and-resume, and W^X
dual-alias JIT mapping via `VirtualAlloc2` placeholders + `MapViewOfFile3`. MSVC's `cl.exe` is excluded
— it has no x64 inline asm and no `__int128`.

**The JIT memory model needs no new concept.** The dual-alias mapping the probe demonstrates is the
same shape the host contract was already designed around for macOS `MAP_JIT`: `hl_host_code_mapping`
already carries both `writable_address` and `executable_address`, and `HL_HOST_CODE_DUAL_ALIAS` already
exists. `begin_code_write`/`end_code_write` may legitimately no-op, which the contract permits in so
many words.

**Signal-context extraction is cheaper than feared.** The Windows cells of `src/host/native_context.h`
can `typedef CONTEXT ucontext_t`, so all 18 existing cast sites compile unchanged; x64 `CONTEXT`
declares `Rax…R15,Rip` contiguously *in encoding order*, so the Linux `gregs`-plus-index idiom survives
with natural indices. `EXCEPTION_CONTINUE_EXECUTION` with a mutated `ContextRecord` is semantically
identical to POSIX returning from a handler with a mutated `ucontext_t`, and simpler: with no signal
mask, `interp_restore_handler_mask`'s hand-rolled `rt_sigreturn` debt disappears.

## 2. The reframing that made this tractable

"Fork" names three unrelated things in this tree, and conflating them is what made the port look
impossible. `fork-model.md` separates them:

| Use | Sites | Needs a real clone? |
| --- | --- | --- |
| Engine launch — `spawn_cloned` / `spawn_prepared` | `lifecycle.c:133`, `linux_abi.c:606` | **No.** The child cold-loads and inherits nothing warm; `hl_linux_abi_fork_prepare` already transfers descriptors explicitly rather than relying on inheritance. `CreateProcess` + `DuplicateHandle` serves it. |
| The `--server` forkserver | launch-latency only | **No.** No correctness content; a zygote pool serves it. |
| Guest `fork()` — `proc.c:1823` | the Linux ABI itself | **Yes.** |

Only the third is hard. `RtlCloneUserProcess` is the recommendation there, measured on a Windows 11
26200 box at 2.9–3.5 ms from a ten-thread parent; checkpoint/restore into a freshly spawned engine is
the fallback, and is credible because the engine already owns a complete, 127/127-tested guest
serializer.

A zygote does **not** rescue guest fork, and it was worth asking: the fork point is arbitrary guest
state, and the parent is *known* not to be quiescent — `cache.c:1697-1705` names the go/npm/cargo hang
caused by a peer holding `g_jit_lock` at the fork instant.

Deferring guest fork costs **245 of ~1600 compat cases (15.3%)**, with `process` gutted at 62/80. There
is no cheap subset: glibc's plain `fork()` carries no `CLONE_VM` signal, so an exec-only fork recovers
1–2%, not 15%.

## 3. The blocker that is not about Windows at all

`hl_host_posix_attachment_services` is an explicitly *optional* POSIX adapter that hands out a raw
native descriptor. Windows has nothing to hand out. Omitting the capability is legal — validation's
clause is inert when the bit is clear, and the fake backend already ships that way — but
`root_handle_bind` (`src/linux_abi/container/vfs.c`) hard-fails on the NULL pointer and
`src/core/target/x86_64.c:801/812` turns that into unconditional init failure **for bare guests too**.

**The scoping pass corrected two things about this, in opposite directions.**

It is *smaller* than first reported. `root_handle_bind` turns out to be a **gate, not a functional
dependency**: `jail_routed_at` (`fs.c:90-97`) returns `g_rootfs ? 1 : jail_match(...) >= 0`, so a bare
guest with no volumes never enters the jail lane at all, and the ELF load already goes typed
(`x86.c:70` → `image.c:43`). The entire boot blocker is **one call, `vfs.c:1475`, and about 15 lines**.
A *statically linked* bare guest then needs nothing further in this lane, because it opens no path.

The "61 sites" figure was also wrong — it counted identifier occurrences, which here are dominated by
comments and `switch` labels. `fs.c` *mentions* `openat` 86 times and *calls* it 3 times; all 21 hits in
`nonpie_args.h` are `case 56: // openat(...)` labels. Comment-stripped and `(`-anchored the real figure
is **66 `*at`-family call sites**, of which 50 need nothing new and 13 need one of four appended
callbacks.

But it is *larger* somewhere else, and this is the finding that matters for scheduling. The `*at` family
is the **middle** of three surfaces. Surface 3 is ambient POSIX on descriptors, where **a guest fd number
literally is a host fd number** (`ATFD` at `dispatch.c:32`, `close(cf)` at `fs.c:3588`). That, not
`posix_attachment`, is the real Windows schedule driver.

A full sweep ([`prior-art-flinux-fs.md`](prior-art-flinux-fs.md)) found 1,289 sites across 52 distinct
POSIX functions — but they are **three populations with three different answers**, and the first
estimate of "~900 to port" was wrong because it did not separate them:

| Population | Size | Where | Disposition |
| --- | --- | --- | --- |
| **A** — genuine guest fds | **~441** | `syscall/{fs,io,net,event,binding}.c` | Route through `hl_linux_abi`. The API already exists, and `bound_shadow_reserve`'s shadow descriptor disappears with it |
| **B** — engine-internal | ~351 | `checkpoint.c` alone is 276, incl. `SCM_RIGHTS` capture | A **host-private** table inside `src/host/windows/`, never exposed above the seam |
| **C** — Linux-only features | ~319 | `container/{netns,vfs}.c` | Do not compile |

So the real figure is **~441 sites, roughly half of them one-line `close`/`fcntl` substitutions**.

The recommendation is to route them (option b), **not** to write a Windows fd-emulation shim (option a).
The CRT already *is* such a shim and it was measured insufficient: `_fstat64` returns `st_ino = 0,
st_dev = 0`, and a Winsock `SOCKET` wrapped by `_open_osfhandle` cannot be `_write`n — so files and
sockets can never share one namespace. A hand-written shim would be a second descriptor table beside
`hl_linux_abi`: the same dual-lane trap `jail_open_plan` already demonstrates, exercised in production
only on the host with no reference implementation to differ against.

[`linux-abi-fd-lane.md`](linux-abi-fd-lane.md) has the four-milestone breakdown and the seven-step
ordering, each step green on its own.

## 4. Where the tests can silently lie

`testing-and-ci.md` found three mechanisms that would let Windows *appear* green without running
anything, which matters more than usual because the goal is a fully passing matrix:

1. **`tools/matrix_runner.c:375-386` defaults any non-ELF magic to Mach-O.** A PE engine therefore
   silently inherits all ~60 `excluded-macos` rows — skipping them while reporting success. Manifest
   column 12 also holds a single token, so "excluded on both macOS and Windows" is currently unspellable
   without widening the format.
2. **The stall detector treats an unanswerable host as progress** (`docs/ci-green.md`). Unported, it goes
   *inert* rather than failing, reopening the five-hour-hang hole it was written to close.
3. **The host token can be declared with nothing checking it.** I20 hardcodes the literal `Linux-x86_64`
   (`check_ci_workflows.sh:346`), and `gate.ci-lane-parity` is registered only under `HL_HAVE_GUEST_CC`
   (`LaneParity.cmake:10-12`), so neither guard exists on a corpus-less Windows runner.

All three must be closed **before** the token is declared, not after.

## 5. Cost

| Piece | Estimate |
| --- | --- |
| Host services, phase 1 | ~7,700 lines |
| + `directory` and `watch` | ~8,500 |
| `linux_abi` fd-lane routing | not yet counted; see its document |
| Rust crate Unix port | not yet counted |

Several groups come out *simpler* than their Linux counterparts: `file.append` is a native atomic
`FILE_APPEND_DATA` and retires the `/proc/self/fd` re-open hack, `event.wake` is one
`PostQueuedCompletionStatus`, `counter.unsubscribe` maps exactly onto `UnregisterWaitEx`, and the whole
`SIGPIPE` bracket in `stream.write` disappears.

## 6. What is still unmeasured

Named here so it is not mistaken for settled:

- Whether the `MAP_SHARED` arenas survive `RtlCloneUserProcess` as genuinely *shared*. Failure is silent:
  lost futex wakes, diverging eventfd counters.
- Thread creation in a clone. `CreateThread` faulted `0xC0000005` 4/4; `NtCreateThreadEx` with
  `SKIP_THREAD_ATTACH` worked, but that skips TLS callbacks and the engine's `__thread` use is structural.
- Whether a VEH handler can meet the async-safety contract `repair_signal_page` demands.
- Whether rustc honours `+whole-archive` on `windows-msvc`, and whether clang's `constructor` lowers to
  `.CRT$XCU` and survives `/WHOLEARCHIVE:`. Both fail **silently** — the engine would link but register
  no backend.
- Whether Windows faults past EOF (reasoned, not measured; the BUS-ledger design depends on it).
- Guest fixture supply: ~3,200 cross-built static-glibc ELFs with no nix and no cross-glibc on Windows.
