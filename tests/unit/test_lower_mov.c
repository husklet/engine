#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/lower/mov.h"
#include "../../src/translator/guest/x86_64/lower/primitives.h"

typedef struct calls {
    int ea, ea_core, load_mem, guards, loads, stores, constants, moves, inserts, bytes, writebacks;
    int width, destination, source, base, offset, bias;
    uint64_t value, rip;
} calls;
static calls seen;
static int fold;

static void reset(void) { memset(&seen, 0, sizeof(seen)); fold = 0; }
int byte_val(struct insn *i, int r, int s) { (void)i; (void)s; seen.bytes++; return r + 20; }
void byte_wb(struct insn *i, int r, int v) { (void)i; seen.writebacks++; seen.destination = r; seen.source = v; }
void emit_ea(struct insn *i, uint64_t n) { (void)i; (void)n; seen.ea++; }
void emit_ea_core(struct insn *i, uint64_t n, int b) { (void)i; (void)n; seen.ea_core++; seen.bias = b; }
void emit_load_mem(struct insn *i, uint64_t n, int w, int d) {
    (void)i; (void)n; seen.load_mem++; seen.width = w; seen.destination = d;
}
int ea_imm_fold(struct insn *i, int w, int *r, int *o) {
    (void)i; seen.width = w; *r = 7; *o = 24; return fold;
}
void emit_bus_guard(int r, uint64_t s, uint64_t pc) { seen.guards++; seen.base = r; seen.width = (int)s; seen.rip = pc; }
void e_movz(int d, uint32_t v, int s) { (void)s; seen.constants++; seen.destination = d; seen.value = v; }
void e_movconst(int d, uint64_t v) { seen.constants++; seen.destination = d; seen.value = v; }
void e_bfi(int d, int s, int l, int w, int sf) {
    (void)l; (void)sf; seen.inserts++; seen.destination = d; seen.source = s; seen.width = w;
}
void e_load(int w, int d, int a) { seen.loads++; seen.width = w; seen.destination = d; seen.base = a; }
void e_store(int w, int s, int a) { seen.stores++; seen.width = w; seen.source = s; seen.base = a; }
void e_ldrs(int w, int d, int a) { e_load(w, d, a); }
void e_store_uoff(int w, int s, int b, unsigned o) { e_store(w, s, b); seen.offset = (int)o; }
void e_stur(int w, int s, int b, int o) { e_store(w, s, b); seen.offset = o; }
void e_mov_rr(int d, int s, int sf) { (void)sf; seen.moves++; seen.destination = d; seen.source = s; }
void e_sxt(int d, int s, int w) { seen.moves++; seen.destination = d; seen.source = s; seen.width = w; }
void e_addi(int d, int s, unsigned v, int sf) { (void)sf; seen.moves++; seen.destination = d; seen.source = s; seen.value = v; }
void e_subi(int d, int s, unsigned v, int sf) { e_addi(d, s, v, sf); }

/* Unused ALU primitives are supplied because this private header intentionally groups lowerer calls. */
int alu_kind_primary(uint8_t o) { (void)o; return -1; }
int rm_load(struct insn *i, uint64_t n, int w, int *m) { (void)i; (void)n; (void)w; *m = 0; return 0; }
void rm_store(struct insn *i, int w, int v) { (void)i; (void)w; (void)v; }
int xaludirect_on(void) { return 1; }
void do_alu(int k, int d, int a, int b, int w) { (void)k; (void)d; (void)a; (void)b; (void)w; }
void narrow_adcsbb(int a, int d, int l, int r, int w) { (void)a; (void)d; (void)l; (void)r; (void)w; }
int lock_rmw(int k, int w, int s) { (void)k; (void)w; (void)s; return 0; }

int main(void) {
    struct insn i;
    hl_x86_move_image image = {0};
    memset(&i, 0, sizeof(i)); i.opsize = 8; i.len = 5;

    i.op = 0x90;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_FALL && seen.constants == 0);

    reset(); i.op = 0xb8; i.imm = 0x1234; image.blob_code = 0x1234; image.bias = 0x100000;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_NEXT);
    HL_CHECK(seen.destination == 0 && seen.value == 0x101234);

    reset(); i.op = 0xc7; i.is_mem = 1; i.imm = -4;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_NEXT);
    HL_CHECK(seen.ea == 1 && seen.guards == 1 && seen.stores == 1 && seen.rip == 95);

    /* moffs is normalized into the standard effective-address path. */
    reset(); i.op = 0xa1; i.imm = 0x4455; i.is_mem = 0; i.opsize = 8;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_NEXT);
    HL_CHECK(i.is_mem && !i.m_hasbase && !i.m_hasindex && !i.rip_rel && i.disp == i.imm);
    HL_CHECK(seen.ea == 1 && seen.loads == 1 && seen.destination == 0);

    /* A foldable memory store uses the direct displacement emitter and no EA temporary. */
    reset(); i.op = 0x89; i.is_mem = 1; i.reg = 3; i.opsize = 8; fold = 1;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_NEXT);
    HL_CHECK(seen.ea == 0 && seen.stores == 1 && seen.base == 7 && seen.offset == 24);

    /* A non-PIE type LEA materializes its low identity; other LEAs retain the generic path. */
    reset(); i.op = 0x8d; i.is_mem = 1; i.rip_rel = 1; i.reg = 9; i.disp = 0x20;
    image.low = 0x1000; image.high = 0x2000; image.bias = 0x100000; image.types_low = 0x1100; image.types_high = 0x1200;
    HL_CHECK(hl_x86_lower_mov(&i, image.bias + 0x1100 - 0x20, &image) == TX_NEXT);
    HL_CHECK(seen.constants == 1 && seen.destination == 9 && seen.value == 0x1100 && seen.ea_core == 0);

    reset(); image.types_low = 0x1800; image.types_high = 0x1900;
    HL_CHECK(hl_x86_lower_mov(&i, image.bias + 0x1100 - 0x20, &image) == TX_NEXT);
    HL_CHECK(seen.ea_core == 1 && seen.bias == 0 && seen.moves == 1);

    reset(); i.op = 0x50; i.rexB = 1;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_NEXT);
    HL_CHECK(seen.moves == 1 && seen.destination == 4 && seen.value == 8 && seen.stores == 1 && seen.source == 8);

    reset(); i.op = 0x63; i.opsize = 8; i.is_mem = 0; i.reg = 2; i.rm_reg = 3;
    HL_CHECK(hl_x86_lower_mov(&i, 100, &image) == TX_NEXT);
    HL_CHECK(seen.moves == 1 && seen.destination == 2 && seen.source == 3 && seen.width == 4);
    return 0;
}
