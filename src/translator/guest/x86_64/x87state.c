#include "x87state.h"
#include "cpu.h"

#include <string.h>

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
        uint64_t bits = (uint64_t)sign << 63 | UINT64_C(0x7ff) << 52 |
                        (fraction != 0 ? UINT64_C(1) << 51 : 0);
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
#if defined(__aarch64__)
    uint64_t fpcr;
    uint64_t fpsr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    {
        uint32_t arm_rounding = (uint32_t)((fpcr >> 22) & 3u);
        uint32_t x86_rounding = ((arm_rounding & 1u) << 1) | ((arm_rounding >> 1) & 1u);
        static const unsigned fpsr_bit[6] = {0, 7, 1, 2, 3, 4};
        mxcsr |= x86_rounding << 13;
        for (unsigned bit = 0; bit < 6; ++bit)
            mxcsr |= (uint32_t)((fpsr >> fpsr_bit[bit]) & 1u) << bit;
    }
#endif
    memcpy(image, &cpu->fpcw, 2);
    {
        uint16_t status = (uint16_t)((cpu->fpsw & 0x4700) | ((cpu->fptop & 7) << 11));
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
#if defined(__aarch64__)
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
            fpsr |= (uint64_t)((mxcsr >> bit) & 1u) << fpsr_bit[bit];
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
        __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
    }
#endif
}
