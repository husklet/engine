#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/lower/alu.h"
#include "../../src/translator/guest/x86_64/lower/primitives.h"

typedef struct calls {
    int loads, stores, alus, narrows, locks, constants, inserts, bytes, writebacks;
    int kind, destination, left, right, width, memory, value, constant_value;
} calls;

static calls seen;
static int loaded = 6;
static int memory;
static int lock_succeeds;

static void reset(void) {
    memset(&seen, 0, sizeof(seen));
    loaded = 6;
    memory = 0;
    lock_succeeds = 0;
}

int alu_kind_primary(uint8_t op) {
    return ((op & 7) <= 5) ? (op >> 3) & 7 : -1;
}

int byte_val(struct insn *insn, int reg, int scratch) {
    (void)insn;
    (void)scratch;
    seen.bytes++;
    return reg + 20;
}

void byte_wb(struct insn *insn, int reg, int value) {
    (void)insn;
    (void)reg;
    seen.writebacks++;
    seen.value = value;
}

int rm_load(struct insn *insn, uint64_t next, int width, int *is_memory) {
    (void)insn;
    (void)next;
    seen.loads++;
    seen.width = width;
    *is_memory = memory;
    return loaded;
}

void rm_store(struct insn *insn, int width, int value) {
    (void)insn;
    seen.stores++;
    seen.width = width;
    seen.value = value;
}

int xaludirect_on(void) {
    return 1;
}

void do_alu(int kind, int destination, int left, int right, int width) {
    seen.alus++;
    seen.kind = kind;
    seen.destination = destination;
    seen.left = left;
    seen.right = right;
    seen.width = width;
}

void narrow_adcsbb(int adc, int destination, int left, int right, int width) {
    seen.narrows++;
    seen.kind = adc;
    seen.destination = destination;
    seen.left = left;
    seen.right = right;
    seen.width = width;
}

int lock_rmw(int kind, int width, int source) {
    seen.locks++;
    seen.kind = kind;
    seen.width = width;
    seen.value = source;
    return lock_succeeds;
}

void e_movconst(int destination, uint64_t value) {
    seen.constants++;
    seen.destination = destination;
    seen.constant_value = (int)value;
}

void e_bfi(int destination, int source, int lsb, int width, int sf) {
    (void)lsb;
    (void)sf;
    seen.inserts++;
    seen.destination = destination;
    seen.value = source;
    seen.width = width;
}

int main(void) {
    struct insn insn;
    memset(&insn, 0, sizeof(insn));
    insn.opsize = 8;

    insn.op = 0x90;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_FALL);
    HL_CHECK(seen.loads == 0 && seen.alus == 0);

    /* add r64,r/m64 writes directly to the register destination. */
    reset();
    insn.op = 0x03;
    insn.reg = 2;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_NEXT);
    HL_CHECK(seen.loads == 1 && seen.alus == 1 && seen.kind == 0);
    HL_CHECK(seen.destination == 2 && seen.left == 2 && seen.right == loaded && seen.stores == 0);

    /* cmp discards its result and never writes the r/m operand. */
    reset();
    insn.op = 0x39;
    insn.reg = 3;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_NEXT);
    HL_CHECK(seen.kind == 7 && seen.destination == -1 && seen.stores == 0);

    /* Byte ADC uses the narrow flag path and high-byte-aware helpers. */
    reset();
    insn.op = 0x12;
    insn.reg = 4;
    insn.has_rex = 0;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_NEXT);
    HL_CHECK(seen.narrows == 1 && seen.kind == 1 && seen.bytes == 1 && seen.writebacks == 1);

    /* A successful locked memory operation replaces the ordinary ALU/store sequence. */
    reset();
    insn.op = 0x01;
    insn.reg = 1;
    insn.lock = 1;
    memory = 1;
    lock_succeeds = 1;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_NEXT);
    HL_CHECK(seen.locks == 1 && seen.alus == 0 && seen.stores == 0);

    /* Group-1 immediate forwards the exact immediate and stores a memory result. */
    reset();
    insn.op = 0x83;
    insn.reg = 6;
    insn.lock = 0;
    insn.imm = -7;
    memory = 1;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_NEXT);
    HL_CHECK(seen.constants == 1 && seen.constant_value == -7 && seen.kind == 6);
    HL_CHECK(seen.alus == 1 && seen.destination == 16 && seen.stores == 1);

    /* TEST computes flags only. */
    reset();
    insn.op = 0x85;
    insn.reg = 5;
    HL_CHECK(hl_x86_lower_alu(&insn, 0x1000) == TX_NEXT);
    HL_CHECK(seen.kind == 4 && seen.destination == -1 && seen.stores == 0);
    return 0;
}
