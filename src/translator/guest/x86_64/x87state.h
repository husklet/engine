#ifndef HL_TRANSLATOR_GUEST_X86_64_X87STATE_H
#define HL_TRANSLATOR_GUEST_X86_64_X87STATE_H

#include <stdint.h>

struct cpu;

double hl_x86_ext80_load(const uint8_t image[10]);
void hl_x86_ext80_store(double value, uint8_t image[10]);
void hl_x86_x87_load_ext80(struct cpu *cpu);
void hl_x86_x87_store_ext80_pop(struct cpu *cpu);
void hl_x86_fxsave(struct cpu *cpu);
void hl_x86_fxrstor(struct cpu *cpu);

#endif
