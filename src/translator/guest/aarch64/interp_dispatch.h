// translator/guest/aarch64/interp_dispatch.h -- the AArch64 guest's dispatch seam for the INTERPRETER
// backend, i.e. for every host CPU that is not AArch64.
//
// Read dispatch.h beside this file first: it is the same seam for the same-ISA transliterating JIT, and it
// is what runs on an AArch64 host. The shared run_guest() loop in core/dispatch.c calls the four mandatory
// hooks (G_DISPATCH_DEBUG, G_SHADOW_CLEAR, G_IBTC_FILL, G_DISPATCH_REASON) plus G_BLOCK_ALIGN, and its
// entire contract with a backend is:
//
//     code = translate_block(G_PC(c));   // produce something callable for this guest PC
//     run_block(c, code);                // ... call it
//     // on return: c->reason says why it stopped, G_PC(c) is the next guest PC, and every piece of
//     // guest architectural state is back in *c.
//
// Nothing in that contract requires `code` to be host machine code. The interpreter satisfies it by
// making `code` a decoded-block descriptor and run_block a C loop over it, which is why this backend can
// reuse the dispatcher, the block cache, the whole of linux_abi (syscalls, signals, container, ELF) and
// even the checkpoint format unchanged -- struct cpu is untouched, so `sizeof(struct cpu)` and the
// serialized architectural state stay byte-compatible with the JIT's.
//
// The hooks that exist to service host code generation therefore collapse to nothing here:
//
//   G_IBTC_FILL     The JIT's inline-branch-target cache patches AArch64 `b` instructions into the W^X
//                   code arena (dispatch.h:99-105). There is no emitted branch to patch; an indirect
//                   branch in the interpreter is a plain assignment to c->pc, so every indirect branch
//                   simply returns to the dispatcher and takes the normal map_host() lookup. Slower, and
//                   exactly as correct.
//   G_BLOCK_ALIGN   Padding a block entry to 16 bytes tunes the host instruction fetcher. Meaningless.
//   G_SHADOW_CLEAR  The §B shadow stack caches HOST return addresses inside the arena, so a wholesale
//                   arena flush invalidates it. The interpreter never records a host return address.
//                   Kept as the same c->ssp = 0 store anyway: the field is part of the checkpoint image,
//                   and leaving a stale non-zero ssp in a checkpoint written by this backend and restored
//                   by the JIT would hand the JIT a shadow stack full of addresses from another process.
//
// G_DISPATCH_REASON is the one hook with real work, and it is deliberately a near-copy of dispatch.h's:
// the reason codes are the same because they describe GUEST events (a syscall, a fetch fault, a guest
// icache flush), not host ones. Where the two differ, the difference is called out inline below.

// SMC (self-modifying guest code). Same model as the JIT: the guest is architecturally required to issue
// the icache-flush dance before executing freshly written bytes, and the frontend intercepts `ic ivau` to
// drop stale translations. The interpreter still needs this: its decoded blocks are a cache of guest
// instruction bytes, so bytes rewritten behind its back must invalidate the block exactly as they
// invalidate emitted host code. The reason codes must match dispatch.h's, because linux_abi and the
// checkpoint format see them.
#define R_ICFLUSH 4
#define R_ICCOMMIT 6
static int g_smc_seen;

static inline int smc_seen(void) {
    return __atomic_load_n(&g_smc_seen, __ATOMIC_ACQUIRE);
}

static uint64_t g_smc_flushes;

// Top-of-loop instrumentation is an x86-frontend feature; the AArch64 JIT has none and neither does this.
#define G_DISPATCH_DEBUG(c) ((void)0)

// See the note above: this store is about the checkpoint image, not about a host arena.
#define G_SHADOW_CLEAR(c) ((c)->ssp = 0)

// No emitted code, so no entry alignment to tune.
#define G_BLOCK_ALIGN 0

// No inline branch-target cache. c->ic_site is never set by the interpreter, so the JIT's fill would be
// dead code anyway; spell that out rather than leaving the reader to infer it.
#define G_IBTC_FILL(c) ((void)0)

// Post-run_block reason handling. Structurally identical to dispatch.h's, including the `pc += 4` past the
// SVC on the non-redirect syscall path -- that advance is an AArch64 GUEST ABI property (the guest's PC
// still points at the SVC when the kernel is entered), so it is the same on every host.
//
// The soft-TLB reasons are the exception. The JIT emits an inline soft-TLB probe and exits with R_SOFTMISS
// when it misses, because re-entering C from emitted code is expensive enough to be worth the inline
// fast path. The interpreter is already in C: it resolves a logical-VMA access inline in its load/store
// path and never needs to leave the block to ask. It can therefore only produce these reasons via a
// checkpoint restored from a JIT-written image, which the restore path rejects on host-ISA identity --
// but handle them anyway rather than falling through to the `else R_BRANCH` case and silently resuming at
// a bogus PC.
#define G_DISPATCH_REASON(c)                                                                                           \
    if ((c)->reason == R_SOFTMISS || (c)->reason == R_SOFTCOMMIT || (c)->reason == R_SOFTSPAN ||                       \
        (c)->reason == R_FETCHFAULT) {                                                                                 \
        if ((c)->reason != R_FETCHFAULT) (c)->fault_addr = (c)->soft_ea;                                               \
        if (raise_guest_fetch_fault(c)) {                                                                              \
            maybe_deliver_signal(c);                                                                                   \
            continue;                                                                                                  \
        }                                                                                                              \
        break;                                                                                                         \
    } else if ((c)->reason == R_BUS) {                                                                                 \
        if (raise_guest_bus(c)) {                                                                                      \
            maybe_deliver_signal(c);                                                                                   \
            continue;                                                                                                  \
        }                                                                                                              \
        break;                                                                                                         \
    } else if ((c)->reason == R_ICFLUSH) {                                                                             \
        uint64_t _line = (c)->smc_va & ~UINT64_C(0xfff);                                                               \
        filemap_refresh_emulated(_line, _line + UINT64_C(0x1000));                                                     \
        smc_icflush((c), (c)->smc_va);                                                                                 \
    } else if ((c)->reason == R_ICCOMMIT) {                                                                            \
        if ((c)->smc_range_overflow)                                                                                   \
            filemap_refresh_emulated(0, UINT64_MAX);                                                                   \
        else                                                                                                           \
            for (uint32_t _index = 0; _index < (c)->smc_range_count; ++_index)                                         \
                filemap_refresh_emulated((c)->smc_ranges[_index][0], (c)->smc_ranges[_index][1]);                      \
        if (smc_commit(c)) g_smc_flushes++;                                                                            \
    } else if ((c)->reason == R_SYSCALL) {                                                                             \
        if (g_prof) g_prof_sys++;                                                                                      \
        service(c);                                                                                                    \
        if ((c)->exited) break;                                                                                        \
        if ((c)->redirect)                                                                                             \
            (c)->redirect = 0;                                                                                         \
        else                                                                                                           \
            (c)->pc += 4; /* execve/sigreturn set pc directly */                                                       \
    }                                                                                                                  \
    /* else R_BRANCH: c->pc already holds the target */

// The interpreter supplies its own run_block/block_return (interp.c), so core/dispatch.c must not emit the
// shared AArch64 assembly pair. This is the same escape hatch guest/x86_64/dispatch.h uses.
#define G_OWN_TRAMPOLINES 1
