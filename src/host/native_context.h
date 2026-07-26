#ifndef HL_HOST_NATIVE_CONTEXT_H
#define HL_HOST_NATIVE_CONTEXT_H

#include <stdint.h>

/* Signal-context register extraction: a matrix over host OS (shape of
 * ucontext_t) x host CPU (register file). HL_HOST_UC_PC / HL_HOST_UC_SP exist
 * on every (OS, CPU) pair and are the only ones portable code may use;
 * CPU-shaped accessors sit behind HL_HOST_HAS_{A64,X64}_CONTEXT. */

#include "host_cpu.h"

/* macOS / AArch64 */
#if defined(__APPLE__) && defined(HL_HOST_CPU_AARCH64)
#include <sys/ucontext.h>
#define HL_HOST_HAS_A64_CONTEXT 1
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define HL_HOST_UC_REGS(uc) ((uc)->uc_mcontext->__ss.__x)
#define HL_HOST_UC_VREGS(uc) ((uc)->uc_mcontext->__ns.__v)
#define HL_HOST_UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define HL_HOST_UC_PSTATE(uc) ((uc)->uc_mcontext->__ss.__cpsr)

/* macOS / x86-64 -- NOT supported; host-neutral macros only, so the matrix is
 * total. Without the CPU test `__APPLE__` defined the AArch64 accessors
 * unconditionally on an Intel Mac. */
#elif defined(__APPLE__) && defined(HL_HOST_CPU_X86_64)
#include <sys/ucontext.h>
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext->__ss.__rip)
#define HL_HOST_UC_SP(uc) ((uc)->uc_mcontext->__ss.__rsp)

/* Linux / AArch64 */
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

/* Linux / x86-64 */
#elif defined(__linux__) && defined(HL_HOST_CPU_X86_64)
#include <ucontext.h>
#define HL_HOST_HAS_X64_CONTEXT 1
/* Re-export the <sys/ucontext.h> gregset_t indices: REG_* is not universal. */
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

/* xmm0..15, or NULL: fpregs is optional, so callers must null-check. */
static inline void *hl_host_uc_xmm(ucontext_t *context) {
    if (context == NULL || context->uc_mcontext.fpregs == NULL) return NULL;
    return (void *)context->uc_mcontext.fpregs->_xmm;
}

#define HL_HOST_UC_XMM(uc) hl_host_uc_xmm(uc)

#else
#error "hl engine has no signal-context mapping for this host OS and CPU"
#endif

#endif
