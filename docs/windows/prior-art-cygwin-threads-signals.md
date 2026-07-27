# Prior art: Cygwin's threads, signals, and select

What Cygwin actually does to provide POSIX threads, POSIX signals, and readiness-based I/O multiplexing on
top of Win32, read out of the source, and what of it we should reuse for the native Windows host.

This is a reading of the code, not a benchmark. Every mechanism below is cited to `file:line`. Where a claim
is a derivation from the code rather than something the code states or something I measured, it says so.

**Source pinned at** `cygwin/cygwin` commit `fa7b0cd4d530c54210106b9b16dd11433b17a46a`, 2026-07-22, cloned to
the session scratchpad. Paths are relative to `winsup/cygwin/` unless noted. Line numbers drift; the claims
were checked against that commit.

**Companion document:** `docs/windows/prior-art-cygwin-fork.md` covers `fork()`. Fork appears here only where
it interacts with threads and signals, which is often.

---

## 1. The one-paragraph version

Cygwin does not have signals. It has a **per-process helper thread** (`wait_sig`) reading a **named pipe**,
which decides what a "signal" means, and then either (a) sets a flag in the target thread's TLS if that
thread is currently inside a Cygwin function, or (b) `SuspendThread`s the target, rewrites its `CONTEXT` to
point at an assembly trampoline, and `ResumeThread`s it. Hardware faults are a separate path: a **table-based
SEH handler** installed in the outermost frame of every Cygwin thread converts `EXCEPTION_RECORD` to
`siginfo_t` and runs the handler **on the faulting thread inside the exception dispatch**. `select()` is a
third, unrelated machine: it rebuilds a per-descriptor record chain on every call, starts one **polling
helper thread per descriptor type**, and blocks the caller in `WaitForMultipleObjects` until a helper says
something looks ready — then re-polls everything to confirm.

All three are load-bearing for Cygwin's goal (run arbitrary unmodified POSIX programs). Two of the three are
overkill for ours.

---

## 2. Signals

### 2.1 The problem Cygwin is solving

Win32 has no asynchronous, thread-directed, maskable, queued, user-handler-invoking interrupt for a *running*
thread. It has: console control handlers (`SetConsoleCtrlHandler`), APCs (only delivered at alertable waits),
SEH/VEH (only on exceptions), and `SuspendThread`/`SetThreadContext` (arbitrary, dangerous). Cygwin builds
POSIX signals out of the last one plus a lot of bookkeeping.

### 2.2 The `sigproc` thread and the signal pipe

`sigproc_init()` (`sigproc.cc:523-548`) runs once per process from `dll_crt0_0` (`dcrt0.cc:766-767`):

- creates a **named pipe pair** via `fhandler_pipe::create (sa, &my_readsig, &my_sendsig, PIPE_DEPTH *
  sizeof (sigpacket), "sigwait", PIPE_ADD_PID)` (`sigproc.cc:528-530`). `PIPE_DEPTH` is
  `wincap.allocation_granularity () / sizeof (sigpacket)` (`sigproc.cc:32`) — 64 KiB of pipe buffer. By my
  reading of the struct layout (`sigproc.h:43-58`, `siginfo_t` at `include/cygwin/signal.h:236-282` with
  `__SI_PAD_SIZE 32`) a `sigpacket` is 200 bytes, so ≈327 in-flight packets. That is a derivation, not a
  measurement.
- publishes the write end as `myself->sendsig` in the shared `_pinfo` (`sigproc.cc:537`), so *other processes*
  can find it.
- creates `my_pendingsigs_evt`, `sigflush_evt`, `sigflush_done_evt` (`sigproc.cc:538-540`).
- starts the signal thread: `new cygthread (wait_sig, cygself, "sig")` (`sigproc.cc:547`).

**Sending** is `sig_send (_pinfo *p, siginfo_t& si, _cygtls *tls)` (`sigproc.cc:603-891`). Two cases:

*To another process* (`sigproc.cc:646-719`): `OpenProcess (PROCESS_DUP_HANDLE, ...)` on the target, then
`DuplicateHandle` the target's `sendsig` into ourselves, then `SetNamedPipeHandleState (sendsig, PIPE_NOWAIT)`
— with the comment "Yes, I know MSDN says not to use this. We can't ever block here because it causes a
deadlock when debugging with gdb" (`sigproc.cc:691-697`). So sending a signal to another process requires
`PROCESS_DUP_HANDLE` rights on it. That is the whole security model of `kill(2)` under Cygwin.

*To ourselves*: `sendsig = my_sendsig` (`sigproc.cc:644`).

Either way the send is serialized by a **named mutex** `shared_name (mtx_name, "sig_send", p->pid)`
(`sigproc.cc:769-772`), then `WriteFile` in a retry loop of up to 100 attempts with `Sleep (10)` between
partial writes (`sigproc.cc:805-814`) — so a full pipe can stall a sender for up to a second.

If the signal is to ourselves, the sender then **blocks** on a per-send `wakeup` event with a 60 s timeout
(`WSSC`, `sigproc.cc:29`; wait at `sigproc.cc:844-849`), and finally calls
`_my_tls.call_signal_handler ()` itself (`sigproc.cc:880-881`). This is why `raise()`/`kill(getpid())` is
synchronous, as POSIX requires — the *sending* thread runs the handler, not the signal thread.

**Receiving** is `wait_sig()` (`sigproc.cc:1467-1677`), an infinite `ReadFile` loop on `my_readsig`. It:

- distinguishes real signals (`si_signo > 0`) from internal pseudo-signals `__SIGFLUSH`, `__SIGPENDING`,
  `__SIGCOMMUNE`, `__SIGHOLD`, `__SIGTHREADEXIT`, … (`sigproc.h:14-27`),
- queues real signals through `pending_signals::add` (`sigproc.cc:1404-1463`), which implements the POSIX
  ordering rules explicitly, quoting `signal(7)` in comments: RT signals ordered lowest-number-first, standard
  signals given priority over RT signals, non-`SA_SIGINFO` standard signals collapsed to one instance,
- then drains the queue calling `sigpacket::process()` on each (`sigproc.cc:1632-1651`), stopping early on an
  RT or `SA_SIGINFO` signal to preserve ordering,
- if `ReadFile` ever fails, it does `Sleep (INFINITE)` — "Assume were exiting. Never exit this thread"
  (`sigproc.cc:1498-1499`).

`sigpacket::process()` (`exceptions.cc:1532-1701`) is the POSIX policy engine: pick a target thread
(`cygheap->find_tls`), honour `SIG_IGN`/`SIG_DFL`/stop/continue/exit dispositions, and finally call
`setup_handler()` (`exceptions.cc:1694`).

**Note the thread count.** A hello-world Cygwin process already has: the main thread, `wait_sig`, and one
`proc_waiter` cygthread per live child (`pinfo.cc:1240-1326`, spawned at `pinfo.cc:1336`). Add helper threads
per `select()` call (§4) and per timerfd/POSIX timer.

### 2.3 Interrupting a target thread: the two paths

`sigpacket::setup_handler (void *handler, struct sigaction& siga, _cygtls *tls)`
(`exceptions.cc:1047-1124`) is where the interesting part lives. It loops
`CALL_HANDLER_RETRY_OUTER` (10) × `CALL_HANDLER_RETRY_INNER` (10) times (`exceptions.cc:34-35, 1060-1062`),
with a `yield()` between inner attempts and `Sleep (1)` between outer ones, then gives up and leaves the
signal pending.

**Path A — target is inside Cygwin (`tls->incyg`).** No hijack at all. `interrupt_setup()` is called
directly (`exceptions.cc:1065-1073`), which pushes the address of the assembly trampoline `sigdelayed` onto
the thread's **TLS shadow stack** (`exceptions.cc:1014`) and sets `current_sig`, `func`, `deltamask`,
`infodata`. The target thread will run the handler when it returns from the Cygwin call it is in. It is also
woken via `set_signal_arrived()` (`exceptions.cc:1031-1032`), which sets a per-thread auto-reset event
(`cygtls.h:283-286`) that every blocking Cygwin wait includes in its `WaitForMultipleObjects` set
(`cygwait.cc:38-46`).

The shadow stack exists because of `_sigfe`/`_sigbe`: **every** exported Cygwin function marked `SIGFE` in
`cygwin.din` is wrapped by generated assembly (`scripts/gendef:151-190`). `_sigfe` spin-acquires
`_cygtls.stacklock`, `xadd`s `_cygtls.stackptr`, swaps the real return address onto the shadow stack and
substitutes `_sigbe`, and increments `_cygtls.incyg` — all reached through `%gs:8` (the TEB `StackBase`),
because `_cygtls` lives at `StackBase - __CYGTLS_PADSIZE__` (`cygtls.h:307-308`). This is a fixed tax on
every Cygwin libc entry point.

**Path B — target is running user code.** `setup_handler` does the classic dangerous thing
(`exceptions.cc:1085-1108`):

```
res = SuspendThread (hth);
cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
GetThreadContext (hth, &cx);
interrupted = tls->interrupt_now (&cx, si, handler, siga);
ResumeThread (hth);
```

`_cygtls::interrupt_now` (`exceptions.cc:950-1009`) first refuses if the thread is in Cygwin or "in the
kernel": `inside_kernel()` (`exceptions.cc:451-492`) does a `VirtualQuery` on the suspended `Rip`, then
`GetModuleFileNameW` on the containing module and a case-insensitive prefix compare against the Windows
system directory — with a special case because "Calling GetModuleFilename on ntdll.dll can hang"
(`exceptions.cc:471-473`). If the thread is inside a system DLL, delivery is abandoned and retried.

If it is safe, the redirection is three lines (`exceptions.cc:1001-1006`):

```
DWORD64 &ip = cx->_CX_instPtr;
push (ip);                       /* real PC onto the TLS shadow stack */
interrupt_setup (si, handler, siga);
ip = pop ();                     /* ... and sigdelayed is now the return address */
SetThreadContext (*this, cx);
```

That is: push the interrupted PC, push `sigdelayed`, then set `Rip` to `sigdelayed`. On resume the thread
enters `sigdelayed`, which saves *everything* — all GPRs, flags, and the full FPU/SSE/AVX state via
`fxsave64` or `xsave64` chosen by `cpuid`/`xgetbv` at runtime (`scripts/gendef:192-329`, save at 236-269,
restore at 296-307) — calls `_cygtls::call_signal_handler`, restores, and returns to the original PC.

**Path B is still being fixed.** Since commit `b0a9b628` (2025-06-26, "Cygwin: signal: Do not suspend myself
and use VEH") `interrupt_now` contains an extra dance on x86-64 (`exceptions.cc:963-999`): before rewriting
`Rip`, it registers a temporary VEH via `AddVectoredExceptionHandler`, sets the trap flag
(`cx->EFlags |= 0x100`), resumes the thread for exactly one instruction, waits up to 10 ms on
`RtlWaitOnAddress` for the VEH to report it caught the single-step, then re-suspends and re-reads the
context. The stated reason: "When the Rip points to an instruction that causes an exception, modifying Rip
and calling ResumeThread() may sometimes result in a crash." Thirty years in, the hijack still needs a
single-step trampoline to be safe. Treat that as the headline finding about this technique.

### 2.4 Running the handler: `call_signal_handler`

`_cygtls::call_signal_handler` (`exceptions.cc:1756-1979`) is the common tail of both paths, called from
`sigdelayed`, from `cygwait` (`cygwait.cc:94`), from `sig_send` (`sigproc.cc:881`), and from blocking
syscalls directly (`wait.cc:95`, `select.cc:456`). It:

- pops `sigdelayed` off the shadow stack if that is the current return address (`exceptions.cc:1773-1774`),
- applies the delta signal mask (`set_process_mask_delta`, `exceptions.cc:1347-1362`),
- builds a `ucontext_t` **only for `SA_SIGINFO` handlers** (`exceptions.cc:1795-1847`): for hardware faults
  it `memcpy`s the `CONTEXT` captured by the exception dispatch (`exceptions.cc:1800-1803`); for software
  signals it does `RtlCaptureContext` + one virtual unwind (`exceptions.cc:1811-1817`),
- calls the handler, on the alternate stack if requested (§2.7),
- restores the mask and `errno`, and loops if another signal arrived (`exceptions.cc:1762, 1966-1969`).

**One consequence worth flagging.** I found no path that writes a handler's modifications to `ucontext_t`
back into the `CONTEXT` the kernel resumes from. `exception::handle` returns `ExceptionContinueExecution`
with its `in` pointer untouched (`exceptions.cc:856-858`), and `call_signal_handler` copies the
possibly-modified context only into `_cygtls::context` (`exceptions.cc:1971`). So on Cygwin, unlike Linux,
mutating registers in a `SA_SIGINFO` handler and returning does not change where execution resumes; a handler
that wants to redirect must call `setcontext()` (`exceptions.cc:2007-2016`, which is `RtlRestoreContext` and
never returns) or `siglongjmp`. **This is a reading of the code, not something I tested**, and it matters to
us because our guest-fault path may want to fix up state and resume.

### 2.5 Hardware exceptions → POSIX signals: SEH, precisely

**It is table-based SEH, not VEH, and not an unhandled-exception filter.**

The mechanism is the `exception` class (`local_includes/exception.h:31-50`). Its constructor is
`always_inline` and emits nothing but SEH directives (`exception.h:20-29`):

```
.seh_handler _ZN9exception6handleEP17_EXCEPTION_RECORDPvP8_CONTEXTP19_DISPATCHER_CONTEXT, @except
.seh_handlerdata
.long 1
.rva 1b, 2f, 2f, 2f
```

i.e. it registers `exception::handle` as the language-specific handler for the enclosing function's unwind
info, covering the region between the constructor and destructor. A bare `exception protect;` object is
declared in exactly three places, each of which is the outermost frame of a thread:

| Site | Covers |
| --- | --- |
| `cygtls.cc:27` (`_cygtls::call`) | the main thread and every internal cygthread |
| `thread.cc:2044` (`pthread::thread_init_wrapper`) | every `pthread_create`d thread |
| `dll_init.cc:585` | DLL init |

So every Cygwin thread has `exception::handle` sitting at the bottom of its SEH chain. Faults are dispatched
by the normal `RtlDispatchException` frame walk.

VEH appears exactly **twice** in the whole tree (verified by grep over `*.cc`/`*.h`):

1. `AddVectoredExceptionHandler (0, singlestep_handler)` — the temporary, per-delivery handler in
   `interrupt_now` described in §2.3 (`exceptions.cc:976`, removed at `:995`).
2. `AddVectoredContinueHandler (0, myfault_altstack_handler)` at `dcrt0.cc:770` — note **Continue**, not
   Exception. Its purpose is spelled out at `exceptions.cc:633-640`: "If another exception occurs while
   running a signal handler on an alternate signal stack, the normal SEH handlers are skipped, because the OS
   exception handling considers the current (alternate) stack 'broken'. However, it still calls vectored
   exception handlers." The handler manually unwinds to `_my_tls.andreas->frame` and `RtlRestoreContext`s
   (`exceptions.cc:646-657`), because `RtlUnwindEx` validates the stack and would fail. The in-tree comment
   admits this only rescues Cygwin's own `__try/__except` blocks: "'Normal' exceptions will simply exit the
   process. Still, better than nothing…"

`SetUnhandledExceptionFilter` is never called.

There is a second SEH handler, `exception::myfault` (`exceptions.cc:617-631`), used by Cygwin's hand-rolled
`__try`/`__except`/`__endtry` macros (`cygtls.h:356-396`) for internal fault-tolerant memory access (e.g.
validating user pointers). It simply `RtlUnwindEx`es to the scope table's jump target.

Note the historical name in the task brief: **`_cygtls::handle_exceptions` does not exist in this tree.** It
was the 32-bit-era entry point when Cygwin used the `fs:0` `exception_list` chain. The current entry point is
`exception::handle`.

### 2.6 `EXCEPTION_RECORD` → `siginfo_t`

`exception::handle` (`exceptions.cc:662-859`) is a single `switch` on `e->ExceptionCode`
(`exceptions.cc:683-808`). The full mapping:

| NTSTATUS | `si_signo` | `si_code` |
| --- | --- | --- |
| `FLOAT_DIVIDE_BY_ZERO` | `SIGFPE` | `FPE_FLTDIV` |
| `FLOAT_DENORMAL_OPERAND`, `FLOAT_INVALID_OPERATION` | `SIGFPE` | `FPE_FLTINV` |
| `FLOAT_STACK_CHECK` / `INEXACT_RESULT` / `OVERFLOW` / `UNDERFLOW` | `SIGFPE` | `FPE_FLTSUB` / `FLTRES` / `FLTOVF` / `FLTUND` |
| `INTEGER_DIVIDE_BY_ZERO`, `INTEGER_OVERFLOW` | `SIGFPE` | `FPE_INTDIV`, `FPE_INTOVF` |
| `ILLEGAL_INSTRUCTION` | `SIGILL` | `ILL_ILLOPC` |
| `PRIVILEGED_INSTRUCTION` | `SIGILL` | `ILL_PRVOPC` |
| `NONCONTINUABLE_EXCEPTION` | `SIGILL` | `ILL_ILLADR` |
| `GUARD_PAGE_VIOLATION` | `SIGBUS` | `BUS_OBJERR` |
| `DATATYPE_MISALIGNMENT` | `SIGBUS` | `BUS_ADRALN` |
| `ACCESS_VIOLATION` | `SIGSEGV`/`SIGBUS` | see below |
| `STACK_OVERFLOW`, `ARRAY_BOUNDS_EXCEEDED`, `IN_PAGE_ERROR`, `NO_MEMORY`, `INVALID_DISPOSITION` | `SIGSEGV` | `SEGV_MAPERR` |
| `CONTROL_C_EXIT` | `SIGINT` | — |
| `INVALID_HANDLE` | — | `ExceptionContinueExecution` (swallowed) |
| `STATUS_GCC_THROW` / `UNWIND` / `FORCED` | — | `ExceptionContinueExecution` |
| anything else | — | `ExceptionContinueSearch` |

`si_code` defaults to `SI_KERNEL` (`exceptions.cc:681`).

**`SEGV_MAPERR` vs `SEGV_ACCERR` is decided by a `VirtualQuery`** on the faulting address
(`exceptions.cc:760-764`):

```
VirtualQuery ((PVOID) e->ExceptionInformation[1], &m, sizeof m);
si.si_signo = SIGSEGV;
si.si_code  = m.State == MEM_FREE ? SEGV_MAPERR : SEGV_ACCERR;
```

`si_addr` is `e->ExceptionInformation[1]` for `SIGSEGV`/`SIGBUS`, and the faulting *instruction* pointer for
everything else (`exceptions.cc:835-840`) — with an explicit comment that POSIX only defines the first case.

Windows does not tell you read-vs-write in a portable `si_code`; `ExceptionInformation[0]` carries it (0
read / 1 write / 8 DEP) but Cygwin does not use it. **If we need write-vs-read discrimination for JIT
write-protect faults, we read `ExceptionInformation[0]` ourselves; Cygwin is not a model for that.**

Two special cases in the `ACCESS_VIOLATION` arm are directly relevant to us
(`exceptions.cc:748-766`). Before deciding it is a signal at all, Cygwin calls
`mmap_is_attached_or_noreserve (addr, 1)` (`mm/mmap.cc`), which walks the mmap record list under a read lock
and, for a `MAP_NORESERVE` region, does `VirtualAlloc (u_addr, u_len, MEM_COMMIT, rec->gen_protect ())` and
returns `MMAP_NORESERVE_COMMITED`, whereupon the handler returns `ExceptionContinueExecution` **without
involving the signal machinery at all**. This is Cygwin implementing Linux's lazy commit in an exception
filter. It is exactly the shape our JIT-W^X and guest-lazy-page faults want.

Stack overflow gets a two-shot treatment (`exceptions.cc:768-774, 843-855`): if the thread has no alternate
stack the `SIGSEGV` disposition is forced to `SIG_DFL`; and if a handler *did* run and returned, Cygwin
re-sends the signal with `SIG_DFL` because "on Windows the NTDLL::__stkchk function will simply kill the
process" rather than re-executing the faulting instruction.

### 2.7 `sigaltstack`

`sigaltstack(2)` is `signal.cc:750-814`. The stack descriptor lives in `_cygtls::altstack`
(`cygtls.h:182`), initialised to `SS_DISABLE` per thread in `_cygtls::init_thread` (`cygtls.cc:58`).
`MINSIGSTKSZ` is 8192 and `SIGSTKSZ` 32768 (`include/cygwin/signal.h:418-421`) — larger than newlib's
generic 2048/8192, presumably because of the `xsave` area `sigdelayed` pushes.

Honouring it is a hand-written assembly stack switch inside `call_signal_handler`
(`exceptions.cc:1863-1937`). It computes a 16-byte-aligned top from `ss_sp + ss_size`, **copies the
`ucontext_t` onto the alternate stack** (`exceptions.cc:1886-1891` — "the context1 allocated in the normal
stack area is not accessible from the signal handler that uses alternate signal stack"; that comment is
about the stack-overflow case), saves the volatile registers into shadow space on the new stack, moves `rsp`,
calls `altstack_wrapper`, and restores.

The critical caveat is in the source, verbatim (`exceptions.cc:1869-1880`):

> **We DO NOT change the TEB's stack addresses and we DO NOT move the `_cygtls` area to the alternate stack.**
> This seems to work fine, but there may be Windows functions not working correctly under these
> circumstances. On the other hand, if a Windows function crashed and we're handling this here, moving the TEB
> stack addresses may be fatal.

So under Cygwin, while on the alternate signal stack: `NtCurrentTeb()->Tib.StackBase/StackLimit` still
describe the *original* stack, `_my_tls` still resolves to the original stack's TLS block (that is a direct
consequence of `cygtls.h:307-308`), and SEH unwinding does not work (§2.5, the continue-handler hack).

`altstack_wrapper` (`exceptions.cc:1703-1754`) additionally re-arms the original stack's guard pages via
`VirtualProtect (…, PAGE_READWRITE | PAGE_GUARD)` before calling the handler, emulating MSVCRT's
`_resetstkoflw`, and removes them again if the handler returns.

### 2.8 Async-signal safety: what is and is not guaranteed

Cygwin's guarantee is narrower than Linux's, and the code is honest about it.

**Guaranteed:**

- A handler will not be invoked while the thread is inside a Windows system DLL (`inside_kernel`,
  `exceptions.cc:451-492`). This is *why* delivery can be delayed or dropped, and it is a deliberate trade.
- A handler will not be invoked while the thread is inside a Cygwin function; instead it runs at the
  function's return via the shadow stack. That makes Cygwin's own libc effectively non-reentrant-by-design
  rather than reentrant.
- Intraprocess signals are synchronous: `sig_send` blocks the sender until `wait_sig` acknowledges
  (`sigproc.cc:844-849`), and the sender runs the handler itself. `doc/highlights.xml:355-361` states this as
  the design intent.
- `fork()` is documented as async-signal-safe (`fork.cc:620-625`, in the `_Fork` comment: "Our fork() already
  is async-signal-safe").

**Not guaranteed:**

- **Delivery is best-effort under contention.** `setup_handler` retries 100 times over roughly 10 ms of
  `Sleep(1)`s and then returns `false`, leaving the signal pending until some later `__SIGFLUSH`
  (`exceptions.cc:1060-1118`). If the thread stays inside a system call, the signal simply waits.
- **Only one signal can be armed per thread at a time**: `setup_handler` bails immediately if
  `tls->current_sig` is already set (`exceptions.cc:1053-1058`).
- **The handler runs with the OS in an arbitrary state.** Path B rewrites `Rip` from a suspended thread. Any
  loader lock, heap lock, or CRT lock held at that instant is still held when the handler runs. Cygwin's
  mitigation is `inside_kernel`, which is a heuristic on the module containing `Rip` — it says nothing about
  locks acquired by user code.
- **Cygwin's own internals are not async-signal-safe.** The `SIGSEGV` path takes a list read lock inside
  `mmap_is_attached_or_noreserve`; `wait_sig` allocates (`talktome` does `alloca` and `new cygthread`,
  `sigproc.cc:1358-1381`); `signal_exit` does file I/O and may spawn a debugger (`exceptions.cc:1391-1474`).
- **`SIGSTOP`/`SIGTSTP` only really stop the main thread.** `sig_handle_tty_stop` has an in-source FIXME:
  "This does nothing to suspend anything other than the main thread" (`exceptions.cc:909-910`), then does
  `pthread::suspend_all_except_self ()` as a partial fix (`exceptions.cc:914`).
- **`sigqueue` to another process does not block until delivery**, contrary to SUSv3 — stated as a FIXME at
  `signal.cc:723-726`.

---

## 3. Threads

### 3.1 `pthread_create`

`pthread::create` (`thread.cc:490-526`) → `create_posix_thread` (`create_posix_thread.cc:273-375`). Cygwin
does **not** let Win32 own the stack. It:

1. Reserves the real stack itself from a dedicated allocator (`thread_allocator`,
   `create_posix_thread.cc:132-232`) in 1 MiB slots (`THREAD_STACK_SLOT`) inside a fixed address window
   `[THREAD_STORAGE_LOW, THREAD_STORAGE_HIGH)` via `VirtualAlloc2` with
   `MemExtendedParameterAddressRequirements`, falling back to the mmap window for stacks above 1 GiB
   (`THREAD_STACK_MAX`).
2. Lays out the guard region and committed region by hand, exactly like the OS would
   (`create_posix_thread.cc:319-349`).
3. Calls `CreateThread` with a *throwaway* 256 KiB reservation and
   `STACK_SIZE_PARAM_IS_A_RESERVATION` (`create_posix_thread.cc:356-363`), entering at `pthread_wrapper`.
4. `pthread_wrapper` (`create_posix_thread.cc:38-125`) overwrites `teb->Tib.StackBase`, `StackLimit`, and
   `DeallocationStack`, calls `SetThreadStackGuarantee`, initialises `_cygtls` at
   `stackbase - __CYGTLS_PADSIZE__`, then drops into inline assembly that switches `rsp` to the new stack,
   `VirtualFree`s the OS-provided stack, and calls the real thread function.

The reason for all of this is `fork()` — a forked child must be able to reproduce its stacks at the same
addresses. That is the other agent's topic, but it explains why thread creation is this expensive.

**`_cygtls` placement is the single most consequential design decision here.** It is not Win32 TLS; it is a
`__CYGTLS_PADSIZE__` = **12800 bytes** (`include/cygwin/config.h:31`) block carved from the top of every
thread's own stack, addressed as `*((_cygtls *)(NtCurrentTeb()->Tib.StackBase - __CYGTLS_PADSIZE__))`
(`cygtls.h:307-308`) — one `%gs:8` load plus a constant offset, cheap enough for the `_sigfe` assembly to use
on every libc call (`scripts/gendef:141, 154, 177`). It holds the signal mask, pending siginfo, altstack, the
signal shadow stack (`TLS_STACK_SIZE` = 5 slots, `cygtls.h:31, 203-204`), the newlib `_reent`, and
per-subsystem scratch including select's socket-event arrays (`cygtls.h:107-113`).

`pthread_key_*` is separate and *is* Win32 TLS: `TlsAlloc` per key (`thread.cc:1702`), with a global
`keys[PTHREAD_KEYS_MAX]` registry so destructors can be run and the values carried across fork
(`thread.cc:1695, 1741-1754`).

### 3.2 Mutexes and condition variables

Everything is built on `Event` objects plus interlocked counters, deliberately avoiding kernel transitions
on the uncontended path.

`pthread_mutex` (`thread.cc:1790-1942`) is an **auto-reset Event** (`thread.cc:1801`) plus a `lock_counter`:

```
lock():   if (InterlockedIncrement (&lock_counter) == 1) set_owner (self);
          else if (NORMAL || owner != self)  cygwait (win32_obj_id, timeout, cw_sig | cw_sig_restart);
          ...
unlock(): if (--recursion_counter == 0) { owner = _unlocked;
                                          if (InterlockedDecrement (&lock_counter)) SetEvent (obj); }
```
(`thread.cc:1829-1890`.) Uncontended lock/unlock is two interlocked ops and no syscall. Contended waits go
through `cygwait`, so **a blocked `pthread_mutex_lock` is interruptible by a signal** — that is the point of
`cw_sig | cw_sig_restart`.

`PTHREAD_PROCESS_SHARED` mutexes are **not implemented**: the constructor silently leaves `magic` unset
(`thread.cc:1807-1810`), and the fork fixup `api_fatal`s if it ever sees one (`thread.cc:1929-1930`).

`pthread_cond` (`thread.cc:1163-1318`) is a **semaphore plus two internal mutexes** (`mtx_in`, `mtx_out`) and
`waiting`/`pending` counters implementing a two-phase gate so a broadcast releases exactly the threads that
were waiting when it was issued. `wait()` releases the user mutex, `cygwait (sem_wait, timeout, cw_cancel |
cw_sig_restart)`, then re-acquires (`thread.cc:1286-1309`). `PTHREAD_PROCESS_SHARED` condvars are likewise
rejected (`thread.cc:1174-1178`).

Two lighter internal locks exist alongside:

- `fast_mutex` (`local_includes/thread.h:30-71`): interlocked counter + auto-reset Event, same shape as
  `pthread_mutex` minus ownership tracking. Used for the object registries.
- `muto` (`sync.cc:23-142`, `lock_process` and `sync_proc_subproc` are instances): recursive,
  TLS-pointer-owned, `InterlockedExchange` fast path with a `bruteforce` Event fallback. Comment at
  `sync.cc:69-71`: "The goal here is to minimize, as much as possible, calls to the OS."

`pthread_spinlock` (`thread.cc:1958-2039`) spins with `pause` for 1000 iterations *only if*
`wincap.cpu_count () != 1`, then falls back to a 1 ms timed `cygwait`.

`pthread_cancel` (`thread.cc:589-664`) is the other user of thread hijacking: for asynchronous cancellation
it `SuspendThread`s, checks `inside_kernel`, aligns the stack to the Windows ABI's 16n+8 requirement, and
`SetThreadContext`s `Rip` to `pthread::static_cancel_self` — with the comment "The OS is not foolproof in
terms of asynchronous thread cancellation and tends to hang infinitely if we change the instruction pointer.
So just don't cancel asynchronously if the thread is currently executing Windows code."

### 3.3 What survives fork, and what does not

`MTinterface::fixup_before_fork` / `fixup_after_fork` (`thread.cc:338-359`) are called from
`pthread::atforkprepare` / `atforkchild` (`thread.cc:2138-2179`), which are driven by the RAII
`hold_everything` bundle in `fork()` (`sigproc.h:158-191`) whose lock order is documented as
pthread → signals → process.

In the child:

| Object | Fate |
| --- | --- |
| Threads other than the forking one | `magic = 0; valid = false; win32_obj_id = NULL; cancel_event = NULL` (`thread.cc:1096-1108`) — the `pthread_t` becomes invalid, matching POSIX |
| `pthread_mutex` | `recursion_counter = lock_counter = condwaits = 0`, Event **recreated** (`thread.cc:1925-1942`) — i.e. all mutexes are force-unlocked |
| `pthread_cond` | `waiting = pending = 0`, both internal mutexes unlocked, semaphore **recreated** (`thread.cc:1320-1333`) |
| `pthread_key` values | saved to `fork_buf` before fork, `TlsAlloc`'d again and restored after (`thread.cc:1741-1754`) |
| `_cygtls` | mostly survives (it is in the copied stack), but `signal_arrived`, `select.sockevt`, `cw_timer`, `wq.thread_ev` are nulled and any armed signal is popped (`cygtls.cc:77-92`) |
| stdio locks | `__fp_lock_all()` before, `__fp_unlock_all()` after, in both parent and child (`thread.cc:2148, 2156, 2171`) |

The pattern is uniform and worth naming: **every Win32 kernel object embedded in a synchronisation primitive
is discarded and recreated in the child, and every counter is reset to "unlocked, nobody waiting."** That is
only sound because the child is single-threaded by definition.

---

## 4. `select`/`poll`: the readiness/completion mismatch

### 4.1 The mismatch

Windows I/O is completion-based. `WaitForMultipleObjects` waits on *object signalled* states, and the only
descriptor-like objects whose signalled state means "readable" are sockets under `WSAEventSelect` and
overlapped handles with a pending operation. Anonymous pipes, consoles, ptys, serial ports, and character
devices have no readiness object at all. Cygwin's own documentation states the problem and the answer
(`doc/highlights.xml:395-419`):

> Much to our dismay, we discovered that the Win32 select in Winsock only worked on socket handles. […] This
> is accomplished by the main thread suspending itself, after starting one thread for each type of file
> descriptor present. Each thread polls the file descriptors of its respective type with the appropriate
> Win32 API call. As soon as a thread identifies a ready descriptor, that thread signals the main thread to
> wake up. […] So select returns, after polling all of the file descriptors one last time.

### 4.2 The `select_*` machinery

Three structures (`local_includes/select.h`):

- `select_record` (`select.h:20-56`): one per selected fd. Carries `read/write/except_selected`,
  `read/write/except_ready`, a `HANDLE h` to wait on, and **four function pointers**: `startup`, `peek`,
  `verify`, `cleanup`.
- `select_info` and its per-type subclasses (`select.h:58-93`): one per *device type* present in the fd set,
  holding the single helper `cygthread *`, a `bye` handle, and the head of the record chain.
- `select_stuff` (`select.h:95-137`): the whole call's state, including one `device_specific_*` pointer per
  supported type (console, pipe, ptys, fifo, socket, dsp).

Every `fhandler` subclass implements three virtuals — `select_read`, `select_write`, `select_except`
(`local_includes/fhandler.h:646-648`, pure virtual on `fhandler_base`) — which fill in the four function
pointers for their type. That is the whole descriptor abstraction: **a v-table of polling callbacks, not a
readiness object.**

`select()` proper (`select.cc:154-242`) is a retry loop:

1. `test_and_set` every fd in the masks, building the record chain (`select.cc:179-184, 299-336`).
2. If any record is `always_ready` (disk files, `/dev/null`, `fhandler_base` default — `select.cc:1740-1782`,
   `1562-1604`) or the timeout is zero, skip waiting.
3. Otherwise `select_stuff::wait()` (`select.cc:339-516`).
4. On wake, `select_stuff::poll()` calls every record's `peek` again to build the answer
   (`select.cc:554-571`).
5. `cleanup()` + `destroy()` — **tear down all helper threads and free the whole chain**
   (`select.cc:210-213, 246-274`).
6. If nothing was actually ready, recompute the remaining timeout and **go back to step 1**, rebuilding
   everything (`select.cc:215-237`).

`select_stuff::wait` (`select.cc:339-516`) assembles a `HANDLE w4[MAXIMUM_WAIT_OBJECTS]` array containing,
in order: the thread's `signal_arrived` event (always, `select.cc:348`), the pthread cancel event if any
(`:354`), each record's `h` deduplicated (`:373-379`), and finally a per-thread cached `NtCreateTimer`
notification timer if the timeout is finite (`:388-417`). Then `WaitForMultipleObjects` — or
`MsgWaitForMultipleObjectsEx` with `QS_ALLINPUT | MWMO_INPUTAVAILABLE` if a `/dev/windows` descriptor is in
the set (`select.cc:422-431`).

Waking is not trusting. Index 0 means a signal: everything is torn down, `call_signal_handler()` runs, and
`select` returns `EINTR` **unconditionally, ignoring `SA_RESTART`** (`select.cc:447-459`, with a comment
saying so). Any other index runs each record's `verify` callback, because "Some types of objects (e.g.,
consoles) wake up on 'inappropriate' events like mouse movements. […] If it returns false, then this wakeup
was a false alarm and we should go back to waiting" (`select.cc:486-511`).

### 4.3 What the helper threads actually do

This is the part that matters. Take pipes (`select.cc:797-852`); FIFOs, consoles, pty slaves and `/dev/dsp`
are structurally identical (`select.cc:1010-1065, 1200-1249, 1436-1490, 2303-2358`):

```c
static DWORD thread_pipe (void *arg) {
  select_pipe_info *pi = ...;  DWORD sleep_time = 0;  bool looping = true;
  while (looping) {
      for (select_record *s = pi->start; (s = s->next); )
        if (s->startup == start_thread_pipe) {
            if (peek_pipe (s, true)) looping = false;
            if (pi->stop_thread) { looping = false; break; }
        }
      if (!looping) break;
      cygwait (pi->bye, sleep_time >> 3);
      if (sleep_time < 80) ++sleep_time;
  }
  return 0;
}
```

It is a **poll loop with a ramp**: sleep 0 ms, then 0, …, up to `80 >> 3` = **10 ms** steady state
(`select.cc:820-822`). The thread handle itself is what the main thread waits on (`select.cc:834, 847`) —
thread exit *is* the readiness signal. Teardown sets `stop_thread`, releases the `bye` semaphore, and
`detach()`es, which is a synchronous `WaitForSingleObject` on the helper (`select.cc:854-869`,
`cygthread.cc:437-441`).

`peek_pipe` itself (`select.cc:697-793`) is `NtQueryInformationFile(FilePipeLocalInformation)` via
`pipe_data_available` (`select.cc:598-693`). The write-side readiness check there is worth reading in full:
`WriteQuotaAvailable` is unreliable because a pending read on the other end decrements it, so Cygwin
distinguishes "buffer non-empty" from "reader is blocked in a read" by **toggling the pipe's non-blocking
mode and observing whether it returns `STATUS_PIPE_BUSY`** (`select.cc:645-672`). That is the level of
heroics required to answer "is this pipe writable" on Windows.

Sockets are the one type with real kernel readiness. `start_thread_socket` (`select.cc:1882-1953`) collects
each socket's `WSAEventSelect` event into a per-thread cached array (`_my_tls.locals.select.w4`,
`cygtls.h:107-113`, grown in `MAXIMUM_WAIT_OBJECTS` increments) plus a `sockevt` cancel event at index 0, and
`thread_socket` (`select.cc:1812-1850`) waits on them — but in chunks of 64, and if there are more than 64
sockets the wait timeout degrades to a computed poll interval
(`timeout = 64 / (roundup2 (num_w4, 64) / 64)`, `select.cc:1816-1819`), i.e. **more than 64 sockets in one
`select()` turns socket readiness into polling too.**

`signalfd` is special-cased to wait on the process-wide `my_pendingsigs_evt` that `wait_sig` maintains
(`select.cc:2169-2185`, set/reset at `sigproc.cc:1658-1661`), with the honest comment "This method wakes up
all threads hanging in select and having a signalfd […] but it's certainly better than constant polling."
`timerfd` waits on the timer handle directly (`select.cc:2237-2251`) — no helper thread.

### 4.4 `poll()`

`poll()` is a thin wrapper that converts to three `fd_set`s and calls `cygwin_select`
(`poll.cc:24-139`), with `FD_SETSIZE` locally redefined to 16384 (`poll.cc:9`). `ppoll` adds mask swapping
(`poll.cc:141-163`). There is no separate implementation and therefore no epoll-like registration model
anywhere in Cygwin: **every `poll()` pays the full `select()` rebuild.**

---

## 5. Costs and limitations, collected

Numbers below are constants read out of the source, not measurements. No timing was performed for this
document.

**Signals**

| Item | Value | Cite |
| --- | --- | --- |
| Dedicated threads at rest | 1 (`wait_sig`) + 1 per live child (`proc_waiter`) | `sigproc.cc:547`, `pinfo.cc:1336` |
| Signal pipe capacity | 64 KiB ≈ 327 packets (derived) | `sigproc.cc:32, 528-530` |
| Queue slots | `SIGQUEUE_MAX` = 1024 | `include/cygwin/limits.h:44` |
| Cross-process send | `OpenProcess(PROCESS_DUP_HANDLE)` + `DuplicateHandle` + `WriteFile` per signal | `sigproc.cc:671-698` |
| Send serialization | named mutex per target pid | `sigproc.cc:769-772` |
| Self-signal round trip | write pipe → `wait_sig` → `setup_handler` → wake sender; 60 s timeout | `sigproc.cc:29, 844-849` |
| Delivery attempt budget | 10 × 10 tries, ~10 ms of `Sleep(1)` before giving up | `exceptions.cc:34-35, 1060-1118` |
| Concurrent armed signals per thread | **1** | `exceptions.cc:1053-1058` |
| Hijack path syscalls | `SuspendThread` + `GetThreadContext` + (`AddVEH` + `SetThreadContext` + `ResumeThread` + `RtlWaitOnAddress` ≤10 ms + `SuspendThread` + `GetThreadContext` + `RemoveVEH`) + `SetThreadContext` + `ResumeThread` | `exceptions.cc:963-1008, 1092-1107` |
| Plus, per attempt | `VirtualQuery` + possibly `GetModuleFileNameW` | `exceptions.cc:451-492` |
| Handler prologue | full GPR + flags + `fxsave64`/`xsave64` (size from `cpuid` leaf 0xd) | `scripts/gendef:236-269` |
| Per-libc-call tax | `_sigfe`/`_sigbe` shadow-stack push/pop with a spin lock | `scripts/gendef:151-190` |
| Per-thread fixed cost | 12800 B of stack reserved for `_cygtls` | `include/cygwin/config.h:31` |

Known limitations: no delivery while in a system DLL; one armed signal per thread; `SIGSTOP` does not
reliably stop non-main threads (`exceptions.cc:909-910`); `sigqueue` cross-process is not synchronous
(`signal.cc:723-726`); SEH unwinding is broken on the alternate stack (`exceptions.cc:633-640`); handler
register writes to `ucontext_t` appear not to be honoured on return (§2.4, unverified).

**Threads**

| Item | Value | Cite |
| --- | --- | --- |
| Stack allocation | 1 MiB slots in a fixed VA window, `VirtualAlloc2` + manual guard/commit, then a throwaway 256 KiB OS stack freed at entry | `create_posix_thread.cc:127-232, 294-363` |
| Max pooled stack | 1 GiB, above which it comes from the mmap window | `create_posix_thread.cc:130, 151-172` |
| Internal thread pool | 64 slots, then heap-allocated "freerange" threads | `cygthread.cc:98-99, 228-234` |
| Uncontended mutex | 2 interlocked ops, no syscall | `thread.cc:1835-1836, 1874-1884` |
| Contended mutex | `WaitForMultipleObjects` over {event, signal_arrived, cancel, timer} | `cygwait.cc:27-99` |
| `PTHREAD_PROCESS_SHARED` mutex/cond | **not implemented** | `thread.cc:1807-1810, 1174-1178` |
| Async `pthread_cancel` | `SuspendThread` + `SetThreadContext`, skipped if inside a system DLL | `thread.cc:619-662` |

**select/poll**

| Item | Value | Cite |
| --- | --- | --- |
| Per-call setup | allocate a `select_record` per fd, start one helper cygthread per device type | `select.cc:299-336, 368` |
| Per-call teardown | signal + synchronously join every helper | `select.cc:854-869`, `cygthread.cc:437-441` |
| False wake | rebuild everything and loop | `select.cc:210-237` |
| Handle limit | `MAXIMUM_WAIT_OBJECTS` = 64 minus signal/cancel/timer slots; over that, **`EINVAL`** | `select.cc:343, 363-367` |
| Pipe/fifo/console/pty/dsp readiness | poll loop, backing off to **10 ms** | `select.cc:820-822` |
| >64 sockets | socket readiness also degrades to polling | `select.cc:1816-1819` |
| `poll()` | reimplemented on `select()`, no registration model | `poll.cc:83-84` |
| Signal during select | always `EINTR`, `SA_RESTART` ignored | `select.cc:447-459` |

The 10 ms figure is the one to remember: **on Cygwin, a write to a pipe becomes visible to a blocked
`select()` after up to 10 ms of latency**, because nothing in Windows can tell the waiter otherwise.

---

## 6. Applicability to this engine

Read against `include/hl/host_services.h` (ABI 4) and `src/host/native_context.h`.

### 6.1 Signals: we need four things, not POSIX

Our requirement is not "deliver arbitrary POSIX signals to arbitrary threads." It is:

**(a) Guest memory faults → guest `SIGSEGV`/`SIGBUS`.**
Cygwin's structure transfers almost verbatim, minus the signal thread. The fault arrives on the guest's own
thread; the handler must run on that thread; nothing needs to cross a thread boundary. Cygwin's
`exception::handle` does exactly this: classify `EXCEPTION_RECORD`, build `siginfo_t`, and call the handler
on the faulting thread inside the exception dispatch (`exceptions.cc:662-859`, handler invocation at
`sigproc.cc:880-881`). We should do the same thing directly, with no pipe and no `wait_sig`.

The pieces to lift:
- the NTSTATUS→signal table (§2.6) — it is complete, correct, and hard-won,
- `si_code = m.State == MEM_FREE ? SEGV_MAPERR : SEGV_ACCERR` via `VirtualQuery` (`exceptions.cc:760-764`),
- `si_addr = e->ExceptionInformation[1]` for SEGV/BUS, `Rip` otherwise (`exceptions.cc:835-840`),
- `ExceptionInformation[0]` for read/write/DEP, which Cygwin ignores and we need.

Our `src/host/native_context.h` is a `(host OS × host CPU)` matrix of `ucontext_t` accessors. Windows needs a
fourth arm whose "context" is `PCONTEXT`, with `HL_HOST_UC_PC(c) → c->Rip` and `HL_HOST_UC_SP(c) → c->Rsp`,
and an x64 register mapping to `Rax…R15` instead of `gregs[REG_*]`. That is straightforward — but note that
`hl_host_uc_xmm` currently returns `fpregs->_xmm`; on Windows the equivalent is `&ctx->Xmm0`, contiguous
`Xmm0..Xmm15` in `CONTEXT`, which is layout-compatible enough to keep the accessor shape.

**(b) JIT write-protect faults.**
`hl_host_memory_services.repair_signal_page` (`host_services.h:216-222`) already states the contract we need:
"no userspace allocation, locks, logging, ownership registries, or errno-dependent decisions." Cygwin's
`MAP_NORESERVE` handling is the same idea and *violates* the same contract — it takes a list read lock inside
the exception handler (`mm/mmap.cc`, `LIST_READ_LOCK`). Take the shape, not the implementation: **classify
and repair inside the exception filter and return `EXCEPTION_CONTINUE_EXECUTION` without ever entering
signal machinery** (`exceptions.cc:748-753`). On Windows the repair is a bare `VirtualProtect` /
`VirtualAlloc(MEM_COMMIT)`, which satisfies our lock-free requirement as written.

**(c) Child-process reaping.**
Cygwin's model — one `proc_waiter` cygthread per child, blocking on a `ReadFile` of a per-child pipe, turning
exit into a synthesized `SIGCHLD` (`pinfo.cc:1240-1326`) — is a thread per child and a pipe per child. We
already have `hl_host_process_services.wait` with an absolute deadline and retained completion
(`host_services.h:448-454`); on Windows that is `WaitForSingleObject(hProcess)` +
`GetExitCodeProcess`, and for the pollset case a process handle is *already* a waitable object, so it drops
straight into `WaitForMultipleObjects` with no helper thread at all. **Do not copy the per-child thread.**

**(d) `SIGINT`/`SIGTERM`.**
`SetConsoleCtrlHandler` is the only mechanism and Cygwin uses it (`exceptions.cc:86-99, 1148-1262`). Two
facts from that code are worth carrying: the handler runs **on a thread the OS creates**, so it must only
post to something (Cygwin does `sig_send`); and returning `FALSE` for `CTRL_SHUTDOWN_EVENT`/`CTRL_CLOSE_EVENT`
is what suppresses the "End task" dialog (`exceptions.cc:1165-1188`). Our version should set an event /
counter that the engine's event loop observes, nothing more.

**What is overkill for us, explicitly:**

- The `wait_sig` thread and signal pipe. They exist so that *any* process can signal *any* other and so
  signal policy is centralized. We have no such requirement; our guest signals are internal to one process
  and our host signals are two console events.
- `SuspendThread`/`SetThreadContext` thread hijacking. This is the highest-risk mechanism in Cygwin and,
  per §2.3, still needed a new single-step workaround in 2025. We should not adopt it. If we ever need to
  interrupt a running guest thread (STW for the block cache, guest signal delivery to a *different* thread),
  use a cooperative safepoint poll in generated code — which the dispatcher already has a natural place for —
  not context surgery.
- `_sigfe`/`_sigbe` shadow stacks around every libc entry point. That exists to make Cygwin's own non-reentrant
  libc survive signals. We control our own call boundaries.
- `sigaltstack` machinery. Cygwin's version does not move the TEB and cannot unwind
  (`exceptions.cc:1869-1880`, `633-640`). Windows already gives us the thing sigaltstack exists for:
  `SetThreadStackGuarantee` plus a guard-page-aware handler. If we need stack-overflow recovery, use that.
  If a guest needs `sigaltstack` semantics, that is the Linux ABI layer's problem on the guest stack, not the
  host's.
- The 60-second `WSSC` completion timeout, the 100-retry pipe write, and the named per-pid mutex — all
  artifacts of the pipe transport.

### 6.2 The `event` group: do **not** build Cygwin's `select`

**Firm recommendation: implement `hl_host_event_services` directly on Win32 waitable objects —
`WaitForMultipleObjects` over a registered handle set, with `WaitForSingleObject`-style fan-out above 64
handles — and use IOCP only for the socket/overlapped-file subset if and when it is needed. Do not port
`select.cc`.**

The reason is specific to our code, not general taste. Read `src/linux_abi/epoll.c:414-447`:

```c
hl_host_event_record ignored;
waited = linux_abi->host->event->wait (…, epoll->wake, &ignored, 1, deadline_ns);
```

The guest `epoll_wait` implementation uses the host pollset **purely as a "something may have changed, wake
me" edge**. It discards the returned record. Actual readiness comes from `epoll_sample()`
(`epoll.c:355-399`), which re-derives every watch's state with `hl_linux_object_ready()` — a per-object
query. Registration (`epoll.c:186-188`) always passes `HL_HOST_READY_READ` regardless of the guest's declared
interests, and only when the object exposes a `wait_handle`; objects without one fall back to a callback
`subscribe` (`epoll.c:195-204`).

That is a readiness-agnostic wakeup bus, not an epoll. The implications:

1. **We do not need per-descriptor readiness classification from the host.** The single hardest and most
   fragile part of `select.cc` — `peek_pipe`, the `WriteQuotaAvailable`/`STATUS_PIPE_BUSY` heuristic, the
   per-type `verify` callbacks — is work our design does not ask the host to do. `hl_host_stream_services.
   readiness` and `hl_host_counter_services.readiness` (`host_services.h:645, 513`) already own that job, are
   called on demand, and can use `PeekNamedPipe`/`NtQueryInformationFile` synchronously without a helper
   thread.
2. **A `HANDLE` that becomes signalled is a sufficient wakeup source**, and Windows gives us signallable
   handles for exactly the objects we register: manual/auto-reset events (`counter`, `wake`), waitable timers
   (`arm_timer`), process handles (`process.wait`), directory-change handles via
   `ReadDirectoryChangesW` completion (`watch`, `directory`), and sockets via `WSAEventSelect`
   (`network`). Named pipes are the gap — see below.
3. **`wake()` is `SetEvent` on a reserved slot 0.** `arm_timer`/`disarm_timer` are
   `CreateWaitableTimerEx` + `SetWaitableTimer`/`CancelWaitableTimer` with the token stashed alongside;
   `hl_host_event_record.token` is only actually consumed by the timer path
   (`src/linux_abi/syscall/time.c:186`) and the bound-watch path
   (`src/linux_abi/syscall/binding.c:201`), both of which are small fixed-size sets.

**The 64-handle limit is the only real design constraint**, and it is not the blocker it looks like:

- Above 64 registered handles, split into ⌈n/63⌉ groups and give each group a waiter thread that signals a
  shared "group N fired" event; the caller waits on those. This is `RegisterWaitForSingleObject` /
  `CreateThreadpoolWait` in the OS thread pool, which is exactly this construction done for us and is what we
  should use before writing our own. Cost is one pool thread per 63 handles, not one per descriptor type per
  call, and it is *persistent* across `wait()` calls rather than rebuilt.
- Cygwin never did this because `select()`'s fd set is per-call and unregistered, so it had nothing to
  amortise. **We do**: `hl_host_event_services.control` is an explicit registration step
  (`host_services.h:470-472`), just like `epoll_ctl`. That is the single structural advantage our ABI has over
  `select`, and it is the reason the Cygwin design does not apply.

**Where IOCP belongs.** IOCP is the right primitive for *completion* — overlapped reads/writes on files,
pipes, and sockets — and the wrong primitive for "is this object signalled." Our `event` group is the latter.
The place IOCP earns its keep is anonymous pipes: a Win32 named pipe has no readiness event, so either
(i) `stream.readiness` polls with `PeekNamedPipe` and the pollset never blocks on a pipe (acceptable if the
guest's pipe reads are the thing that blocks, not the pollset), or (ii) each pipe endpoint keeps one
zero-byte overlapped `ReadFile` outstanding whose `OVERLAPPED.hEvent` **is** the readiness handle — the
standard trick, and it makes a pipe a first-class waitable object at the cost of one event and one pending
IRP per endpoint. Option (ii) is the right one and should be decided at `stream.pipe_pair` time. Either way
this is a property of the *stream* backend, not of the `event` backend, and it does not push us toward IOCP
for `event->wait`.

I have not benchmarked either option; that choice should be measured once the stream backend exists.

### 6.3 Copy in spirit / do not copy

**Copy in spirit:**

- **Classify and repair faults inside the exception filter.** `exceptions.cc:748-753` returning
  `ExceptionContinueExecution` for a lazily-committed page is precisely the pattern for our JIT and
  guest-memory paths.
- **The NTSTATUS→signal/`si_code`/`si_addr` mapping table** (§2.6) — reuse it as a table, we will not
  rediscover it more cheaply.
- **One entry-point SEH scope per thread**, installed in the thread wrapper, so there is exactly one place
  faults are classified (`cygtls.cc:22-29`, `thread.cc:2044`).
- **Interlocked-counter-plus-Event locks with no kernel transition when uncontended**
  (`thread.h:30-71`, `thread.cc:1829-1890`). This is the right implementation for
  `hl_host_sync_services.mutex_*` (`host_services.h:588-597`): `SRWLOCK` is even better and is what we should
  actually use, but the shape — never enter the kernel on the fast path — is Cygwin's and it is correct.
- **"Discard and recreate every kernel object in the fork child, reset every counter to unlocked."**
  (`thread.cc:1925-1942, 1320-1333`.) Whatever the fork agent concludes, `hl_host_sync_services.fork_child`
  (`host_services.h:596`) should follow this rule uniformly rather than case-by-case.
- **Deduplicate handles in the wait array and reserve fixed low slots for control channels**
  (`select.cc:348-379`). Slot 0 = wake event is the right convention for our pollset too.
- **Distrust wakeups.** Cygwin's `verify` pass exists because Windows signals objects for reasons you did not
  ask about (`select.cc:486-511`). Our `epoll_sample` already re-derives readiness after every wake, which is
  the same discipline arrived at independently — keep it.

**Do not copy:**

- The signal thread, the signal pipe, `sigpacket`, `__SIGFLUSH`, and the whole `sig_send` transport. We do
  not need cross-process POSIX signals.
- `SuspendThread` + `GetThreadContext` + `SetThreadContext` thread hijacking, in either the signal
  (`exceptions.cc:1085-1108`) or the cancellation (`thread.cc:619-662`) form. Both of Cygwin's users of it
  carry in-source warnings that it hangs or crashes, and the signal one needed a new single-step workaround
  in 2025.
- `_sigfe`/`_sigbe` shadow stacks and the `SIGFE` annotation discipline.
- `inside_kernel()`'s `VirtualQuery` + `GetModuleFileNameW` + system-directory string compare
  (`exceptions.cc:451-492`). It is a per-delivery heuristic in service of a mechanism we are not adopting.
- Cygwin's `sigaltstack` stack switch, which knowingly leaves the TEB inconsistent
  (`exceptions.cc:1869-1880`).
- **The entire `select.cc` architecture**: per-call record chains, per-device-type helper threads, 10 ms
  polling loops, per-call thread start/join, and rebuilding everything after a false wake. Our
  `control`/`wait` split makes all of it unnecessary.
- `poll()` reimplemented on `select()` (`poll.cc:83-84`). Our `event` group is the registration-based
  primitive; nothing should be layered on a scanning one.
- One `proc_waiter` thread per child (`pinfo.cc:1336`). Process handles are waitable; put them in the pollset.

---

## 7. What this document does not establish

Stated plainly so nobody treats these as settled:

- **No measurements.** Every number in §5 is a source constant. The 10 ms select latency is the code's
  backoff ceiling, not an observed figure. Signal delivery latency, hijack cost, and thread-creation cost were
  not timed.
- **The `ucontext_t` write-back claim in §2.4 is a code reading**, not a test. If our design depends on
  mutating registers from a fault handler and resuming, verify it directly against the Windows behaviour we
  choose — do not rely on Cygwin's precedent either way.
- **`AddVectoredContinueHandler` semantics.** I have recorded which API Cygwin calls (`dcrt0.cc:770`) and the
  intent stated in its comment (`exceptions.cc:633-640`). I did not verify against the Windows exception
  dispatch documentation exactly when continue handlers run relative to SEH frames on a "broken" stack. If we
  ever need a fallback handler for a broken stack, that ordering must be confirmed first.
- **The `sigpacket` size of 200 bytes** (and therefore the ≈327 pipe depth) is computed from the struct
  layout, not from a `sizeof` on a real build.
- **AArch64.** Cygwin has an ARM64 port in this tree (`exceptions.cc:237-261`, `scripts/gendef:368-622`), but
  `call_signal_handler`'s alternate-stack switch is x86-64 only and `#error`s otherwise
  (`exceptions.cc:1933-1935`). If a Windows-on-ARM host is ever in scope, none of the assembly-level findings
  here transfer.
- **Sockets and pipes in our `event` group.** §6.2 recommends the zero-byte-overlapped-read trick for making
  anonymous pipes waitable. That is a known Win32 technique, not something demonstrated in Cygwin (Cygwin
  polls instead), and it has not been prototyped here.
