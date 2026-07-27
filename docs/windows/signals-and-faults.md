# Signals and faults on a Windows host

Design, not a record: nothing here has been compiled or run. Every Win32 claim is either cited to Microsoft's
own documentation (§9) or marked as reasoned-but-unverified. Every engine claim was checked against the tree
on `feat/windows-amd64`; line numbers drift, the symbol names do not.

`DOCS.md` is normative. `docs/arch.md` §4.2 item 5 names `src/host/native_context.h` as the seam this
document fills in; item 2 explains why the fault path forks on the host CPU at all.

The scope is the host-OS axis only. The host CPU is x86-64 first; the AArch64-Windows cell is designed here
because the matrix must be total, not because anything will run on it soon.

---

## 1. The shape of the problem

The engine does not "use signals" in one place. It uses them in four unrelated ways, and only three of them
have to exist on Windows at all. Getting that split right is most of the work, because the fourth is the
largest by line count and is entirely portable.

### 1.1 Inventory

**(a) Guest memory fault → guest SIGSEGV/SIGBUS delivery.** The load-bearing one.

| Site | What it does |
|---|---|
| `src/core/target/x86_64.c:925-935` | installs `jit86_lazyguard` on host SIGSEGV+SIGBUS, `SA_SIGINFO`, `SA_ONSTACK` iff `translit_enabled()` |
| `src/linux_abi/elf.c:676-683` | the aarch64 twin: `nonpie_guard` on SIGSEGV+SIGBUS |
| `src/linux_abi/x86.c:1101-1236` | `jit86_lazyguard` — the classifier chain, in order: `ldapr_align_fixup`, `lse_align_fixup`, `hrm_fault_hook`, external-kill routing (`si_code <= 0`), `fastclk_fault_fixup`, hardware SIGBUS, `nonpie_fixup`, `gna_hit`, `gro_hit`, `smc_on_write`, `deliver_guest_fault`, the lazy zero-page grower, `deliver_guest_fatal_fault`, re-raise |
| `src/linux_abi/signal.c:831-897` | `deliver_guest_fault_hint` — the common tail: `sigframe_capture_fault`, set `sync_signal`/`sync_code`/`sync_address`, force-unmask, set `tpending`, `sigframe_resume_dispatch` |
| `src/core/target/{x86_64,aarch64}.c` | `sigframe_capture_fault` / `sigframe_resume_dispatch`, forked on `HL_HOST_CPU_AARCH64` |
| `src/translator/guest/*/signal.c` | `hl_{x86,aarch64}_signal_capture` / `_resume` — reconstruct guest state from the host mcontext (JIT), rewrite `HL_HOST_UC_PC` to `block_return`, and return |
| `src/translator/guest/*/interp.c:112-172` | `interp_signal_capture` / `interp_signal_resume` — the interpreter's counterpart: `struct cpu` is already exact, so the handler only has to *abandon* the in-flight `memcpy` by `siglongjmp` to `run_block`'s pad |
| `src/linux_abi/thread.c:1634-1691` | `hrm_fault_hook` + `host_range_mapped` — a deliberately *faulting probe read* used as the kernel's `access_ok()`; the handler long-jumps back to report "unmapped" |
| `src/translator/guest/x86_64/signal.c:186` | `hl_x86_signal_fast_clock_fault` — recovers a bad guest pointer in the inlined vDSO clock path as `-EFAULT` by rewriting a host GPR and the host PC |
| `src/core/target/aarch64.c:527-580` | `mach_resolve_fault` — the macOS-only Mach exception port. **No Windows counterpart is needed**; VEH is the single entry point and does not need a second, out-of-band one |

**(b) JIT / self-modifying-code write-protect faults.** `smc_on_write()` inside `jit86_lazyguard`
(`src/linux_abi/x86.c:1172`), plus `gro_hit` (guest read-only mappings) and `gna_hit` (guest `PROT_NONE`)
immediately above it, plus the lazy zero-page grower's single permitted VM call
`host->memory->repair_signal_page` (`include/hl/host_services.h:216-222`). Also W^X: `jit_cache_init`'s
dual RW/RX alias, and `elf_protect.h`'s `.rodata` enforcement.

**(c) Engine-internal control.**

| Signal | Where | Purpose |
|---|---|---|
| `SIGCHLD` | `src/host/child.c` | self-pipe wake for the fork-server's `poll()` loop (`src/linux_abi/fork.c:410,453,469`) |
| `SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGUSR1/SIGUSR2` | `src/core/activation.c:241-289` | `activation_forwarded[]` → self-pipe → relay thread → `hl_engine_request(HL_ENGINE_REQUEST_SIGNAL)` |
| `STW_SIG` (= `SIGEMT`) | `src/translator/cache.c:1002,1159-1199` | stop-the-world: park every peer guest thread at a safepoint so the code cache can be flushed; also the checkpoint freeze (`src/linux_abi/checkpoint.c:1183`) |
| `THREAD_INT_SIG` (= `SIGINFO`, `SIGRTMIN+7` on Linux) | `src/linux_abi/thread.c:2296-2519`, `src/host/linux/system.c:261-269` | EINTR a peer blocked in a host syscall; `hl_host_process_interrupt` |
| `SIGFPE` | `src/core/target/x86_64.c:666` | `raise(SIGFPE)` so the parent's `wait4` reconstructs `WIFSIGNALED` for a guest `#DE` with no handler |
| host-mask mirroring | `src/linux_abi/syscall/signal.c` case 135 | guest SIGTSTP/TTIN/TTOU are mirrored onto the **real host mask** for bash job control |

`sig_host_is_engine_control()` (`src/linux_abi/signal.c:220`) exists solely to keep `STW_SIG` and
`THREAD_INT_SIG` out of the guest's disposition table.

**(d) Guest signal emulation that never touches a host signal.** Needs no Windows answer and ports
unchanged: `hl_{x86,aarch64}_signal_build`/`_restore` (the rt_sigframe, `src/translator/guest/*/signal.c`),
`g_sigact`/`g_pending`/`c->tpending`, the per-signal `sigq_*` FIFO, `maybe_deliver_signal`,
`sigreturn_frame`/`SIGRETURN_PC`, the guest `sigaltstack` model (`c->alt_sp`/`alt_size`/`alt_flags`),
`sig_default_terminates`, `sig_coredumps`, the `sigexit` shared table, `signalfd`, `raise_guest_signal`,
`interp_raise_sync_signal`, `raise_guest_de`/`_trap`/`_bus`/`_fetch_fault`/`_data_map_fault`, the ptrace
signal- and group-stop machinery, and all of `rt_sigaction`/`rt_sigprocmask`/`sigsuspend`/`sigtimedwait`
**as long as sender and target are inside one engine process tree**.

Three leaks out of (d) into (c) that must not be missed:

- `rt_sigaction` (`src/linux_abi/syscall/signal.c` case 134) installs a **real host handler**
  (`host_sigh_si` on `sig_l2m(sig)`) so that a cross-*process* kill lands. Windows has no host disposition
  to install; cross-engine-process signalling has to move onto an engine control channel (§7.3).
- case 135 mirrors guest stop-signals onto the real host mask. There is no host mask on Windows.
- `sigsuspend(&empty)` is used as "sleep until any host signal" (cases at `signal.c:340,404`). Becomes a
  wait on a per-thread wake event.

### 1.2 What Win32 gives back, and what it takes away

Two structural differences drive everything below.

**A Windows VEH is only ever entered synchronously.** There is no asynchronous vectored handler and no
mechanism to run user code on a thread at an arbitrary instruction: `SuspendThread` runs nothing,
`QueueUserAPC` fires only at an alertable wait, and a console control event runs on a *new* thread. So
(a) and (b) map cleanly and (c) does not map at all — every category-(c) use has to be rebuilt on a
different primitive.

**A Windows exception carries strictly more information than a POSIX one.** `ExceptionInformation[0]`
gives read / write / DEP-execute authoritatively, where POSIX gives only `si_addr` plus an ucontext error
word the engine never reads. Several places in `jit86_lazyguard` currently *infer* write-ness ("Host reads
remain legal under `PROT_READ`, so any protection fault in this registry is a write fault",
`src/linux_abi/x86.c:1159`); on Windows they can stop inferring.

---

## 2. `native_context.h`: the two Windows cells

### 2.1 The typing decision

Every consumer of the matrix casts the opaque `void *native_context` to `ucontext_t *` and then applies the
`HL_HOST_UC_*` macros. There are 18 such sites across six files (`src/core/target/aarch64.c`,
`src/linux_abi/{elf.c,x86.c,signal.c}`, `src/translator/guest/{aarch64,x86_64}/signal.c`,
`src/translator/guest/x86_64/translit/translit.c`).

On Win32 the native context *is* the `CONTEXT` record the VEH receives as
`ExceptionInfo->ContextRecord`. Nothing else in a Windows build declares `ucontext_t`. So the Windows cells
`typedef CONTEXT ucontext_t;` and every existing cast compiles unchanged.

This is a deliberate, temporary lie about the name. The honest fix is to rename the engine-wide spelling to
`hl_host_native_context_t` and have each cell typedef *that*; it is a mechanical ~18-line change across
files other agents own, so it is proposed as a follow-up rather than taken here.

### 2.2 Proposed diff

```diff
--- a/src/host/native_context.h
+++ b/src/host/native_context.h
@@
 #ifndef HL_HOST_NATIVE_CONTEXT_H
 #define HL_HOST_NATIVE_CONTEXT_H
 
 #include <stdint.h>
+#include <stddef.h>
 
 /* Signal-context register extraction: a matrix over host OS (shape of
- * ucontext_t) x host CPU (register file). HL_HOST_UC_PC / HL_HOST_UC_SP exist
+ * the native fault context) x host CPU (register file).  On POSIX hosts that
+ * context is a ucontext_t; on Win32 it is the CONTEXT record a vectored
+ * exception handler receives.  HL_HOST_UC_PC / HL_HOST_UC_SP exist
  * on every (OS, CPU) pair and are the only ones portable code may use;
  * CPU-shaped accessors sit behind HL_HOST_HAS_{A64,X64}_CONTEXT. */
 
 #include "host_cpu.h"
@@ (macOS/AArch64, macOS/x86-64, Linux/AArch64 cells unchanged)
@@ (Linux / x86-64 cell)
 #define HL_HOST_UC_REG_RIP REG_RIP
 #define HL_HOST_UC_REG_EFL REG_EFL
+/* EFLAGS is reachable as a greg index only on Linux; the Win32 CONTEXT sites
+ * it outside the GPR block.  Portable x86-64 code must use this accessor.
+ * (translit.c's `gregs[HL_HOST_UC_REG_EFL]` is the one call site to move.) */
+#define HL_HOST_UC_EFLAGS(uc) ((uc)->uc_mcontext.gregs[REG_EFL])
 
 /* xmm0..15, or NULL: fpregs is optional, so callers must null-check. */
 static inline void *hl_host_uc_xmm(ucontext_t *context) {
@@
 #define HL_HOST_UC_XMM(uc) hl_host_uc_xmm(uc)
 
+/* Windows / x86-64 */
+#elif defined(_WIN32) && defined(HL_HOST_CPU_X86_64)
+#ifndef WIN32_LEAN_AND_MEAN
+#define WIN32_LEAN_AND_MEAN 1
+#endif
+#ifndef NOMINMAX
+#define NOMINMAX 1
+#endif
+#ifndef NOGDI
+#define NOGDI 1
+#endif
+#include <windows.h>
+
+/* The fault path threads its native context as `void *` and every consumer
+ * casts it to ucontext_t.  On Win32 that context IS the CONTEXT record the
+ * vectored handler receives (ExceptionInfo->ContextRecord), and nothing else
+ * in a Windows build declares ucontext_t, so name it here rather than churn
+ * 18 cast sites.  FOLLOW-UP: rename the engine-wide spelling to
+ * hl_host_native_context_t and drop this typedef. */
+typedef CONTEXT ucontext_t;
+
+#define HL_HOST_HAS_X64_CONTEXT 1
+#define HL_HOST_UC_PC(uc) ((uc)->Rip)
+#define HL_HOST_UC_SP(uc) ((uc)->Rsp)
+
+/* CONTEXT declares Rax,Rcx,Rdx,Rbx,Rsp,Rbp,Rsi,Rdi,R8..R15,Rip as consecutive
+ * DWORD64 members -- i.e. in x86 register-encoding order -- so the Linux
+ * cell's flat "gregs + index" idiom survives verbatim, with the natural
+ * register numbers as the indices.  Asserted below, not assumed. */
+#define HL_HOST_UC_GREGS(uc) ((DWORD64 *)&(uc)->Rax)
+#define HL_HOST_UC_REG_RAX 0
+#define HL_HOST_UC_REG_RCX 1
+#define HL_HOST_UC_REG_RDX 2
+#define HL_HOST_UC_REG_RBX 3
+#define HL_HOST_UC_REG_RSP 4
+#define HL_HOST_UC_REG_RBP 5
+#define HL_HOST_UC_REG_RSI 6
+#define HL_HOST_UC_REG_RDI 7
+#define HL_HOST_UC_REG_R8 8
+#define HL_HOST_UC_REG_R9 9
+#define HL_HOST_UC_REG_R10 10
+#define HL_HOST_UC_REG_R11 11
+#define HL_HOST_UC_REG_R12 12
+#define HL_HOST_UC_REG_R13 13
+#define HL_HOST_UC_REG_R14 14
+#define HL_HOST_UC_REG_R15 15
+#define HL_HOST_UC_REG_RIP 16
+/* EFlags is a DWORD sited BEFORE Rax; it cannot join that index space.
+ * There is deliberately no HL_HOST_UC_REG_EFL here -- a build that still
+ * wants one must fail to compile rather than index past R15. */
+#define HL_HOST_UC_EFLAGS(uc) ((uc)->EFlags)
+#define HL_HOST_UC_MXCSR(uc) ((uc)->MxCsr)
+
+/* xmm0..15, contiguous M128A inside the legacy FXSAVE image.  Unlike Linux's
+ * optional uc_mcontext.fpregs this is never NULL -- but it is only MEANINGFUL,
+ * and only restored on continue, when ContextFlags carries
+ * CONTEXT_FLOATING_POINT.  The VEH asserts that at entry. */
+#define HL_HOST_UC_XMM(uc) ((void *)(uc)->FltSave.XmmRegisters)
+
+_Static_assert(offsetof(CONTEXT, R15) - offsetof(CONTEXT, Rax) == 15 * 8,
+               "Win32 x64 CONTEXT GPRs are not a contiguous encoding-ordered block");
+_Static_assert(offsetof(CONTEXT, Rip) - offsetof(CONTEXT, Rax) == 16 * 8,
+               "Win32 x64 CONTEXT Rip does not follow R15");
+_Static_assert(offsetof(CONTEXT, Rsp) - offsetof(CONTEXT, Rax) == 4 * 8,
+               "Win32 x64 CONTEXT Rsp is not at encoding index 4");
+_Static_assert(sizeof(((CONTEXT *)0)->FltSave.XmmRegisters) == 16 * 16,
+               "Win32 x64 CONTEXT xmm area is not 16 x 128 bits");
+
+/* Windows / AArch64 -- provided so the matrix stays total.  No Windows-ARM64
+ * backend exists; these are the accessors it would use. */
+#elif defined(_WIN32) && defined(HL_HOST_CPU_AARCH64)
+#ifndef WIN32_LEAN_AND_MEAN
+#define WIN32_LEAN_AND_MEAN 1
+#endif
+#ifndef NOMINMAX
+#define NOMINMAX 1
+#endif
+#ifndef NOGDI
+#define NOGDI 1
+#endif
+#include <windows.h>
+
+typedef CONTEXT ucontext_t; /* see the x86-64 cell */
+
+#define HL_HOST_HAS_A64_CONTEXT 1
+#define HL_HOST_UC_PC(uc) ((uc)->Pc)
+#define HL_HOST_UC_SP(uc) ((uc)->Sp)
+/* X[0..30], with X[29]==Fp and X[30]==Lr -- the SAME indices Linux's
+ * uc_mcontext.regs[] uses, so no call site re-indexes. */
+#define HL_HOST_UC_REGS(uc) ((uint64_t *)(void *)(uc)->X)
+/* V[0..31] is a plain member.  The Linux cell's __reserved FPSIMD_MAGIC chain
+ * walk (hl_host_uc_vregs) has no counterpart here and the result is never
+ * NULL; the `if (V == NULL) return 0;` guards in the per-guest signal.c files
+ * become dead but stay correct. */
+#define HL_HOST_UC_VREGS(uc) ((__uint128_t *)(void *)(uc)->V)
+/* Cpsr is a DWORD.  NZCV occupy bits 31..28 exactly as in Linux's 64-bit
+ * pstate, so hl_aarch64_signal_capture's widening assignment is
+ * value-preserving for the flags.  Windows exposes none of pstate's other
+ * bits (SS/IL/D/A/I/F); no engine path reads them today. */
+#define HL_HOST_UC_PSTATE(uc) ((uc)->Cpsr)
+
+_Static_assert(sizeof(((CONTEXT *)0)->V[0]) == 16, "Win32 ARM64 V[] is not 128-bit");
+_Static_assert(sizeof(((CONTEXT *)0)->X) == 31 * 8, "Win32 ARM64 X[] is not 31 x 64-bit");
+
 #else
 #error "hl engine has no signal-context mapping for this host OS and CPU"
 #endif
```

Three things to check before this compiles, all of them cheap and none of them assumed here:

1. **Anonymous unions.** `(uc)->Rax`, `(uc)->FltSave`, `(uc)->X` and `(uc)->V` all live inside
   `DUMMYUNIONNAME`. mingw-w64 spells it `__C89_NAMELESS`, which expands to nothing under GCC/clang, so
   the members are genuinely anonymous and the macros work. If a toolchain names the union, every accessor
   gains a `.DUMMYUNIONNAME`.
2. **`windows.h` in a header included by ten TUs.** It defines `ERROR`, `IN`, `OUT`, `near`, `far`,
   `interface`, `min`/`max`. `WIN32_LEAN_AND_MEAN` + `NOMINMAX` + `NOGDI` covers the worst of it;
   a collision sweep over the engine's own identifiers is required, not optional.
3. **`_Static_assert` at file scope** in C11 — fine for clang, and the engine already builds `-std=gnu11`
   equivalents elsewhere; if a TU compiles as C99 the asserts need the `HL_STATIC_ASSERT` idiom instead.

### 2.3 What the matrix gains and loses

| | POSIX | Win32 |
|---|---|---|
| `HL_HOST_UC_PC` | `uc_mcontext.pc` / `gregs[REG_RIP]` | `Pc` / `Rip` — same |
| `HL_HOST_UC_SP` | `uc_mcontext.sp` / `gregs[REG_RSP]` | `Sp` / `Rsp` — same |
| AArch64 V regs | `__reserved` chain walk for `FPSIMD_MAGIC`, may return NULL | plain `V[32]`, never NULL — **strictly simpler** |
| x86-64 XMM | `uc_mcontext.fpregs` is optional, may be NULL | always present, gated on `ContextFlags` |
| x86-64 EFLAGS | `gregs[REG_EFL]`, in the GPR index space | separate `EFlags` member — **the one shape break** |
| signal mask | `uc_sigmask`, and `interp_restore_handler_mask` owes a restore | does not exist; **nothing is owed** (§4.3) |
| MXCSR | not exposed by the matrix | `MxCsr` is a first-class member |

---

## 3. Exception → guest signal mapping

One process-wide VEH, registered once at `engine_global_init` with
`AddVectoredExceptionHandler(1, hl_win_veh)` — `First != 0` puts it ahead of any handler a loaded DLL adds
later. VEH is the right primitive precisely because it is *not* frame-based: it fires on every thread
regardless of where in a call frame the fault happened, which is exactly the property
`sigaction(SIGSEGV, ...)` has and `__try/__except` does not.

Everything the engine does not own returns `EXCEPTION_CONTINUE_SEARCH` **immediately, before touching
anything**. That list must include at minimum `DBG_PRINTEXCEPTION_C` (0x40010006),
`MS_VC_EXCEPTION` (0x406D1388, thread naming), `0xE06D7363` (C++ throw) and any code with the top nibble
`0x4` (informational). Getting this filter wrong turns every debugger `OutputDebugString` into an engine
fault path.

| `ExceptionCode` | Linux signal / si_code | Notes |
|---|---|---|
| `EXCEPTION_ACCESS_VIOLATION` | 11 SIGSEGV, `SEGV_MAPERR`/`SEGV_ACCERR` | `Info[0]` 0=read 1=write 8=DEP-exec; `Info[1]`=address |
| `EXCEPTION_IN_PAGE_ERROR` | 7 SIGBUS, `BUS_ADRERR` | same `Info[0]`/`Info[1]`, plus `Info[2]` = the underlying `NTSTATUS` |
| `EXCEPTION_DATATYPE_MISALIGNMENT` | 7 SIGBUS, `BUS_ADRALN` | the `ldapr_align_fixup`/`lse_align_fixup` class |
| `EXCEPTION_ILLEGAL_INSTRUCTION` | 4 SIGILL, `ILL_ILLOPC` | |
| `EXCEPTION_PRIV_INSTRUCTION` | 4 SIGILL, `ILL_PRVOPC` | |
| `EXCEPTION_INT_DIVIDE_BY_ZERO` | 8 SIGFPE, `FPE_INTDIV` | the `#DE` `raise_guest_de` path |
| `EXCEPTION_INT_OVERFLOW` | 8 SIGFPE, `FPE_INTOVF` | |
| `EXCEPTION_FLT_DIVIDE_BY_ZERO` / `_OVERFLOW` / `_UNDERFLOW` / `_INEXACT_RESULT` / `_INVALID_OPERATION` / `_DENORMAL_OPERAND` | 8 SIGFPE, `FPE_FLT*` | only reachable with unmasked FP exceptions; the engine masks them |
| `EXCEPTION_BREAKPOINT` | 5 SIGTRAP, `TRAP_BRKPT` | guest `int3` / `brk` |
| `EXCEPTION_SINGLE_STEP` | 5 SIGTRAP, `TRAP_TRACE` | |
| `EXCEPTION_STACK_OVERFLOW` | — | the **engine's own** stack is exhausted. Never a guest fault; see §5 |
| anything else | — | `EXCEPTION_CONTINUE_SEARCH` |

### 3.1 Fault address and read-vs-write

`ExceptionInformation[1]` is the `si_addr` equivalent and is exactly as precise. `ExceptionInformation[0]`
is *better* than anything POSIX hands the engine today, and three call sites should consume it directly
rather than inferring:

- `gro_hit` (`x86.c:1160`) currently comments that any fault in the read-only registry "is a write fault".
  On Windows that becomes a check, not an assumption.
- `smc_on_write` (`x86.c:1172`) is only correct for `Info[0] == 1`.
- `raise_guest_fetch_fault`'s job — a fetch from a non-`PROT_EXEC` guest page — currently has to be
  synthesised at translation time because a POSIX handler cannot tell an instruction fetch from a data
  read. `Info[0] == 8` says so directly. That does not remove the translation-time check (a fetch fault
  must be delivered in ordinary guest execution context, per `signal.c:920-931`) but it does give the
  handler a second, independent oracle.

**`SEGV_MAPERR` vs `SEGV_ACCERR` is *not* available.** Windows reports both as `EXCEPTION_ACCESS_VIOLATION`
with the same `Info[0]`. `deliver_guest_fault_hint` already computes `sync_code` itself, from
`gna_hit(...) || host_addr_mapped(...)` (`signal.c:886-889`), so nothing is lost — but `host_addr_mapped`
becomes `VirtualQuery`, which is a real syscall inside the handler. That is permitted (§4.4) and is what
the macOS arm already does with `mach_vm_region`, but it is on the fault path and should be measured.

### 3.2 Past-EOF SIGBUS

On Linux the kernel raises the guest's past-EOF `SIGBUS` for free, and `deliver_guest_fault_hint` says so
explicitly ("On a Linux host the guest runs on real host file mappings, so the kernel already raises host
SIGBUS exactly … there the ledger is empty"). On macOS the same host signal is overloaded across
`PROT_NONE` guard accesses *and* real bus errors, which is why the `hl_linux_bus_hit` ledger exists.

Windows is a third case: a view of a section cannot extend past the section, and a section's size is
clamped to the file's, so **a Windows host will never raise a past-EOF fault at all**. The tail of the last
page reads as zero. Consequently:

- the BUS ledger (`mem.c`'s past-EOF tracking, `core/bus.h`, `interp_bus_ledger_check`,
  `raise_guest_bus`) becomes the **sole** source of guest `SIGBUS(BUS_ADRERR)` on Windows, and is
  load-bearing rather than a Darwin workaround;
- `EXCEPTION_IN_PAGE_ERROR` is left meaning only what it says — a genuine backing-store I/O failure — and
  should map to guest `SIGBUS` and *not* be routed through the ledger;
- the macOS-only `hostsig == SIGBUS → sig = 11` rewrite at `signal.c:849` stays macOS-only, and so does
  `lifecycle.c:159`'s `waited->value == SIGBUS ? 11` fixup.

This "Windows never faults past EOF" claim is **reasoned from the section-size rule, not measured**. It is
the single cheapest probe to write first, and it changes the design if it is wrong.

---

## 4. Resuming: `EXCEPTION_CONTINUE_EXECUTION` vs a mutated `ucontext`

### 4.1 They are the same operation

`sigframe_resume_dispatch` on POSIX does:

```c
memcpy(HL_HOST_UC_REGS(uc), &cpu_address, sizeof(cpu_address)); /* x0 = &cpu   */
HL_HOST_UC_PC(uc) = (uint64_t)dispatcher_return;                /* pc = block_return */
/* ... and RETURNS from the handler; the kernel's rt_sigreturn installs it. */
```

On Windows the handler mutates `ExceptionInfo->ContextRecord` identically and returns
`EXCEPTION_CONTINUE_EXECUTION` (0xFFFFFFFF); `RtlDispatchException` then continues with the modified
record. The documented contract is "To return control to the point at which the exception occurred, return
`EXCEPTION_CONTINUE_EXECUTION`" — the *point* is whatever `Rip`/`Pc` now says.

So `hl_x86_signal_resume`, `hl_aarch64_signal_resume` and `hl_x86_signal_fast_clock_fault` port with **no
change beyond the accessors** and one added `return EXCEPTION_CONTINUE_EXECUTION` at the VEH boundary.
Likewise `nonpie_fixup`'s "serve the access at +bias and advance the host PC" and `smc_on_write`'s
"unprotect and retry" (retry = return with `Rip` untouched).

Windows is in one respect *simpler*: there is no signal mask, so the entire "leaving a handler by long jump
means the kernel never runs sigreturn, so the mask restore is a debt owed by us" problem
(`interp.c:125-146`, and `docs/amd64-host.md` §6.1) does not exist. `interp_restore_handler_mask` becomes
an empty stub on Windows.

### 4.2 Two things that are *not* the same

**`ContextFlags` gates what is restored.** `NtContinue` honours the record according to its `ContextFlags`.
Mutating a GPR needs `CONTEXT_INTEGER`; `Rip`/`Rsp`/`EFlags` need `CONTEXT_CONTROL`; XMM/V need
`CONTEXT_FLOATING_POINT`. The exception-dispatch context is believed to carry `CONTEXT_ALL` on x64, but the
handler must **assert it, not assume it** — `hl_x86_signal_capture` reads XMM and `hl_aarch64_signal_capture`
reads V, and a silently absent `CONTEXT_FLOATING_POINT` would make a guest fault resume with garbage vector
state. Cheapest form: `if ((ctx->ContextFlags & CONTEXT_FLOATING_POINT) == 0) return EXCEPTION_CONTINUE_SEARCH;`
on the paths that need it, which fails loudly instead of quietly.

**There is no `EXCEPTION_NONCONTINUABLE` on the paths we care about, but there is on some.** Attempting to
continue a noncontinuable exception raises `EXCEPTION_NONCONTINUABLE_EXCEPTION`. Check
`ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE` before returning
`EXCEPTION_CONTINUE_EXECUTION`.

### 4.3 The interpreter's `siglongjmp` must not survive

This is the sharpest correctness point in the whole design.

The interpreter's fault model (`src/translator/guest/*/interp.c:60-172`) is: arm a marker around every guest
access, `sigsetjmp` at the top of `run_block`, and have the handler `siglongjmp` back so the in-flight
`memcpy` is abandoned rather than re-executed forever.

On x86-64 Windows, `longjmp` is **not** a bare register restore. The Win64 ABI implements `setjmp`/`longjmp`
over SEH: `longjmp` calls `RtlUnwindEx` to unwind the frames between the jump and the target. Jumping out of
a VEH means unwinding through `ntdll!RtlDispatchException` and `ntdll!KiUserExceptionDispatcher` while the
kernel still believes exception dispatch is in progress. That is not a supported operation and it is exactly
the sort of thing that works on one Windows build and not the next.

**Recommendation: replace the interpreter's long jump with a context edit — the JIT's own idiom.**

```
run_block (Windows arm):
  RtlCaptureContext(&tls_pad);          /* once per block entry, ~1.3 KB, no syscall */
  tls_pad_armed = 1;
  ... execute guest instructions ...

interp_signal_resume (Windows arm):
  *ExceptionInfo->ContextRecord = tls_pad;   /* Rip/Rsp/nonvolatiles all restored */
  ContextRecord->Rax = 1;                    /* the "arrived by fault" discriminator */
  return EXCEPTION_CONTINUE_EXECUTION;
```

This is strictly more correct than `longjmp` (no unwinder involvement at all) and it is precisely what
`hl_x86_signal_resume` already does for the JIT. The cost is `RtlCaptureContext` per block — which must be
measured, because §6 of `docs/amd64-host.md` records that the *previous* per-block cost on this exact path
(`sigsetjmp(pad, 1)`'s `rt_sigprocmask`) was 44% of compute CPU. `RtlCaptureContext` is a pure user-mode
register store with no syscall, so it should be tens of nanoseconds rather than 272, but "should be" is not
a measurement.

The fallback, if context-editing proves fragile, is a real `__try`/`__except` around `run_block`: clang for
mingw-w64 supports MSVC SEH on x64 (GCC does not), and `__except` runs after a genuine unwind. It cannot
replace the VEH — faults also occur outside `run_block`, e.g. `host_range_mapped`'s probe from `service()` —
so it would be an *additional* mechanism, not a substitute.

`hrm_fault_hook` (`thread.c:1634`) has the same problem and the same fix: it long-jumps back to
`host_range_mapped` to report "unmapped". Its probe window is tiny and its landing pad is a single function,
so the context-edit version is easy. Note its POSIX body also unblocks SIGSEGV/SIGBUS before jumping — on
Windows that line simply deletes.

### 4.4 Reentrancy and safety inside a VEH

`repair_signal_page`'s contract (`include/hl/host_services.h:216-222`) says: "Supported host
implementations use only direct VM operations: no userspace allocation, locks, logging, ownership
registries, or errno-dependent decisions."

**That contract survives on Windows, but for a different reason, and with one addition.**

The POSIX reason is that a signal handler can interrupt the *same thread* in the middle of `malloc`, so
anything non-async-signal-safe can deadlock or corrupt. On Windows that hazard does not exist: a VEH is
entered only synchronously, from a faulting instruction, on the faulting thread. There is no asynchronous
VEH. The Windows constraints are:

1. **Same-thread lock recursion.** If the faulting instruction was inside code holding a lock the handler
   also takes, a non-recursive `SRWLOCK` deadlocks and a recursive one corrupts. The engine's `g_sigq_lk`,
   `g_jit_lock`, `g_stw_reg_lock` and the host backend's handle-registry lock all qualify. **The rule
   survives verbatim: the VEH takes no engine lock.**
2. **The loader lock.** A fault raised while the loader lock is held (inside `DllMain`, or in a delay-load
   thunk) means the handler must not do anything that loads a DLL — including the *first* call to a
   delay-loaded import. Every Win32 API the VEH uses must be resolved at init, not lazily.
3. **CRT reentrancy.** `fprintf`/`malloc` take CRT locks; same conclusion as (1) — no logging, no
   allocation — for a different reason. The engine's existing signal-safe diagnostic writer
   (`sig_diag_write`, a raw `write(2)` loop) becomes a raw `WriteFile` loop on the engine's own stderr
   handle.
4. **Kernel VM calls are safe here.** `VirtualAlloc`/`VirtualProtect`/`VirtualQuery` take the process
   address-space lock *in the kernel*; the faulting thread was executing user-mode code and cannot already
   hold it. So the Windows `repair_signal_page` is legitimately
   `VirtualProtect(page, 4096, PAGE_READWRITE, &old)` falling back to
   `VirtualAlloc(page, 4096, MEM_COMMIT, PAGE_READWRITE)` — the exact shape of the Linux and macOS ones.
5. **`GetLastError` must be saved and restored.** It is a TEB field the faulting code may be about to read,
   and every Win32 call in the handler clobbers it. This has no POSIX counterpart in the current code —
   and, noted honestly, the POSIX handlers do not save/restore `errno` either, which is the same latent
   defect. Worth checking on the existing hosts.

Point (4) is the one that matters for wording: the header comment's "This is an engine contract, not a claim
that arbitrary POSIX VM APIs are generally async-signal-safe" should gain a sentence saying the Windows
implementation is safe by a *structural* argument (synchronous-only dispatch) rather than by an
async-signal-safety list.

---

## 5. Stack overflow: what replaces `sigaltstack`

### 5.1 Why the reservation exists at all

`install_host_sigaltstack` (`src/linux_abi/thread.c:1560`) exists for exactly one reason, stated in its own
comment: **on the aarch64 frontend the host SP *is* the guest SP while a translated block runs**, so a guest
stack overflow leaves no room for the kernel to push the SIGSEGV frame and the handler that would deliver
the guest's signal never runs.

On an x86-64 host that condition holds only for the transliterator. `src/core/target/x86_64.c:927-931` sets
`SA_ONSTACK` iff `translit_enabled()`, and `dispatch.c:254` calls the reservation "No-op … on x86 (host SP
differs)". The interpreter — the first and only Windows backend for the foreseeable future — runs on its own
host stack.

### 5.2 Windows has no sigaltstack, and cannot have one

The exception frame (`EXCEPTION_RECORD` + `CONTEXT`, ~1.3 KB on x64, more with `CONTEXT_XSTATE`) is pushed
by the kernel at the faulting thread's current SP, and `KiUserExceptionDispatcher` runs there. There is no
API to redirect it. The three consequences:

**For the engine's own thread stack: `SetThreadStackGuarantee` is the analogue.** It "sets the minimum size
of the stack … that will be available during any stack overflow exceptions … the application can safely use
the specified number of bytes during exception handling." It should be called per guest thread at the top of
`run_guest`, in the slot `install_host_sigaltstack` occupies today, with something like 64 KiB. On Windows
that call replaces a no-op reservation with a real one — the *only* place where the Windows arm does more
work than the x86-64 Linux arm rather than less.

**For the guest's stack: nothing is needed, and nothing works.** A guest stack is an ordinary
`VirtualAlloc` region, not the thread's stack: it has no `PAGE_GUARD` growth machinery and never raises
`EXCEPTION_STACK_OVERFLOW`. A store into the guest's 1 MiB `PROT_NONE` `LOGUARD` region
(`src/linux_abi/x86.c:520-540`) raises a plain `EXCEPTION_ACCESS_VIOLATION` that `gna_hit` already
classifies as a hard fault and delivers as guest `SIGSEGV(SEGV_MAPERR)`. On the interpreter, where host SP
is the engine's, that is complete and correct.

**The gap is the transliterator cell, and it is unfixable.** If host SP == guest SP, a guest stack overflow
pushes its exception frame at the exhausted guest SP, into the guard region, and the resulting second fault
during dispatch terminates the thread with no handler ever running. There is no Windows mechanism to avoid
this. **Any future Windows transliterator must switch to a host stack in its block prologue, or guest
stack-overflow delivery is lost outright.** That is a hard architectural constraint, and it should be
written into `src/core/target/x86_64.c`'s comment beside `SA_ONSTACK` before anyone starts.

### 5.3 `EXCEPTION_STACK_OVERFLOW` and guard-page recovery

When the *engine's* own stack overflows, the guard page has already been consumed — "guard pages raise the
guard page exception only on first access". MSVC's `_resetstkoflw` restores it; mingw-w64's msvcrt does not
export an equivalent, so recovery would mean reimplementing it
(`VirtualProtect(page_below_limit, ..., PAGE_READWRITE | PAGE_GUARD)`).

**Recommendation: do not recover.** An engine stack overflow is an engine bug; the honest behaviour is the
one the POSIX arm already has for an unclaimed fault — decline to `EXCEPTION_CONTINUE_SEARCH` and let the
process die with a diagnostic. `EXCEPTION_STACK_OVERFLOW` must be filtered out *before* the guest-fault
classifiers, or the lazy zero-page grower will happily map the next page and turn an engine bug into silent
memory corruption. That is the direct Windows analogue of the existing "route it before the lazy-map
classifier examines si_addr" rule at `x86.c:1114-1119`.

---

## 6. SIGCHLD and child reaping

### 6.1 What the interface actually promises

`src/host/child.c` is 69 lines and has exactly one consumer: the fork-server in `src/linux_abi/fork.c`, which
puts `hl_host_child_watch_descriptor()` into a `poll()` set (line 453), drains it on wake (469), and also
calls `hl_host_child_watch_notify()` from a non-signal context (311) to self-wake. So the contract is
narrow:

- an object that can join a multi-object wait,
- edge-triggered "some child changed state" *or* "someone poked me",
- level-insensitive: the consumer always re-sweeps after a wake.

That last property is what makes a Windows port tractable, because it means the notification may be lossy.

### 6.2 Design

Keep the five-function interface; change the substrate.

- **The waitable object becomes a manual-reset `Event`.** `hl_host_child_watch_notify` → `SetEvent`;
  `hl_host_child_watch_drain` → `ResetEvent`. Add `hl_host_child_watch_handle()` returning an
  `hl_host_handle`; `hl_host_child_watch_descriptor()` returns −1 on Windows. `fork.c`'s `poll()` loop is
  being rewritten for Windows anyway (there is no unified fd/handle wait), so this is not extra churn.

- **The notification source is a job object with an attached completion port.** Create one job at engine
  init, `AssignProcessToJobObject` every guest child, and
  `SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &JOBOBJECT_ASSOCIATE_COMPLETION_PORT{...})`.
  A dedicated pump thread blocks in `GetQueuedCompletionStatus` and `SetEvent`s the watch on
  `JOB_OBJECT_MSG_EXIT_PROCESS` / `JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS`.

  This is the closest thing Win32 has to SIGCHLD: one object, unbounded child count, carries the child's
  PID, and the job additionally gives `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` containment the engine has no
  equivalent of today.

  **Verified caveat that determines the design:** Microsoft documents that, except for
  `JobObjectNotificationLimitInformation` limits, job messages "are intended only as notifications and their
  delivery to the completion port is not guaranteed." So the IOCP edge is a *hint* that must trigger a
  sweep — `WaitForSingleObject(h, 0)` + `GetExitCodeProcess` over the known child handles — and can never
  be the authoritative reap. That is exactly how SIGCHLD is used here already, so the shapes match.

- **Fallback if the job route is blocked:** `RegisterWaitForSingleObject` per child handle with
  `WT_EXECUTEONLYONCE` (process handles are in the documented supported list). One threadpool registration
  per child, `UnregisterWait` mandatory for every one including `WT_EXECUTEONLYONCE`, and the callback must
  not block or call `UnregisterWait` on itself.

- **Rejected:** a reaper thread doing `WaitForMultipleObjects` over child handles. `MAXIMUM_WAIT_OBJECTS`
  is 64, so it needs a tree of threads past 64 children. Not worth it when a job object exists.

### 6.3 Zombies

On Windows there is no zombie process: a terminated process's PID is released, and what keeps the exit
status alive is the open `HANDLE`, not the process table entry. So the engine's guest-visible
`waitpid`/`waitid` semantics — including the `sigexit` shared table that reconstructs `WIFSIGNALED`
(`src/linux_abi/signal.c:280-340`) and the "a pid can never be reused while a stale entry survives (the
zombie holds the pid until reap)" invariant it relies on — must be maintained entirely by `linux_abi`,
keeping every child `HANDLE` open until the guest reaps. That invariant is currently incidental; on Windows
it becomes load-bearing, and the pid-reuse hazard the comment rules out becomes real if a handle is closed
early.

---

## 7. SIGINT / SIGTERM, and `terminate`

### 7.1 Inbound: `SetConsoleCtrlHandler`

Verified facts that shape the design: the system **creates a new thread in the process** to run the handler;
handlers run last-registered-first-called until one returns `TRUE`; `CTRL_C_EVENT`/`CTRL_BREAK_EVENT` have
**no timeout**, while `CTRL_CLOSE_EVENT` gets `SPI_GETHUNGAPPTIMEOUT` (5000 ms) before the process is killed;
and `AttachConsole`/`AllocConsole`/`FreeConsole` **reset the handler table**, so it must be re-registered
after any console change.

Mapping:

| Console event | Guest signal |
|---|---|
| `CTRL_C_EVENT` | 2 SIGINT |
| `CTRL_BREAK_EVENT` | 3 SIGQUIT (Ctrl-\\ is the nearest Linux analogue, and it is the only other *generatable* event) |
| `CTRL_CLOSE_EVENT` | 1 SIGHUP, then 15 SIGTERM on a deadline well inside 5 s |
| `CTRL_LOGOFF_EVENT` / `CTRL_SHUTDOWN_EVENT` | 15 SIGTERM (services only) |

This replaces `activation.c`'s `activation_signal_relay`. The console handler thread *is* the relay thread,
so the self-pipe collapses — but only halfway: the handler must return promptly (5 s on close) and must not
block on `activation_engine_lock`, so keep the queue-and-return shape and let the existing relay consumer
drain it. `activation_signal_relay_fork_child`'s `pthread_atfork` reset has no counterpart (there is no
`fork`), and the whole hazard it guards — a child inheriting the relay handler and looping — disappears with
it.

**What is lost inbound:** `activation_forwarded[]` is `{SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2}`.
Windows offers a console counterpart for two of the six, and **none for SIGTERM** — Win32 has no polite
"please exit" signal at all; `TerminateProcess` is `SIGKILL` and nothing else. `SIGUSR1`/`SIGUSR2` have no
source whatsoever. The engine's own checkpoint trigger already survives this (it is a shared-memory word
`g_ckpt_trigger` polled at the dispatcher safepoint, `checkpoint.c:867`, **not** a signal), but
`kill -USR1 <engine-pid>` from outside is simply gone.

### 7.2 Outbound: `terminate`

`hl_host_process_terminate` (`src/host/linux/host.c:3601`) takes three reason classes:

| Reason | Linux | Windows |
|---|---|---|
| `HL_HOST_PROCESS_TERMINATE_FORCE` | `SIGKILL` | `TerminateProcess(h, 128 + 9)` — exact analogue: unblockable, no cleanup, no handler |
| `HL_HOST_PROCESS_TERMINATE_INTERRUPT` | `SIGINT` | `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` — see below |
| `HL_HOST_PROCESS_TERMINATE_SIGNAL + n` | `kill(pid, n)` | **no Win32 mechanism exists** |

The `INTERRUPT` case is only partially expressible, and the constraint is documented and blunt:
`CTRL_C_EVENT` "cannot be limited to a specific process group. If `dwProcessGroupId` is nonzero, this
function will succeed, but the CTRL+C signal will not be received by processes within the specified process
group." So a *targeted* Ctrl-C is impossible — the only working form is a broadcast to group 0, which also
hits the engine itself and every sibling.

`CTRL_BREAK_EVENT` *can* be targeted, but only at a group created with `CREATE_NEW_PROCESS_GROUP`, and only
at processes sharing the caller's console. So the design is: spawn every guest child with
`CREATE_NEW_PROCESS_GROUP`, and deliver `INTERRUPT` as `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, childPid)`.

That overloads `CTRL_BREAK_EVENT`, which §7.1 already assigned to guest `SIGQUIT`. The child cannot tell a
user's Ctrl-Break from the engine's interrupt request. Two ways out, and the second is better:

1. Accept the conflation. `INTERRUPT` delivers guest `SIGQUIT` instead of `SIGINT`. Cheap, wrong, and
   guest-visible.
2. **Build the control channel.** A per-child named pipe or shared-memory word + event, polled at the
   dispatcher safepoint next to `G_CKPT_POLL`. It can carry *any* Linux signal number, which is what
   `HL_HOST_PROCESS_TERMINATE_SIGNAL + n` requires and which no console mechanism can express, and it is
   also the substrate `rt_sigaction`'s cross-process host handler (§1.1) and `hl_host_process_interrupt`
   need anyway. Use the console event only as a *kick* to make a blocked child notice the channel.

Option 2 is more work and is the only one that makes the host-services contract honest. It should be
decided before any of `child.c`, `activation.c` or `host/windows/process.c` is written, because all three
depend on it.

Also note: a child with no console (`DETACHED_PROCESS`, a service, a GUI-subsystem parent) receives nothing
from `GenerateConsoleCtrlEvent` at all. The control channel has no such dependency.

### 7.3 STW and thread interrupt — the two the brief did not name

These are category (c) and they are harder than SIGCHLD.

**`STW_SIG` (park peers at a safepoint).** Windows cannot run a function on another thread at an arbitrary
instruction. But `stw_park_handler`'s *purpose* is only to guarantee "this peer is not executing translated
code", and `SuspendThread` guarantees that directly and more strongly than a signal does. So STW becomes
suspend-inspect-resume, with the spin loop moving from the parked thread to the flusher:

```
for each peer: SuspendThread(h);
               GetThreadContext(h, &ctx);   /* forces the suspend to have taken effect */
               peer->cpu->irq = 1;
               publish dispatch_ack;
... flush ...
for each peer: ResumeThread(h);
```

`SuspendThread` is asynchronous; the documented idiom is that the subsequent `GetThreadContext` is what
makes the suspension observable. Two hazards, both real: suspending a thread that is inside kernel exception
dispatch can return a blended context, and **suspending a thread that holds the CRT heap lock and then
allocating deadlocks the suspender** — so the STW path must not allocate, log, or take any lock a peer could
hold while any peer is suspended. That is the same discipline §4.4 imposes on the VEH, applied to a
different code path.

**`THREAD_INT_SIG` (EINTR a peer blocked in a host syscall).** There is no general "make any blocking call
return `EINTR`" primitive on Windows. The pieces:

- `CancelSynchronousIo(hThread)` for a blocking file/pipe read or write;
- `QueueUserAPC` only fires at an *alertable* wait, so it is not a substitute;
- a socket blocked in `WSAPoll` needs a dedicated wake handle in its wait set.

The only design that works is structural: **every engine blocking wait on Windows is a
`WaitForMultipleObjects` that includes a per-thread wake event**, and thread interrupt becomes
`SetEvent(thread->wake)` plus `CancelSynchronousIo` for the I/O case. That is a `linux_abi` and
`src/host/windows/` obligation rather than a signal one, but skipping it reproduces the exact failure
`src/host/linux/system.c:261-269` already documents: an interactive shell parked in `read(2)` on its pty is
never bounced to its safepoint, the tree waits for it, and the embedder sees only a deadline.

---

## 8. What is genuinely not expressible

Ranked by how much guest behaviour it costs.

- **Job control.** `syscall/signal.c` case 135 mirrors guest `SIGTSTP`/`SIGTTIN`/`SIGTTOU` onto the **real
  host mask** so bash's `tcsetpgrp` dance works, and `docs/amd64-host.md` §6.1 records that this mirroring is
  precisely why a mask snapshot was unsafe. Windows has no signal mask, no `SIGSTOP`/`SIGCONT` of a real
  host process, and no process groups in the POSIX sense. A guest child stopped by job control cannot be
  represented by a stopped host process. **`WUNTRACED`/`WCONTINUED`, `CLD_STOPPED`/`CLD_CONTINUED`, and
  interactive shell job control are lost** unless the whole thing is emulated inside `linux_abi` with a
  guest-side stop state — which is possible, but is a project, not a port.

- **Externally delivered signals to the engine process.** `kill -USR1`, `kill -HUP`, `kill -TERM` from
  outside all vanish. Only Ctrl-C, Ctrl-Break and console-close reach a Windows process.

- **`HL_HOST_PROCESS_TERMINATE_SIGNAL + n` for arbitrary `n`.** No console or Win32 mechanism carries a
  signal number. Only the engine's own control channel (§7.2 option 2) can.

- **Targeted `SIGINT`.** Documented as impossible: `CTRL_C_EVENT` cannot be limited to a process group.

- **Guest stack-overflow delivery under a future transliterator** (§5.2). No sigaltstack analogue exists for
  a stack that is not the thread's own.

- **`SEGV_MAPERR` vs `SEGV_ACCERR` from the hardware.** Recoverable by asking `VirtualQuery`, so this is a
  cost rather than a loss, but it moves a syscall onto the fault path.

- **An engine-visible "this fault-class signal was sent externally, not raised by hardware" discriminator.**
  `jit86_lazyguard`'s `si_code <= 0` test and the `g_in_service` fallback exist to tell an external
  `kill(SIGSEGV)` at a thread blocked in `pause()` (LTP `pause01`) from a genuine fault. On Windows the VEH
  is *always* a genuine fault, which makes that branch simpler — but it also means the LTP case has to be
  served by the control channel or not at all.

- **`raise(SIGFPE)` for faithful `WIFSIGNALED` on a handler-less guest `#DE`** (`x86_64.c:666`). Already
  guarded `#if defined(__linux__)`, and the `sigexit` shared table is the portable path, so this is a
  no-op — noted only so nobody adds a Windows arm to it.

---

## 9. What was verified, and where

Everything below was read from Microsoft's documentation for this design; nothing was taken from memory.

- Vectored handlers run "in the order that they were added, after the debugger gets a first chance
  notification, but before the system begins unwinding the stack"; `First != 0` makes a handler first —
  [Vectored Exception Handling](https://learn.microsoft.com/en-us/windows/win32/debug/vectored-exception-handling),
  [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler).
- Handler return values, and the guidance that "the handler should not call functions that acquire
  synchronization objects or allocate memory" —
  [PVECTORED_EXCEPTION_HANDLER](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nc-winnt-pvectored_exception_handler).
- `ExceptionInformation[0]` = 0 read / 1 write / 8 DEP, `[1]` = address, and `[2]` = the underlying
  `NTSTATUS` for `EXCEPTION_IN_PAGE_ERROR` only —
  [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).
- x64 `CONTEXT` field order (`Rax`…`R15`, `Rip`; `EFlags` before them; `FltSave`/`Xmm0..15`) —
  [CONTEXT (x86 64-bit)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context).
- ARM64 `CONTEXT`: `Cpsr`, `X[31]` (with `Fp`/`Lr` as X29/X30), `Sp`, `Pc`, `V[32]`, `Fpcr`, `Fpsr` —
  [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context).
- `SetThreadStackGuarantee` "sets the minimum size of the stack … available during any stack overflow
  exceptions", and `_resetstkoflw` is the MSVC recovery step —
  [SetThreadStackGuarantee](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadstackguarantee),
  [_resetstkoflw](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/resetstkoflw).
- The stack guard page is consumed on first touch and re-armed by the kernel one page lower; nothing
  user-mode is involved until reserve is exhausted —
  [A closer look at the stack guard page](https://devblogs.microsoft.com/oldnewthing/20220203-00/?p=106215),
  [Creating Guard Pages](https://learn.microsoft.com/en-us/windows/win32/memory/creating-guard-pages).
- Console control handlers run on "a new thread in the process", are called last-registered-first, and the
  documented timeouts (`CTRL_C`/`CTRL_BREAK`: none; `CTRL_CLOSE`: `SPI_GETHUNGAPPTIMEOUT`, 5000 ms) —
  [HandlerRoutine](https://learn.microsoft.com/en-us/windows/console/handlerroutine),
  [SetConsoleCtrlHandler](https://learn.microsoft.com/en-us/windows/console/setconsolectrlhandler).
- `CTRL_C_EVENT` "cannot be limited to a specific process group"; `CTRL_BREAK_EVENT` can, for a group made
  with `CREATE_NEW_PROCESS_GROUP`, and only among processes sharing the caller's console —
  [GenerateConsoleCtrlEvent](https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent).
- `RegisterWaitForSingleObject` supports **Process** handles; `UnregisterWait` is mandatory even with
  `WT_EXECUTEONLYONCE` —
  [RegisterWaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-registerwaitforsingleobject).
- Job completion-port messages other than `JobObjectNotificationLimitInformation` limits are "intended only
  as notifications and their delivery to the completion port is not guaranteed" —
  [Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects),
  [JOBOBJECT_ASSOCIATE_COMPLETION_PORT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port).
- `SuspendThread` is asynchronous and `GetThreadContext` is what forces it to have taken effect —
  [The SuspendThread function suspends a thread, but it does so asynchronously](https://devblogs.microsoft.com/oldnewthing/20150205-00/?p=44743).
- clang has SEH (`__try`/`__except`) support for mingw-w64 x86-64 where GCC does not —
  [MinGW-w64 Exception Handling](https://sourceforge.net/p/mingw-w64/wiki2/Exception%20Handling/).

**Reasoned but NOT verified — probe these first:**

1. Windows never raises a past-EOF fault on a mapped view (§3.2), because a view cannot exceed the section
   and the section cannot exceed the file. This determines whether the BUS ledger is the sole source of
   guest `SIGBUS`.
2. The exception-dispatch `CONTEXT` always carries `CONTEXT_FLOATING_POINT` (§4.2). Assert it at runtime
   regardless.
3. `RtlCaptureContext`'s per-block cost (§4.3), against the 272 ns / 44% of compute that the *previous*
   per-block fault-pad cost on this exact path.
4. That mingw-w64's `__C89_NAMELESS` unions make `ctx->Rax` / `ctx->X` / `ctx->V` / `ctx->FltSave` valid
   under clang with the engine's `-std=` (§2.2).
5. Whether `MEM_WRITE_WATCH` + `GetWriteWatch` is usable on executable pages. If it is, it is a strictly
   better SMC primitive than the `VirtualProtect` toggle in `smc_on_write`, because it logs dirtied pages
   without a fault per page. Unmeasured, and not on the critical path.

---

## 10. Order to build in

1. `native_context.h`'s two cells plus the `HL_HOST_UC_EFLAGS` follow-up (§2). Purely additive; every
   existing arm is untouched.
2. The probe list in §9 — items 1, 2 and 4 gate design decisions, not just performance.
3. The VEH: registration, the exception filter, the `siginfo_t` shim in `src/host/native_compat.h`, and the
   `EXCEPTION_STACK_OVERFLOW` early-out (§3, §5.3).
4. `repair_signal_page` + the `gna`/`gro`/SMC/lazy-map classifiers, which is the whole of (a) and (b)
   (§3, §4.4).
5. The interpreter's resume path — context edit, not `longjmp` (§4.3). This is the item most likely to be
   got wrong quietly.
6. **Decide the control channel** (§7.2). Everything in (c) depends on it: `child.c`, `activation.c`,
   `hl_host_process_interrupt`, cross-process `rt_sigaction`, and `terminate`'s signal reasons.
7. `child.c` on a job object + IOCP (§6), `SetConsoleCtrlHandler` (§7.1), then STW on
   suspend/inspect/resume (§7.3).
