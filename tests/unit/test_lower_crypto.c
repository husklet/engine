#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/lower/crypto.h"
#include "../../src/translator/guest/x86_64/lower/x87.h"

typedef struct observations {
    uint32_t instructions[32];
    unsigned count;
    unsigned addresses;
    unsigned loads;
    unsigned inserts64;
    unsigned drops;
} observations;

static observations seen;

static void reset_seen(void) { memset(&seen, 0, sizeof(seen)); }

void emit32(uint32_t instruction) {
    if (seen.count < 32) seen.instructions[seen.count] = instruction;
    seen.count++;
}
void emit_ea(struct insn *insn, uint64_t next) { (void)insn; (void)next; seen.addresses++; }
void e_movconst(int destination, uint64_t value) { (void)destination; (void)value; }
void e_movz(int destination, uint32_t value, int shift) { (void)destination; (void)value; (void)shift; }
void e_fmov_to_d(int destination, int source) { (void)destination; (void)source; }
void hl_x86_emit_constant_part(int destination, uint32_t value, int shift) {
    (void)destination; (void)value; (void)shift;
}
void hl_x86_emit_vector3(uint32_t base, int destination, int left, int right) {
    emit32(base | ((uint32_t)right << 16) | ((uint32_t)left << 5) | (uint32_t)destination);
}
void hl_x86_emit_vector_copy(int destination, int source) { (void)destination; (void)source; }
void hl_x86_emit_vector_broadcast32(int destination, int source, int lane) {
    (void)destination; (void)source; (void)lane;
}
void hl_x86_emit_vector_extract(int destination, int low, int high, int byte) {
    (void)destination; (void)low; (void)high; (void)byte;
}
void hl_x86_emit_vector_insert32(int destination, int destination_lane, int source, int source_lane) {
    (void)destination; (void)destination_lane; (void)source; (void)source_lane;
}
void hl_x86_emit_vector_insert64(int destination, int destination_lane, int source, int source_lane) {
    (void)destination; (void)destination_lane; (void)source; (void)source_lane;
    seen.inserts64++;
}
void hl_x86_emit_vector_shift_right(int destination, int source, int width, int shift, int arithmetic) {
    (void)destination; (void)source; (void)width; (void)shift; (void)arithmetic;
}
void hl_x86_emit_vector_load128(int destination, int address, int offset) {
    (void)destination; (void)address; (void)offset; seen.loads++;
}
void hl_x86_emit_vector_load64(int destination, int address) { (void)destination; (void)address; seen.loads++; }
void hl_x86_emit_vector_load32(int destination, int address) { (void)destination; (void)address; seen.loads++; }
int hl_x86_x87_known(void) { return 0; }
void hl_x86_x87_drop(void) { seen.drops++; }

static int check_aes_state(void) {
    struct insn insn = {.map3 = 2, .op = 0xdc, .reg = 2, .rm_reg = 3};
    hl_x86_crypto_state state = {.optimize = 1};
    HL_CHECK(hl_x86_lower_crypto(&insn, 0, &state) == TX_NEXT);
    HL_CHECK(state.zero_ready == 1 && state.mask_ready == 0);
    HL_CHECK(seen.count == 4);
    HL_CHECK(seen.instructions[0] == UINT32_C(0x6e3a1f5a));
    HL_CHECK(seen.instructions[1] == UINT32_C(0x4e284b42));
    HL_CHECK(seen.instructions[2] == UINT32_C(0x4e286842));
    HL_CHECK(seen.instructions[3] == UINT32_C(0x6e231c42));

    reset_seen();
    HL_CHECK(hl_x86_lower_crypto(&insn, 0, &state) == TX_NEXT);
    HL_CHECK(seen.count == 3);
    HL_CHECK(seen.instructions[0] == UINT32_C(0x4e284b42));
    return 0;
}

static int check_pclmul_selection(void) {
    struct insn insn = {.map3 = 3, .op = 0x44, .reg = 4, .rm_reg = 5, .imm = 0x11};
    hl_x86_crypto_state state = {.optimize = 1};
    reset_seen();
    HL_CHECK(hl_x86_lower_crypto(&insn, 0, &state) == TX_NEXT);
    HL_CHECK(seen.count == 1 && seen.instructions[0] == UINT32_C(0x4ee5e084));
    HL_CHECK(seen.inserts64 == 0);

    reset_seen();
    insn.imm = 1;
    HL_CHECK(hl_x86_lower_crypto(&insn, 0, &state) == TX_NEXT);
    HL_CHECK(seen.inserts64 == 1);
    HL_CHECK(seen.count == 1 && seen.instructions[0] == UINT32_C(0x0ee5e204));
    return 0;
}

static int check_policy_and_memory(void) {
    struct insn insn = {.map3 = 2, .op = 0x00, .reg = 1, .rm_reg = 2};
    hl_x86_crypto_state disabled = {.optimize = 0};
    reset_seen();
    HL_CHECK(hl_x86_lower_crypto(&insn, 0, &disabled) == TX_FALL);
    HL_CHECK(seen.count == 0);

    struct insn aesimc = {.map3 = 2, .op = 0xdb, .reg = 1, .is_mem = 1};
    hl_x86_crypto_state enabled = {.optimize = 1};
    HL_CHECK(hl_x86_lower_crypto(&aesimc, UINT64_C(0x1234), &enabled) == TX_NEXT);
    HL_CHECK(seen.addresses == 1 && seen.loads == 1);
    HL_CHECK(seen.count == 1 && seen.instructions[0] == UINT32_C(0x4e287a61));
    return 0;
}

int main(void) {
    HL_CHECK(check_aes_state() == 0);
    HL_CHECK(check_pclmul_selection() == 0);
    HL_CHECK(check_policy_and_memory() == 0);
    return 0;
}
