# Windows prior art: everything except Cygwin

The survey that says which of the two hard problems — a real `fork()`, and JIT mapping plus
hardware-fault interception — are solved elsewhere, which are solved *differently* elsewhere, and
which have no prior art at all.

Cygwin itself is covered by `docs/windows/prior-art-cygwin-threads-signals.md`, and the design that
comes out of all of this is `docs/windows/fork-model.md` and `docs/windows/signals-and-faults.md`.
This document deliberately does not re-derive any of them; it covers the wider field so we do not
reinvent a solved problem or repeat a known failure.

`docs/windows/toolchain.md` is the environment record. Where this document contradicts folklore it
says so, and where it could not verify something it says that instead of guessing.

---

## 1. Method, and how to read the claims

Three grades of evidence appear below, and they are never blurred:

| Grade | Meaning |
|---|---|
| **Measured** | Run on this development host (Windows 11 Pro 10.0.26200, x86-64) by a probe written for this survey. The probes are in the session scratchpad, not the repo. |
| **Read** | The claim was checked against source that was cloned and opened — file and function named. |
| **Cited** | A primary source (vendor documentation, a talk, a maintainer's post) says it and no code was read. |
| **Unverified** | Stated because it is load-bearing and widely repeated, but *not* confirmed. Marked inline every time. |

Nothing here is estimated. Where a number would have been useful and does not exist, it says the
number does not exist.

---

## 2. What this host actually does

This is the part of the survey with no prior art to cite, because nobody publishes it. Every number
was produced by a probe on this box. Two of these results contradict advice that is repeated widely
enough to be worth stating flatly.

### 2.1 Virtual memory shape

| Property | Value | Consequence |
|---|---|---|
| `dwPageSize` | 4096 | Guest `PROT_*` granularity is fine. |
| `dwAllocationGranularity` | **65536** | *Reservations and section views are 64 KiB-granular.* |
| `lpMaximumApplicationAddress` | `0x7ffffffeffff` (128 TiB − 64 KiB) | Room for an identity-mapped guest space. |
| Contiguous `MEM_RESERVE` | 4 TiB succeeded in one call | Reserving the whole guest space up front is viable. |
| Commit granularity inside a reservation | 4096 | Commit and `VirtualProtect` are per-page even though reserve is not. |

**The 64 KiB allocation granularity is the single most under-appreciated porting constraint, and it
fails silently.** `VirtualAlloc(MEM_RESERVE)` at a 4 KiB-aligned but not 64 KiB-aligned address does
not fail — it **rounds the base down** to the enclosing 64 KiB boundary and reports success:

```
VirtualAlloc(RESERVE) want=0x300000001000 -> 0x300000000000   ROUNDED DOWN
VirtualAlloc(RESERVE) want=0x300000101000 -> 0x300000100000   ROUNDED DOWN
VirtualAlloc(RESERVE) want=0x300000201000 -> 0x300000200000   ROUNDED DOWN
```

A Linux guest issuing `mmap(MAP_FIXED, addr)` at a 4 KiB-aligned address is entitled to get that
address or `MAP_FIXED` semantics. Passing the request through to `VirtualAlloc` returns a *different*
mapping and a success code, and the guest will never learn. The engine must reserve 64 KiB-granular
and manage 4 KiB sub-allocation itself — which is exactly what flinux does (§3.7) with a 64 KiB block
per NT section object, and what NT had to be *modified* to relax for WSL1 (§3.3).

`MEM_COMMIT` and `VirtualProtect` inside a reservation are 4 KiB-granular, so once the reservation
layer is right, guest page protections map cleanly.

### 2.2 W^X JIT memory — all three patterns work

| Pattern | Result |
|---|---|
| `VirtualAlloc(PAGE_EXECUTE_READWRITE)` (flat RWX) | Works. |
| `CreateFileMapping(INVALID_HANDLE_VALUE, PAGE_EXECUTE_READWRITE)` + two `MapViewOfFile` views (`FILE_MAP_WRITE`, `FILE_MAP_READ\|FILE_MAP_EXECUTE`) | **Works.** Views report `READWRITE` and `EXECUTE_READ`. Write through the RW alias, execute at the RX alias — value observed. |
| `VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)` + `VirtualFree(MEM_PRESERVE_PLACEHOLDER)` split + two `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` | **Works, at addresses we choose, with a constant alias delta.** |

The dual-alias arena the engine already uses on Linux and macOS is therefore available on Windows
essentially unchanged. Aliases are **coherent**: rewriting through the RW alias while the RX alias is
live and mapped changed what executed, with no unmap/remap and no protection flip.

The placeholder result matters more than it looks. Plain `MapViewOfFileEx` at a chosen address
**cannot** map over a live reservation — it fails `487 ERROR_INVALID_ADDRESS` — so the classic
"reserve the range, free it, map it" dance has a window in which another thread can steal the hole.
That window is precisely the bug PostgreSQL papers over with a 100-iteration retry loop (§3.8). The
placeholder API closes it: the reservation is *replaced* atomically, never released. Any fixed-address
mapping we do — JIT alias, guest image, a re-attached shared segment in a child — should use
placeholders, not reserve-and-free.

### 2.3 Costs

| Operation | Cost |
|---|---|
| Plain store via the RW alias (baseline) | 0.0003 µs |
| `FlushInstructionCache` (64 KiB) | **0.004 µs** |
| `VirtualProtect` RW→RX→RW, 4 KiB | 1.85 µs |
| `VirtualProtect` RW→RX→RW, 64 KiB | 2.14 µs |
| VEH write-fault + `VirtualProtect` + resume | **2.85 µs** |
| `CreateProcess` + `WaitForSingleObject`, trivial PE child | **5.05 ms** |
| `CreateProcess` only, no wait | 1.54 ms |

Read these together and the design falls out:

- **`FlushInstructionCache` is free on x86-64 and can be called unconditionally.** It measured 4 ns
  for 64 KiB, which is a function-call floor, not cache work — and code rewritten through the RW alias
  executed correctly *without* it. Keep calling it (it is the documented contract, and an ARM64
  Windows host would need it) but never design around its cost.
- **A protection flip costs ~600× a store, and a fault costs ~1000×.** Dual mapping is not a
  micro-optimisation over `VirtualProtect` flipping; it removes a microsecond-scale operation from
  the translation path entirely.
- **`CreateProcess` of a *trivial* PE costs 5 ms round-trip.** The engine's fork-server exists because
  macOS `posix_spawn` + dyld cost ~2 ms of a ~3–5 ms launch (`src/linux_abi/fork.c`). Windows starts
  worse, before the engine binary, its DLL set, or engine init are counted. **Every fork strategy that
  spells fork as `CreateProcess` pays at least 1.5 ms of unavoidable process-creation latency**, and a
  guest that forks in a loop will be unusable. This number, not any API detail, is the strongest
  argument for keeping the fork-server/zygote shape.

### 2.4 Mitigations actually in force

| Policy | This host, default process |
|---|---|
| `ProcessControlFlowGuardPolicy` (7) | `0x0` — **CFG off** |
| `ProcessDynamicCodePolicy` / ACG (8) | `0x0` — **ACG off** |
| `ProcessUserShadowStackPolicy` (17) | query failed, `ERROR_NOT_SUPPORTED` (50) |

A default, non-opted-in process has neither CFG nor ACG. `SetProcessValidCallTargets` is therefore
not something the JIT must call. This is only true *because* nothing opted in — see §4.4 for what
would change it.

### 2.5 The mingw-w64 clang facts

Toolchain: clang 22.1.8, target `x86_64-w64-windows-gnu`, from `C:\msys64\clang64`.

| Question | Answer |
|---|---|
| `__try` / `__except` | **Not supported.** `error: use of undeclared identifier '__try'` |
| `__try` with `-fms-extensions` | **Compiles — and segfaults at run time.** |
| `.pdata` emitted for ordinary C | Yes, by default |
| `IMAGE_DLLCHARACTERISTICS_GUARD_CF` in the PE | **Absent** — clang's `-cfguard` is an MSVC-driver flag; the GNU driver rejects it |
| Default DLL characteristics | `DYNAMIC_BASE`, `HIGH_ENTROPY_VA`, `NX_COMPAT` |
| ASLR defeatable at link time | Yes — `-Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va` pins the image at `0x140000000` across runs |
| VEH + `ContextRecord->Rip` rewrite + `EXCEPTION_CONTINUE_EXECUTION` | Works |
| `ExceptionInformation[0]` / `[1]` on an access violation | `1` for a write, and the **exact** faulting address |

**`-fms-extensions` making `__try` compile and then crash is the trap in this list.** It is not a
diagnostic failure we would catch in review; it is a silently broken exception path. SEH is not
available to us: **VEH is the only fault-interception mechanism this toolchain actually has.**

The ASLR result is the useful lever. Our own image base can be pinned. `ntdll` cannot — but it does
not need to be: its base is randomised **per boot, not per process**, so it was identical in parent
and child in every run. The Cygwin fork failure mode ("unable to remap $dll to same address as
parent", [cygwin-developers, Aug 2011](https://cygwin.com/pipermail/cygwin-developers/2011-August/010567.html))
is driven by *third-party* DLLs and by heap/stack placement, not by the system DLLs.

---

## 3. `fork()` on Windows

### 3.0 The one fact that reorganises this whole section

**The NT kernel has supported address-space cloning, with copy-on-write, for as long as it has
existed, and it is reachable from user mode.** This is not folklore — it is stated by Microsoft and
confirmed by working code.

Microsoft, [Pico Process Overview](https://learn.microsoft.com/en-us/archive/blogs/wsl/pico-process-overview)
(Nick Judge, 2016-05-23), verbatim:

> **Improved fork support**: Yes – the Windows kernel has supported 'fork' for a long time (going
> back to earlier POSIX and SFU application support), but it is not exposed in the Win32 programming
> model… We have improved the fork implementation to meet some new requirements as part of the WSL
> work.

The mechanism: `NtCreateUserProcess` and the legacy `NtCreateProcessEx` enter **clone mode** when
given no image file and no section object; the kernel then hands the child a copy-on-write replica of
the parent's address space. `RtlCloneUserProcess` is ntdll's wrapper over it.

All of these are **present in `ntdll.dll` on this host** (measured by `GetProcAddress`):
`RtlCloneUserProcess`, `RtlCreateProcessReflection`, `NtCreateUserProcess`, `NtCreateProcessEx`,
`NtCreateProcess`, `NtCreateSection`, `NtMapViewOfSection`, `NtMapViewOfSectionEx`,
`RtlAddFunctionTable`, `RtlInstallFunctionTableCallback`, `NtAllocateVirtualMemoryEx`. From
`kernelbase.dll`: `VirtualAlloc2`, `MapViewOfFile3`, `CreateFileMapping2`, `UnmapViewOfFile2`,
`SetProcessValidCallTargets`.

So "Windows has no fork" is false as a statement about the *kernel*. It is true as a statement about
the *supported programming model*, and the gap between those two is where every project in this
section lives.

### 3.1 `RtlCloneUserProcess` — what is real and what breaks

Primary source: Hunt & Hackett, *The Definitive Guide to Process Cloning on Windows*
([blog](https://huntandhackett.com/blog/the-definitive-guide-to-process-cloning-on-windows),
[code](https://github.com/huntandhackett/process-cloning)). This is the only rigorous public treatment
and it ships runnable projects.

**Verified behaviour:**

- Signature `(ULONG ProcessFlags, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR, HANDLE DebugPort, PRTL_USER_PROCESS_INFORMATION)`.
  Flags: `CREATE_SUSPENDED`=1, `INHERIT_HANDLES`=2, `NO_SYNCHRONIZE`=4.
- The "returns twice" analogue: the cloned thread returns **`STATUS_PROCESS_CLONED` (0x129)**; the
  parent gets `STATUS_SUCCESS`. The kernel fixes up `TEB.ClientID`, so `GetCurrentProcessId()` is
  correct in the child. `TEB.ClonedThread` stays set and distinguishes the two.
- **Only the calling thread survives.** Other threads' stacks and TEBs are copied but the threads do
  not exist. This is `fork()`'s POSIX semantics exactly, and for us it is a feature, not a defect.
- Unless `NO_SYNCHRONIZE`, it drains the ntdll thread-pool queue and takes the loader, PEB, TLS/FLS
  and heap locks so the snapshot is consistent. Prefer it over the raw syscall for this alone.
- ntdll also exports `RtlUpdateClonedCriticalSection` and `RtlUpdateClonedSRWLock` — **which exist
  because cloning a multithreaded process leaves locks corrupt.** Their existence is the honest
  warning label on the whole mechanism.

**Verified failure modes** — these are the ones people actually hit:

| Breaks | Why |
|---|---|
| Loading a DLL in the child | Deadlocks on ntdll loader locks; with `NO_SYNCHRONIZE`, access-violates on the CSR port heap. **Preload everything before cloning.** |
| Window/GDI/graphics, even `MessageBox` | The clone skipped win32k init; CSR shared memory, CSR port heap and the GDI shared handle table are `ViewUnmap` and are *unmapped*, not copied, in the child. |
| Console I/O | Needs `FreeConsole()` + `AttachConsole(ATTACH_PARENT_PROCESS)` in the child. |
| SRW locks, critical sections, condvars, `WaitOnAddress` | Built on `NtWaitForAlertByThreadId`, keyed on thread IDs, which differ in the clone. |
| Inherited kernel mutex/event/semaphore handles | Now contended across parent and child. |

**Production use:** Windows Error Reporting uses it to snapshot hung processes, and the documented
`PssCaptureSnapshot` family is `NtCreateProcessEx`-based cloning underneath. Microsoft ships cloning —
only ever for *snapshot-and-die*, never as a general-purpose fork.

**The classic `NtCreateProcessEx(ParentProcess=GetCurrentProcess())` trick is dead for running code.**
It still clones the address space, but **since Windows 8.1 you cannot create a thread in the
resulting clone** — `NtCreateThread` fails `STATUS_PROCESS_IS_TERMINATING`. The Nebbett recipe
(*Windows NT/2000 Native API Reference*, 2000) that half the internet still quotes survives only as a
memory-snapshot primitive. **This is the single most common piece of fork-on-Windows folklore and it
is wrong.**

*Unverified:* no Raymond Chen "why Windows has no fork" post was located, despite being widely
referenced. No Microsoft statement endorsing `RtlCloneUserProcess` for general use was found.

### 3.2 MSYS2 — no delta, verified by diff

MSYS2's runtime is a friendly fork of Cygwin, and the question "how does its fork differ, and why" has
a short answer: **it does not.**

Diffed `winsup/cygwin/fork.cc` between `msys2/msys2-runtime` @ `2105f6c` and upstream newlib-cygwin.
The entire delta is: `sched_reset_on_fork` / nice handling, an added `cygheap->lock()` alongside
`__malloc_lock()`, and moving `pthread::atforkchild()` after `cygwin_finished_initializing`. The
mechanism — `CreateProcessW` of itself suspended, then reconstruct the child's address space —
is untouched.

MSYS2's documented differences from Cygwin ([wiki](https://www.msys2.org/wiki/How-does-MSYS2-differ-from-Cygwin/))
are path mangling of arguments/environment, `MSYSTEM`, CRLF stripping, symlink-as-copy, and `noacl`
mounts. None touch process creation.

**Lesson: there is no second independent implementation to learn from.** MSYS2 inherits Cygwin's fork
wholesale, including its failure modes. Anything the Cygwin study concludes applies unchanged.

### 3.3 WSL1 — a real NT Linux personality, built on a door we cannot open

The published primary sources are three archived MSDN posts, and that is all there is:
[Overview](https://learn.microsoft.com/en-us/archive/blogs/wsl/windows-subsystem-for-linux-overview)
(Thomas, 2016-04-22), [Pico Process Overview](https://learn.microsoft.com/en-us/archive/blogs/wsl/pico-process-overview)
(Judge, 2016-05-23), [WSL System Calls](https://learn.microsoft.com/en-us/archive/blogs/wsl/wsl-system-calls)
(Hufnagel, 2016-06-08). *There is no "WSL Process Architecture" post; search results that cite one are
hallucinating it.*

**What it was.** A *minimal process* is "simply an empty user-mode address space" — no ntdll, no PEB,
no TEB, no initial thread. A *pico process* is a minimal process plus a registered **pico provider**
driver (`lxcore.sys`) that receives every syscall and every user-mode exception from it. NT was
modified for WSL to permit **4 KiB-granularity VA management** where the norm is 64 KiB — the exact
constraint §2.1 measured — and per-thread case-sensitivity.

**How fork worked**, verbatim from *WSL System Calls*:

> lxcore.sys does some of the initial work to prepare for copying the process. It then calls
> **internal NT APIs** to create the process with the correct semantics and create a thread in the
> process with an identical register context.

Copy-on-write is confirmed only in a blog *comment* reply — **unverified**. **No published measurement
of WSL1 fork cost exists.** Any number quoted for it is invented.

**Applicability: none as an implementation model.** `PsRegisterPicoProvider` and
`PsCreateMinimalProcess` are kernel-only — absent from ntdll, confirmed — and pico-provider
registration requires a Microsoft-signed early-boot driver. It is not a third-party extension point.
The internal NT APIs the quote refers to are not the ones we can call.

**Why WSL1 was superseded — and the myth to kill.** Microsoft's stated reasons are exactly two:
[compare-versions](https://learn.microsoft.com/en-us/windows/wsl/compare-versions) — *"increase file
system performance and add full system call compatibility"*; and
[Announcing WSL 2](https://devblogs.microsoft.com/commandline/announcing-wsl-2/) — *"it's challenging
to implement all of these system calls, resulting in some apps being unable to run in WSL 1."* The
named blockers were Docker/cgroups, FUSE, systemd, and `inotify` on `/mnt`. lxcore shipped ~235
syscalls. **`fork()` was never cited as a WSL1 weakness — it was one of its strengths.** WSL1 is not
formally deprecated (the FAQ says there are no plans to), but when WSL was open-sourced on
[2025-05-19](https://blogs.windows.com/windowsdeveloper/2025/05/19/the-windows-subsystem-for-linux-is-now-open-source/),
`lxcore.sys` was **explicitly excluded**.

**Most useful lesson: syscall surface and filesystem performance are what kill a personality, not
fork.** We are building a WSL1-shaped thing; the record says budget accordingly.

### 3.4 Interix / SFU / SUA — the same kernel door, from inside

Interix was a genuine NT **environment subsystem** (peer of CSRSS): `psxss.exe` server, `psxdll`
client library, `posix.exe`/`psxrun` launcher, and an `init` at PID 1. Microsoft's
[UNIX Migration Guide ch.3](https://learn.microsoft.com/en-us/previous-versions/tn-archive/bb497006(v=technet.10))
confirms real `fork()` and `vfork()` and a full POSIX process hierarchy. Lineage: NT POSIX subsystem →
OpenNT (Softway) → acquired by Microsoft 1999 → SFU → SUA → discontinued after Windows 8.1 /
Server 2012 R2, replaced by WSL.

**Mechanism: `NtCreateProcess` with the parent's process handle**, i.e. the same kernel COW clone as
§3.0/§3.1. This is corroborated and architecturally consistent, but the Interix architecture
whitepaper naming the exact call sequence could not be retrieved — **treat the precise internals as
unverified**. There was never a user-mode-only path; Interix fork depended on the subsystem process
plus kernel support.

**Lesson: the fast, correct answer to fork on NT has always been "be a subsystem and let the kernel
COW-clone."** We cannot be a subsystem. The kernel half is available to us; the CSRSS/win32k
registration half is not, and that is precisely the half whose absence produces §3.1's failure table.

### 3.5 Midipix — the closest thing to a maintained blueprint

POSIX-on-NT built directly on the Native API: `ntapi` (native core), `psxscl` (libc-agnostic syscall
layer), a musl port, `ntctty`. Fork lives in `git.midipix.org/ntapi`, `src/process/ntapi_tt_fork.c`
and `ntapi_tt_fork_v{1,2}.c`.

**Read:** `__ntapi_tt_fork_v1` calls `zw_create_process(&hprocess, NT_PROCESS_ALL_ACCESS, &oa,
NT_CURRENT_PROCESS_HANDLE, 1, 0, 0, 0)` — `NtCreateProcess` with the current process as parent, so the
kernel COW-clones the address space — then `NtQueryInformationProcess` for the child PID and
`NtCreateThread` with a hand-built context resuming at the child entry. The wrapper drives a **retry
loop up to 32×**, selects v1/v2 by kernel support, and synchronises parent and child with two
inheritable notification events (`hresumed`/`hready`, 250/500 ms timeouts), re-registers the TTY
session, and re-initialises IPC in `__ntapi_tt_fork_finalize`.

Status: actively maintained (news dated 2024-08-16), self-described "/pre/alpha".

**Caveat that must travel with this entry:** it uses `NtCreateThread` on a `NtCreateProcess` clone,
which §3.1 records as failing `STATUS_PROCESS_IS_TERMINATING` since Windows 8.1. Either Midipix's v2
path avoids this, or the retry loop is masking it. **Whether Midipix's fork works on Windows 11 was
not tested and is unverified.** If we pursue this route, that is the first experiment to run.

### 3.6 flinux — our twin, and the most valuable single find

[`wishstudio/flinux`](https://github.com/wishstudio/flinux) @ `a041253` (last commit 2016-03-29) is
"a Linux system call interface emulator for Windows **without any drivers**". It is architecturally
the same product as this engine, minus the JIT, and it implements COW fork in pure user mode.

**`fork_process()` — read, `src/syscall/fork.c:163`:**

```c
CreateProcessW(filename, L"/?/fork", NULL, NULL, TRUE, CREATE_SUSPENDED, ...);
tls_fork(hProcess); vfs_fork(...); mm_fork(...); shared_fork(...);
heap_fork(...); signal_fork(...); process_fork(...); exec_fork(...);
NtWriteVirtualMemory(hProcess, &fork->context, context, sizeof(struct syscall_context), NULL);
VirtualAllocEx(hProcess, stack_base, STACK_SIZE, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
NtWriteVirtualMemory(hProcess, context->esp, context->esp, stack_top - context->esp, NULL);
ResumeThread(info.hThread);
vfs_afterfork_parent(); tls_afterfork_parent(); ... mm_afterfork_parent();
```

The child re-enters at `fork_init()` (`:78`), which detects the `/?/fork` command line.

**The COW design — read, `src/syscall/mm.c`:** guest memory is a table of fixed-size blocks, each
backed by its **own NT section object** (`NtCreateSection`, `:683`). `mm_fork` (`:965`) duplicates the
section-*handle* table into the child but **maps nothing**: the sections arrive "detached", and the
child faults them in lazily. Parent COW regions are marked non-writable. Faults land in
`AddVectoredExceptionHandler` (`syscall.c:131`) → `mm_handle_page_fault` → `handle_cow_page_fault`
(`mm.c:857`), which decides sole ownership with
`NtQueryObject(..., ObjectBasicInformation).HandleCount == 1` and, when shared, allocates a fresh
section and `CopyMemory`s the block.

**Why this matters to us more than anything else in the survey:**

1. It is a **complete, driver-free, user-mode COW fork** for a Linux personality on Windows. It exists,
   it is readable, and it needs no undocumented process-cloning call.
2. Its per-subsystem `*_fork(hProcess)` / `*_afterfork_parent()` decomposition is **the same shape the
   engine already has** — `fork_child_hooks` in `proc.c`, `jit_after_fork` in `engine/cache.c`. The
   engine's own note that "the dual-mapped RW/RX arena does NOT survive fork on its own" and must be
   re-remapped in the child is exactly the discipline flinux formalises.
3. It independently reinvents PostgreSQL's address-reservation trick (`fork_init` reserves `0x400000`
   in the suspended child and relaunches when occupied), which is strong evidence that the pattern is
   forced by the platform rather than chosen.
4. Its block size is the 64 KiB allocation granularity of §2.1 — the constraint drove the design.

**Cost caveat:** it is `CreateProcess`-based, so it pays §2.3's 1.5 ms floor per fork. It buys COW
semantics, not COW speed.

### 3.7 Zygote and pre-fork, as an alternative to fork

**Android Zygote.** The value is preloaded warm state, not the fork. `ForkCommon()` in
`com_android_internal_os_Zygote.cpp` carries the comment *"Note that the zygote process is single
threaded at this point"* — and Android *enforces* it by making thread creation throw. ART's
`zygote_space.h` sets the mark bit on all live objects pre-fork specifically to stop GC dirtying
shared pages. **Lesson: a zygote's correctness comes from being provably single-threaded at the fork
point, and its benefit comes from not dirtying the shared pages afterwards.** Both are design
obligations, not free.

**Chromium.** `content/public/common/zygote/features.gni`: `use_zygote = is_posix && !is_android &&
!is_apple`. **There is definitively no zygote on Windows.** Windows uses
`SandboxWin::StartSandboxedProcess` → `CreateProcessAsUserW(CREATE_SUSPENDED)`, with state passed via
`LaunchOptions::handles_to_inherit`, `--mojo-platform-channel-handle`, and `switches::kFieldTrialHandle`
— a shared-memory handle **on the command line**.

**PostgreSQL — the directly relevant precedent.** PostgreSQL has **no `fork()` on Windows at all**:
`src/backend/postmaster/fork_process.c` is entirely `#ifndef WIN32`. The Windows path was refactored
into `launch_backend.c` by commit `aafc05de1bf5c0324cb5e690c6742118c1ac4af6` (PG17).

Read, `src/backend/postmaster/launch_backend.c`:

| Step | Mechanism |
|---|---|
| Launch | `internal_forkexec()` `:395` — `CreateProcess(..., inherit=TRUE, CREATE_SUSPENDED)` |
| Parameter transport | An **inheritable anonymous `CreateFileMapping`**; the handle value is passed **as a decimal string on the command line** (`"%s" --forkchild=%d %s`). The Unix `EXEC_BACKEND` path uses a temp file instead — same payload, two transports. |
| Payload | `save_backend_variables()` `:707`, struct at `:75–149`: DataDir, shmem handle **and address**, `ProcGlobal`/`AuxiliaryProcs`/`PMSignalState`/`ProcSignal` pointers, `PostmasterPid`, `MaxBackends`, exec paths, syslog pipes, a `startup_data[]` flexible array. |
| Sockets | `write_inheritable_socket()` `:818` → `WSADuplicateSocket(src, childpid, &wsainfo)`; child `read_inheritable_socket()` `:839` → `WSASocket(FROM_PROTOCOL_INFO, ...)`, then closes the original. |
| Other handles | `write_duplicated_handle()` `:788` → `DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE\|DUPLICATE_SAME_ACCESS)`. |
| **Same-address guarantee** | `pgwin32_ReserveSharedMemoryRegion()` (`src/backend/port/win32_shmem.c:573`) calls **`VirtualAllocEx(hChild, UsedShmemSegAddr, UsedShmemSegSize, MEM_RESERVE, PAGE_READWRITE)` into the still-suspended child**, then `ResumeThread`. The child's `PGSharedMemoryReAttach` (`:424`) does `VirtualFree(MEM_RELEASE)` then `MapViewOfFileEx(..., UsedShmemSegAddr)` and **`elog(FATAL)`s if the address differs**. The postmaster itself lets Windows choose (`MapViewOfFileEx(..., NULL)`, `:371`); only children are pinned. |
| When reservation fails | `launch_backend.c:522` — `if (++retry_count < 100) goto retry;` then *"giving up after too many tries to reserve shared memory"*, hint *"This might be caused by ASLR or antivirus software."* Introduced by `59892b1209b9202338dbfc65c9a59cbed182befb` (Tom Lane, 2017-07-10): *"It's not very clear what, given that we disable ASLR in Windows builds, but suspicion falls on antivirus products."* PG16 later **re-enabled** ASLR (`36389a060`) — the retry loop is what made that safe. |
| Thread-pool hazard | `ShmemProtectiveRegion` (`win32_shmem.c:22–41`) — a decoy `10 * WIN32_STACK_RLIMIT` reservation released immediately before the real one, because *"Windows asynchronously creates threads for the process's default thread pool"* whose stacks would otherwise land in the freed hole. |

**What PostgreSQL gave up, in its own words.** `bgworker.sgml:190`: on `EXEC_BACKEND` *"it is not safe
to pass a `Datum` by reference, only by value… the pointer won't be valid from the new background
worker process."* `storage/aio/README.md:226`: *"a backend's executable code and other process local
state is not necessarily mapped to the same addresses in each process due to ASLR. This means that
the shared memory cannot contain pointers to callbacks."* `SubPostmasterMain` `:576` must re-run
`process_shared_preload_libraries()`, `LocalProcessControlFile()` and `read_nondefault_variables()`.
Testing `EXEC_BACKEND` on Linux requires `kernel.randomize_va_space=0`.

**Most useful lesson from PostgreSQL: they did not emulate fork, they enumerated the state.** The cost
is a hard, permanent architectural rule — *no pointers may cross the process boundary* — and the
benefit is that nothing is undocumented and nothing races. It is also a warning: their
`BackendParameters` struct is a few dozen fields, and a process whose "state" is an entire Linux guest
address space cannot be enumerated this way.

**Also confirmed:** Python `multiprocessing` has **no `fork` start method on Windows** — only `spawn`,
which launches a fresh `python.exe` and pickles (`popen_spawn_win32.py`, `reduction.py::DupHandle`).
Go has no `syscall.ForkExec` on Windows. Ruby's `Process.fork` simply raises. libuv duplicates sockets
with `WSADuplicateSocketW` (`src/win/tcp.c`).

### 3.8 Verdict

| Route | Real COW? | Driver? | Cost/fork | Verdict for us |
|---|---|---|---|---|
| Cygwin/MSYS2-style `CreateProcess` + address-space reconstruction | Partial | No | ≥1.5 ms | Prior art, known failure modes; see the Cygwin doc |
| `RtlCloneUserProcess` | **Yes, kernel COW** | No | Untested here | Genuine NT fork; undocumented; safe only from a provably single-threaded moment |
| `NtCreateProcessEx(parent=self)` + `NtCreateThread` | Clones AS | No | — | **Dead since 8.1** for running code. Do not build on it. |
| WSL1 pico process | Yes | **Yes, signed** | Unpublished | Not available to third parties |
| Interix subsystem | Yes | **Yes** | — | Discontinued; not available |
| flinux section-object COW + VEH | **Yes, user-mode** | No | ≥1.5 ms | **The blueprint.** Driver-free, readable, same problem domain |
| PostgreSQL state re-transfer | No | No | ≥1.5 ms | Right pattern for *engine* state; cannot express a guest address space |
| Zygote / fork-server | Inherits whatever fork does | No | — | Orthogonal, and the engine already has it |

---

## 4. JIT and guest execution on Windows

### 4.1 What shipping JITs actually do — and the surprise

| Project | Windows x64 code memory | Source |
|---|---|---|
| **QEMU / TCG** | Flat `VirtualAlloc(PAGE_EXECUTE_READWRITE)`. `splitwx > 0` → `error_setg("jit split-wx not supported")` | `tcg/region.c:519-540` |
| **V8** | **RWX.** `SetPermissions(..., kReadWriteExecute)` → `PAGE_EXECUTE_READWRITE \| PAGE_TARGETS_INVALID`. W^X exists only via PKU, which is `V8_HAS_PKU_SUPPORT` = **Linux x64 only** | `heap/code-range.cc:397`, `platform-win32.cc:898`, `base/build_config.h:31-37` |
| **JavaScriptCore** | **RWX.** The JIT cage and `MPROTECT_RX_TO_RWX` are POSIX/Darwin only | `WTF/wtf/win/OSAllocatorWin.cpp:41-45` |
| **LuaJIT** | `VirtualProtect` RW↔RX flip of one in-construction `mcarea`, `LUAJIT_SECURITY_MCODE 1` default on | `lj_mcode.c` `mcode_setprot`, `lj_arch.h:763-766` |
| **HotSpot** | RWX code cache | — |
| **.NET CoreCLR** | **Real dual mapping, default ON since .NET 7** | `inc/clrconfigvalues.h:646-648` `EnableWriteXorExecute` default 1 |
| **FEX-Emu (Windows)** | `VirtualAlloc(MEM_COMMIT\|MEM_RESERVE\|MEM_TOP_DOWN, PAGE_EXECUTE_READWRITE)` | `FEXCore/include/FEXCore/Utils/AllocatorHooks.h:48-62` |
| **Wine** | No dual mapping; `force_exec_prot` adds `PROT_EXEC` to every `PROT_READ` mapping | `dlls/ntdll/unix/virtual.c:241`, `:1935-1939` |

The naive reading is "everyone ships RWX on Windows, so should we." That reading is wrong for us, for two
reasons. First, **CoreCLR is the counter-example and it is the most recent, most measured one**: it ships
exactly mechanism (a) below, on by default, after
[dotnet/runtime#54954](https://github.com/dotnet/runtime/pull/54954). Second — and decisively — **the
engine already has a dual-alias arena on Linux and macOS**, and §2.2 measured that the same shape works
on Windows unchanged. For us, RWX is not the low-diff option; the dual alias is.

CoreCLR's exact shape (`src/coreclr/minipal/Windows/doublemapping.cpp`) is worth copying:

```c
CreateFileMapping(INVALID_HANDLE_VALUE, NULL,
                  PAGE_EXECUTE_READWRITE | SEC_RESERVE,
                  HIDWORD(2TB), LODWORD(2TB), NULL);
/* RX view: MapViewOfFile(h, FILE_MAP_EXECUTE|FILE_MAP_READ|FILE_MAP_WRITE, off, size)
   RW view: MapViewOfFile(h, FILE_MAP_READ|FILE_MAP_WRITE, off, size)
   commit : VirtualAlloc(p, size, MEM_COMMIT, exec ? PAGE_EXECUTE_READ : PAGE_READWRITE) */
```

One 2 TiB `SEC_RESERVE` pagefile-backed section, sub-allocated by offset, committed per page on the
mapped view. AsmJit's `VirtMem::allocDualMapping` is the same shape independently.

MSDN's constraint, verbatim from [CreateFileMappingA](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga):
*"To have a mapping with executable permissions, an application must call CreateFileMapping with either
PAGE_EXECUTE_READWRITE or PAGE_EXECUTE_READ, and then call MapViewOfFile with `FILE_MAP_EXECUTE |
FILE_MAP_WRITE` or `FILE_MAP_EXECUTE | FILE_MAP_READ`."* No privilege is needed with
`INVALID_HANDLE_VALUE`. **Release views with `UnmapViewOfFile`, never `VirtualFree`** — AsmJit records
this as a Windows-specific failure mode.

**Two traps recorded by CoreCLR's rollout, both of which we would hit:**

1. Finding the RW alias for a given RX address was an O(n) linked-list walk
   (`ExecutableAllocator::FindRWBlock`, ~27 steps average on a real workload) and needed an LRU cache.
   **A fixed RW−RX delta avoids the whole problem** — and §2.2 measured that the placeholder API gives
   us exactly that, at addresses we choose. The engine's Linux arena already assumes a constant alias
   delta.
2. [dotnet/runtime#71786](https://github.com/dotnet/runtime/issues/71786): perf/JIT-map tooling recorded
   the **RW** address rather than the RX address that actually executed. **Every symbol, profiler entry,
   unwind table and backtrace must use the RX address.** The engine's `perf_event_open` sampler
   (`docs/amd64-host.md` §6) has the same hazard on Windows.

**Availability under mingw-w64 (verified by reading the mingw-w64 tree):** `memoryapi.h` declares
`VirtualAlloc2`, `MapViewOfFile3`, `UnmapViewOfFile2`, `SetProcessValidCallTargets`,
`CreateFileMapping2`; `winnt.h:5933-6005` has `PAGE_TARGETS_INVALID`, `MEM_RESERVE_PLACEHOLDER`,
`MEM_REPLACE_PLACEHOLDER`, `MEM_EXTENDED_PARAMETER`. Import libs exist
(`api-ms-win-core-memory-l1-1-6.def` etc.). `RtlAddFunctionTable`, `RtlInstallFunctionTableCallback`,
`FlushInstructionCache`, `AddVectoredExceptionHandler`, `SetThreadStackGuarantee`,
`LocateXStateFeature` are all in `kernel32.def.in`. **`RtlAddGrowableFunctionTable` is not** —
`GetProcAddress` it from ntdll, as V8 does. `RUNTIME_FUNCTION` is present; **`UNWIND_INFO` /
`UNWIND_CODE` are not** (the MS SDK omits them too) — declare them ourselves.

### 4.2 `FlushInstructionCache`

MSDN's Remarks, complete: *"Applications should call FlushInstructionCache if they generate or modify
code in memory. The CPU cannot detect the change, and may execute the old code it cached."* No
x86/ARM distinction is drawn.

Practice contradicts the doc on x86-64, and two independent JITs say so in comments. V8's
`src/codegen/x64/cpu-x64.cc` makes `CpuFeatures::FlushICache` an **empty function**: *"No need to flush
the instruction cache on Intel… the core performing the patching will have its own instruction cache
updated automatically."* LuaJIT's `lj_mcode.c:41-45` is `#if LJ_TARGET_X86ORX64 UNUSED(start);
UNUSED(end);` — the Windows branch is `#elif` and is never taken on x64. CoreCLR still calls it,
conservatively.

§2.3 measured it at **4 ns for 64 KiB**, and §2.2 measured code rewritten through the RW alias
executing correctly without it. **Call it once per published region — it costs nothing and it is the
documented contract, and a Windows-on-ARM host would need it — but never put it in a patch loop.** This
matches the engine's existing posture, where `docs/amd64-host.md` §5.1 records qemu being *stricter*
than native x86 about cache maintenance; the same discipline transfers.

### 4.3 Self-modifying code and write-fault interception

**QEMU proves the strategic point: if you can afford a software MMU, Windows costs you nothing on the
fault path.** In system mode, guest writes to translated code are caught by the **softmmu TLB miss**,
never by a host page fault — `tlb_protect_code()` (`accel/tcg/cputlb.c:858`) clears
`DIRTY_MEMORY_CODE` and the store slow path calls `notdirty_write()` (`cputlb.c:1337`). In user mode it
uses host page protections plus SIGSEGV — `handle_sigsegv_accerr_write` (`accel/tcg/user-exec.c:134`) →
`page_unprotect` (`:668`). Grepping the entire QEMU tree for `AddVectoredExceptionHandler`,
`SetUnhandledExceptionFilter` and `RtlAddFunctionTable` yields **one** hit, in a socket-close wrapper.
QEMU on Windows intercepts no hardware faults at all.

**FEX's `InvalidationTracker` is the directly copyable design** if we keep host page protections
(`Source/Windows/Common/InvalidationTracker.cpp`): two interval lists (`XIntervals`, `RWXIntervals`)
seeded by walking `VirtualQuery` at init and maintained from memory-protection notifications;
exec+write pages downgraded to `PAGE_EXECUTE_READ` (`GetTrapProt`, `:320`); on an access violation,
`HandleRWXAccessViolation` invalidates the code range under a mutex and re-protects the page. **Inline
SMC — a block writing to itself — is handled by rewinding to the dispatcher with a single-step flag**,
which is the case most engines get wrong. box64 adds a **"hotpage" heuristic**
(`src/custommem.c:2470-2570`) that stops re-protecting pages which fault repeatedly, the standard cure
for protect/fault thrash on mixed code/data pages, and keeps guest protections in a **shadow rbtree**
(`memprot`, `custommem.c:72`) rather than querying the OS. Both are worth stealing regardless of route.

**Measured cost (§2.3): a VEH write-fault round trip is 2.85 µs.** For comparison the engine's whole
guest-access bracket is ~0.36 ns (`docs/amd64-host.md` §6). A fault is therefore ~8000× a bracketed
access — SMC-by-fault is affordable only if faults are rare. The engine's current posture on the
x86-64 transliterator is *refusal* (it declines to run while an emulated `PROT_EXEC` mapping is live),
and the interpreter re-decodes so it has no SMC gap at all. **Both existing postures survive the port
unchanged; neither requires write-fault interception.** That is a real piece of good news.

`ExceptionInformation[0]` = 0 read / 1 write / 8 DEP and `[1]` = the faulting VA
([EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record)),
confirmed by measurement in §2.5. Caveats: there is **no atomic "unprotect and continue"** — either
emulate the store or use a one-shot `EFlags.TF`; and **kernel-mode writes to a protected page return
`STATUS_ACCESS_VIOLATION` to the caller instead of trapping**, so a guest page written by a syscall
will not fault. `MEM_WRITE_WATCH` + `GetWriteWatch` is a zero-fault alternative but is polling,
page-granular, and carries no instruction context.

*Aside worth knowing:* NT has a first-class emulator SMC hook that Wine implements —
`NtSetInformationProcess(ProcessExecuteFlags, .ProcessEnableWriteExceptions)` marks exec+write pages
write-watched and raises `STATUS_IN_PAGE_ERROR` with
`ExceptionInformation[2] = STATUS_EXECUTABLE_MEMORY_WRITE` (`dlls/ntdll/unix/virtual.c:5025`, `:4640`).
**Whether this works on real Windows for a normal process was not tested — unverified.** If it does, it
is a better SMC primitive than protection flipping.

### 4.4 Exceptions: VEH is our only mechanism

**Measured, and it overrides a widely-repeated claim: `__try`/`__except` does not work in this
toolchain.** Plain clang rejects `__try` outright. With `-fms-extensions` it **compiles and then
segfaults** — confirmed at `-O0` and `-O2`, with the canonical null-store-in-`__try` test, on clang
22.1.8 `x86_64-w64-windows-gnu`. Secondary sources that say "clang+mingw supports `__try` with
`-fms-extensions`" are describing the *driver flag gating*, not working codegen. **Do not use SEH.**

VEH is sufficient and was measured working (§2.5): `AddVectoredExceptionHandler`, read/modify
`ContextRecord`, rewrite `Rip`, return `EXCEPTION_CONTINUE_EXECUTION`. Differences from Linux
`ucontext` that matter to `signal_capture` and the interpreter's fault pad:

- One shared context across all VEH handlers; there is no per-handler copy.
- **No alternate signal stack.** `sigaltstack` has no analogue — a guest stack overflow is handled with
  `PAGE_GUARD` / `STATUS_GUARD_PAGE_VIOLATION` (0x80000001) plus **`SetThreadStackGuarantee` (≥64 KiB)
  on every JIT thread**. `PAGE_GUARD` is one-shot: the OS clears it before re-running the instruction,
  so it is poor for repeated watchpoints. `_resetstkoflw` must **not** be called from a VEH.
- Do not clear `ContextFlags`.
- **AVX/AVX-512 state is present and writable in the VEH `ContextRecord`** — a `CONTEXT_EX` follows the
  `CONTEXT`; use `LocateXStateFeature`, and **you must set the corresponding bit in `XSTATE.Mask` or
  the feature is restored as zeros.** This is directly load-bearing for the engine's SSE/AVX fault
  bracket. Caveat: **unwind-pass contexts do not carry XSTATE — only first-chance dispatch does.**
- `CONTEXT_DEBUG_REGISTERS` (Dr0–7) are writable from a VEH context, but the flag bit is not reliably
  set on entry — OR it in ourselves and treat incoming Dr values as untrustworthy.

**Unwind info is insurance, not a prerequisite.** MSDN
([exception-handling-x64](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64)): *"If the
search doesn't find a function table entry, the code is assumed to be part of a leaf function, and RSP
directly addresses the return pointer"* — so a missing `RUNTIME_FUNCTION` pops garbage as RIP and
`RtlUnwindEx` raises `STATUS_BAD_STACK` (0xC0000028), non-continuable, terminating the process. That is
Mozilla bug [844196](https://bugzilla.mozilla.org/show_bug.cgi?id=844196), *"jits totally break SEH on
Win64, including SetUnhandledExceptionFilter"*.

**But VEH is dispatched before any unwind**, so if our handler returns
`EXCEPTION_CONTINUE_EXECUTION`, missing `.pdata` costs nothing on the hot path. Register unwind info so
that crash reporters, debuggers and any host unwind crossing JIT frames work. The cheap precedent is
HotSpot's: **one** `RUNTIME_FUNCTION` over the entire code cache with EH-only unwind info
(`Version=1, Flags=UNW_FLAG_EHANDLER, SizeOfProlog=0, CountOfCodes=0`) plus a 16-byte `jmp` thunk,
registered with `RtlAddFunctionTable` (`os_windows_x86.cpp:124-162`). V8 does the richer version in
`src/diagnostics/unwinding-info-win64.cc` via `RtlAddGrowableFunctionTable`; CoreCLR uses
`RtlInstallFunctionTableCallback`. **All unwind offsets are 32-bit image-relative, so xdata must live
within 4 GiB of — in practice, in the first page of — the code range.**

Note the interaction with §4.1: unwind tables must describe the **RX** alias.

### 4.5 Mitigations: nothing bites by default, and one thing cannot even be turned on

§2.4 measured CFG and ACG off in a default process. The mechanisms, for completeness:

- **CFG is caller-side and off.** `-mguard=cf` exists for the mingw driver
  ([D132810](https://reviews.llvm.org/D132810), LLVM 16) but `lld/MinGW/Driver.cpp` defaults it to
  `false`, and mingw-w64's CRT needs `--enable-cfguard` to supply `__guard_dispatch_icall_fptr`.
  Loader enforcement needs `IMAGE_DLLCHARACTERISTICS_GUARD_CF`, which §2.5 measured absent.
- **`SetProcessValidCallTargets` is almost never needed**: *"The VirtualProtect and VirtualAlloc
  functions will by default treat a specified region of executable and committed pages as valid
  indirect call targets"*
  ([CFG doc](https://learn.microsoft.com/en-us/windows/win32/secbp/control-flow-guard)). It is required
  only if we opt into `PAGE_TARGETS_INVALID` (as V8 does), and it *"does not succeed if Control Flow
  Guard is not enabled for the target process."* **`SetProcessValidCallTargets2` appears not to exist
  in the public SDK — do not design around it.**
- **ACG is strictly opt-in** (`PROCESS_MITIGATION_DYNAMIC_CODE_POLICY.ProhibitDynamicCode`). When on,
  any execute-protection `VirtualAlloc`/`VirtualProtect` fails `STATUS_DYNAMIC_CODE_BLOCKED` and only
  signed `SEC_IMAGE` mappings work. Chromium sets it for jitless renderers only
  (`sandbox/win/src/process_mitigations.cc`). **Never set it.**
- **CET user shadow stacks cannot currently be enabled for a mingw-clang binary at all**:
  `lld/MinGW/Options.td` has no `cetcompat` option, nor does GNU `ld` for PE, so the EXE cannot be
  marked `/CETCOMPAT`. §2.4's `ERROR_NOT_SUPPORTED` on the shadow-stack policy query is consistent.
  Were it ever enabled, JIT ranges need `SetProcessDynamicEnforcedCetCompatibleRanges`.

**Only DEP/NX and ASLR are live by default.** The residual external risk is that an administrator or
Defender exploit-protection profile forces ACG or hardware-enforced stack protection **on our EXE
name**, which would break the JIT from outside our control. That is a deployment note, not a design
constraint.

### 4.6 The precedents that look relevant and are not

- **qemu-user does not build on Windows, and the gate is one line.** `configure:793-798`: `linux-user`
  targets are added only when `host_os = linux`, with no override. The assumptions are structural, not
  incidental — `docs/about/emulation.rst:842`, *"In user mode, the memory mapping is directly copied
  from `/proc/self/maps`"*; `linux-user/mmap.c` is built on `MAP_FIXED_NOREPLACE`; `linux-user/signal.c`
  on host `sigaction` + `ucontext_t`; `linux-user/syscall.c` is 14.7k lines of thunking to a host Linux
  kernel. **`bsd-user` is the precedent for a non-Linux user-mode port** (`configure:803-809`,
  `meson.build:4301`) and its structural pattern — per-host-OS include directories — is worth copying,
  but it changed nothing conceptually: still POSIX mmap, still `sigaction`. It is not evidence that a
  Windows port is tractable.
- **FEX and box64 do run on Windows — as privileged plugins, which we cannot be.** FEX ships
  `libwow64fex.dll` / `libarm64ecfex.dll` and registers under
  `HKLM\Software\Microsoft\Wow64\amd64`. It installs **no VEH and no SEH**: it exports
  `BTCpuResetToConsistentState(EXCEPTION_POINTERS*)`, which ntdll calls **from inside
  `KiUserExceptionDispatcher`, before any user handler runs**. The full contract
  (`Source/Windows/WOW64/libwow64fex.def`) includes `BTCpuSimulate`, `BTCpuGet/SetContext`,
  `BTCpuNotifyMemoryAlloc/Free/Protect/Dirty`, `BTCpuNotifyMapViewOfSection`,
  `BTCpuFlushInstructionCache2`. box64's `wine/wow64/wowbox64.c` implements the same set, built with
  `aarch64-w64-mingw32-clang`. **This is a machine-wide, admin-only, undocumented plugin slot that
  breaks on servicing updates. As a normal `.exe` we get VEH, which is strictly weaker** — it runs
  after the debugger's first chance and cannot fix up a fault taken during
  `KiUserExceptionDispatcher` setup. Budget for that gap explicitly.
- **box64 has no native Windows port** and the blocker is what one would guess: it loads Linux ELF
  `.so` files and thunks to real host Linux libraries (`src/elfs/elfloader.c`, `src/wrapped/`). Only 31
  files in `src/` mention `_WIN32`.
- **Wine runs the other way** (Windows guest on Unix host) and does not run on Windows. Its value to us
  is as the reverse-engineered specification of NT's exception dispatch: `setup_raise_exception`
  (`dlls/ntdll/unix/signal_x86_64.c:1471`) builds `struct exc_stack_layout` — documented at `:425`,
  `CONTEXT` @0x000, `CONTEXT_EX` @0x4d0, `EXCEPTION_RECORD` @0x4f0, machine frame @0x590, total 0x5c0 —
  on the guest stack, then sets `RIP = KiUserExceptionDispatcher` and clears TF/DF/AC. If we ever need
  to synthesise or interpret an NT exception frame, that is the reference.
- **Rosetta 2 and Microsoft's Prism / `xtajit64` are closed.** Publicly: Rosetta is primarily AOT with
  caches in `/var/db/oah`; Prism caches translations as `.JC` files in `C:\Windows\XtaCache` and
  exposes a user-toggleable "strict self-rewriting code compatibility" switch — which is itself
  evidence that SMC handling is a correctness/performance dial even for the vendor. **All of this is
  unverified secondary reporting.**

---

## 5. Containers on Windows

The end goal is a container runtime. The finding is uncomfortable but clean: **none of Windows' native
isolation primitives isolate anything a Linux guest can perceive.**

### 5.1 Job objects — use them, but not as cgroups

`CreateJobObject` / `AssignProcessToJobObject` / `SetInformationJobObject`. Genuinely enforceable:
`JOBOBJECT_EXTENDED_LIMIT_INFORMATION` (`JobMemoryLimit`, `ProcessMemoryLimit`, `ActiveProcessLimit`,
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), and `JOBOBJECT_CPU_RATE_CONTROL_INFORMATION` (Win8+) with
`CpuRate` in 1/100 %, `WEIGHT_BASED`, `HARD_CAP`, `MIN_MAX_RATE`. Nested jobs (Win8+) may only tighten,
and accounting aggregates upward.

Where it falls short of cgroups:

| Gap | Detail |
|---|---|
| **IO rate control is dead** | `JOBOBJECT_IO_RATE_CONTROL_INFORMATION` is documented *"Windows 10, version 1607, and newer: This structure is not supported."* hcsshim still calls it (`internal/jobobject/limits.go:266`). Do not rely on it. |
| Memory is **commit charge**, not RSS | Guest `mmap`/overcommit semantics do not map onto it cleanly |
| No `memory.high` | Only a hard cap plus *advisory* `JobObjectNotificationLimitInformation` via IOCP; no reclaim-pressure tier |
| No per-device IO throttling | — |
| Some limits are silently ignored | Working set and priority limits do not fail, they do nothing |

**Applicable: yes, as a coarse outer guardrail — a kill-switch and a CPU cap.** We must implement cgroup
v1/v2 semantics in the emulator regardless, because the guest reads `/sys/fs/cgroup`.

### 5.2 Silos — real, undocumented, privileged, and pointless for us

A silo is a job object with silo state. **Application silos** redirect the object-manager root only.
**Server silos** are the actual Windows container: own object manager root (`\Silos\<id>`), registry
hive redirection, network compartment, session, `\Device` / `\??` redirection, own `\SystemRoot`.

Creation is via **undocumented `NtSetInformationJobObject` info classes** (Quarkslab, corroborated
against hcsshim): `0x23` create silo, `0x25` silo root directory, `0x28` convert to server silo, `0x2D`
system root. Microsoft documents only the **kernel-mode consumer** side — `PsGetCurrentServerSilo`,
`PsAttachSiloToCurrentThread`. **There is no documented user-mode silo API.**

Measured on this host, unelevated:

| Call | Result |
|---|---|
| `NtSetInformationJobObject(job, 0x23, NULL, 0)` | `STATUS_INVALID_PARAMETER` |
| `NtQueryInformationJobObject(job, 0x24)` | `STATUS_NOT_SILO` (0xC0000509) — confirms the class numbering |
| `NtSetInformationJobObject(job, 0x28, {0,1}, 16)` | **`STATUS_PRIVILEGE_NOT_HELD`** — server silos need `SeTcbPrivilege` |

hcsshim does create *application* silos from user mode (`internal/jobobject/jobobject.go`,
`PromoteToSilo()` → `JobObjectCreateSilo = 35`, requiring **zero** running processes in the job), but
it does so for privileged HostProcess containers. Forshaw reported unprivileged app-silo creation as
an object-manager bug in 2021; **it does not reproduce here**, which suggests hardening — but no
specific fix was identified, and **whether elevation alone makes class 35 succeed was not tested —
unverified.**

**Most useful lesson: silos virtualize the *NT* namespace, and our guest does not have one.** We would
be taking undocumented-API and privilege risk to isolate names no Linux program ever touches.

Sources: [Quarkslab, *Reversing Windows Container* I & II](https://blog.quarkslab.com/reversing-windows-container-episode-i-silo.html);
Forshaw, [*Who Contains the Containers?*](https://projectzero.google/2021/04/who-contains-containers.html),
Project Zero 2021; Ionescu, *The Noble Gases of Windows Containers*, SyScan360 2017; *Windows Internals*
7th ed. Part 1 ch. 3.

### 5.3 HCS — documented, and unusable

`computecore.dll` (`HcsCreateComputeSystem`, `HcsStartComputeSystem`, `HcsCreateProcess`), legacy
`vmcompute.dll`, driven by containerd/Docker through hcsshim's `cmd/containerd-shim-runhcs-v1`.
Process-isolated containers are server silos (`"HvPartition": false`); Hyper-V-isolated containers are
utility VMs.

**It cannot host a non-Windows image.** Windows containers require a matching Windows base image
because the container must contain Windows system DLLs and registry hives; there is no empty Windows
rootfs. A mismatch yields `0xc0370101`, *"The operating system of the container does not match the
operating system of the host."* Microsoft's stated cause: *"Linux has a monolithic kernel, while in
Windows User and Kernel mode are more tightly bound."*

**Lesson: the base-image requirement is the structural statement that Windows containers are a
Windows-userland packaging mechanism, not a generic isolation substrate.**

### 5.4 Filesystem namespacing — do it in user space

| Mechanism | Verdict |
|---|---|
| **`BfSetupFilter` + `BINDFLT_FLAG_USE_CURRENT_SILO_MAPPING`** | The only real per-container mount namespace on Windows. hcsshim uses it for HostProcess containers (`internal/winapi/bindflt.go`, `internal/jobobject/jobobject.go:483 ApplyFileBinding`); the rootfs lands at `C:\hpc\<id>`. `bindfltapi.dll` and `bindflt.sys` are **present in-box on this stock host** — exports confirmed: `BfSetupFilter`, `BfSetupFilterEx`, `BfSetupFilterBatched`, `BfRemoveMapping(Ex)`, `BfGetMappings`, `BfAttachFilter`, `BfConfigureFilter`, `BfTrackWritesFromSilo`. But it is **entirely undocumented** (no SDK header, no learn.microsoft.com page) and requires a silo, therefore admin. |
| **`CreateBindLink`** (bindlink.h, Win11 24H2+) | The documented face of bindflt, but **machine-global and admin-only** — a shadow link hides content *"to all users"*. Not a namespace. |
| **`wcifs.sys`** | The container layering/COW filter. No user-mode API outside `HcsAttachLayerStorageFilter`. |
| **ProjFS** (`PrjStartVirtualizing`) | An optional Windows feature, **not present on this stock host**; designed for VFS-for-Git, not container roots. |
| **`DefineDosDevice`** | Per-**logon-session**, not per-process. Junctions and symlinks are machine-global. |

**The engine already intercepts every guest `open`/`openat`/`stat` and owns path resolution** —
`src/linux_abi/container/vfs.c` exists. A user-space VFS with its own dentry cache, mount table,
per-process root/cwd and overlay merge needs **zero kernel help, zero admin, and zero undocumented
APIs**, and delivers exactly the Linux semantics no NT filter can: case sensitivity, unlink-while-open,
`/proc`, `/sys`, `/dev`, mount namespaces. Precedents: gVisor's Sentry, and WSL1's own VFS. Every
kernel option above buys *less* fidelity for *more* risk.

**Take the user-space path.** The only mechanism worth revisiting is bindflt, and only if host-visible
mounts ever become a requirement.

### 5.5 WSL2, and why it is the wrong model for us

WSL2 is a real Hyper-V utility VM running a real Linux kernel. `\\wsl$` is served over **9P**
(`P9rdr.sys` / `p9np.dll`); virtiofs is opt-in, not the default. Microsoft's stated reasons for the
move, verbatim: *"The primary goals of this update are to
increase file system performance and add full system call compatibility"*; *"WSL 2 includes its own
Linux kernel with full system call compatibility. Benefits include: A whole new set of apps that you
can run inside of WSL, such as Docker."* Quoted gains: up to 20× unpacking a tarball, 2–5× on
`git clone` / `npm install`.

**The one scoreboard entry in our favour:** Microsoft's own comparison table still scores
*"Performance across OS file systems: WSL 1 ✅ / WSL 2 ❌."* A personality beats a VM at the host
filesystem boundary, and that is the differentiated position for a Windows-native Linux container
runtime.

**The sobering entry:** WSL1 implemented ~235 syscalls, ran **in ring 0**, and *still* needed kernel
changes (4 KiB VA granularity, fork, per-thread case sensitivity) — and it never got Docker. The long
tail is not syscall *count*, it is flag and semantic coverage, and the true blockers (namespaces,
cgroups, netfilter, `AF_PACKET`, `io_uring`, full `ptrace`, seccomp-BPF, FUSE, inotify over NTFS) have
no NT analogue. We get none of WSL1's kernel levers.

### 5.6 Pico providers are not an extension point — definitively

On Microsoft's own *Pico Process Overview* page, asked directly whether third parties can register a
pico provider, Microsoft replied **"There are no APIs available at this time"** (2016, never revised).
`PsRegisterPicoProvider` is absent from public WDK headers; registration is single-slot,
Microsoft-signer-gated, and latched before third-party drivers load. The 2025 open-sourcing confirms
it: `learn.microsoft.com/en-us/windows/wsl/opensource` lists *"`Lxcore.sys`: the kernel side driver that
powers WSL 1"* as **not** open sourced.

### 5.7 What we would actually ship

A normal user-mode Win32 process: a restricted token (untrusted integrity, no privileges) or an LPAC
AppContainer; an outer job object for memory / CPU / process count / kill-on-close;
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, `_JOB_LIST`, `_CHILD_PROCESS_POLICY` and
`WIN32K_SYSTEM_CALL_DISABLE` on child creation; **ACG off, CFG unset**. VFS, PID and network
namespaces plus cgroup semantics implemented **in the engine, in user space**.

AppContainers and restricted tokens give ACL-based confinement only — **no filesystem namespace**;
Chromium's sandbox documentation says the same about itself. Windows Sandbox is a Hyper-V container and
boots a VM per instance. `ProcessInstrumentationCallback` intercepts *NT syscall returns*, which is
irrelevant when we own the guest code stream.

---

## 6. What this changes, and the dead ends

### 6.1 Load-bearing conclusions

1. **The dual-alias W^X arena survives the port.** Measured working three ways, coherent, with a
   constant alias delta available via placeholders. The engine's existing Linux/macOS arena design is
   the low-diff path, and CoreCLR is the production precedent. Do **not** be talked into flat RWX by the
   observation that V8/JSC/QEMU ship it — they never had a dual-alias design to preserve.
2. **VEH is the only fault mechanism, and it is enough.** SEH is unavailable in this toolchain
   (measured). VEH gives the exact faulting address, a read/write flag, a writable `Rip`, and — the
   pleasant surprise — **writable AVX/AVX-512 state via `LocateXStateFeature`, provided `XSTATE.Mask`
   is set.** The engine's interpreter fault model (thread-local marker, `sigsetjmp` at the top of
   `run_block`) ports onto it directly.
3. **Every `fork()` route that spells fork as `CreateProcess` costs ≥1.5 ms, and 5 ms round trip.**
   That is measured, and it is the number that should drive the design — not any API detail.
   `RtlCloneUserProcess` is the only route that avoids it, and it is undocumented and safe only from a
   provably single-threaded moment.

### 6.2 Strategy changes

- **flinux is the blueprint to read before writing any code** — a driver-free, user-mode, COW `fork()`
  for a Linux personality on Windows, built from NT section objects plus VEH, with a per-subsystem
  `*_fork(hProcess)` / `*_afterfork_parent()` decomposition that is already the engine's shape.
- **The fork-server/zygote is now more valuable on Windows than on macOS, not less.** It was worth
  ~2 ms there; here the floor it hides is ~5 ms and growing with the engine's DLL set. It also supplies
  the discipline `RtlCloneUserProcess` requires: a warm, **provably single-threaded** process at the
  moment of the clone. If we ever want the kernel COW route, the zygote is what makes it safe.
- **Pin our own image base** (`-Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va`, measured
  effective) and use **placeholder mappings, never reserve-then-free**, for every fixed-address
  mapping. That removes both halves of the failure mode PostgreSQL retries 100 times around.
- **The 64 KiB allocation granularity must be designed for on day one.** `VirtualAlloc` silently rounds
  a fixed-address request down, so a naive `mmap` passthrough is wrong *and quiet*. Reserve 64 KiB
  granular, commit and protect 4 KiB granular.
- **Container isolation is our code, not Windows'.** Job objects for a kill-switch and a CPU cap;
  everything else — VFS, PID, network, cgroups — in user space. This is not a compromise; §5.4 argues
  it is strictly higher fidelity.

### 6.3 Dead ends — do not spend time here

| Dead end | Why |
|---|---|
| **`NtCreateProcessEx(parent=self)` + `NtCreateThread`** | Cloning still works, but you **cannot create a thread** in the clone since Windows 8.1 (`STATUS_PROCESS_IS_TERMINATING`). The Nebbett recipe that most "fork on Windows" writing quotes is dead for running code. |
| **`__try` / `__except`, including with `-fms-extensions`** | Compiles, then segfaults. Measured at `-O0` and `-O2`. |
| **Pico processes / a WSL1-shaped kernel component** | *"There are no APIs available at this time."* Not an extension point, and `lxcore.sys` was withheld from the 2025 open-sourcing. |
| **`BTCpu*` / the WOW64 binary-translator slot** | Real, and FEX and box64 use it — but machine-wide, admin-only, undocumented, and it breaks on servicing updates. Not available to a normal `.exe`. |
| **Porting `qemu-user`** | Gated on `host_os = linux` by one line in `configure`, and the assumptions behind it (`/proc/self/maps`, `MAP_FIXED_NOREPLACE`, `sigaction`/`ucontext_t`) are structural. |
| **Server silos / HCS for a Linux guest** | `SeTcbPrivilege` (measured `STATUS_PRIVILEGE_NOT_HELD`), entirely undocumented from user mode, and HCS cannot host a non-Windows image at all. |
| **`JOBOBJECT_IO_RATE_CONTROL_INFORMATION`** | Microsoft documents it as unsupported since Windows 10 1607. |
| **Expecting MSYS2 to differ from Cygwin** | Verified by diff: it does not. |
| **Setting ACG, or planning around CET** | ACG would break the JIT outright; CET cannot even be enabled for a mingw-clang binary today. |

### 6.4 Open questions

- Does `RtlCloneUserProcess` actually produce a usable child for *our* process shape? Not tested here.
  This is the single highest-value experiment remaining.
- Does Midipix's fork work on Windows 11, given §3.1's `STATUS_PROCESS_IS_TERMINATING` finding? Untested.
- Does `NtSetInformationProcess(ProcessExecuteFlags, ProcessEnableWriteExceptions)` (§4.3) work for a
  normal process on real Windows? If it does, it is a better SMC primitive than protection flipping.
- Does application-silo creation require elevation on current Windows 11? The probe here was unelevated.
- WSL1's fork cost was never published, so there is no baseline to compare any of this against.
