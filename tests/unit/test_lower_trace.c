#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/lower/trace.h"
#include "../../src/translator/guest/x86_64/lower/primitives.h"

static uint32_t code[64];
static size_t code_count;
static unsigned flag_loads;
static unsigned chain_exits;

void emit32(uint32_t instruction) { code[code_count++] = instruction; }
uint32_t *hl_x86_emit_cursor(void) { return &code[code_count]; }
void hl_x86_emit_flags_load(void) { flag_loads++; }
void hl_x86_emit_host_pointer(int destination, uint64_t pointer) { (void)destination; (void)pointer; }
void e_ldr(int destination, int base, int offset) { (void)destination; (void)base; (void)offset; }
void e_str(int source, int base, int offset) { (void)source; (void)base; (void)offset; }
void e_subi(int destination, int source, unsigned immediate, int wide) {
    (void)destination; (void)source; (void)immediate; (void)wide;
}
void e_nzcv_save(void) {}
void emit_exit_const(uint64_t rip, uint64_t reason) { (void)rip; (void)reason; }
int alu_kind_primary(uint8_t opcode) { return opcode < 0x40 && (opcode & 7) <= 5 ? (opcode >> 3) & 7 : -1; }

int hl_x86_decode(uint64_t address, struct insn *insn) {
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;
    memset(insn, 0, sizeof(*insn));
    insn->op = bytes[0];
    insn->len = 1;
    return 0;
}

static void no_flags(void) {}
static int page_present(uint64_t address) { (void)address; return 1; }
static void chain_exit(uint64_t target) { (void)target; chain_exits++; emit32(UINT32_C(0xd503201f)); }

static hl_x86_trace_state state(int *pending, uint64_t counters[2], uint64_t profiles[3]) {
    return (hl_x86_trace_state){
        .pending_flags = pending,
        .tier_counters = counters,
        .flag_elisions = &profiles[0],
        .flag_scans = &profiles[1],
        .tier_folds = &profiles[2],
        .materialize_flags = no_flags,
        .fix_add_flags = no_flags,
        .fix_logic_flags = no_flags,
        .emit_chain_exit = chain_exit,
        .page_translated = page_present,
        .flag_elision = 1,
        .tier_two = 1,
    };
}

static int check_analysis(void) {
    uint64_t seen[] = {3, 7, 11};
    uint8_t trap[] = {0x0f, 0x0b};
    HL_CHECK(hl_x86_trace_seen(seen, 3, 7));
    HL_CHECK(!hl_x86_trace_seen(seen, 3, 9));
    HL_CHECK(hl_x86_trace_trap_head((uint64_t)(uintptr_t)trap));

    uint32_t same[] = {UINT32_C(0xf9000420), UINT32_C(0xf9400421)};
    uint32_t distinct[] = {UINT32_C(0xf9000420), UINT32_C(0xf9400821)};
    HL_CHECK(hl_x86_trace_loop_hazard((uint64_t)(uintptr_t)same,
                                      (uint64_t)(uintptr_t)(same + 2)));
    HL_CHECK(!hl_x86_trace_loop_hazard((uint64_t)(uintptr_t)distinct,
                                       (uint64_t)(uintptr_t)(distinct + 2)));
    return 0;
}

static int check_liveness(void) {
    uint8_t guest[] = {0x90, 0x74}; /* nop; jz */
    int pending = 0;
    uint64_t counters[2] = {0}, profiles[3] = {0};
    hl_x86_trace_state trace = state(&pending, counters, profiles);
    int live = hl_x86_trace_flags_livein(&trace, (uint64_t)(uintptr_t)guest,
                                         (uint64_t)(uintptr_t)guest);
    HL_CHECK((live & HL_X86_FLAG_ZF) != 0);
    HL_CHECK(profiles[1] == 1);
    return 0;
}

static int check_tier_two_emission(void) {
    int pending = 0;
    uint64_t counters[2] = {0}, profiles[3] = {0};
    hl_x86_trace_state trace = state(&pending, counters, profiles);
    memset(code, 0, sizeof(code));
    code_count = flag_loads = chain_exits = 0;
    hl_x86_trace_self_loop(&trace, 1, 0, 4, code, 0);
    HL_CHECK(code_count == 2 && flag_loads == 1 && chain_exits == 1);
    HL_CHECK(code[0] == UINT32_C(0x54000001));
    HL_CHECK(code[1] == UINT32_C(0xd503201f));
    return 0;
}

int main(void) {
    HL_CHECK(check_analysis() == 0);
    HL_CHECK(check_liveness() == 0);
    HL_CHECK(check_tier_two_emission() == 0);
    return 0;
}
