#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/lower/repstr.h"

static unsigned movs_count;
static unsigned stos_count;
static unsigned emitted;
static unsigned exits;
static uint64_t last_constant;
static int last_store_offset;
static uint64_t last_exit_rip;
static uint64_t last_exit_reason;
static uint32_t code[32];

uint64_t hl_x86_guest_pointer(uint64_t address) { return address; }
void hl_x86_count_rep_movs(void) { movs_count++; }
void hl_x86_count_rep_stos(void) { stos_count++; }
void emit32(uint32_t instruction) { code[emitted++] = instruction; }
uint32_t *hl_x86_emit_cursor(void) { return &code[emitted]; }
void hl_x86_emit_spill(void) {}
void hl_x86_emit_reload(void) {}
void hl_x86_emit_vector_reset(void) {}
void hl_x86_emit_host_pointer(int destination, uint64_t pointer) {
    (void)destination;
    (void)pointer;
}
void e_movconst(int destination, uint64_t value) {
    (void)destination;
    last_constant = value;
}
void e_str(int source, int base, int offset) {
    (void)source;
    (void)base;
    last_store_offset = offset;
}
void emit_exit_const(uint64_t rip, uint64_t reason) {
    exits++;
    last_exit_rip = rip;
    last_exit_reason = reason;
}
void e_addi(int d, int s, unsigned i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_subi(int d, int s, unsigned i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_ldr(int d, int b, int o) { (void)d; (void)b; (void)o; }
void e_load(int w, int d, int a) { (void)w; (void)d; (void)a; }
void e_store(int w, int s, int a) { (void)w; (void)s; (void)a; }
void e_lsl_i(int d, int s, int n, int sf) { (void)d; (void)s; (void)n; (void)sf; }
void e_mov_rr(int d, int s, int sf) { (void)d; (void)s; (void)sf; }
void e_rrr(uint32_t op, int d, int l, int r, int sf, int sh) {
    (void)op; (void)d; (void)l; (void)r; (void)sf; (void)sh;
}

static int check_copy_semantics(void) {
    uint8_t smear[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t smeared[] = {0, 1, 0, 1, 0, 1, 0, 1};
    hl_x86_rep_movs(smear + 2, smear, 6, 2, 0);
    HL_CHECK(memcmp(smear, smeared, sizeof(smear)) == 0);

    uint8_t backward[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t moved[] = {0, 1, 0, 1, 2, 3, 4, 5};
    hl_x86_rep_movs(backward + 6, backward + 4, 6, 2, 1);
    HL_CHECK(memcmp(backward, moved, sizeof(backward)) == 0);
    HL_CHECK(movs_count == 2);
    return 0;
}

static int check_fill_semantics(void) {
    uint8_t bytes[8] = {0};
    const uint8_t forward[] = {0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0, 0};
    hl_x86_rep_stos(bytes, UINT64_C(0x1234), 3, 2, 0);
    HL_CHECK(memcmp(bytes, forward, sizeof(bytes)) == 0);

    memset(bytes, 0, sizeof(bytes));
    hl_x86_rep_stos(bytes + 4, UINT64_C(0xab), 3, 1, 1);
    HL_CHECK(bytes[2] == 0xab && bytes[3] == 0xab && bytes[4] == 0xab);
    HL_CHECK(stos_count == 2);
    return 0;
}

static int check_direction_emission(void) {
    struct insn insn = {0};
    hl_x86_repstr_state state = {.direction = HL_X86_DIRECTION_DYNAMIC, .optimize = 1};
    insn.op = 0xfd;
    HL_CHECK(hl_x86_lower_repstr(&insn, UINT64_C(0x1000), &state) == TX_NEXT);
    HL_CHECK(state.direction == HL_X86_DIRECTION_BACKWARD);
    HL_CHECK(last_constant == 1 && last_store_offset == OFF_DF);

    insn.op = 0xfc;
    HL_CHECK(hl_x86_lower_repstr(&insn, UINT64_C(0x1001), &state) == TX_NEXT);
    HL_CHECK(state.direction == HL_X86_DIRECTION_FORWARD && last_constant == 0);

    insn.op = 0x90;
    HL_CHECK(hl_x86_lower_repstr(&insn, 0, &state) == TX_FALL);
    return 0;
}

static int check_compare_exit(void) {
    struct insn insn = {.op = 0xae, .repne = 1, .opsize = 1};
    hl_x86_repstr_state state = {.direction = HL_X86_DIRECTION_BACKWARD, .optimize = 1};
    HL_CHECK(hl_x86_lower_repstr(&insn, UINT64_C(0x4321), &state) == TX_BREAK);
    HL_CHECK(exits == 1 && last_exit_rip == UINT64_C(0x4321) && last_exit_reason == R_REPSTR);
    HL_CHECK(last_constant == (UINT64_C(1) | (UINT64_C(1) << 8) | (UINT64_C(1) << 9) |
                               (UINT64_C(1) << 10) | (UINT64_C(1) << 11)));
    HL_CHECK(last_store_offset == OFF_DIVOP);
    return 0;
}

int main(void) {
    HL_CHECK(check_copy_semantics() == 0);
    HL_CHECK(check_fill_semantics() == 0);
    HL_CHECK(check_direction_emission() == 0);
    HL_CHECK(check_compare_exit() == 0);
    return 0;
}
