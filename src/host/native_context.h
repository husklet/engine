#ifndef HL_HOST_NATIVE_CONTEXT_H
#define HL_HOST_NATIVE_CONTEXT_H

#include <stdint.h>

#if defined(__APPLE__)
#include <sys/ucontext.h>
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define HL_HOST_UC_REGS(uc) ((uc)->uc_mcontext->__ss.__x)
#define HL_HOST_UC_VREGS(uc) ((uc)->uc_mcontext->__ns.__v)
#elif defined(__linux__) && defined(__aarch64__)
#include <ucontext.h>
#include <asm/sigcontext.h>
#define HL_HOST_UC_PC(uc) ((uc)->uc_mcontext.pc)
#define HL_HOST_UC_REGS(uc) ((uint64_t *)(void *)((uc)->uc_mcontext.regs))
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
#else
#error unsupported native signal context
#endif

#endif
