#include "cpuid.h"
#include "cmpxchg.h"
#include "rotate.h"
#include "rep.h"
#include "x87math.h"
#include "x87state.h"
#include "operand.h"
#include "flags.h"

// translator/guest/x86_64/interp_dispatch.h -- the x86-64 guest's dispatch seam for the INTERPRETER
// backend, i.e. for every host CPU that is not AArch64.
//
// Read dispatch.h beside this file first: it is the same seam for the x86-64 -> ARM64 JIT, and it is what
// runs on an AArch64 host. The shared run_guest() loop in core/dispatch.c calls the hooks below at every
// point where the dispatcher historically diverged per guest arch, and its ENTIRE contract with a backend
// is:
//
//     code = translate_block(G_PC(c));   // produce something callable for this guest PC
//     run_block(c, code);                // ... call it
//     // on return: c->reason says why it stopped, c->rip is the next guest PC, and every piece of guest
//     // architectural state is back in *c.
//
// Nothing in that contract requires `code` to be host machine code. interp.c satisfies it by making `code`
// a decoded-block DESCRIPTOR and run_block a C decode-and-execute loop over the guest bytes, which is why
// this backend reuses the dispatcher, the block cache, the whole of linux_abi (syscalls, signals,
// container, ELF) and even the checkpoint format unchanged: struct cpu is untouched, so
// `sizeof(struct cpu)` and the serialized architectural state stay byte-compatible with the JIT's, and
// EFLAGS keeps living on the ARM-NZCV substrate that hl_x86_signal_nzcv_to_eflags / ..._eflags_to_nzcv
// (signal.c) convert at every guest-visible boundary -- ptrace(GETREGS), the rt_sigframe builder, pushfq.
//
// The hooks that exist purely to service HOST CODE GENERATION collapse to nothing here, and each one is
// spelled out rather than left to the reader to infer:
//
//   G_IBTC_FILL       The JIT's inline branch-target cache patches an absolute host branch target into the
//                     W^X arena (dispatch.h's 1-way g_ibtc / 2-way g_xibtc fill, keyed on cpu->ic_miss).
//                     There is no emitted indirect branch to accelerate: an indirect branch in the
//                     interpreter is a plain assignment to cpu->rip followed by a return to the
//                     dispatcher, which takes the ordinary map_host() lookup. Slower, and exactly as
//                     correct. cpu->ic_miss is never set here, so the JIT's fill would be dead code.
//   G_DISPATCH_ENTER  The JIT publishes &g_xibtc into cpu->ibtc_base so its emitted indirect hot path can
//                     load the table base in one instruction. Nothing loads it here; the field is
//                     nevertheless ZEROED rather than ignored, because it is part of the checkpoint image
//                     and a stale host pointer written by this backend and restored by the JIT would hand
//                     the JIT a table base from another process. (G_CKPT_CPU_SANITIZE zeroes it at capture
//                     for the same reason; this makes the live value agree with the captured one.)
//   G_BLOCK_ALIGN     Padding a block entry to 16 bytes tunes the host instruction fetcher for emitted
//                     code. A descriptor is data and is never fetched as instructions, so this is
//                     meaningless -- and it must be a compile-time 0 so the shared loop's alignment `while`
//                     (which calls emit32) is dead-stripped.
//   G_DISPATCH_CHAIN  The JIT chains inside translate_block (patch_links_to on a still-pending edge);
//                     aarch64 chains here. There are no emitted edges to backpatch, so neither applies.
//   G_AFTER_TRANSLATE The JIT write-protects a translated block's source page so a self-modifying (RWX
//                     mmap) guest's later overwrite traps and drops the now-stale HOST CODE. This backend
//                     caches no guest instruction bytes at all -- run_block re-decodes from guest memory
//                     on every execution (see interp.c) -- so freshly written bytes are picked up by
//                     construction and there is nothing to protect or invalidate. See smc_on_write below.
//   G_SHADOW_CLEAR    x86 has no shadow stack; the JIT drops the 2-way IBTC here because its bodies point
//                     into the arena that was just flushed. The interpreter never fills it, but the store
//                     is kept for the same reason G_DISPATCH_ENTER zeroes ibtc_base: g_xibtc holds HOST
//                     code pointers, and abi.h's G_ACTIVATION_CLEAR_GLOBAL already clears it on an arena
//                     rotation, so keeping the two in step costs one memset per wholesale flush.
//
// G_DISPATCH_REASON is the one hook with real work, and it is deliberately a near-copy of dispatch.h's:
// the reason codes describe GUEST events (a syscall, a CPUID, an integer #DE, an AVX instruction), not host
// ones, so they are the same on both backends and linux_abi + the checkpoint format keep seeing the same
// values. Where the two differ, the difference is called out inline below.

// debug: track block transitions for fault diagnosis (read by linux_abi/x86.c's fault guards). These name
// GUEST PCs, so they mean the same thing under an interpreter as under the JIT.
static uint64_t g_prevpc, g_curpc;

// ---- SMC (self-modifying guest code) -----------------------------------------------------------------
// g_rwx_guest is set (linux_abi/syscall/{dispatch,mem}.c) once the guest takes a PROT_EXEC mapping, i.e.
// once it might overwrite code it already ran. The JIT reacts by mprotect()ing each translated source page
// read-only and dropping stale HOST CODE when the guest's write traps (dispatch.h smc_protect /
// smc_on_write). An interpreter has no stale host code to drop: its block descriptors carry no guest
// instruction bytes, so every execution re-decodes whatever is in guest memory at that moment. Writing to
// executable guest memory therefore needs no interception at all, which is why there is no smc_protect
// here and why G_AFTER_TRANSLATE is empty.
//
// smc_on_write must still EXIST, because linux_abi/x86.c's jit86_lazyguard calls it while classifying a
// SIGSEGV. It answers 0 unconditionally -- "this fault is not a code-page write-protect of mine" -- which
// is the truthful answer when no page was ever protected, and which lets the guard continue down its chain
// (the PROT_NONE / read-only-mapping classifiers, the lazy grower, deliver_guest_fault) instead of
// swallowing a genuine guest fault.
extern int g_rwx_guest;

static int smc_on_write(uint64_t address) {
    (void)address;
    return 0;
}

// The shared loop's default G_DISPATCH_CHAIN consults this, and translator/cache.c's arena-rollover policy
// consults g_smc_seen (behind `#if G_GPC_HASH_SHIFT != 0`, so not on x86). Both questions are about
// retaining EMITTED code across an icache-flush observation; with nothing emitted, the answer is a
// constant. `static inline` so an unused definition costs neither code nor a warning.
static inline int smc_seen(void) {
    return 0;
}

// x86 keeps its own run_block/block_return (interp.c: a C decode-and-execute loop, plus the symbol the
// fault path unwinds through). Defining this tells core/dispatch.c NOT to emit the shared AArch64 assembly
// trampoline pair -- the same escape hatch guest/x86_64/dispatch.h uses, for a different reason: there the
// register model diverges, here there is no host register model at all.
#define G_OWN_TRAMPOLINES 1

// One-time per-thread entry setup. See the G_DISPATCH_ENTER note in the header comment: the JIT publishes
// the 2-way IBTC base, this backend publishes the absence of one.
#define G_DISPATCH_ENTER(c) ((c)->ibtc_base = 0)

// Top-of-loop instrumentation. The async-signal poll and the block-transition/trace bookkeeping are kept
// from dispatch.h: every line reads GUEST state (cpu->rip, cpu->r[], guest heap words) or engine counters,
// none of it reads emitted code, so all of it is host-neutral and both backends report identically. A PLAIN
// brace block (NOT do/while(0)) so the trace-cap `break` reaches the shared dispatcher while-loop, exactly
// as in dispatch.h.
#define G_DISPATCH_DEBUG(c)                                                                                            \
    {                                                                                                                  \
        if (g_pending) maybe_deliver_signal(c); /* async signal pending -> redirect to guest handler */                 \
        g_prevpc = g_curpc;                                                                                             \
        g_curpc = (c)->rip;                                                                                             \
        g_disp_n++;                                                                                                     \
        if (g_trace && g_tracecap && g_disp_n > g_tracecap) { /* bound trace output for runaway guests */               \
            fprintf(stderr, "[hl] trace cap %llu blocks reached -> stop\n", (unsigned long long)g_tracecap);            \
            (c)->exited = 1;                                                                                            \
            (c)->exit_code = 99;                                                                                        \
            break;                                                                                                      \
        }                                                                                                               \
        if (g_w8 && *g_w8 != g_w8v) { /* byte-watchpoint: report the block that just changed it */                     \
            fprintf(stderr, "[w8] @%p %02x -> %02x  by block +%llx  malloc#=%llu  rsi=%llx\n", (void *)g_w8, g_w8v,     \
                    *g_w8, (unsigned long long)(g_prevpc - g_loadbase), (unsigned long long)g_malloc_n,                 \
                    (unsigned long long)(c)->r[6]);                                                                     \
            g_w8v = *g_w8;                                                                                              \
        }                                                                                                               \
    }

// On-flush engine reset. See the G_SHADOW_CLEAR note in the header comment.
#define G_SHADOW_CLEAR(c) memset(g_xibtc, 0, sizeof g_xibtc)

// No emitted code, so no entry alignment to tune -- and a compile-time 0 so the shared loop's emit32()
// alignment pad is dead-stripped rather than appending ARM64 nops to a descriptor arena.
#define G_BLOCK_ALIGN 0

// No emitted inter-block edges to backpatch.
#define G_DISPATCH_CHAIN(c) ((void)0)

// No source page to write-protect: the interpreter re-decodes guest bytes on every execution.
#define G_AFTER_TRANSLATE(c) ((void)0)

// Per-block trace dump. The shared default in core/dispatch.c reads cpu->x[]/cpu->sp, which exist only on
// the AArch64 guest, so an x86 guest MUST override it on either backend. Identical to dispatch.h's: the
// x86 register file plus the four EFLAGS bits decoded out of the NZCV substrate (stored C is NOT x86 CF).
#define G_TRACE_DUMP(c)                                                                                                \
    if (g_trace) {                                                                                                     \
        unsigned nz = (unsigned)(c)->nzcv;                                                                             \
        int CF = !((nz >> 29) & 1), ZF = (nz >> 30) & 1, SF = (nz >> 31) & 1, OF = (nz >> 28) & 1;                     \
        fprintf(stderr,                                                                                                \
                "[blk] rip=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx r8=%llx r9=%llx "       \
                "r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx fl=C%dZ%dS%dO%d\n",                             \
                (unsigned long long)(c)->rip, (unsigned long long)(c)->r[RAX], (unsigned long long)(c)->r[3],          \
                (unsigned long long)(c)->r[RCX], (unsigned long long)(c)->r[RDX], (unsigned long long)(c)->r[RSI],     \
                (unsigned long long)(c)->r[RDI], (unsigned long long)(c)->r[RBP], (unsigned long long)(c)->r[8],       \
                (unsigned long long)(c)->r[9], (unsigned long long)(c)->r[10], (unsigned long long)(c)->r[11],         \
                (unsigned long long)(c)->r[12], (unsigned long long)(c)->r[13], (unsigned long long)(c)->r[14],        \
                (unsigned long long)(c)->r[15], CF, ZF, SF, OF);                                                       \
    }

// No inline branch-target cache to fill. cpu->ic_miss is never set by the interpreter (see the header
// comment), so the JIT's fill body would be unreachable; say so rather than leaving it to be inferred.
#define G_IBTC_FILL(c) ((void)0)

// Post-run_block reason handling.
//
// Structurally dispatch.h's, because the reason codes name guest events. In particular the SYSCALL tail is
// identical and deliberately does NO pc advance: this frontend's convention is that rip is ALREADY past
// the `0F 05` when the block exits (the JIT pre-advances it at emit time; interp.c advances it before
// setting R_SYSCALL, and says so at that site). aarch64, whose guest ABI leaves the PC on the SVC, does
// `pc += 4` here instead -- that per-arch convention is exactly what this hook exists to hold.
//
// Four arms differ from dispatch.h's. Three of them name a reason an interpreter CANNOT produce, handled
// anyway so it can never fall through to the `R_BRANCH` default and resume at a PC nobody set:
//
//   R_TIER2      The JIT's tier-2 promoter is driven by a counter EMITTED into a hot self-loop's in-cache
//                back-edge; there is no such counter here, so this reason is unreachable in a live run and
//                could only arrive from a checkpoint written by the JIT (which the restore path already
//                rejects on host-ISA identity). dispatch.h calls tier2_promote and `continue`s, which for
//                an interpreter -- whose tier2_promote cannot change anything -- would re-run the same
//                block and produce the same reason forever. Normalize it to R_BRANCH instead: rip is the
//                loop start, so resuming there is precisely right.
//   R_SOFTSPAN   Emitted soft-memory guards report an access that straddles two logical mappings. The
//                interpreter emits no guards; soft_tlb_miss() can still RAISE this while servicing an
//                R_SOFTMISS queued by the shared rep movs/stos helpers, so normalize it to a plain retry
//                of the (idempotent, restartable) string instruction rather than leaving it unhandled.
//   R_SMC        Emitted stores append executable-alias ranges and exit here. The interpreter's store path
//                appends the same ranges (jit86_store_alias_changed) but never needs to leave the block
//                to do it, so the arm survives only for a restored image -- and committing is harmless
//                and correct in either case.
//
// The fourth is reason 99 (unimplemented opcode): dispatch.h dumps candidate heap-pointer registers under
// g_trace because its report_unimpl overwrote rip with a 0xDEAD0000 marker and had nothing else to say.
// interp_undefined() keeps rip EXACT and has already printed the opcode bytes, the decoded prefixes and the
// instruction class it could not run, so that dump would add nothing here.
#define G_DISPATCH_REASON(c)                                                                                           \
    if ((c)->reason == R_SOFTMISS) {                                                                                   \
        if (soft_tlb_miss(c)) maybe_deliver_signal(c);                                                                 \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_SOFTSPAN) { /* see the header note: retry the restartable string op */                         \
        (c)->reason = R_BRANCH;                                                                                        \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == 99) {                                                                                           \
        fprintf(stderr, "[hl] aborting at rip marker %llx (unimplemented opcode)\n", (unsigned long long)(c)->rip);     \
        (c)->exited = 1;                                                                                               \
        (c)->exit_code = 70;                                                                                           \
        break;                                                                                                         \
    }                                                                                                                  \
    if ((c)->reason == R_TIER2) { /* not producible by an interpreter -- see the header note */                         \
        (c)->reason = R_BRANCH;                                                                                        \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_CPUID) {                                                                                      \
        hl_x86_cpuid(c);                                                                                               \
        continue;                                                                                                      \
    } /* rip already = next */                                                                                         \
    if ((c)->reason == R_AVX) { /* VEX/EVEX insn: emulate in C; rip = the insn, hl_x86_avx_run advances it */           \
        hl_x86_avx_run(&g_avx_state, (c));                                                                             \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_SSE3B) { /* legacy 0F38/0F3A insn: emulate in C; rip = the insn, callee advances it */         \
        hl_x86_sse_run(&g_avx_state, (c));                                                                             \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_REPSTR) { /* rep cmps/scas idiom (rip already = next) */                                       \
        hl_x86_rep_compare(c, g_nonpie_lo, g_nonpie_hi, g_nonpie_bias);                                                \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_X87FLD) { /* fld m80 (rip already = next) */                                                   \
        hl_x86_x87_load_ext80(c);                                                                                      \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_X87FSTP) { /* fstp m80 */                                                                      \
        hl_x86_x87_store_ext80_pop(c);                                                                                 \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_X87FUNC) { /* x87 transcendental (rip already = next) */                                       \
        hl_x86_x87_math(c);                                                                                            \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_RCL) { /* RCL/RCR by CL: rotate-through-carry in C (rip already = next) */                     \
        hl_x86_rotate_carry(c);                                                                                        \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_CMPXCHG16) { /* atomic 128-bit compare-exchange in C (rip already = next) */                   \
        hl_x86_cmpxchg16(c);                                                                                           \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_FXSAVE) { /* fxsave x87-register-DATA + FSW tail (rip already = next) */                       \
        hl_x86_fxsave(c);                                                                                              \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_FXRSTOR) { /* fxrstor x87-register-DATA + FSW tail (rip already = next) */                      \
        hl_x86_fxrstor(c);                                                                                             \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_DIV) { /* 128/64 unsigned div (rip already = next; divop==0 means #DE) */                      \
        uint64_t d = (c)->divop;                                                                                       \
        if (d == 0) {                                                                                                  \
            if (raise_guest_de(c, 1 /*FPE_INTDIV*/)) {                                                                 \
                maybe_deliver_signal(c);                                                                               \
                continue;                                                                                              \
            }                                                                                                          \
            break; /* raise_guest_de recorded death-by-SIGFPE / set exited+exit_code */                                \
        }                                                                                                              \
        if ((c)->r[RDX] >= d) { /* quotient overflow (high half >= divisor): x86 #DE, same as /0 */                     \
            if (raise_guest_de(c, 2 /*FPE_INTOVF*/)) {                                                                 \
                maybe_deliver_signal(c);                                                                               \
                continue;                                                                                              \
            }                                                                                                          \
            break;                                                                                                     \
        }                                                                                                              \
        unsigned __int128 num = ((unsigned __int128)(c)->r[RDX] << 64) | (c)->r[RAX];                                   \
        (c)->r[RAX] = (uint64_t)(num / d);                                                                              \
        (c)->r[RDX] = (uint64_t)(num % d);                                                                              \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_IDIV) { /* 128/64 signed idiv */                                                               \
        int64_t d = (int64_t)(c)->divop;                                                                                \
        if (d == 0) {                                                                                                  \
            if (raise_guest_de(c, 1 /*FPE_INTDIV*/)) {                                                                 \
                maybe_deliver_signal(c);                                                                               \
                continue;                                                                                              \
            }                                                                                                          \
            break;                                                                                                     \
        }                                                                                                              \
        __int128 num = ((__int128)(int64_t)(c)->r[RDX] << 64) | (c)->r[RAX];                                            \
        __int128 q = num / d;                                                                                           \
        if ((__int128)(int64_t)q != q) { /* quotient doesn't fit int64 (incl. INT_MIN/-1): x86 #DE */                   \
            if (raise_guest_de(c, 2 /*FPE_INTOVF*/)) {                                                                 \
                maybe_deliver_signal(c);                                                                               \
                continue;                                                                                              \
            }                                                                                                          \
            break;                                                                                                     \
        }                                                                                                              \
        (c)->r[RAX] = (uint64_t)q;                                                                                      \
        (c)->r[RDX] = (uint64_t)(num % d);                                                                              \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_TRAP) { /* int3 -> SIGTRAP, UD2 -> SIGILL (cpu->divop = signo|code<<8) */                      \
        if (raise_guest_trap(c)) {                                                                                     \
            maybe_deliver_signal(c);                                                                                   \
            continue;                                                                                                  \
        }                                                                                                              \
        (c)->exited = 1; /* no guest handler: default action terminates with the signal */                              \
        (c)->exit_code = 128 + ((int)((c)->divop & 0xff));                                                              \
        break;                                                                                                         \
    }                                                                                                                  \
    if ((c)->reason == R_BUS) {                                                                                        \
        if (raise_guest_bus(c)) {                                                                                      \
            maybe_deliver_signal(c);                                                                                   \
            continue;                                                                                                  \
        }                                                                                                              \
        break;                                                                                                         \
    }                                                                                                                  \
    if ((c)->reason == R_SMC) {                                                                                        \
        jit86_smc_commit(c);                                                                                           \
        (c)->reason = R_BRANCH;                                                                                        \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_SYSCALL) {                                                                                    \
        /* Publish emulated MAP_SHARED stores before a write/socket/futex syscall can notify a peer. */                 \
        if ((c)->smc_range_count || (c)->smc_range_overflow) jit86_smc_commit(c);                                       \
        service(c);                                                                                                    \
        if ((c)->exited) break;                                                                                        \
        /* And after: the syscall's own copyout (G_SMC_COPYOUT) may have written an executable alias. */                \
        if ((c)->smc_range_count || (c)->smc_range_overflow) jit86_smc_commit(c);                                       \
        if ((c)->redirect) (c)->redirect = 0; /* else rip already = next (advanced before the exit) */                  \
    }                                                                                                                  \
    /* R_BRANCH: c->rip already holds the target */
