#ifndef HL_HOST_NATIVE_CONTEXT_H
#define HL_HOST_NATIVE_CONTEXT_H

#include <stdint.h>

/* Signal-context register extraction. This header is a matrix over TWO
 * independent axes -- host OS and host CPU -- because the shape of
 * ucontext_t is set by the OS while the register file it exposes is set by
 * the CPU. Keeping the axes separate is what lets a new host be one block
 * here rather than a rewrite: an x86-64 Windows host, for instance, reuses
 * the x86-64 register vocabulary below with its own context struct.
 *
 * Two tiers of accessor:
 *
 *   HOST-NEUTRAL -- HL_HOST_UC_PC / HL_HOST_UC_SP. Defined on every
 *   (OS, CPU) pair. Portable code (the linux_abi/signal.c diagnostics, the
 *   per-target fault attribution in core/target) may use only these.
 *
 *   AArch64-SHAPED -- HL_HOST_UC_REGS (x0..x30), HL_HOST_UC_VREGS (v0..v31),
 *   HL_HOST_UC_PSTATE. These name a register file that exists only on
 *   AArch64, and they are guarded by HL_HOST_HAS_A64_CONTEXT. They serve the
 *   AArch64-host JIT: the emitted code keeps guest state in real host
 *   registers, so a fault taken inside a translated block is recovered by
 *   reading them. Code using them belongs behind HL_HOST_HAS_A64_CONTEXT and
 *   must have a host-neutral counterpart for other host CPUs.
 *
 *   x86-64-SHAPED -- HL_HOST_UC_GREGS plus HL_HOST_UC_REG_* index names, and
 *   HL_HOST_UC_XMM. Guarded by HL_HOST_HAS_X64_CONTEXT, same rationale
 *   mirrored.
 */

/* The host-CPU half of the matrix comes from host_cpu.h rather than being
 * re-derived here, so the two can never disagree about what CPU this is. */
#include "host_cpu.h"

/* ---------------- macOS / AArch64 ---------------- */
#if defined(__APPLE__) && defined(HL_HOST_CPU_AARCH64)
#include <sys/ucontext.h>
#define HL_HOST_HAS_A64_CONTEXT 1
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define HL_HOST_UC_REGS(uc) ((uc)->uc_mcontext->__ss.__x)
#define HL_HOST_UC_VREGS(uc) ((uc)->uc_mcontext->__ns.__v)
#define HL_HOST_UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define HL_HOST_UC_PSTATE(uc) ((uc)->uc_mcontext->__ss.__cpsr)

/* ---------------- macOS / x86-64 -- NOT a supported host ----------------
 *
 * Intel macOS is not a supported host and no lane builds or tests it. This arm
 * exists only so the matrix is total: previously the `__APPLE__` case carried
 * no CPU test at all and defined the AArch64 accessors unconditionally, so an
 * Intel Mac silently compiled `__ss.__pc` against a register file that has no
 * such member. Failing here on purpose, with the right accessors for the two
 * host-neutral macros and nothing else, is more honest than miscompiling.
 * Do not read this block as a commitment to the platform. */
#elif defined(__APPLE__) && defined(HL_HOST_CPU_X86_64)
#include <sys/ucontext.h>
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext->__ss.__rip)
#define HL_HOST_UC_SP(uc) ((uc)->uc_mcontext->__ss.__rsp)

/* ---------------- Linux / AArch64 ---------------- */
#elif defined(__linux__) && defined(HL_HOST_CPU_AARCH64)
#include <ucontext.h>
#include <asm/sigcontext.h>
#define HL_HOST_HAS_A64_CONTEXT 1
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext.pc)
#define HL_HOST_UC_REGS(uc) ((uint64_t *)(void *)((uc)->uc_mcontext.regs))
#define HL_HOST_UC_SP(uc) ((uc)->uc_mcontext.sp)
#define HL_HOST_UC_PSTATE(uc) ((uc)->uc_mcontext.pstate)

static inline __uint128_t *hl_host_uc_vregs(ucontext_t *context) {
    struct _aarch64_ctx *record = (struct _aarch64_ctx *)(void *)context->uc_mcontext.__reserved;
    unsigned char *end = context->uc_mcontext.__reserved + sizeof(context->uc_mcontext.__reserved);
    while ((unsigned char *)record + sizeof(*record) <= end && record->size >= sizeof(*record) &&
           (unsigned char *)record + record->size <= end) {
        if (record->magic == FPSIMD_MAGIC) return (__uint128_t *)((struct fpsimd_context *)record)->vregs;
        if (record->magic == 0 || record->size == 0) break;
        record = (struct _aarch64_ctx *)((unsigned char *)record + record->size);
    }
    return NULL;
}

#define HL_HOST_UC_VREGS(uc) hl_host_uc_vregs(uc)

/* ---------------- Linux / x86-64 ---------------- */
#elif defined(__linux__) && defined(HL_HOST_CPU_X86_64)
#include <ucontext.h>
#define HL_HOST_HAS_X64_CONTEXT 1
/* glibc exposes the register file as gregset_t, an array of greg_t indexed by
 * the REG_* enumerators from <sys/ucontext.h>. Re-export the indices under
 * HL_HOST_UC_REG_* so callers do not depend on the REG_ spelling, which is not
 * the same on every libc (musl agrees, but Windows will not). */
#define HL_HOST_UC_GREGS(uc) ((uc)->uc_mcontext.gregs)
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
#define HL_HOST_UC_SP(uc) ((uc)->uc_mcontext.gregs[REG_RSP])
#define HL_HOST_UC_REG_RAX REG_RAX
#define HL_HOST_UC_REG_RCX REG_RCX
#define HL_HOST_UC_REG_RDX REG_RDX
#define HL_HOST_UC_REG_RBX REG_RBX
#define HL_HOST_UC_REG_RSP REG_RSP
#define HL_HOST_UC_REG_RBP REG_RBP
#define HL_HOST_UC_REG_RSI REG_RSI
#define HL_HOST_UC_REG_RDI REG_RDI
#define HL_HOST_UC_REG_R8 REG_R8
#define HL_HOST_UC_REG_R9 REG_R9
#define HL_HOST_UC_REG_R10 REG_R10
#define HL_HOST_UC_REG_R11 REG_R11
#define HL_HOST_UC_REG_R12 REG_R12
#define HL_HOST_UC_REG_R13 REG_R13
#define HL_HOST_UC_REG_R14 REG_R14
#define HL_HOST_UC_REG_R15 REG_R15
#define HL_HOST_UC_REG_RIP REG_RIP
#define HL_HOST_UC_REG_EFL REG_EFL

/* xmm0..15, or NULL when the kernel did not attach an FP state block. The
 * fpregs pointer is optional in exactly the same way AArch64's FPSIMD_MAGIC
 * record is, so callers must null-check both. */
static inline void *hl_host_uc_xmm(ucontext_t *context) {
    if (context == NULL || context->uc_mcontext.fpregs == NULL) return NULL;
    return (void *)context->uc_mcontext.fpregs->_xmm;
}

#define HL_HOST_UC_XMM(uc) hl_host_uc_xmm(uc)

#else
#error "hl engine has no signal-context mapping for this host OS and CPU"
#endif

#endif
