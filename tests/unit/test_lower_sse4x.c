#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/lower/sse4x.h"
#include "../../src/translator/guest/x86_64/lower/x87.h"

typedef struct calls {
    unsigned raw;
    unsigned loads;
    unsigned stores;
    unsigned addresses;
    unsigned drops;
    uint32_t instructions[8];
} calls;

static calls seen;

static void reset_calls(void) {
    memset(&seen, 0, sizeof(seen));
}

void emit32(uint32_t instruction) {
    if (seen.raw < 8) seen.instructions[seen.raw] = instruction;
    seen.raw++;
}

void emit_ea(struct insn *insn, uint64_t next) {
    (void)insn;
    (void)next;
    seen.addresses++;
}

void e_load(int width, int destination, int address) {
    (void)width;
    (void)destination;
    (void)address;
    seen.loads++;
}

void e_store(int width, int source, int address) {
    (void)width;
    (void)source;
    (void)address;
    seen.stores++;
}

int byte_val(struct insn *insn, int register_number, int scratch) {
    (void)insn;
    (void)scratch;
    return register_number;
}

void e_bfi(int destination, int source, int bit, int width, int sixty_four_bit) {
    (void)destination;
    (void)source;
    (void)bit;
    (void)width;
    (void)sixty_four_bit;
}

void e_movconst(int destination, uint64_t value) {
    (void)destination;
    (void)value;
}

int hl_x86_x87_known(void) {
    return 0;
}

void hl_x86_x87_drop(void) {
    seen.drops++;
}

void hl_x86_emit_load_scalar32(int destination, int address) {
    (void)destination;
    (void)address;
    seen.loads++;
}

void hl_x86_emit_insert_scalar32(int destination, int destination_lane, int source, int source_lane) {
    emit32(UINT32_C(0x6E000400) | ((uint32_t)((destination_lane << 3) | 4) << 16) |
           ((uint32_t)(source_lane << 2) << 11) | ((uint32_t)source << 5) | (uint32_t)destination);
}

void hl_x86_emit_vector3(uint32_t base, int destination, int left, int right) {
    emit32(base | ((uint32_t)right << 16) | ((uint32_t)left << 5) | (uint32_t)destination);
}

int main(void) {
    struct insn insn;
    hl_x86_sse4x_state disabled = {.optimize = 0};
    hl_x86_sse4x_state enabled = {.optimize = 1};

    memset(&insn, 0, sizeof(insn));
    insn.map3 = 2;
    insn.op = 0xF0;
    insn.repne = 1;
    insn.opsize = 4;
    insn.reg = 3;
    insn.rm_reg = 5;
    HL_CHECK(hl_x86_lower_sse4x(&insn, 0, &disabled) == TX_FALL);
    HL_CHECK(seen.raw == 0);
    HL_CHECK(hl_x86_lower_sse4x(&insn, 0, &enabled) == TX_NEXT);
    HL_CHECK(seen.raw == 1 && seen.instructions[0] == UINT32_C(0x1AC55063));

    reset_calls();
    memset(&insn, 0, sizeof(insn));
    insn.map3 = 3;
    insn.op = 0x14;
    insn.reg = 7;
    insn.rm_reg = 2;
    insn.imm = 15;
    HL_CHECK(hl_x86_lower_sse4x(&insn, 0, &enabled) == TX_NEXT);
    HL_CHECK(seen.raw == 1 && seen.instructions[0] == UINT32_C(0x0E1F3CE2));

    reset_calls();
    memset(&insn, 0, sizeof(insn));
    insn.map3 = 3;
    insn.op = 0x22;
    insn.reg = 4;
    insn.rm_reg = 6;
    insn.rexW = 1;
    insn.imm = 1;
    HL_CHECK(hl_x86_lower_sse4x(&insn, 0, &enabled) == TX_NEXT);
    HL_CHECK(seen.raw == 1);
    HL_CHECK(seen.instructions[0] == UINT32_C(0x4E181CC4));
    return 0;
}
