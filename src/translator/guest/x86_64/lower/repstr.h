#ifndef HL_TRANSLATOR_GUEST_X86_64_LOWER_REPSTR_H
#define HL_TRANSLATOR_GUEST_X86_64_LOWER_REPSTR_H

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

int hl_x86_lower_repstr(struct insn *insn, uint64_t next, hl_x86_repstr_state *state);
void hl_x86_rep_movs(void *destination, const void *source, uint64_t bytes, int width, int backward);
void hl_x86_rep_stos(void *destination, uint64_t value, uint64_t count, int width, int backward);

#endif
