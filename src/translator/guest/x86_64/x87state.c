#include "x87state.h"
#include "cpu.h"
#include "../../../host/host_cpu.h" // HL_HOST_CPU_*: FXSAVE/FXRSTOR project MXCSR onto the host FP control

#include <string.h>

#if defined(HL_HOST_CPU_X86_64)
#include <xmmintrin.h> // _mm_getcsr/_mm_setcsr == STMXCSR/LDMXCSR; baseline SSE, so no -m flag is needed
#endif

double hl_x86_ext80_load(const uint8_t image[10]) {
    uint64_t significand;
    uint16_t sign_exponent;
    int sign;
    int exponent;
    double value;
    memcpy(&significand, image, sizeof(significand));
    memcpy(&sign_exponent, image + 8, sizeof(sign_exponent));
    sign = sign_exponent >> 15;
    exponent = sign_exponent & 0x7fff;
    if (significand == 0 && exponent == 0) {
        uint64_t bits = (uint64_t)sign << 63;
        memcpy(&value, &bits, sizeof(value));
    } else if (exponent == 0x7fff) {
        uint64_t fraction = significand & ((UINT64_C(1) << 63) - 1);
        uint64_t bits = (uint64_t)sign << 63 | UINT64_C(0x7ff) << 52 | (fraction != 0 ? UINT64_C(1) << 51 : 0);
        memcpy(&value, &bits, sizeof(value));
    } else {
        uint64_t bits;
        int scaled_exponent;
        value = (double)significand;
        memcpy(&bits, &value, sizeof(bits));
        scaled_exponent = (int)((bits >> 52) & 0x7ff) + exponent - 16383 - 63;
        if (scaled_exponent <= 0) {
            bits = (uint64_t)sign << 63;
        } else if (scaled_exponent >= 0x7ff) {
            bits = (uint64_t)sign << 63 | UINT64_C(0x7ff) << 52;
        } else {
            bits = (bits & ~(UINT64_C(0x7ff) << 52)) | (uint64_t)scaled_exponent << 52;
            bits = (bits & ~(UINT64_C(1) << 63)) | (uint64_t)sign << 63;
        }
        memcpy(&value, &bits, sizeof(value));
    }
    return value;
}

void hl_x86_ext80_store(double value, uint8_t image[10]) {
    uint64_t bits;
    uint64_t fraction;
    uint64_t significand;
    uint16_t sign_exponent;
    int sign;
    int exponent;
    memcpy(&bits, &value, sizeof(bits));
    sign = (int)(bits >> 63);
    exponent = (int)((bits >> 52) & 0x7ff);
    fraction = bits & ((UINT64_C(1) << 52) - 1);
    if (exponent == 0) {
        significand = 0;
        sign_exponent = (uint16_t)(sign != 0 ? 0x8000 : 0);
    } else if (exponent == 0x7ff) {
        significand = (UINT64_C(1) << 63) | (fraction << 11);
        sign_exponent = (uint16_t)((sign << 15) | 0x7fff);
    } else {
        significand = (UINT64_C(1) << 63) | (fraction << 11);
        sign_exponent = (uint16_t)((sign << 15) | ((exponent - 1023 + 16383) & 0x7fff));
    }
    memcpy(image, &significand, sizeof(significand));
    memcpy(image + 8, &sign_exponent, sizeof(sign_exponent));
}

void hl_x86_x87_load_ext80(struct cpu *cpu) {
    cpu->fptop = (cpu->fptop - 1) & 7;
    cpu->st[cpu->fptop & 7] = hl_x86_ext80_load((const uint8_t *)(uintptr_t)cpu->x87_ea);
}

void hl_x86_x87_store_ext80_pop(struct cpu *cpu) {
    double value = cpu->st[cpu->fptop & 7];
    cpu->fptop = (cpu->fptop + 1) & 7;
    hl_x86_ext80_store(value, (uint8_t *)(uintptr_t)cpu->x87_ea);
}

void hl_x86_fxsave(struct cpu *cpu) {
    uint8_t *image = (uint8_t *)(uintptr_t)cpu->x87_ea;
    uint32_t mxcsr = 0x1f80;
    uint16_t fsw_exc = 0; // x87 FSW exception flags (bits 0..5) + ES(7)/B(15), projected from host FPSR
#if defined(HL_HOST_CPU_AARCH64)
    uint64_t fpcr;
    uint64_t fpsr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    {
        uint32_t arm_rounding = (uint32_t)((fpcr >> 22) & 3u);
        uint32_t x86_rounding = ((arm_rounding & 1u) << 1) | ((arm_rounding >> 1) & 1u);
        static const unsigned fpsr_bit[6] = {0, 7, 1, 2, 3, 4};
        mxcsr |= x86_rounding << 13;
        for (unsigned bit = 0; bit < 6; ++bit) {
            uint16_t raised = (uint16_t)((fpsr >> fpsr_bit[bit]) & 1u);
            mxcsr |= (uint32_t)raised << bit;
            fsw_exc |= (uint16_t)(raised << bit); // FSW exception bits are at the same positions as MXCSR
        }
        if (fsw_exc & (uint16_t)(~cpu->fpcw & 0x3f))
            fsw_exc |= (uint16_t)0x8080; // ES(7) + B(15) if any raised exception is unmasked per FCW
    }
#elif defined(HL_HOST_CPU_X86_64)
    // On an x86-64 host the guest MXCSR needs no projection at all: it IS the host MXCSR. The AArch64 arm
    // above has to re-map the rounding field (ARM RMode orders the two directed modes the other way round)
    // and scatter the exception flags across FPSR's non-contiguous bits; here the guest's SSE work executes
    // as host SSE work in the very same architectural register, so rounding control (bits 14:13) and the six
    // raised-exception flags (bits 0..5 = IE,DE,ZE,OE,UE,PE) are already in their guest-visible positions.
    // Take the whole word rather than OR-ing selected fields into the 0x1f80 default -- the exception MASKS
    // (bits 12:7), FZ (15) and DAZ (6) are equally the guest's, written there by its own LDMXCSR/FXRSTOR.
    mxcsr = _mm_getcsr();
    {
        // The x87 FSW exception flags sit at the SAME bit positions as MXCSR's (IE,DE,ZE,OE,UE,PE), and the
        // engine models the guest x87 stack in software on top of host SSE arithmetic -- so, exactly as on the
        // AArch64 host, the flags the host FP unit raised are the ones the guest's x87 status word must show.
        fsw_exc = (uint16_t)(mxcsr & 0x3fu);
        if (fsw_exc & (uint16_t)(~cpu->fpcw & 0x3f))
            fsw_exc |= (uint16_t)0x8080; // ES(7) + B(15) if any raised exception is unmasked per FCW
    }
#endif
    memcpy(image, &cpu->fpcw, 2);
    {
        uint16_t status = (uint16_t)((cpu->fpsw & 0x4700) | ((cpu->fptop & 7) << 11) | fsw_exc);
        memcpy(image + 2, &status, sizeof(status));
    }
    image[4] = 0xff;
    image[5] = 0;
    for (int index = 0; index < 8; ++index)
        hl_x86_ext80_store(cpu->st[index], image + 32 + index * 16);
    memcpy(image + 24, &mxcsr, sizeof(mxcsr));
    memcpy(image + 160, cpu->v, 16 * 16);
}

void hl_x86_fxrstor(struct cpu *cpu) {
    const uint8_t *image = (const uint8_t *)(uintptr_t)cpu->x87_ea;
    uint16_t status;
    uint16_t control;
    uint32_t mxcsr;
    memcpy(&control, image, sizeof(control));
    memcpy(&status, image + 2, sizeof(status));
    memcpy(&mxcsr, image + 24, sizeof(mxcsr));
    cpu->fpcw = control;
    cpu->fpsw = status & 0x4700;
    cpu->fptop = (status >> 11) & 7;
    for (int index = 0; index < 8; ++index)
        cpu->st[index] = hl_x86_ext80_load(image + 32 + index * 16);
    memcpy(cpu->v, image + 160, 16 * 16);
#if defined(HL_HOST_CPU_AARCH64)
    {
        uint64_t fpcr;
        uint64_t fpsr;
        uint32_t x86_rounding = (mxcsr >> 13) & 3u;
        uint32_t arm_rounding = ((x86_rounding & 1u) << 1) | ((x86_rounding >> 1) & 1u);
        static const unsigned fpsr_bit[6] = {0, 7, 1, 2, 3, 4};
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
        fpcr = (fpcr & ~(UINT64_C(3) << 22)) | (uint64_t)arm_rounding << 22;
        fpsr &= ~UINT64_C(0x9f);
        for (unsigned bit = 0; bit < 6; ++bit)
            fpsr |= (uint64_t)(((mxcsr | status) >> bit) & 1u) << fpsr_bit[bit];
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
        __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
    }
#elif defined(HL_HOST_CPU_X86_64)
    // The direct mapping again (see hl_x86_fxsave): the saved image's MXCSR word goes straight back into the
    // host MXCSR, rounding field, masks and flags in place. The FSW exception bits are OR-ed in for the same
    // reason the AArch64 arm merges `mxcsr | status`: cpu->fpsw keeps only the condition codes (0x4700), so
    // the host FP status word is the sole carrier of the guest's raised x87 exceptions across the restore.
    //
    // MASK the word. Unlike the AArch64 arm -- which builds a fresh FPCR/FPSR out of individual fields -- this
    // hands a GUEST-CONTROLLED value to LDMXCSR, and LDMXCSR raises #GP (delivered to us as a fatal SIGSEGV,
    // i.e. a guest-triggered engine crash) if any bit outside the CPU's MXCSR_MASK is set. Real FXRSTOR
    // #GPs on such an image too, but that fault belongs to the GUEST; ours would kill the engine. 0xffff keeps
    // every architecturally defined field, including DAZ, and drops only the reserved high half.
    _mm_setcsr((mxcsr | (uint32_t)(status & 0x3fu)) & 0xffffu);
#endif
}
