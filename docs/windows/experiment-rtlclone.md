# Experiment: `RtlCloneUserProcess` against the engine's real process shape

`docs/windows/prior-art-survey.md` §9 closes by naming the gap this document fills:

> Does `RtlCloneUserProcess` actually produce a usable child for *our* process shape? Not tested here.
> This is the single highest-value experiment remaining.

`docs/windows/fork-model.md` §5 measured the primitive from **C#**, against a trivial process, and listed two
consequences of that: an unmeasured `MAP_SHARED` question (§5.4, risk #1) and a thread-creation failure whose
faulting module was never identified (§5.2, risk #2). Phase 1 of its staged plan is exactly those two
measurements, "with a native mingw binary", "before writing the clone path".

This is that measurement, plus everything else that could be settled in the same harness. It is a lab report:
method, raw numbers, a verdict per property, and a recommendation. **Everything below was run.** Nothing is
inferred from the literature except where a line is explicitly marked as a code read of ntdll.

---

## 1. Verdict first

**`RtlCloneUserProcess` carries the engine's process shape. Every property that guest `fork()` needs
survived, including the two that `fork-model.md` flagged as unresolved risks.** The recommendation in
`fork-model.md` §7 — strategy A for guest fork — **stands, and is now stronger than when it was written.**

| Property | fork-model.md | Measured here | Verdict |
|---|---|---|---|
| Clone succeeds from a multithreaded parent with a JIT arena | ✔ (CLR, 10 threads) | ✔ `STATUS_SUCCESS`, 9-thread native parent, W^X arena live | **holds** |
| Child continues the calling thread, executes user code | ✔ | ✔ `STATUS_PROCESS_CLONED` (`0x129`) in the child | **holds** |
| RX alias of the dual-mapped JIT arena executes in the child | not measured | ✔ and the RW→RX **alias coupling** survives too | **new, PASS** |
| `MAP_SHARED`-equivalent sections stay genuinely shared (**risk #1**) | **unknown** | ✔ bidirectionally, for 3 of 4 mapping methods | **risk closed** |
| `__thread` on the clone's initial thread | unmeasured | ✔ correct value | **PASS** |
| Host threads in the child (**risk #2**) | ✖ `CreateThread` faulted 4/4 | ✔ **50/50 clean once USER32/IMM32 is not imported** | **risk closed, cause named** |
| Locks held by a dead peer | undecided | recoverable; both ntdll helpers work, and plain re-init works | **PASS** |
| Handles | inheritable only | confirmed, and stricter than described | **holds** |
| Address identity (R1/C2) | by construction | ✔ every region at a byte-identical VA | **PASS** |
| Cost | 2.9–3.5 ms | **2.46 ms p50** at engine-baseline size; **9.0 ms p50** at +1 GiB | **holds, with a scaling caveat** |
| Failure rate | not characterised | **0 failures in ~14,000 clone calls** | **PASS** |

The two things that changed materially versus `fork-model.md`:

1. **§5.2's thread-creation failure is not a property of cloning.** It is a property of `IMM32.DLL`'s
   `DLL_THREAD_DETACH` handler, reached only because `USER32.dll` was in the import table. Remove that one
   import and plain `kernel32!CreateThread` works in a clone, with correct `__thread`, 50/50. **C5 flips from
   ✖ to ✔ and the `SKIP_THREAD_ATTACH` + engine-managed-TLS workaround becomes unnecessary** (it also works,
   and is measured here, but it is no longer the only route).
2. **Cost scales with the size of the address space**, which nothing in the tree had measured. At the engine's
   baseline shape it is 2.5 ms; with a 1 GiB dirty guest image and 10,000 VADs it is 17 ms. That does not
   change the strategy, but it changes the `forkchurn` arithmetic and it is the number to design against.

---

## 2. Method

### 2.1 Host

| | |
|---|---|
| OS | Windows 11 Pro, build **10.0.26200.8246** (`cmd /c ver`) |
| CPU | **AMD Ryzen 7 7800X3D**, 8 cores / 16 threads, 4201 MHz |
| RAM | 32 GiB |
| Defender | **real-time protection ON**, `AMRunningMode = Normal` |
| Debugger | none attached |
| Toolchain | clang **22.1.8**, target `x86_64-w64-windows-gnu`, `C:\msys64\clang64\bin\clang.exe` |
| Link | `-lmincore` (for `VirtualAlloc2` / `MapViewOfFile3`) |

Build line, matching `docs/windows/toolchain.md` §3:

```sh
clang -O1 -g -std=c11 -Wall -Wextra -D_WIN32_WINNT=0x0A00 \
      main.c child.c shape.c -o clonetest.exe -lmincore
```

Host-specific factors worth naming: this is an 8-core desktop with real-time AV enabled and no EDR beyond
Defender; the process is not in a Job object; the harness was launched from a shell with stdout redirected to
a file, so §3.8's console test opens `CONOUT$` explicitly rather than trusting `STD_OUTPUT_HANDLE`.

### 2.2 The harness

Three files, kept in the session scratchpad
(`…/scratchpad/clonetest/{shape.h,shape.c,child.c,main.c}`):

- `shape.c` builds the parent's process shape and holds the CRT-free logger.
- `child.c` is everything that runs after the clone returns `STATUS_PROCESS_CLONED`. It allocates nothing
  through the CRT, calls no stdio, and logs through `WriteFile` on a handle opened before the clone.
- `main.c` is the parent-side driver and the statistics.

Two binaries are built from the same sources: `clonetest_nouser32.exe` (imports `KERNEL32.dll` only) and
`clonetest_user32.exe` (`-DWITH_USER32`, which calls `wsprintfW` and therefore imports `USER32.dll`). §3.5
is the diff between them.

### 2.3 The shape the parent has before it clones

Every item below is live at the instant `RtlCloneUserProcess` is called.

1. **W^X dual-alias JIT arena.** `VirtualAlloc2` 128 KiB `MEM_RESERVE_PLACEHOLDER`, split with
   `VirtualFree(MEM_PRESERVE_PLACEHOLDER)`, one 64 KiB pagefile section mapped twice with
   `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` — `PAGE_READWRITE` and `PAGE_EXECUTE_READ`. Real x86-64 machine
   code (`mov rax,rcx; add rax,1; ret`) written through RW, `FlushInstructionCache`d, and *called through RX*
   in the parent before cloning. This is `tools/windows/toolchain_probe.c`'s pattern at 64 KiB granularity.
2. **Four `MAP_SHARED`-equivalent sections**, each 64 KiB, mapped four different ways so the C3 question is
   answered per-mechanism rather than in aggregate:
   `CreateFileMappingW`+`MapViewOfFile`; `CreateFileMappingW`+`MapViewOfFile3`;
   `NtCreateSection`+`NtMapViewOfSection(InheritDisposition=ViewShare)`; and the same with `ViewUnmap`.
   Each carries a setup cookie, a parent slot, a child slot, and a counter that six parent threads increment
   continuously.
3. **Eight live peer threads.** Thread 0 holds a `CRITICAL_SECTION` and thread 1 holds an `SRWLOCK`
   exclusive, from before the clone until after it. Threads 2–7 spin, incrementing the shared-section
   counters and dirtying the private heap. Total parent thread count at clone time: **9–10** (Toolhelp count).
4. **`__thread` TLS.** `__thread uint64_t t_magic`, set to a distinctive value on every thread.
5. **Open handles**: a file, an event and a section, each created twice — once with
   `SECURITY_ATTRIBUTES.bInheritHandle = TRUE` and once `FALSE` — plus an inheritable `CONOUT$`.
6. **A 1 MiB committed private heap** filled with a position-dependent pattern (`HEAP_PATTERN ^ index`).

### 2.4 What the child does

`VirtualQuery` every region first (so an *unmapped* region is reported rather than faulted on), then: call the
parent's JIT'd code through RX; emit *new* code through RW and call it through RX; check `__thread`; verify
all 131,072 heap words; write and poll each shared view; probe each handle for presence
(`GetHandleInformation`) **and** function (`WriteFile` / `SetEvent` / `MapViewOfFile`); probe the locks; and
`NtTerminateProcess(GetCurrentProcess(), 0x5A)`. A vectored exception handler installed at child entry names
the module and offset of any first-chance exception. The parent distinguishes a clean `0x5A` exit from
`0xC0000005` from a hang (`WaitForSingleObject` timeout, then `TerminateProcess`).

Independent liveness evidence: each child appends one byte to an inherited evidence file, and the parent
checks its growth. Across the latency phases this is `1000/1000` every time — no child was counted as
"succeeded" without proof it executed user code.

---

## 3. Results, property by property

### 3.1 Does the clone succeed at all?

**Yes, unconditionally, from the full shape.**

```
RtlCloneUserProcess(RTL_CLONE_INHERIT_HANDLES) parent status = 0x00000000   STATUS_SUCCESS
status observed on the cloned thread                         = 0x00000129   STATUS_PROCESS_CLONED
parent thread count at clone time                            = 9 (main + 8 peers)
2 of those peers holding locks; 6 spinning in shared memory
```

`NTSTATUS` was `0x00000000` on **every one of ~14,000 calls** across all runs of this harness. Not one
`STATUS_*` failure, not one hang, not one address collision. The four instrumented latency phases of the
definitive run each report `attempts=1000, clone-call failures=0, hung children=0, abnormal exits=0`.

### 3.2 The JIT arena — the single most important result

```
region jit RW view   addr=0x028abcaf0000  state=COMMIT  type=MAPPED  prot=PAGE_READWRITE
region jit RX view   addr=0x028abcb00000  state=COMMIT  type=MAPPED  prot=PAGE_EXECUTE_READ
call parent-emitted code through RX alias: 42 (expect 42)  -> PASS
emit NEW code through RW, call through RX: 43 (expect 43)  -> PASS (aliases still coupled)
```

Parent addresses: `rw=0000028ABCAF0000 rx=0000028ABCB00000`. Identical in the child, to the byte.

Two distinct facts, and the second is the one that was at risk:

- The RX view is still `MEM_MAPPED` (a section view, not a privatised copy) and still `PAGE_EXECUTE_READ`,
  and the parent's translated code runs.
- **The two views are still views of the same section.** The child wrote fresh machine code through the RW
  alias at offset `0x100` and executed it through the RX alias, getting the right answer. Had the clone
  privatised the two views independently — the plausible bad outcome — the RW write would not have appeared
  through RX and the child would have executed stale bytes or faulted. It did not.

This matters more than `fork-model.md` §N1 suggests. That section argues the JIT arena does not *have* to
survive, because `jit_after_fork` (`src/translator/cache.c:1694`) sets `preserve = 0` on Linux. That remains
the right default. But the measurement says the Windows backend is not *forced* into it: the dual-alias
arrangement survives a clone intact, so `hl_arena_repair(..., preserve=1)` is available as a future option
rather than foreclosed, and — more immediately — the child cannot be poisoned by a half-cloned arena while it
runs `hl_arena_repair`.

### 3.3 Shared sections — open risk #1, closed

`fork-model.md` §9 ranks this first: *"the failures are silent and non-local: a lost futex wake, an `eventfd`
counter that diverges, two `flock` brokers that disagree."* Three independent proofs were taken.

| Mapping method | `Type` in clone | Parent's live writes visible in child | Child's write visible in parent | Parent's post-clone write visible in child |
|---|---|---|---|---|
| `CreateFileMappingW` + `MapViewOfFile` | `MEM_MAPPED` | **yes** (counter advanced 1,696,382 → 3,410,073 in 150 ms) | **yes** | **yes** |
| `CreateFileMappingW` + `MapViewOfFile3` | `MEM_MAPPED` | **yes** | **yes** | **yes** |
| `NtCreateSection` + `NtMapViewOfSection(ViewShare)` | `MEM_MAPPED` | **yes** | **yes** | **yes** |
| `NtCreateSection` + `NtMapViewOfSection(ViewUnmap)` | region is **`MEM_FREE`** | n/a | no | n/a |

The "live counter" test is the strongest of the three and it is worth stating plainly: six of the parent's
threads increment a word in each shared view continuously. The child read that word, slept 150 ms, and read
it again. **It moved.** The child is watching the parent's threads write, in real time, through the section.
That is not a copy.

Three consequences:

- **`MapViewOfFile` / `MapViewOfFile3` are sufficient.** `fork-model.md` §5.4 worried that these "do not
  expose the parameter" (`InheritDisposition`) and that the arenas might therefore have to be mapped through
  `NtMapViewOfSection` directly. They do not. The kernel32 wrappers default to `ViewShare` and the view
  survives a clone shared. The Windows backend can use `MapViewOfFile3` uniformly.
- **`ViewUnmap` is the observed failure mode, and it is loud, not silent.** The view is simply *absent* in
  the child — `MEM_FREE`, not a private copy. A child touching it takes an access violation immediately
  rather than diverging quietly. That is a much better failure mode than the one the risk register feared,
  and it means an accidental `ViewUnmap` cannot produce the "lost futex wake" scenario; it produces a crash.
- **Strategy D is now optional, not mandatory.** `fork-model.md` §7 recommends placing the R2 arenas via
  strategy D (`VirtualAlloc2` placeholder in the child + `MapViewOfFile3` with a target process handle) "so
  their sharing does not depend on clone inheritance disposition". Measured: the sharing does not need that
  protection. Strategy D remains useful for a *checkpoint-restore* child (strategy B), where there is no
  inheritance to rely on.

### 3.4 `__thread` TLS on the clone's initial thread

```
TEB.ThreadLocalStoragePointer = 0x0000028abc7d92b0
t_magic = c10e1234abcd0001   expect = c10e1234abcd0001   -> PASS
```

The static-TLS block is COW-copied with everything else and the TEB's pointer is valid. The engine's
structural `__thread` use (`g_fbk_active` at `src/linux_abi/thread.c:160`, the interpreter's fault pad, the
per-thread `struct cpu` binding) is intact on the thread that continues into the child, which is the thread
that matters — it is the guest thread that issued the fork.

### 3.5 Thread creation in the clone — risk #2, closed, with the cause named

This is the result that most changes the plan. `fork-model.md` §5.2 reports `CreateThread` faulting
`0xC0000005` 4/4 in a CLR host and says "**which module faults was not identified**… This must be re-measured
with a native mingw binary before any plan depends on either answer."

**It was measured, the module was identified, and it is avoidable.**

First, reproduction. With `USER32.dll` in the import table (`clonetest_user32.exe`), 50 runs each:

| Probe in the clone | Result |
|---|---|
| `NtCreateThreadEx(CreateFlags = SKIP_THREAD_ATTACH)` | **50/50 clean** |
| `NtCreateThreadEx(CreateFlags = SKIP_THREAD_ATTACH)` + engine-managed TLS | **50/50 clean** |
| `NtCreateThreadEx(CreateFlags = 0)` | **0/50 clean, 50/50 died `0xC0000005`** |
| `kernel32!CreateThread` | **0/50 clean, 50/50 died `0xC0000005`** |

So §5.2 reproduces exactly, in a native mingw-w64 binary, 100% deterministically — the CLR was not the cause.

Second, localisation. The vectored handler names it, identically on every occurrence:

```
!! first-chance exception 0xc0000005 at 0x7fffca317f72
   module=C:\Windows\System32\IMM32.DLL+0x0000000000007f72   (tid = the spawned thread)
   access violation: op=0 (read) target=0x01404f2d1040
```

Third — and this is the part that was missing — **the spawned thread runs to completion before the fault.**
The child log ordering is unambiguous:

```
[spawned thread] entered proc, tid=8424
[spawned thread] blocking on a CRITICAL_SECTION held by the initial thread
[initial thread] spawned-thread ran=1 tlsptr=0x1404efe5e90 tls_readback=c10e1234abcd00fe  (correct)
[spawned thread] got + released the lock; about to RETURN (thread-detach path next)
!! first-chance exception 0xc0000005 ... IMM32.DLL+0x7f72
```

The thread was created, `DLL_THREAD_ATTACH` completed, its `__thread` storage was allocated and read back
correctly, it *blocked on a `CRITICAL_SECTION` held by another thread and was correctly woken* — so NT's
wait/wake machinery works in a clone despite the TID change — and only then did it die. Two control probes
confirm the location:

| Control | Result |
|---|---|
| Same thread, but it never returns (parks forever) | **50/50 clean** |
| Same thread, but it exits via `NtTerminateThread` instead of returning | **50/50 clean** |

**The fault is in thread *detach*, not thread creation or thread attach.** `IMM32`'s `DLL_THREAD_DETACH`
handler reads a pointer that lives in memory the clone did not receive — consistent with §5.2's reasoning
about `ViewUnmap` CSR/win32k regions, but now with the module named and the phase pinned.

Fourth, the fix. `IMM32` is loaded only because `USER32` is, and `USER32` was in the import table only
because the harness called `wsprintfW`. Replacing that one call with a hand-rolled string join leaves
`KERNEL32.dll` as the sole non-apiset import. Re-run, same sources, 50 runs each:

| Probe in the clone (`clonetest_nouser32.exe`, no `USER32`) | Result |
|---|---|
| `NtCreateThreadEx(SKIP_THREAD_ATTACH)` | **50/50 clean** |
| `NtCreateThreadEx(SKIP_THREAD_ATTACH)` + engine-managed TLS | **50/50 clean** |
| `NtCreateThreadEx(CreateFlags = 0)` | **50/50 clean** |
| `kernel32!CreateThread` | **50/50 clean** |
| thread never exits | **50/50 clean** |
| thread self-terminates | **50/50 clean** |

Zero first-chance exceptions in the entire child log for that build.

So the rule for the Windows backend is a build constraint, not a fork constraint:

> **The engine image must not import `USER32.dll`** (nor anything else that drags in `IMM32`, `GDI32` or the
> rest of the win32k-dependent set) **if it is going to clone.** `wsprintfW` is the trap — it is in `USER32`,
> it looks like a CRT function, and it is easy to reach for in path handling. `docs/windows/build-system.md`
> should carry this as a link-time gate: assert the import table of the shipped engine binary contains only
> `KERNEL32` and apiset stubs.

Fifth, the escape hatch is still there and still works, and its price is now measured rather than assumed.
`fork-model.md` §5.2 notes that `SKIP_THREAD_ATTACH` "also skips TLS callbacks, so `__thread` variables are
not initialised in such a thread… Filed as a known risk, not a solved problem." Both halves confirmed, and
the second half is now solved:

```
SKIP_THREAD_ATTACH, no intervention:
  tlsptr = 0x0000000000000000     -- TEB.ThreadLocalStoragePointer is NULL; any __thread access faults

SKIP_THREAD_ATTACH + engine-managed TLS:
  manual TLS install = 1  newptr = 0x000001404f0d0000
  tls_readback = c10e1234abcd00fe (correct)  -- write then read back through __thread
  contended CRITICAL_SECTION acquired, thread exited cleanly, 50/50
```

The engine-managed path is about fifteen lines: read the image's `IMAGE_TLS_DIRECTORY` (`_tls_used`),
allocate a pointer array and a raw block, copy the template from `StartAddressOfRawData`, store the block at
`array[_tls_index]`, and write the array into `TEB.ThreadLocalStoragePointer` (`gs:[0x58]`). Caveat measured
but not solved: this initialises **only this module's** TLS slot; any other module's `__thread` access on
such a thread would still fault, so it is a fallback, not a default. With the `USER32` rule in force it is
not needed.

### 3.6 Lock state

`fork-model.md` §N3 and `src/translator/cache.c:1697-1705` — "**This is THE go/npm/cargo build hang**" — make
this concrete: a peer holding `g_jit_lock` at fork instant leaves the child deadlocked forever.

Measured with a `CRITICAL_SECTION` and an `SRWLOCK` each held by a peer thread that does not survive the
clone:

```
before repair: CS(free)=ACQUIRED  CS(held-by-dead-peer)=BLOCKED
               SRW(free)=ACQUIRED SRW(held-by-dead-peer)=BLOCKED
raw state:     CS.LockCount=0xfffffffe  CS.RecursionCount=1  CS.OwningThread=0x5bb8 (a dead tid)
               SRW.Ptr=0x0000000000000001 (locked)
```

Exactly the documented hazard: uncontended locks are fine (they are plain atomics), locks held by a dead peer
are permanently unacquirable.

**Both ntdll helpers work, and they do not do what their name suggests.** Disassembling them on this host
(`ntdll.dll`, build 10.0.26200) settles the semantics:

```
RtlUpdateClonedCriticalSection:            RtlUpdateClonedSRWLock:
  mov  rax, gs:[0x30]        ; TEB           neg  edx                ; edx = Shared
  mov  rdx, [rax+0x48]       ; my TID        sbb  rax, rax
  mov  [rcx+0x10], rdx       ; OwningThread  and  eax, 0x10
  mov  dword [rcx+0x08], -2  ; LockCount     inc  rax                ; 1 (excl) or 0x11 (shared)
  mov  dword [rcx+0x0c], 1   ; Recursion     mov  [rcx], rax
  mov  qword [rcx+0x18], 0   ; LockSemaphore ret
  ret
```

Neither **unlocks**. Both **re-own the lock to the calling thread in a clean, waiter-free state** — the
critical section becomes "held by me, recursion 1, no semaphore"; the SRW lock becomes "held exclusive, no
wait blocks". The intended recipe is repair-*then*-release, and that is what was verified:

```
raw state after repair: CS.LockCount=0xfffffffe  CS.Recursion=1  CS.Owner=0x0b88 (= this tid)
documented recipe (repair then release): CS=FREE+ACQUIRED   SRW=FREE+ACQUIRED
```

A single `LeaveCriticalSection` / `ReleaseSRWLockExclusive` after the helper leaves both locks genuinely free
and re-acquirable. (An earlier iteration of this harness reported `RtlUpdateClonedSRWLock` as a no-op; that
was a test bug — it called `TryAcquire` after the repair and read "still held" as failure. The disassembly
and the repair-then-release probe correct it. Recorded here because the wrong reading is the intuitive one.)

**But the engine does not need either helper.** Plain re-initialisation works and is what
`jit_after_fork`/`thread_after_fork` already do on Linux:

```
after plain InitializeSRWLock        : SRW(held) = ACQUIRED
after plain InitializeCriticalSection: CS(held)  = ACQUIRED
```

So `fork-model.md` §N3's position holds on Windows unchanged: the child wants engine locks *reset*, and
`InitializeSRWLock` / `InitializeCriticalSection` in `fork_child_hooks` is the whole story. The `Rtl*`
helpers matter only for locks the engine does not own and cannot re-init — and for those, calling
`RtlCloneUserProcess` **without** `NO_SYNCHRONIZE` already takes the loader/PEB/TLS/FLS/heap locks before
cloning, which is why that is the right default (§4.1 shows it costs ~0.2 ms and buys this).

One more datum, from §3.5: a thread created *inside* the clone blocked on a `CRITICAL_SECTION` held by the
clone's initial thread and was correctly woken when it was released, 50/50. `SRWLOCK`/`CRITICAL_SECTION`
blocking is built on `NtWaitForAlertByThreadId` keyed on thread IDs, and the survey (§3.1) lists that as a
known breakage. **Within a single clone, with threads created after the clone, it works.** What breaks is
inheriting a *wait* across the clone boundary, which cannot happen — only the calling thread survives.

### 3.7 Handles

```
clone flags = RTL_CLONE_INHERIT_HANDLES (0x2):
  file(inheritable)      value=0x00dc  present=1  flags=0x1  functional=1   (WriteFile succeeded)
  file(NOT inheritable)  value=0x00e0  present=0  flags=0x0  functional=-1  (ERROR_INVALID_HANDLE)
  event(inheritable)     value=0x00e4  present=1  flags=0x1  functional=1   (SetEvent succeeded)
  event(NOT inherit.)    value=0x00e8  present=0  flags=0x0  functional=-1
  section(inheritable)   value=0x00ec  present=1  flags=0x1  functional=1   (MapViewOfFile succeeded)

clone flags = 0:
  ALL FIVE absent, including the inheritable ones.
```

`fork-model.md` §5.1's claim is confirmed and is if anything stricter than stated: with `flags = 0` **nothing
is inherited at all**, not even handles marked `HANDLE_FLAG_INHERIT`. Both conditions are required — the
clone flag *and* per-handle inheritability.

Two details that matter for the port and were not previously recorded:

- **Handle values are preserved.** The inherited handle occupies the same numeric slot in the child. This is
  load-bearing: the engine's globals and its OFD table hold handle values, and those globals are COW-copied
  verbatim. Handle identity therefore needs no fixup, which is a real simplification of `fork_child_hooks`.
- Non-inheritable handles leave *holes* — the slot is empty, not occupied by a stale object. A later
  `CreateFileW` in the child could in principle reuse that numeric value; not measured, worth knowing.

`CreateFileW` inside a clone works (the `flags = 0` child reopened its log by path and wrote to it).

### 3.8 Console

`fork-model.md` §5.4 lists console behaviour as unmeasured. Measured, using a real `CONOUT$` handle opened by
the parent (`AllocConsole` first, because the harness runs with stdout redirected):

```
inherited genuine CONOUT$ handle in the clone:
  GetConsoleMode              ok=0  gle=6 (ERROR_INVALID_HANDLE)
  WriteFile                   ok=0  gle=6
  GetConsoleScreenBufferInfo  ok=0  gle=6

the documented dance:
  FreeConsole()                              -> 1
  AttachConsole(ATTACH_PARENT_PROCESS)       -> 1
  CreateFileW(L"CONOUT$")                    -> 0x14, gle=0
  GetConsoleMode on it                       -> ok=1, mode=0x3
```

**A console handle is dead in a clone; a redirected *file* handle on `STD_OUTPUT_HANDLE` is not** (that
`WriteFile` succeeded and 25 bytes landed). The `FreeConsole` + `AttachConsole(ATTACH_PARENT_PROCESS)`
reattachment dance works and restores a fully functional console handle. For the engine this is a small,
bounded fixup in `fork_child_hooks` on Windows, needed only when a guest's stdio is attached to a console
rather than a pipe or file — which for the compat corpus it essentially never is.

### 3.9 `LoadLibrary` in a clone

Also listed unmeasured in `fork-model.md` §5.4 ("the probe run that tested it died in the earlier
`CreateThread` stage").

```
LoadLibraryW(L"winmm.dll")  -- a DLL not already loaded
  -> NULL, gle = 6 (ERROR_INVALID_HANDLE)
25/25 runs; the child did NOT crash and did NOT hang, and went on to exit cleanly 0x5A.
```

Confirmed unavailable, as the literature says — but it **fails cleanly** on this build rather than
deadlocking on the loader lock. `fork-model.md` §10's rule stands unchanged ("the engine must have every DLL
it will ever need resident before the first clone"), and the failure is at least diagnosable rather than a
hang.

### 3.10 Nested clones

A guest that forks inside a forked child is ordinary (`make -j`, shells), and Linux nested fork already has
its own scar tissue in the tree (`src/translator/cache.c:1710-1719`).

```
clone-from-a-clone: RtlCloneUserProcess again  -> status = 0x00000000, 2.365 ms
[grandchild] pid=16412  JIT=42 (expect 42)  tls=c10e1234abcd0001 (expect c10e1234abcd0001)
grandchild wait=OK exit=0x5a
child's own JIT still works after nesting: 42
25/25 runs clean.
```

The grandchild executes the JIT arena and has correct `__thread`. The child's own arena is undisturbed by
having cloned. A second-generation clone costs the same as a first-generation one.

### 3.11 Address identity and COW divergence

Every region reported by the child sits at the byte-identical address the parent printed — JIT RW/RX, the
private heap, all four shared views. This is by construction (the clone is a COW replica) but it is the R1/C2
requirement and it was checked rather than assumed.

```
private heap: 0 pattern mismatches out of 131,072 words
child wrote MAGIC_CHILD to heap[0]; parent's heap[0] afterwards = a5a5c3c35a5a3c3c (unchanged)
parent's JIT still returns 42 after the child ran and exited
```

COW divergence is correct in both directions: the child sees the parent's memory, and neither perturbs the
other's private pages.

---

## 4. Latency

Definitive run: `clonetest_nouser32.exe 1000 50`. n = 1000 per phase. **`clone call`** is the parent-side
duration of `RtlCloneUserProcess` alone; **`round trip`** is from that call until the child's process object
signals.

### 4.1 At the engine's baseline shape (~2 MiB private, ~40 VADs, 9 threads)

| Phase | n | min | p50 | p90 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| **E1** unguarded, clone call | 1000 | 2.229 | **2.462** | 2.968 | 3.241 | 17.628 | 2.622 |
| **E1** unguarded, round trip | 1000 | 2.679 | **2.974** | 3.514 | 3.769 | 18.176 | 3.124 |
| **E2** guarded, clone call | 1000 | 2.070 | **2.248** | 2.431 | 2.770 | 19.943 | 2.318 |
| **E2** guarded, round trip | 1000 | 2.491 | **2.722** | 2.955 | 3.296 | 20.375 | 2.796 |
| **E3** `NO_SYNCHRONIZE`, clone call | 1000 | 2.203 | **2.650** | 3.335 | 4.525 | 16.967 | 2.788 |
| **E3** `NO_SYNCHRONIZE`, round trip | 1000 | 2.590 | **3.141** | 3.826 | 5.001 | 17.511 | 3.273 |
| **F** `CreateProcess` call only | 50 | 1.345 | **1.504** | 1.659 | 4.358 | 4.358 | 1.564 |
| **F** `CreateProcess` round trip | 50 | 3.776 | **4.115** | 4.492 | 7.547 | 7.547 | 4.187 |

All in milliseconds. The `max` column in every phase is a single outlier attributable to scheduler noise on a
box with Defender live; p99 is the honest tail.

Read against `fork-model.md` §5 and `prior-art-survey.md` §2.3:

- **2.46 ms p50 versus the C# probe's 2.90–3.46 ms.** The native binary is faster, as expected — fewer
  modules, smaller address space. §5's headline number was pessimistic, not optimistic.
- **The clone is *slower* to issue than `CreateProcess` (2.46 ms vs 1.50 ms) but faster round trip
  (2.97 ms vs 4.12 ms).** That is the shape to design around and it is not what the survey's framing
  suggests. `RtlCloneUserProcess` is not a cheap trick that beats `CreateProcess` on raw process creation; it
  wins because the child is *already the engine, warm*, with the guest image, the arena and the whole heap in
  place. `CreateProcess`'s 1.5 ms buys you an empty process that then has to load the engine, run
  `__attribute__((constructor))` registration, initialise, load the guest ELF and re-translate. The clone's
  2.46 ms buys you a child that is one instruction from resuming the guest.
- **Neither is anywhere near a Linux `fork()`'s tens-to-hundreds of microseconds.** §6.1's C6 entry
  ("~3.0–3.5 ms measured, vs. a Linux fork's tens-to-hundreds of µs") stands. A guest that forks in a tight
  loop will be roughly 10–100× slower on Windows than on Linux. That is a property of the platform, not of
  the choice of primitive, and it is the same conclusion `prior-art-survey.md` §2.3 reached from the
  `CreateProcess` number alone.
- **`NO_SYNCHRONIZE` is not worth taking.** It is *slower* here (2.650 vs 2.462 p50) and has a worse tail
  (p99 4.525 vs 3.241), while giving up the loader/PEB/heap-lock quiescence that §3.6 relies on. Keep the
  default.

### 4.2 The guarded (zygote/forkserver) variant

This is the question `fork-model.md` §6.3 poses and it now has a number rather than an argument.

Guarded = all eight peers parked on an event, no peer holding a lock, only the cloning thread runnable.

| | unguarded | guarded | delta |
|---|---|---|---|
| clone call p50 | 2.462 | 2.248 | **−8.7%** |
| clone call p99 | 3.241 | 2.770 | −14.5% |
| round trip p50 | 2.974 | 2.722 | −8.5% |
| **clone-call failures** | **0 / 1000** | **0 / 1000** | — |
| **hung children** | **0 / 1000** | **0 / 1000** | — |
| **abnormal exits** | **0 / 1000** | **0 / 1000** | — |

**Quiescing the process before cloning is a ~9% latency optimisation and a ~15% tail-latency optimisation. It
is not a reliability measure, because there is nothing to make more reliable — the unguarded rate is already
zero over a thousand attempts with two locks held by peers that vanish.**

That is the answer to the forkserver question, and it is the answer `fork-model.md` §6.3 predicted on
different grounds: **a zygote/forkserver is not mandatory on Windows for guest fork.** It remains right for
engine *launch* (§2.1) and launch acceleration (§2.2), where the process genuinely is single-threaded and
cold-start cost dominates. For guest `fork()` it buys 200 µs and cannot be arranged anyway — §6.3's argument
that the quiescence precondition "is *known to be violated* by the workloads that matter" is unaffected by
this measurement; the measurement merely shows the violation is harmless.

### 4.3 Cost versus address-space size — a number nothing in the tree had

The baseline shape is ~2 MiB. A real guest is not. Ballast was added cumulatively to the parent — dirty
private committed memory, then separate 64 KiB VADs with alternating protections so they cannot coalesce.

| Configuration | clone call p50 | clone call p99 | round trip p50 | round trip mean |
|---|---|---|---|---|
| baseline (~2 MiB, ~40 VADs) | 2.462 | 3.241 | 2.974 | 3.124 |
| **+64 MiB** dirty private | 3.213 | 4.889 | 4.234 | 4.349 |
| **+256 MiB** dirty private | 4.154 | 6.043 | 7.021 | 7.145 |
| **+1 GiB** dirty private | 9.021 | 11.879 | 17.868 | 18.514 |
| +1 GiB and **+2,000 VADs** | 10.136 | 13.629 | 20.362 | 21.306 |
| +1 GiB and **+10,000 VADs** | 16.643 | 23.171 | 33.513 | 34.697 |

n = 1000 per row, 0 failures per row. Milliseconds.

Reading:

- **Cost scales with committed memory even though the copy is COW** — roughly **6.6 µs per MiB** of dirty
  private memory on the clone call (1 GiB adds 6.6 ms). The kernel is not copying pages; it is walking the
  VAD tree and reworking page-table state to mark the range copy-on-write. It is cheap per byte, but it is
  not free.
- **VAD count costs about as much as bytes.** 10,000 extra VADs added 7.6 ms on top of the same 1 GiB —
  ~0.76 µs per VAD. A guest with a heavily fragmented `mmap` map (a JVM, a Go runtime with many arenas) pays
  for it. This is the term to watch: the engine's own logical-VMA tracking has no direct control over how
  many *host* VADs the guest's mappings become, and `MAP_FIXED` handling at 64 KiB granularity
  (`prior-art-survey.md` §2.1) tends to increase, not decrease, the count.
- **Round trip roughly doubles the clone call at large sizes** because the *child's* teardown also has to
  walk that address space. For a guest fork that is followed by `execve` (the common `fork`+`exec` idiom),
  the teardown cost is on the critical path.
- **Compare against strategy B/D's eager copy.** `fork-model.md` §5.3 measured `WriteProcessMemory` at
  4713 MiB/s = ~212 µs/MiB, and §6.2 costs a 256 MiB guest at ~55 ms. Cloning the same 256 MiB costs
  **4.15 ms** on the call and 7.02 ms round trip — **~13× cheaper round trip, ~51× cheaper on the call.**
  The gap in favour of strategy A over strategies B and D is real and it *widens* with guest size, because
  clone is 6.6 µs/MiB against eager copy's 212 µs/MiB.

---

## 5. Failure rate and flakiness

Every number in §3 and §4 is from a run with an explicit attempt count and an explicit failure count. The
harness never reports a best run.

| | |
|---|---|
| Total `RtlCloneUserProcess` calls in the definitive run | **8,404** |
| Total across all runs of this harness | **≈14,000** |
| Non-`STATUS_SUCCESS` returns | **0** |
| Hung children (10 s timeout) | **0** |
| Address collisions / regions at unexpected VAs | **0** |
| Abnormal exits in the `USER32`-free build | **0** |
| Abnormal exits in the `USER32` build, thread-detach probes | **100/100 — deterministic, not flaky** |

The only failure mode found in the entire experiment is §3.5's, and it is 100% reproducible in one direction
and 100% absent in the other. There is no flakiness to quantify: this primitive was either completely
reliable or completely broken, never intermittent, in ~14,000 trials.

Caveats on that claim, stated because they bound it:

- One host, one CPU vendor (AMD), one Windows build, one AV product. `prior-art-survey.md` §3.1 flags
  debuggers, EDR and Job objects as risk factors; none of the three was exercised beyond Defender's
  real-time filter being on.
- The harness's parent is small compared to a real engine: ~40 VADs and a handful of modules at baseline.
  §4.3 probes the memory dimension but not the *module* dimension — an engine that ends up with 30 loaded
  DLLs has 30 sets of `DLL_THREAD_DETACH` handlers, and §3.5 shows that exactly one bad one is enough.
- The clone was always issued from the main thread. The engine will issue it from an arbitrary guest thread
  deep in `run_block`. Nothing measured suggests that matters (only the calling thread survives either way),
  but it was not tested.

---

## 6. What this changes in `fork-model.md`

| Section | Claim | Status after this experiment |
|---|---|---|
| §5.4 / §9 risk 1 | `MAP_SHARED` arenas may not survive as shared — "**Measure this first**" | **Measured. They do.** Risk 1 is closed. `MapViewOfFile3` is sufficient; `NtMapViewOfSection` is not required. |
| §5.2 / §9 risk 2 | `CreateThread` faults in a clone; only `SKIP_THREAD_ATTACH` works, and it breaks `__thread` | **Superseded.** The fault is `IMM32!DLL_THREAD_DETACH`, reached only via a `USER32` import. Without it, plain `CreateThread` works 50/50 with correct `__thread`. |
| §6.1 C3 | "**unknown** — the deciding question" | **✔** |
| §6.1 C5 | "✖ as-is; ✔ with `NtCreateThreadEx(SKIP_THREAD_ATTACH)` and engine-managed TLS" | **✔ as-is**, given the no-`USER32` build rule. The `SKIP_THREAD_ATTACH` path also works and engine-managed TLS is demonstrated. |
| §6.1 C6 | ~3.0–3.5 ms | **2.46 ms p50 at baseline**; add ~6.6 µs/MiB and ~0.76 µs/VAD for a real guest. |
| §7 | Guest fork → strategy A, arenas placed by strategy D | **Strategy A confirmed. The strategy-D arena placement is no longer required** for correctness; keep it only for strategy B. |
| §7.1 Phase 1 | "verify the C3 question with a native mingw binary, and re-measure §5.2 there… **Do them before writing the clone path.**" | **Phase 1 is complete.** Both answers are favourable. Phase 2 is unblocked. |
| §10 | `LoadLibrary` in a clone will not work | **Confirmed** — fails cleanly with `ERROR_INVALID_HANDLE`, no hang. |
| §5.4 | Console behaviour unmeasured | **Measured.** Inherited console handles are dead; `FreeConsole` + `AttachConsole(ATTACH_PARENT_PROCESS)` + reopen `CONOUT$` restores them. |

New constraint, not previously in any document, and the most actionable output of this experiment:

> **The shipped Windows engine binary must import `KERNEL32.dll` and apiset stubs only.** An import of
> `USER32.dll` — most easily acquired by calling `wsprintfW` — pulls in `IMM32.DLL`, whose
> `DLL_THREAD_DETACH` handler access-violates in a clone the first time any thread created in that clone
> exits. This should be a link-time or CI gate in `docs/windows/build-system.md`, checked with
> `llvm-readobj --coff-imports`, because the failure is 100% deterministic but only appears after a guest
> forks *and then* creates and joins a thread — i.e. in the JVM/Go/Node workloads, not in the smoke tests.

---

## 7. What this still does not measure

Honest list, so that nothing here is over-read:

- **Behaviour under a debugger, under third-party EDR, and inside a Job object** with
  `JOB_OBJECT_LIMIT_ACTIVE_PROCESS`. All three are flagged by the literature; none was exercised.
- **A clone issued from a non-main thread deep in a C call chain**, which is what the engine will actually do.
- **A realistic module set.** §3.5 proves one bad `DLL_THREAD_DETACH` handler is fatal; the engine's final DLL
  set is not yet known.
- **Guest-visible correctness.** This harness proves the *mechanism* carries the *shape*. It does not run
  `fork_child_hooks`, does not exercise the 25 hooks in `syscall/proc.c:267-324`, and does not run a single
  compat case. `fork-model.md` §8's 245 fork-dependent cases remain the real test.
- **Handle-slot reuse** after a non-inheritable handle leaves a hole in the child's table.
- **Sustained fork churn under memory pressure.** All runs were on a 32 GiB box with ample free memory; the
  1 GiB ballast rows never touched the pagefile hard.
- **ARM64 Windows**, where `FlushInstructionCache` stops being free and the dual-alias arrangement has real
  cache-coherency work to do.

---

## 8. Recommendation

**Keep `fork-model.md` §7 exactly as written for guest `fork()`: strategy A, `RtlCloneUserProcess`, called
from the guest thread issuing the fork.** It is the only strategy that continues the calling thread at its
exact continuation, and every property the engine needs from it has now been verified against the engine's
real process shape rather than a trivial one. The checkpoint/restore fallback (strategy B) is **not**
needed as the primary path and there is no measurement here that would justify promoting it.

Adjustments to the plan, in order of importance:

1. **Add the no-`USER32` link-time gate** to `docs/windows/build-system.md` before any clone code is written.
   It is one CI assertion and it prevents the single failure mode this experiment found.
2. **Drop the strategy-D arena placement from the guest-fork path.** §3.3 shows `MapViewOfFile3` views survive
   a clone shared, bidirectionally, on all three sane mapping methods. Map the R2 arenas the ordinary way.
   Never use `NtMapViewOfSection` with `ViewUnmap`.
3. **Keep strategy B as the differential oracle** (`fork-model.md` §7.1 phase 3). Its value was never that A
   might not work; it is that two independent implementations can be diffed case-for-case, and
   `checkpoint.c` already exists. Nothing here changes that.
4. **Demote the zygote for guest fork explicitly.** §4.2 measures the guarded variant at −9% latency and
   identical (zero) failure rate. The forkserver shape stays where `fork-model.md` §7 already put it — engine
   launch and launch acceleration — and does not become a correctness requirement.
5. **Budget for §4.3.** A guest fork is ~2.5 ms at engine baseline and ~9 ms with a 1 GiB image. Fork-heavy
   guests will be 10–100× slower than on Linux. That is a platform property, it is not fixable by choosing a
   different primitive, and it should be stated in `docs/windows/README.md` as an expected-performance note
   rather than discovered later as a regression.

**If one sentence has to travel: `RtlCloneUserProcess` is viable for guest fork on this host, the two open
risks in `fork-model.md` are both closed favourably, and the only thing standing between here and a working
clone path is a build rule about `USER32`.**

---

## 9. Reproducing

Sources are in the session scratchpad at
`…/scratchpad/clonetest/{shape.h,shape.c,child.c,main.c}` — **worth moving into `tools/windows/` if the
Windows lane is taken further**; the harness is self-contained, has no engine dependency, and would serve as
a regression gate for the two properties most likely to break under a Windows servicing change (§3.3 shared
sections, §3.5 thread creation).

```sh
# the shipping shape: KERNEL32 only
clang -O1 -g -std=c11 -Wall -Wextra -D_WIN32_WINNT=0x0A00 \
      main.c child.c shape.c -o clonetest.exe -lmincore

# the negative control: imports USER32, and therefore IMM32
clang -O1 -g -std=c11 -Wall -Wextra -D_WIN32_WINNT=0x0A00 -DWITH_USER32 \
      main.c child.c shape.c -o clonetest_user32.exe -lmincore -luser32

llvm-readobj --coff-imports clonetest.exe | grep Name:   # must show KERNEL32 + apiset only

./clonetest.exe <latency-iterations> <probe-repetitions>   # e.g. 1000 50
```

Parent-side results go to stdout; child-side detail goes to `%TEMP%\clonetest_child.log`, written through an
inherited `FILE_FLAG_WRITE_THROUGH` handle so a child that dies mid-probe still leaves its evidence.
