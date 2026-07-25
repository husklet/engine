#ifndef HL_TRANSLATOR_GUEST_X86_64_LOWER_REPSTR_H
#define HL_TRANSLATOR_GUEST_X86_64_LOWER_REPSTR_H

#include <stddef.h>
#include <stdint.h>

#include "../decoder.h"
#include "primitives.h"

enum hl_x86_direction {
    HL_X86_DIRECTION_FORWARD = 0,
    HL_X86_DIRECTION_BACKWARD = 1,
    HL_X86_DIRECTION_DYNAMIC = 2,
};

typedef struct hl_x86_repstr_state {
    enum hl_x86_direction direction;
    int optimize;
} hl_x86_repstr_state;

typedef void (*hl_x86_rep_store_commit_fn)(uint64_t guest, uint64_t size);
typedef int (*hl_x86_rep_store_observation_active_fn)(void);
typedef int (*hl_x86_rep_access_fn)(uint64_t guest, size_t size);
struct cpu;
void hl_x86_rep_set_store_commit(hl_x86_rep_store_commit_fn callback,
                                 hl_x86_rep_store_observation_active_fn active);
void hl_x86_rep_set_access_validators(hl_x86_rep_access_fn readable, hl_x86_rep_access_fn writable);

int hl_x86_lower_repstr(struct insn *insn, uint64_t next, hl_x86_repstr_state *state);
uint64_t hl_x86_rep_movs(void *destination, const void *source, uint64_t bytes, int width, int backward,
                         struct cpu *cpu, uint64_t rip);
uint64_t hl_x86_rep_stos(void *destination, uint64_t value, uint64_t count, int width, int backward,
                         struct cpu *cpu, uint64_t rip);

#endif
