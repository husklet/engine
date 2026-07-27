# Prior art: flinux's binary translator, and faults in generated code

**Licensing.** flinux (`wishstudio/flinux`) is **GPLv3+**; hl-engine is MIT. The two are incompatible. No
flinux code is copied into `src/`, and none is reproduced here beyond the few one-line fragments needed to
name a technique. What follows describes *ideas* — architecture, algorithms, constraints, and the reasons
behind them — which are not copyrightable. Anything we adopt will be independently written from this
description. Citations are `path:line` into a read-only clone of flinux at `a041253` (2016-03-29, last
commit on `master`), for verification only.

Every Windows claim in §6 was **measured on this host** (Windows 11 Pro 10.0.26200, x86-64, clang 22.1.8
`x86_64-w64-windows-gnu`) by probes written for this document and kept in the session scratchpad. Claims
about flinux are cited to its source. Where something was not measured or not established, it says so.

`DOCS.md` is normative. This is a survey, not a design; the design it feeds is
`docs/windows/signals-and-faults.md`.

---

## 1. Why `src/dbt/` exists at all

flinux runs unmodified 32-bit x86 Linux binaries on 32-bit x86 Windows. Same ISA, same privilege level,
same instruction encoding. A translator should be unnecessary — and for the first year of the project it
was. The commit that introduced `src/dbt/` says exactly why it stopped being unnecessary:

> `5e2463a` — *"The brand new Dynamic Binary Translator for x86. **This should eternally resolve the %gs
> issue.** … Now I can run though glibc to the point of `mov %gs, blah`."*

**`src/dbt/` exists to emulate one segment register.** Everything else it does — `int 0x80`, `cpuid`,
code-cache management, signal delivery — is downstream of having built a translator for `%gs`.

### 1.1 The `%gs` problem, in the author's own words

The pre-DBT tree carried a long design comment in `src/syscall/tls.c` (recoverable at
`git show 5e2463a^:src/syscall/tls.c`) which is the clearest statement of the constraint anywhere in the
prior art. Paraphrased:

- Linux x86 TLS works by `set_thread_area()` installing a **GDT descriptor**; the guest then sets `%gs` to
  that entry number and reaches TLS as `%gs:0` at zero cost. The kernel reloads the thread's descriptors on
  every context switch.
- Windows uses `%fs` for the TEB and leaves `%gs` alone — so there is no *register* conflict on x86.
- The conflict is over the **descriptor tables**. The GDT is unmodifiable from ring 3 on every Windows
  version. On 32-bit Windows a process could add LDT entries via `NtSetLdtEntries`. On 64-bit Windows
  *"the LDT simply does not exist"* — the call returns `STATUS_NOT_IMPLEMENTED` — and PatchGuard makes a
  driver-based workaround BSOD the machine.

So the guest can hold whatever selector value it likes in `%gs`, but nothing can make that selector name a
per-thread base address. The only remaining option is to stop letting the hardware resolve `%gs` and
resolve it in software instead.

### 1.2 The two approaches they tried, in order

The same comment enumerates them, and the tree shows both being built:

**(1) Trap-and-emulate.** Keep `%gs` at 0 so every `%gs:`-prefixed access faults, and emulate the faulting
instruction from a VEH. This was fully implemented: `git show 5e2463a^:src/syscall/syscall.c` shows the
handler decoding the fault site, and the pre-DBT `tls.c` contains a full ModR/M decoder, an instruction
*generator*, and a `trampoline[2048]` buffer — i.e. on a fault it re-emitted the instruction with the
segment prefix stripped and the TLS base folded into the addressing, wrote it into the trampoline buffer,
pointed `Xip` at it, and returned `EXCEPTION_CONTINUE_EXECUTION`. A per-instruction, fault-driven JIT.

Its own comment records why it lost: *"as exception handling is very expensive, this will not get good
performance"*, plus *"on x86_64 systems, the Windows WOW64 runtimes seems to mess up Win64 TEB pointer to
gs register at context switches"*.

**(2) Patch glibc.** Rejected as requiring modified binaries, which is the project's whole premise.

**(3) — the one they shipped — translate ahead of the fault.** Once you have written an instruction decoder
and an instruction generator to service faults one at a time, the marginal cost of running them over a
whole basic block *before* execution is small, and the marginal benefit is that the fault goes away
entirely. `src/dbt/` is approach (1)'s machinery, hoisted out of the fault handler.

### 1.3 What the translator actually rewrites

Confirmed by reading `dbt_translate` (`src/dbt/x86.c:1283-1977`). The overwhelming majority of instructions
are **copied verbatim** — `dbt_copy_instruction` (`x86.c:1136-1158`) re-emits the original prefixes,
opcode and ModR/M essentially unchanged. Only five classes are rewritten:

| Class | Site | Rewrite |
|---|---|---|
| `%gs:` memory operands | `x86.c:1146-1147`, `1160-1204` | prefix **dropped**; a scratch register is spilled to host TLS, loaded with the guest's TLS base from host `fs:[gs_addr]`, folded into the ModR/M base, and restored |
| `mov r/m, %gs` / `mov %gs, r/m` | `x86.c:1871-1960` | reads/writes a *simulated* selector in host TLS; the write side also calls back into C to recompute the base |
| `int 0x80` | `x86.c:1852-1869` | replaced by `push <next guest pc>; jmp syscall_handler` |
| `cpuid` | `x86.c:1962-1967` | replaced by `call dbt_cpuid_internal` — so guest `/proc/cpuinfo` and glibc's IFUNC selection see a curated feature set (`src/dbt/cpuid.c`) |
| every control transfer | `x86.c:1790-1850`, `992-1080` | retargeted into the code cache: direct branches through patchable trampolines, indirect branches through a 64K-entry "sieve" hash and a 64K-entry return cache |

`%fs:` is not translated — it is **refused** (`x86.c:1399-1402`: `log_error("FS segment override not
supported."); __debugbreak();`), because flinux has taken `%fs` for itself. This is the mirror image of our
transliterator's refusal and is discussed in §5.

There is **no vDSO or vsyscall machinery in the shipped tree** (`grep -ri vsyscall\|vdso src/` returns
nothing). The pre-DBT VEH did emulate the x86-64 vsyscall page by catching the DEP fault at
`0xFFFFFFFFFF600000` and servicing it from the handler; that code did not survive the DBT rewrite. So
vDSO is *not* a reason `src/dbt/` exists, despite being a plausible candidate. Self-modifying code is not
a reason either — see §2.3, where it is handled by invalidation rather than translation.

**Conclusion.** `src/dbt/` exists because a user-mode process on 64-bit Windows cannot create a segment
descriptor, and a same-ISA translator was the cheapest way to make guest segment-relative addressing work
without faulting on every access. That is a *Windows platform* constraint, not an ISA one — which is why
it transfers to us directly even though our situation (x86-64 guest, x86-64 host) is not theirs.

---

## 2. Code cache mechanics

### 2.1 Allocation: RWX, flatly

```
dbt->code_cache = VirtualAlloc(NULL, DBT_CACHE_SIZE, MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN,
                               PAGE_EXECUTE_READWRITE);          /* x86.c:768 */
```

`DBT_CACHE_SIZE` is 8 MiB (`x86.c:481`), and there is a second 8 MiB `PAGE_READWRITE` region for block
metadata (`x86.c:766`). Both are allocated **per guest thread** in `dbt_init_thread` (`x86.c:763-772`), so
each guest thread costs 16 MiB of address space and its own completely independent copy of every translated
block. The shared return trampoline gets its own RWX page at `x86.c:786`.

**There is no W^X anywhere.** No dual alias, no `VirtualProtect` toggling, no per-thread write gate. The
generator writes bytes and jumps to them in the same mapping. flinux predates Windows 10's `VirtualAlloc2`
/ `MapViewOfFile3` placeholder API, so it had no cheap way to do better, but nothing suggests it was
considered.

**There is no `FlushInstructionCache` call in the entire tree** (`grep -rn FlushInstructionCache src/` is
empty). On x86 this is defensible — the architecture's instruction cache is coherent with stores and only a
serialising instruction is architecturally required, which the intervening indirect branch supplies. It is
still not what Microsoft documents, and it is not portable. Our `docs/windows/toolchain.md` §3.1 probe
already calls `FlushInstructionCache` and should keep doing so.

### 2.2 The pointer discipline that RWX buys them

Because writable address == executable address, flinux can do three things we cannot do as directly:

- **Bake absolute cache addresses into generated code.** The sieve dispatcher emits
  `jmp dword ptr [ecx*4 + <literal sieve_table address>]` (`x86.c:867-869`); the return-cache trampoline
  bakes `dbt->return_cache` as a disp32 (`x86.c:1250`).
- **Patch already-executing code in place.** `dbt_find_direct` (`x86.c:2039-2050`) overwrites the rel32 of
  a `jmp`/`call` in the cache the first time it is taken, so the second execution goes direct. There is no
  writable alias to compute; it stores through the same pointer it just ran.
- **Thread the sieve's collision chains through the emitted code itself** (`x86.c:2020-2035`), reading and
  writing a link field embedded inside an instruction.

On a dual-alias host every one of those becomes `write through rw_base + (target - rx_base)`. That is
mechanical, but it is exactly the class of bug that is invisible until a `VirtualProtect` policy changes.
Our contract already carries the two pointers (`hl_host_code_mapping.writable_address` /
`.executable_address`, `HL_HOST_CODE_DUAL_ALIAS`), which is the right shape; the discipline has to be that
*emitters never see the executable pointer except as a value to encode*.

### 2.3 Invalidation: wholesale flush, and nothing finer

`dbt_code_changed(pc, len)` (`x86.c:812-827`) looks for any cached block whose guest PC falls at or after
`pc`, and if one exists within range it calls `dbt_flush()` — which resets every hash bucket, rebuilds the
trampoline tables and restarts the bump allocator from zero (`x86.c:798-805`). Its own comment concedes the
gap: `/* TODO: Take care of signal/thread safety */`.

Its single caller is the unmap path in the memory manager (`src/syscall/mm.c:1346`), fired when a region
with `PROT_EXEC` is destroyed. So the invalidation trigger is *unmapping executable memory*, not *writing
to it*: **flinux has no write-protect-based self-modifying-code detection at all.** A guest that rewrites
its own already-executed code without unmapping it will run stale translations. That is a correctness hole
we do not have and must not import — `smc_on_write` (`src/linux_abi/x86.c:1172`) exists precisely to close
it.

The cache is also flushed on exhaustion (`x86.c:1316-1326`) and on `execve` (`src/syscall/exec.c:408`).
Because everything is per-thread, a flush is thread-local and needs no stop-the-world — the mirror of our
`src/translator/cache.c` STW machinery, bought by giving every thread its own 8 MiB.

`dbt_flushed` (`x86.c:551-555`) is a small idea worth stealing: a thread-local flag the caller sets to
false, then reads after an operation that might have flushed, so it knows whether a pointer it captured
earlier is still valid. `dbt_find_direct` uses it to skip patching a `jmp` that no longer exists
(`x86.c:2042-2048`).

---

## 3. Exception and signal handling

This is where flinux's answer is most surprising, and the surprise is what it *does not* do.

### 3.1 The mechanism is VEH, and only VEH

`AddVectoredExceptionHandler(TRUE, exception_handler)` — `src/syscall/syscall.c:131`, called once from
`main` (`src/main.c:257`) just before `execve`. `TRUE` is first-in-chain. There is no `__try`/`__except`,
no `SetUnhandledExceptionFilter`, no `.seh_handler`, no `RtlAddFunctionTable`, and no per-thread
registration — the handler is process-wide and covers every thread including ones created later by
`clone` (§4). This matches our design in `docs/windows/signals-and-faults.md` §3 exactly.

### 3.2 What the handler is for — and what it is *not* for

The full handler is `syscall.c:38-133`. Its structure:

1. `DBG_CONTROL_C` and `EXCEPTION_BREAKPOINT` → `EXCEPTION_CONTINUE_SEARCH` immediately (`:40-43`). This is
   the "decline what we don't own, before touching anything" rule our §3 already states.
2. `EXCEPTION_ACCESS_VIOLATION` with `ExceptionInformation[0] == 8` (DEP / instruction fetch) → treat the
   *faulting IP* as the address to fault in, `mm_handle_page_fault(code, false)`, and retry (`:47-57`).
   Note the fallback at `:55`: if that fails, try `code + PAGE_SIZE`, because the instruction may straddle
   a page boundary. That is a real hazard for lazily-committed guest text and we get it for free by
   consuming `Info[1]` instead of `Rip`.
3. Otherwise, read/write: `mm_handle_page_fault(ExceptionInformation[1], Info[0] == 1)` and retry (`:63-65`).
   This is the demand-paging and copy-on-write engine — flinux's memory manager commits guest pages lazily
   and implements fork-CoW manually (`src/syscall/mm.c:931-961`), so *most* access violations in this
   process are the engine's own bookkeeping, not guest faults.
4. Three IP-range checks for `mm_check_read` / `mm_check_read_string` / `mm_check_write` (`:67-84`): if the
   fault happened inside one of those hand-written probe loops, rewrite `ContextRecord->Xip` to the loop's
   failure label and continue. This is the same trick as our `hrm_fault_hook` /`host_range_mapped` probe
   (`src/linux_abi/thread.c:1634-1691`) — a deliberate faulting read used as `access_ok()` — implemented
   by PC-range comparison against linker-visible bounds rather than by a jump pad.
5. Everything else: dump the memory map and register file, `log_shutdown()`, `EXCEPTION_CONTINUE_SEARCH`
   (`:93-126`). The process dies.

**Step 5 is the finding. A guest memory fault is never turned into a guest `SIGSEGV`.** There is no
`si_addr`, no `SEGV_MAPERR`/`SEGV_ACCERR` classification, no signal-frame construction on the fault path.
`SIGSEGV` appears in flinux only as something another process can `kill()` you with, and its default
disposition is `process_exit` (`src/syscall/sig.c:104-124`). A guest that installs a `SIGSEGV` handler and
dereferences null does not get its handler called; flinux logs a crash dump and exits.

So flinux answers the *fault-to-resume* half of our problem (§3.3) and does not attempt the
*fault-to-guest-signal* half at all. Our `deliver_guest_fault_hint` path
(`src/linux_abi/signal.c:831-897`) has **no counterpart here**, and flinux is therefore not evidence about
it in either direction.

### 3.3 Resuming

Every successful path is `EXCEPTION_CONTINUE_EXECUTION` with the `ContextRecord` either untouched (retry
the faulting instruction, after a VM call has made it succeed) or with `Xip` rewritten (the probe-loop
bailouts). Both shapes are already in our design at §4.1 and both were already measured working by
`docs/windows/signals-and-faults.md` §0.1. flinux is independent confirmation from a shipping program, not
new information.

### 3.4 Signal delivery: a dedicated signal thread, and no preemption

flinux delivers signals the way Cygwin does, and `docs/windows/prior-art-cygwin-threads-signals.md` covers
the pattern. The flinux specifics:

- A private `signal_thread` (`src/syscall/sig.c:192-269`) owns dispositions and the pending set. Senders
  post fixed-size packets down a message-mode named pipe read through an IOCP (`sig.c:56-102, 348-353`).
  Child death is detected by giving each child a duplicated handle to the write end of a private pipe and
  watching the read end fail — a neat `SIGCHLD` substrate that works even on abnormal exit
  (`sig.c:370-390`).
- To deliver, the signal thread picks a target guest thread whose mask permits the signal, then does
  `SuspendThread` → `GetThreadContext` → **`dbt_deliver_signal`** → `SetThreadContext` → `ResumeThread`
  (`sig.c:154-164`).

`dbt_deliver_signal` (`x86.c:2078-2096`) is the interesting part, and it splits on where the victim was:

- **Inside the code cache** — rewrite `context->Eip` to the signal trampoline immediately, and stash the
  original translated `Eip` in the victim's TLS. Set `signal_need_fixup`.
- **Anywhere else** (i.e. inside flinux's own C code, servicing a syscall) — do *not* touch `Eip`. Instead
  overwrite the victim's TLS "return address" slot with the signal trampoline, so the signal is taken when
  the syscall finishes and control returns to guest code.

That second case is why every return from kernel code to guest code in flinux goes through one indirect
`jmp fs:[return_addr]` (`x86.c:557-571`) instead of a direct branch. The comment says so: *"This enables us
to do efficient return address patching on receipt of signals."* It is a deliberate, permanent cost —
one indirect jump per kernel→guest transition — bought to make asynchronous signals deliverable at a safe
point without any preemption mechanism.

**flinux never delivers a signal at an arbitrary instruction.** It delivers at a translated-block boundary
or at a syscall return. That is the same constraint our §1.2 records (*"A Windows VEH is only ever entered
synchronously… `SuspendThread` runs nothing"*), and flinux's answer is the same one available to us:
make every guest→engine and engine→guest edge go through a patchable indirection.

### 3.5 Reconstructing guest state from a translated PC — the best idea in the tree

The `signal_need_fixup` case has a hard problem: the victim was suspended at some arbitrary byte inside a
*translated* block. To build a Linux `sigcontext` you need the **guest** PC and the **guest** register
values, and at that instant some guest register may be spilled to host TLS while a translator-inserted
scratch sequence is mid-flight.

flinux solves this **without any side table**, by exploiting the fact that translation is deterministic:
`dbt_setup_signal_handler` (`x86.c:2098-2105`) calls `dbt_translate(0, context)` in a second mode. Given a
non-NULL context, `dbt_translate` finds the block containing the translated PC (`x86.c:1302-1311`) and
**re-runs the translation of that block from the start**, tracking the output pointer as it goes. At every
instruction boundary and at every point *inside* a multi-instruction expansion it compares the output
pointer with the trapped PC, and when they meet it rewrites the context to the equivalent guest state and
stops (`x86.c:1345-1350`, and per-handler at `1160-1204`, `1206-1259`, `1871-1908`).

The per-expansion fixups are explicitly two-sided — the code comments them *"the instruction is not yet
executed, rollback"* vs *"the instruction is already executed, commit"* (`x86.c:1886-1902`) — and each one
undoes the scratch spill by reading the value back out of host TLS and putting it in the right context
slot, and un-does any stack adjustment the expansion had made (`x86.c:1199`, `1218`, `1254`).

This is a translator-as-its-own-inverse-map. It costs nothing at translation time and nothing in memory;
it costs one re-translation on the rare signal path. Several handlers carry `/* TODO: Fix context */`
(`x86.c:1912`, `1964`), so coverage was incomplete — a signal arriving inside a `mov %gs, r` or a `cpuid`
expansion would produce wrong guest state. The idea is still sound and the gaps are localised.

### 3.6 Entering and leaving the guest handler

`dbt_gen_signal_trampoline` (`x86.c:637-697`) is generated once per thread into the code cache. It:

1. Saves the guest `esp` to TLS and **switches to a separate kernel stack** held in TLS
   (`x86.c:643-648`). The commit history records this as a deliberate fix: `7d55738 "dbt: Never tamper user
   stack."`
2. Spills the full guest register file into a `struct syscall_context` on that kernel stack, laid out to
   match what `fork()` also consumes (`x86.h:29-45`).
3. Calls into C (`dbt_setup_signal_handler` → `signal_setup_handler`, `sig.c:301-346`) which builds a real
   Linux `struct rt_sigframe` **on the guest stack**, `fxsave`s FP state below it, sets `pretcode`, applies
   the handler's `sa_mask`, and rewrites the context to enter the handler with `eax`/`edx`/`ecx` = sig,
   `&info`, `&uc` per the i386 ABI.
4. Reloads the registers and jumps to `dbt_find_indirect_internal` — i.e. **the guest handler is entered by
   asking the translator for the handler's address**, exactly like any other indirect branch.

Return is by the same route. `frame->pretcode` points at a tiny host-image stub `signal_restorer`
(`src/syscall/stubs.asm:136-139`) whose entire body is `mov eax, 173; int 0x80`. When the guest handler
executes `ret`, the translated `ret` dispatches to that address, the DBT *translates those two
instructions as if they were guest code*, the `int 0x80` becomes a call into `sys_rt_sigreturn`
(`sig.c:404-421`), which restores the mask and FP state and hands the saved `sigcontext` to
`dbt_sigreturn` → the sigreturn trampoline (`x86.c:699-726`), which reloads guest registers and jumps
back through the indirect dispatcher.

Two properties worth naming: the guest never sees a host address it cannot execute, and the restorer costs
no special case in the translator — it is guest-shaped bytes that happen to live in the host image.

### 3.7 `sigaltstack` and stack overflow

`sigaltstack` is **not implemented** — `sig.c:713-718` logs an error and returns 0. There is no alternate
signal stack, no `SetThreadStackGuarantee`, and no handling of `EXCEPTION_STACK_OVERFLOW` anywhere in the
tree. A guest stack overflow in flinux produces a crash dump and exit. So flinux offers **no evidence** for
our §5, and specifically none for the transliterator gap identified there. §6 below closes part of that
gap by measurement instead.

### 3.8 Faults inside generated code: what flinux shows, and what it does not

flinux's VEH plainly *does* fire for faults raised in the code cache — the code cache is where guest code
runs, and demand paging (`mm_handle_page_fault`) is serviced from there constantly, on RWX pages that carry
no unwind information whatsoever. So "a VEH sees faults in JIT'd code on Windows" is established by a
shipping program with no `RtlAddFunctionTable` in it.

But flinux is 32-bit x86 (`src/dbt/x86_trampoline.asm` is `.MODEL FLAT`; the `x64` project configurations
exist in `flinux.vcxproj` but no x64 DBT does), and **x86 has no table-based unwinding at all**. Its
silence about `RtlAddFunctionTable` is therefore not evidence for x64. That question is only answerable by
measurement, which is §6.

---

## 4. Threads

**Yes, `clone` is supported**, and it is a real Windows thread, not a process.

`sys_clone_imp` (`src/syscall/fork.c:296-307`) branches on `CLONE_THREAD`: with it, `fork_thread`
(`fork.c:261-282`) does `CreateThread(..., CREATE_SUSPENDED)`, fills a heap-allocated `fork_info` with the
parent's `syscall_context`, the child stack, the `CLONE_*` tid pointers and **the parent's simulated `%gs`
selector** (`fork.c:276`), then resumes. Without `CLONE_THREAD` it goes down the NT process-cloning path
that `docs/windows/fork-model.md` and `docs/windows/prior-art-cygwin-fork.md` cover.

The child entry point (`fork.c:240-259`) is the interesting list, because it enumerates exactly which
per-thread state a translator owns:

```
log_init_thread();            /* per-thread log buffer            */
dbt_init_thread();            /* a fresh 16 MiB code cache + metadata, per thread */
process_thread_entry(pid);    /* struct thread, 1 MiB RWX stack (process.c:182) */
tls_set_thread_area(...);     /* CLONE_SETTLS                      */
dbt_update_tls(info->gs);     /* recompute the cached TLS base     */
dbt_restore_fork_context(...) /* jump into translated code with eax = 0 */
```

Per-thread state is held in three overlapping ways:

- `__declspec(thread)` for engine-side pointers: `dbt` (`x86.c:550`), `dbt_flushed` (`x86.c:555`), the
  16-byte-aligned 512-byte `fxsave` scratch (`x86.c:538`), `current_thread` (`src/syscall/process.c:55`).
  This is the direct analogue of our `__thread` use, and it is exactly the state that does **not** survive
  NT process cloning — which is why `tls_fork`/`tls_afterfork_child` (`src/syscall/tls.c:74-109`)
  explicitly snapshot every TLS slot value into shared memory before the clone and re-`TlsAlloc` +
  re-`TlsSetValue` them in the child. Any `__thread` variable we add is a line item on that same list.
- Win32 TLS slots (`TlsAlloc`) for anything **generated code must reach**, because generated code cannot
  call `TlsGetValue` — it reads `fs:[offset]` directly, and `tls_slot_to_offset` (`tls.c:116-122`) converts
  a slot index into the `TEB.TlsSlots` / `TEB.TlsExpansionSlots` offset the emitter bakes as a literal.
  Eight such kernel slots exist (`x86.c:484-495`): the `dbt` pointer, a scratch spill word, the simulated
  `%gs` value and its resolved base, the patchable return address, the kernel `esp`, the saved guest `esp`
  and the saved guest `eip`.
- The `struct thread` in shared memory (`src/syscall/process_info.h:39-67`), carrying `sigmask`,
  `current_siginfo`, `can_accept_signal` and the wake event.

**Interaction with exception handling.** The VEH is process-wide and needs no per-thread registration, so a
`clone`d thread is covered the instant it exists. The signal thread reaches a victim thread's engine state
without touching that thread by reading its TEB directly:
`NtQueryInformationThread(ThreadBasicInformation)` → `TebBaseAddress` → `+ tls_dbt_offset`
(`x86.c:2080-2082`). That is a genuinely useful primitive — cross-thread access to another thread's
`__thread` storage, with the thread suspended — and it has no POSIX equivalent.

The hazard it creates is the obvious one: `signal_thread_deliver_signal` suspends a thread at an arbitrary
instruction and then calls `dbt_deliver_signal`, which reads `dbt->internal_trampoline_end` and writes
`dbt->signal_need_fixup` **in the victim's `struct dbt_data`** while the victim is stopped. Safe only
because the victim is suspended and only one signal thread exists. The `TODO`s around `signal->actions`
races (`sig.c:319`, `334`, `342`) show the author knew the locking was not finished.

---

## 5. The `fs:`/`gs:` collision, and how it maps onto us

### 5.1 flinux's resolution (x86)

| | Linux x86 guest | Windows x86 host | flinux's resolution |
|---|---|---|---|
| `%fs` | mostly unused | **TEB** | host keeps it. Guest `%fs` prefix is refused outright (`x86.c:1399-1402`) |
| `%gs` | **TLS**, via a GDT entry | unused | neither: the *register* is never loaded. The selector value is simulated in host TLS and every `%gs:` access is rewritten to an explicit base add |

`set_thread_area` (`tls.c:148-171`) does not create a descriptor. It calls `TlsAlloc()`, stores the guest's
requested base in that Win32 TLS slot, and hands the guest back an **index into flinux's own table** as the
"entry number". The guest dutifully computes a selector from it and `mov`s it to `%gs`; the translator
intercepts that `mov`, stores the value in a TLS word, converts index → TEB offset, reads the base out, and
caches it in a second TLS word (`x86.c:1910-1960`). Subsequent `%gs:disp` accesses become
`mov tmp, fs:[gs_addr]; lea tmp, [tmp + base]; <op> [tmp + index*scale + disp]`.

`arch_prctl` — the x86-64 way to set a segment base — is unimplemented in the shipped tree for all four
codes (`tls.c:178-203`). The pre-DBT x86-64 branch *did* implement `ARCH_SET_FS` by storing the base in a
Win32 TLS slot and then patching every faulting `%fs:` access into a trampoline, i.e. the same
software-fold, reached by trapping instead of translating.

### 5.2 Our situation is structurally identical, one register over

On x86-64, the assignment inverts: **Linux guests use `%fs` for TLS and Windows uses `%gs` for the TEB.**
That is a happy accident — the guest wants the register the host does not — and it is exactly the accident
flinux enjoyed on x86 and could not cash in, because the *registers* were never the problem. The
*descriptor/base machinery* was.

Two of our components are affected, in opposite ways:

- **The interpreter** already does the flinux thing, by construction:
  `address += cpu->fs_base` at `src/translator/guest/x86_64/interp.c:482,494,1424`,
  `src/translator/guest/x86_64/avx.c:157,2260`, with `arch_prctl` maintaining `c->fs_base`/`c->gs_base` in
  `legacy.c:99-116`. Guest `%fs` never touches a host segment register. **This is portable to Windows with
  no change at all**, and it is the same software-fold flinux arrived at.
- **The transliterator** (`HL_X86_TRANSLIT=1`) does the opposite. Its header states the model:
  *"`struct cpu` is reached through the `%gs` segment base, which is set once per guest thread with
  `arch_prctl(ARCH_SET_GS, cpu)`… The engine consequently OWNS the real `%gs`"*
  (`src/translator/guest/x86_64/translit/translit.c:11-16`), and it declines any guest `%fs`/`%gs` prefix
  for that reason. **On Windows that model is unavailable**: `%gs` is the TEB, and §6 measures both what
  that costs and whether the hardware alternative exists.

---

## 6. Measured: what Windows actually permits

Six probes, all in the session scratchpad, all clang 22.1.8 `x86_64-w64-windows-gnu` at `-O2`,
Windows 11 Pro 10.0.26200. Each subtest is a separate process invocation so a crash is attributable.

### 6.1 Faults in generated code

| Probe | Setup | Result |
|---|---|---|
| `rwx-resume` | RWX `VirtualAlloc` code cache, **no `RtlAddFunctionTable`**; JIT block stores through NULL; VEH rewrites `Rip` past the store | **PASS** — 1 VEH hit, `Rip` inside the JIT range, resumed, correct return value |
| `rwx-retry` | same, but target page is `PAGE_READONLY`; VEH `VirtualProtect`s it RW and returns `CONTINUE_EXECUTION` with `Rip` untouched | **PASS** — the store re-executes and the value lands. This is the `smc_on_write` / `repair_signal_page` shape |
| `dual-resume` | **W^X dual alias** — `VirtualAlloc2` placeholder split, one section mapped twice via `MapViewOfFile3` (`PAGE_READWRITE` + `PAGE_EXECUTE_READ`); code written through RW, executed through RX, faults there | **PASS** — VEH fires with `Rip` in the **RX** alias, resume works |

**Conclusion: faults inside dynamically generated code are delivered to a VEH, and resumable there, with no
unwind information registered, on both an RWX cache and a dual-alias cache.** This is the single most
important result for us and it is unambiguous. `EXCEPTION_ACCESS_VIOLATION` arrived with
`Info[0]=1` (write) and `Info[1]` = the exact faulting address in every case.

The reason is structural, not lucky: `RtlDispatchException` calls vectored handlers **before** it begins
the frame walk. If the VEH claims the exception, no unwinder ever runs, so there is nothing for missing
unwind info to break.

### 6.2 Does `RtlAddFunctionTable` matter?

It matters only when the VEH **declines**. Probe: a VEH that always returns `EXCEPTION_CONTINUE_SEARCH`,
and a C caller carrying a `.seh_handler` frame handler that wants the exception. The question is whether
SEH dispatch can walk *past* the JIT frame to reach it.

| JIT block shape | Unwind info | Frame handler reached? |
|---|---|---|
| plain C control (real xdata) | n/a | **yes** |
| balanced leaf (`mov [rcx],imm; ret`) | none | **yes** |
| `push rbx; push rbp; push r12` then fault | none | yes — **by luck, not contract** |
| `sub rsp, 0x100` then fault | none | **NO — process dies** |
| `sub rsp, 0x100` then fault | `RUNTIME_FUNCTION` + `UWOP_ALLOC_LARGE` | **yes** |

The mechanism: with no `RUNTIME_FUNCTION`, NT assumes the frame is a **leaf** — return address at `[Rsp]`,
`Rsp += 8`. A JIT block that has not moved `Rsp` at the fault point therefore unwinds correctly by
accident. One that has moved `Rsp` unwinds into garbage; with 24 bytes of displacement the walk happened to
recover on this host, with 0x100 bytes it did not and the process died.

**Ruling for us.** Unwind info is **not required** for the fault path, because the VEH claims the fault.
It becomes required for exactly three things: (a) any decline-and-let-something-upstack-handle-it design,
(b) debuggers and crash reporters walking through emitted frames, (c) `RtlUnwindEx`, which is what
`longjmp` uses — reinforcing §4.3's ruling that `longjmp` out of a VEH must not happen. This refines
`docs/windows/toolchain.md` §4's open item and `docs/windows/signals-and-faults.md` §4.3's closing remark
from "worth building once the code cache exists" to "**not on the fault path; needed for tooling and for
any non-VEH escape route**".

*(Gotcha for whoever writes it: in `UNWIND_CODE`, `UnwindOp` is bits 8–11 and `OpInfo` is bits 12–15 of the
16-bit slot. Getting those two swapped produces a table `RtlAddFunctionTable` accepts and that then kills
the process — one wasted build here.)*

### 6.3 Faults while the host SP is the guest SP

Two probes, identical except for the protection of the "guest stack" region. The JIT block switches `rsp`
into a `VirtualAlloc`'d region, faults on an unrelated address, and the VEH resumes past it.

| Guest stack protection | Result |
|---|---|
| `PAGE_READWRITE`, SP 128 KiB into a 256 KiB region | **PASS** — VEH fires, resume works |
| `PAGE_NOACCESS` (the `LOGUARD` shape), same SP | **process dies; the VEH never runs** |

This sharpens `docs/windows/signals-and-faults.md` §5.2 in two directions. The constraint is **not** "SP
must be the thread's own stack" — an ordinary committed region is fine, which is what makes a Windows
transliterator conceivable at all. The constraint **is** "the memory below SP must be writable at the
moment of the fault", because `KiUserExceptionDispatcher` pushes the ~1.3 KiB `EXCEPTION_RECORD` +
`CONTEXT` there before any handler runs. And it is broader than stack *overflow*: **any** fault taken while
SP points into inaccessible memory is unrecoverable, and it is unrecoverable *silently* — no handler of any
kind is entered.

For a future Windows transliterator this means the §5.2 conclusion holds and can be stated more strongly:
either switch to a host stack in the block prologue, or accept that a guest whose SP is in the guard region
kills the thread outright, with no signal, no diagnostic and no `SIGSEGV` delivery.

### 6.4 `%gs`, `%fs`, and `FSGSBASE`

| Measurement | Result |
|---|---|
| JIT code executing `mov rax, gs:[0x30]` | returns `NtCurrentTeb()` — Windows owns `%gs`, confirmed from generated code |
| `CPUID.7.0:EBX.FSGSBASE` | **1** — the CPU supports it |
| `wrfsbase` executed from user-mode JIT code | **succeeds**, no `#UD`, no exception. `CR4.FSGSBASE` is enabled on this build of Windows |
| `mov rax, fs:[0]` immediately after `wrfsbase` | returns the written value. Guest-style `%fs` TLS **works** |
| …after a tight compute loop | base **preserved** |
| …after `GetTickCount()` / `SwitchToThread()` / `Sleep(0)` | base **preserved** (these did not deschedule) |
| …after `Sleep(20)` | base is **0** |
| …after a blocking `WaitForSingleObject` | base is **0** |
| …after a VEH round trip (`int3`, handler advances `Rip`) | base **preserved** |
| a freshly created thread | base is **0**, as expected |
| `CONTEXT.SegFs` | `0x53` — a *selector*. `CONTEXT` has **no FS-base field at all** |

Reproduced identically across three runs.

**Conclusion: the hardware FS base is not thread state that Windows preserves.** It survives until the
thread is actually descheduled, at which point the kernel's context restore puts it back to 0. It cannot be
saved or restored through `Get`/`SetThreadContext` either, because `CONTEXT` does not carry it — which also
means it cannot be carried across our fork or checkpoint paths.

So `wrfsbase` is a trap: it works perfectly in every short test and then silently loses guest TLS the first
time the thread blocks. **Guest `%fs` must be folded in software on Windows**, which is exactly what the
interpreter already does with `cpu->fs_base` and exactly the conclusion flinux reached for `%gs` on x86,
for the structurally identical reason — *the OS will not maintain per-thread guest segment bases, and
there is no user-mode way to make it.*

It also means a Windows transliterator cannot reach `struct cpu` through `%gs` (Windows owns it) **or**
through `%fs` (not preserved). It would have to steal a GPR, or reach `struct cpu` through a TEB TLS slot
as `gs:[TlsSlots+n]` — the flinux idiom (`tls_slot_to_offset`, `tls.c:116-122`), and the only one of the
three that is actually available.

---

## 7. Adopt / adapt / reject

| # | Technique | Ruling | Why |
|---|---|---|---|
| 1 | **One process-wide first-chance VEH as the sole fault mechanism** (`syscall.c:131`) | **Adopt** — already our design | flinux confirms it from a shipping program: no SEH, no UEF, no per-thread registration, covers `clone`d threads for free. Our `docs/windows/prior-art-survey.md` §4.4 ruling stands; §0.1's refinement (frame handlers exist and compose) is orthogonal and flinux uses neither |
| 2 | **Decline foreign exception codes before touching anything** (`syscall.c:40-43`) | **Adopt** | flinux's list is two entries and ours must be longer (§3's `DBG_PRINTEXCEPTION_C`, `0x406D1388`, `0xE06D7363`, top-nibble-4), but the placement — first statement in the handler — is right |
| 3 | **Faults in generated code handled by VEH with no unwind info** | **Adopt** — measured (§6.1) | Works on RWX and on the dual alias. Nothing extra to build |
| 4 | **`RtlAddFunctionTable` for JIT frames** | **Adapt — defer, and re-scope** (§6.2) | Not needed for the fault path. Needed for debuggers/crash-reporters and for any path that declines to something upstack. When it is built, the block must be described honestly or dispatch dies |
| 5 | **PC-range test to identify a deliberate probe fault** (`syscall.c:67-84`) | **Adopt** | Cleaner than our `hrm_fault_hook` long-jump and needs no pad arming. Bracket the probe loop with linker-visible labels, compare `Rip`, rewrite it to the failure label. Composes with §4.3's pad design rather than replacing it |
| 6 | **`ExceptionInformation[0]`/`[1]` consumed directly** (`syscall.c:47,63`) | **Adopt** | Confirms §3.1. flinux additionally retries `Rip + PAGE_SIZE` on a DEP fault for straddling instructions (`syscall.c:55`) — a hazard we avoid by using `Info[1]`, but worth knowing exists |
| 7 | **Guest memory fault → guest `SIGSEGV`** | **Reject — no prior art here** | flinux does not do it (§3.2). It logs and exits. We must design `deliver_guest_fault_hint`'s Windows arm without this reference |
| 8 | **Software fold of the guest segment base** (`x86.c:1160-1204`; our `interp.c:482`) | **Adopt** — we already do it | Independently arrived at by both projects under the same constraint. The interpreter ports to Windows unchanged |
| 9 | **`wrfsbase` / `FSGSBASE` for guest `%fs`** | **Reject** — measured (§6.4) | Available, functional, and silently zeroed on the first deschedule. Not in `CONTEXT`, so not forkable or checkpointable. A trap that passes every short test |
| 10 | **Real `%gs` to reach `struct cpu` (our transliterator's model)** | **Reject on Windows** | Windows owns `%gs`. A Windows transliterator must steal a GPR or use a TEB TLS slot (§6.4). This is a hard blocker for that cell and should be recorded next to `translit.c:11-16` |
| 11 | **TEB TLS slot at a baked `gs:[offset]` as the emitted code's engine anchor** (`tls.c:116-122`, `x86.c:484-495`) | **Adapt** | The one available anchor. Note `TlsSlots` (< 64) and `TlsExpansionSlots` have different offsets and the expansion array is a *pointer* — one extra load. Reserve slots at init, never lazily |
| 12 | **RWX code cache** (`x86.c:768`) | **Reject** | We already have a measured dual-alias mapping (`toolchain.md` §3.1) and the contract shape for it. flinux's convenience (patch through the same pointer) is the thing to guard against, not to copy — see #13 |
| 13 | **Baking cache addresses and patching in place** (`x86.c:867,1250,2047`) | **Adapt** | Every one of these becomes `rw_base + (target − rx_base)` under dual alias. Discipline: emitters take the writable pointer; the executable pointer is a *value to encode*, never a destination |
| 14 | **No `FlushInstructionCache`** | **Reject** | Defensible on x86 only, undocumented, and not portable. Keep the call |
| 15 | **Invalidation by wholesale flush, triggered on unmap only** (`x86.c:812-827`) | **Reject** | No write-protect SMC detection at all — a real correctness hole. Our `smc_on_write` is strictly better. The *flush-everything* granularity is a reasonable fallback; the *trigger* is not |
| 16 | **`dbt_flushed` — "did a flush happen while I wasn't looking"** (`x86.c:551-555, 2042`) | **Adopt** | Three lines; prevents patching a pointer into a cache that has since been reset. Cheaper than a generation counter for the single-consumer case |
| 17 | **Per-thread code cache, no sharing, no STW** (`x86.c:763-772`) | **Reject** | 16 MiB per guest thread and every block translated once per thread. We already have shared-cache STW machinery (`src/translator/cache.c`) that this would regress |
| 18 | **Re-run the translator to invert PC and register state** (`x86.c:2098-2105`, `1283-1350`) | **Adopt the idea; evaluate against what we have** | The strongest technique in the tree: no side table, no memory cost, exact at every point *inside* a multi-instruction expansion. Our `hl_x86_signal_capture` solves the same problem differently; if it ever needs sub-instruction precision this is the cheap way to get it. flinux's own `TODO: Fix context` markers (`x86.c:1912,1964`) show the cost is completeness, not concept |
| 19 | **Patchable return-address slot for every kernel→guest edge** (`x86.c:557-571, 728-734`) | **Adapt** | The right answer to "Windows cannot interrupt a thread": make signal delivery a *patch at a known edge* rather than a preemption. Our dispatcher already returns through `block_return`, so the hook point exists; the cost is one indirection per transition |
| 20 | **Signal thread + `Suspend`/`GetThreadContext`/`SetThreadContext`/`Resume`** (`sig.c:154-164`) | **Adapt** | Same conclusion as `prior-art-cygwin-threads-signals.md`. Note the two-case split in `dbt_deliver_signal` (`x86.c:2084-2095`): rewrite `Rip` if inside the cache, patch the return slot if inside engine code. Getting that split wrong means delivering a signal in the middle of a syscall |
| 21 | **Reach a peer thread's TLS via `NtQueryInformationThread` → `TebBaseAddress`** (`x86.c:2080-2082`) | **Adopt where needed** | No POSIX equivalent. Lets a control thread read/write a suspended peer's per-thread engine state with no cooperation. Undocumented-ish (`ntdll.h` in-tree) but stable |
| 22 | **Guest handler return via a host-image stub of guest-shaped bytes** (`stubs.asm:136-139`) | **Adopt** | `mov eax,<nr>; int 0x80` as the `sa_restorer`: costs no translator special case because the translator just translates it. Our `SIGRETURN_PC` machinery is equivalent; this is a cheaper spelling if we ever need the guest to see a real address |
| 23 | **Kernel stack switch before touching engine C code** (`x86.c:643-648`) | **Adopt** | flinux fixed this as a bug (`7d55738 "Never tamper user stack"`), and §6.3 shows why it matters more on Windows than on Linux: exception dispatch needs writable memory below SP, so an engine that runs C on the guest stack loses fault delivery outright when the guest stack is exhausted |
| 24 | **`sigaltstack`** | **No prior art** | Unimplemented in flinux (`sig.c:713-718`). `docs/windows/signals-and-faults.md` §5 stands on its own; §6.3 above is the only new evidence |

---

## 8. What is still open

1. **`Info[0] == 8` for instruction-fetch faults** is used by flinux for demand-paging text, and our §3.1
   proposes it as a second oracle for `raise_guest_fetch_fault`. Not measured here.
2. **§6.3's "writable memory below SP" rule** was measured with `PAGE_NOACCESS`. It was not measured with
   `PAGE_READONLY`, nor with a region large enough for the frame but too small for the frame plus the
   handler's own stack usage. The safe margin is unknown.
3. **`RtlAddFunctionTable` under a flushing code cache.** §6.2 registered one static table. Whether
   `RtlDeleteFunctionTable` / re-registration is cheap enough to run on every cache flush, and whether a
   stale table can be observed during the window, is unmeasured.
4. **flinux's re-translation fixup (#18) has never been validated against a translator with our
   optimisation level.** flinux's translator is a one-pass copier with fixed expansions; determinism is
   trivially true there. It is a precondition, not a free property.
5. **Nothing here touches CFG or CET/shadow stacks**, which `toolchain.md` §4 also leaves open. flinux
   predates both.
