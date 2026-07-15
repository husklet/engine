#ifndef HL_TRANSLATOR_GUEST_X86_64_LEGACY_H
#define HL_TRANSLATOR_GUEST_X86_64_LEGACY_H

#include <stdint.h>

struct cpu;

typedef int64_t (*hl_x86_time_fn)(void *context);
typedef int (*hl_x86_alarm_fn)(void *context, uint64_t seconds, uint64_t *remaining_seconds);

typedef struct hl_x86_legacy_context {
    uint64_t nonpie_low;
    uint64_t nonpie_high;
    uint64_t nonpie_bias;
    hl_x86_time_fn time_seconds;
    hl_x86_alarm_fn set_alarm;
    void *callback_context;
} hl_x86_legacy_context;

int hl_x86_legacy_normalize(struct cpu *cpu, const hl_x86_legacy_context *context);
void hl_x86_legacy_restore_fork(struct cpu *cpu);

#endif
