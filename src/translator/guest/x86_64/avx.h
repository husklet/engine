#ifndef HL_TRANSLATOR_GUEST_X86_64_AVX_H
#define HL_TRANSLATOR_GUEST_X86_64_AVX_H

#include <stdint.h>

struct cpu;

typedef struct hl_x86_avx_state {
    const uint64_t *nonpie_low;
    const uint64_t *nonpie_high;
    const uint64_t *nonpie_bias;
} hl_x86_avx_state;

uint64_t hl_x86_avx_address(const hl_x86_avx_state *state, uint64_t address);
void hl_x86_avx_run(const hl_x86_avx_state *state, struct cpu *cpu);
void hl_x86_sse_run(const hl_x86_avx_state *state, struct cpu *cpu);
void hl_x86_avx_dump(void);

#endif
