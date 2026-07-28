#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/lower/repstr.h"
#include "../../src/linux_abi/logical_vma.h"

static unsigned movs_count;
static unsigned stos_count;
static unsigned emitted;
static unsigned exits;
static uint64_t last_constant;
static int last_store_offset;
static uint64_t last_exit_rip;
static uint64_t last_exit_reason;
static uint32_t code[128];
static int observe_stores;
static uint64_t committed_address[16];
static uint64_t committed_size[16];
static unsigned committed_count;
static uintptr_t readable_until = UINTPTR_MAX;
static uintptr_t writable_until = UINTPTR_MAX;
static int logical_active;

static int range_ends_before(uint64_t address, size_t length, uintptr_t limit) {
    return address <= UINTPTR_MAX && length <= UINTPTR_MAX - (uintptr_t)address && (uintptr_t)address + length <= limit;
}

static int range_readable(uint64_t address, size_t length) {
    return range_ends_before(address, length, readable_until);
}

static int range_writable(uint64_t address, size_t length) {
    return range_ends_before(address, length, writable_until);
}

static int store_observation_active(void) {
    return observe_stores;
}

static void store_committed(uint64_t address, uint64_t size) {
    if (committed_count < 16) {
        committed_address[committed_count] = address;
        committed_size[committed_count] = size;
    }
    committed_count++;
}

int hl_logical_vma_global_active(void) {
    return logical_active;
}

int hl_logical_vma_pin_data(uint64_t address, size_t length, unsigned access, hl_logical_vma_pin *pin) {
    (void)access;
    *pin = (hl_logical_vma_pin){
        .host = (void *)(uintptr_t)address,
        .contiguous = length,
    };
    return 0;
}

void hl_logical_vma_unpin(hl_logical_vma_pin *pin) {
    (void)pin;
}

uint64_t hl_x86_guest_pointer(uint64_t address) {
    return address;
}

void hl_x86_count_rep_movs(void) {
    movs_count++;
}

void hl_x86_count_rep_stos(void) {
    stos_count++;
}

void emit32(uint32_t instruction) {
    code[emitted++] = instruction;
}

uint32_t *hl_x86_emit_cursor(void) {
    return &code[emitted];
}

void hl_x86_emit_spill(void) {
}

void hl_x86_emit_reload(void) {
}

void hl_x86_emit_vector_reset(void) {
}

int emit_soft_memory_active(void) {
    return 0;
}

void emit_memory_guard(int address, uint64_t size, uint64_t rip, uint32_t required) {
    (void)address;
    (void)size;
    (void)rip;
    (void)required;
}

void emit_soft_store_commit(uint64_t size) {
    (void)size;
}

void emit_soft_store_observe(uint64_t size) {
    (void)size;
}

void emit_soft_store_drain(void) {
}

void hl_x86_emit_host_pointer(int destination, uint64_t pointer) {
    (void)destination;
    (void)pointer;
}

void hl_x86_emit_block_return(void) {
    emit32(UINT32_C(0xd61f0200));
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

void e_addi(int d, int s, unsigned i, int sf) {
    (void)d;
    (void)s;
    (void)i;
    (void)sf;
}

void e_subi(int d, int s, unsigned i, int sf) {
    (void)d;
    (void)s;
    (void)i;
    (void)sf;
}

void e_ldr(int d, int b, int o) {
    (void)d;
    (void)b;
    (void)o;
}

void e_load(int w, int d, int a) {
    (void)w;
    (void)d;
    (void)a;
}

void e_store(int w, int s, int a) {
    (void)w;
    (void)s;
    (void)a;
}

void e_lsl_i(int d, int s, int n, int sf) {
    (void)d;
    (void)s;
    (void)n;
    (void)sf;
}

void e_mov_rr(int d, int s, int sf) {
    (void)d;
    (void)s;
    (void)sf;
}

void e_rrr(uint32_t op, int d, int l, int r, int sf, int sh) {
    (void)op;
    (void)d;
    (void)l;
    (void)r;
    (void)sf;
    (void)sh;
}

static int check_copy_semantics(void) {
    uint8_t smear[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t smeared[] = {0, 1, 0, 1, 0, 1, 0, 1};
    HL_CHECK(hl_x86_rep_movs(smear + 2, smear, 6, 2, 0, NULL, 0) == 3);
    HL_CHECK(memcmp(smear, smeared, sizeof(smear)) == 0);

    uint8_t backward[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t moved[] = {0, 1, 0, 1, 2, 3, 4, 5};
    HL_CHECK(hl_x86_rep_movs(backward + 6, backward + 4, 6, 2, 1, NULL, 0) == 3);
    HL_CHECK(memcmp(backward, moved, sizeof(backward)) == 0);
    HL_CHECK(movs_count == 2);

    uint8_t observed[] = {0, 1, 2, 3, 4, 5, 6, 7};
    observe_stores = 1;
    committed_count = 0;
    hl_x86_rep_set_store_commit(store_committed, store_observation_active);
    HL_CHECK(hl_x86_rep_movs(observed + 4, observed, 4, 2, 0, NULL, 0) == 2);
    HL_CHECK(committed_count == 1);
    HL_CHECK(committed_address[0] == (uint64_t)(uintptr_t)(observed + 4) && committed_size[0] == 4);
    observe_stores = 0;

    uint8_t logical_source[4096];
    uint8_t logical_destination[sizeof(logical_source)] = {0};
    memset(logical_source, 0x5a, sizeof(logical_source));
    logical_active = 1;
    observe_stores = 1;
    committed_count = 0;
    HL_CHECK(hl_x86_rep_movs(logical_destination, logical_source, sizeof(logical_source), 1, 0, NULL, 0) ==
             sizeof(logical_source));
    HL_CHECK(memcmp(logical_destination, logical_source, sizeof(logical_source)) == 0);
    HL_CHECK(committed_count == 1 && committed_size[0] == sizeof(logical_source));
    observe_stores = 0;
    logical_active = 0;
    return 0;
}

static int check_copy_stops_before_inaccessible_range(void) {
    uint8_t source[] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t destination[sizeof(source)] = {0};
    struct cpu cpu = {0};

    readable_until = (uintptr_t)(source + 4);
    writable_until = UINTPTR_MAX;
    hl_x86_rep_set_access_validators(range_readable, range_writable, NULL);

    HL_CHECK(hl_x86_rep_movs(destination, source, sizeof(source), 1, 0, &cpu, UINT64_C(0x1234)) == 4);
    HL_CHECK(memcmp(destination, source, 4) == 0);
    HL_CHECK(memcmp(destination + 4, (uint8_t[4]){0}, 4) == 0);
    HL_CHECK(cpu.reason == R_SOFTMISS);
    HL_CHECK(cpu.bus_ea == (uintptr_t)(source + 4));
    HL_CHECK(cpu.soft_required == X86_SOFT_READ);
    HL_CHECK(cpu.rip == UINT64_C(0x1234));

    readable_until = UINTPTR_MAX;
    hl_x86_rep_set_access_validators(NULL, NULL, NULL);
    return 0;
}

static int check_fill_semantics(void) {
    uint8_t bytes[8] = {0};
    const uint8_t forward[] = {0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0, 0};
    HL_CHECK(hl_x86_rep_stos(bytes, UINT64_C(0x1234), 3, 2, 0, NULL, 0) == 3);
    HL_CHECK(memcmp(bytes, forward, sizeof(bytes)) == 0);

    memset(bytes, 0, sizeof(bytes));
    HL_CHECK(hl_x86_rep_stos(bytes + 4, UINT64_C(0xab), 3, 1, 1, NULL, 0) == 3);
    HL_CHECK(bytes[2] == 0xab && bytes[3] == 0xab && bytes[4] == 0xab);
    HL_CHECK(stos_count == 2);

    memset(bytes, 0, sizeof(bytes));
    observe_stores = 1;
    committed_count = 0;
    hl_x86_rep_set_store_commit(store_committed, store_observation_active);
    HL_CHECK(hl_x86_rep_stos(bytes + 6, UINT64_C(0x1234), 3, 2, 1, NULL, 0) == 3);
    HL_CHECK(committed_count == 1);
    HL_CHECK(committed_address[0] == (uint64_t)(uintptr_t)(bytes + 2) && committed_size[0] == 6);
    observe_stores = 0;
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
    HL_CHECK(last_constant ==
             (UINT64_C(1) | (UINT64_C(1) << 8) | (UINT64_C(1) << 9) | (UINT64_C(1) << 10) | (UINT64_C(1) << 11)));
    HL_CHECK(last_store_offset == OFF_DIVOP);
    return 0;
}

int main(void) {
    HL_CHECK(check_copy_semantics() == 0);
    HL_CHECK(check_copy_stops_before_inaccessible_range() == 0);
    HL_CHECK(check_fill_semantics() == 0);
    HL_CHECK(check_direction_emission() == 0);
    HL_CHECK(check_compare_exit() == 0);
    return 0;
}
