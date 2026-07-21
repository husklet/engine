#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/lower/primitives.h"
#include "../../src/translator/guest/x86_64/lower/shift.h"

typedef struct calls {
    int loads, stores, moves, shifts, flags, parity, ea, exits, rcl;
    int width, destination, source, kind, count;
    uint64_t constant, descriptor, reason;
} calls;
static calls seen;
static int loaded = 5;
static int memory;

static void reset(void) { memset(&seen, 0, sizeof(seen)); loaded = 5; memory = 0; }
int rm_load(struct insn *i, uint64_t n, int w, int *m) { (void)i; (void)n; seen.loads++; seen.width = w; *m = memory; return loaded; }
void rm_store(struct insn *i, int w, int v) { (void)i; seen.stores++; seen.width = w; seen.source = v; }
void emit_rcl_rcr(struct insn *i, uint64_t n, int w, int r, int c) { (void)i; (void)n; seen.rcl++; seen.width = w; seen.kind = r; seen.count = c; }
void emit_exit_const(uint64_t rip, uint64_t reason) { (void)rip; seen.exits++; seen.reason = reason; }
void emit_ea(struct insn *i, uint64_t n) { (void)i; (void)n; seen.ea++; }
void e_movconst(int d, uint64_t v) { seen.destination = d; seen.constant = v; seen.descriptor = v; }
void e_mov_rr(int d, int s, int sf) { (void)sf; seen.moves++; seen.destination = d; seen.source = s; }
void e_lsl_i(int d, int s, int c, int sf) { (void)sf; seen.shifts++; seen.kind = 4; seen.destination = d; seen.source = s; seen.count = c; }
void e_lsr_i(int d, int s, int c, int sf) { e_lsl_i(d, s, c, sf); seen.kind = 5; }
void e_asr_i(int d, int s, int c, int sf) { e_lsl_i(d, s, c, sf); seen.kind = 7; }
void e_ror_i(int d, int s, int c, int sf) { e_lsl_i(d, s, c, sf); seen.kind = 1; }
void e_rot_flags_cl(int r, int k, int w) { (void)r; seen.flags++; seen.kind = k; seen.width = w; }
void e_rot_flags_const(int r, int k, int w, int c) { e_rot_flags_cl(r, k, w); seen.count = c; }
void e_nzcv_save(void) { seen.flags++; }
void e_nzcv_save_setcf(int c) { (void)c; seen.flags++; }
void e_nzcv_set_of(int o) { (void)o; seen.flags++; }
void e_pf_save(int s) { (void)s; seen.parity++; }
void e_store(int w, int s, int a) { (void)a; seen.stores++; seen.width = w; seen.source = s; }
void e_str(int s, int b, int o) { (void)b; (void)o; seen.stores++; seen.source = s; }

/* Encoding details are covered by emitter tests; this contract records only lowerer decisions. */
void emit32(uint32_t i) { (void)i; }
void e_ldr(int d, int b, int o) { (void)d; (void)b; (void)o; }
void e_uxt(int d, int s, int w) { (void)d; (void)s; (void)w; }
void e_sxt(int d, int s, int w) { (void)d; (void)s; (void)w; }
void e_bfi(int d, int s, int l, int w, int sf) { (void)d; (void)s; (void)l; (void)w; (void)sf; }
void e_rrr(uint32_t i, int d, int l, int r, int sf, int sh) { (void)i; (void)d; (void)l; (void)r; (void)sf; (void)sh; }
void e_shv(uint32_t i, int d, int s, int c, int sf) { (void)i; (void)sf; seen.shifts++; seen.destination = d; seen.source = s; seen.count = c; }
void e_tst(int s, int sf) { (void)s; (void)sf; seen.flags++; }
void e_csel(int d, int t, int f, int c, int sf) { (void)d; (void)t; (void)f; (void)c; (void)sf; }
void e_subi(int d, int s, unsigned v, int sf) { (void)d; (void)s; (void)v; (void)sf; }
void e_subi_s(int d, int s, unsigned v, int sf) { e_subi(d, s, v, sf); }

int main(void) {
    struct insn i;
    hl_x86_shift_state state = {0, 0, 1};
    memset(&i, 0, sizeof(i)); i.opsize = 8;

    i.op = 0x90;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_FALL && seen.loads == 0);

    /* Immediate RCL/RCR are routed intact to the carry-aware helper. */
    reset(); i.op = 0xc1; i.reg = 2; i.imm = 65;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.rcl == 1 && seen.width == 8 && seen.kind == 0 && seen.count == 1);
    reset(); i.op = 0xd1; i.reg = 3;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.rcl == 1 && seen.kind == 1 && seen.count == 1);

    /* RCL-by-CL exits to the C helper with exact register/high-byte metadata. */
    reset(); i.op = 0xd2; i.reg = 2; i.opsize = 1; i.rm_reg = 4; i.has_rex = 0; i.is_mem = 0;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_BREAK);
    HL_CHECK(seen.exits == 1 && seen.reason == 12 && (seen.descriptor & (UINT64_C(1) << 10)));
    HL_CHECK(((seen.descriptor >> 16) & 31) == 0);

    /* A zero effective count preserves flags; a 32-bit destination is still zero-extended. */
    reset(); i.op = 0xc1; i.reg = 4; i.imm = 32; i.opsize = 4; i.rm_reg = 6; loaded = 6;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.moves == 1 && seen.destination == 6 && seen.source == 6);
    HL_CHECK(seen.flags == 0 && seen.stores == 0);

    /* Direct register SHL avoids scratch/store while retaining exact flag synthesis. */
    reset(); i.op = 0xc1; i.reg = 4; i.imm = 3; i.opsize = 8; i.rm_reg = 9; loaded = 9;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.shifts >= 1 && seen.stores == 1); /* flag path calls rm_store */
    HL_CHECK(seen.flags > 0 && seen.parity == 1);

    /* Proven-dead output flags skip every flag and parity operation but keep the value. */
    reset(); state.output_flags_dead = 1; i.reg = 5; i.imm = 7;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.shifts == 1 && seen.flags == 0 && seen.parity == 0 && seen.stores == 1);

    /* Immediate byte ROL by a nonzero multiple of the width rotates by 0 but STILL sets CF
       (x86 masks the count to 5 bits; mc = imm & 0x1f != 0). Regression for stale-CF on `rolb $8`.
       Count passed to the flag helper is the width (>1) so OF is left untouched (multi-bit rotate). */
    reset(); state.output_flags_dead = 0; i.op = 0xc0; i.reg = 0; i.imm = 8; i.opsize = 1;
    i.is_mem = 0; memory = 0; i.rm_reg = 0; i.has_rex = 1;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.flags == 1 && seen.kind == 0 && seen.count == 8);

    /* Immediate word ROR by 16: same 0-rotate-but-CF-set case. */
    reset(); i.op = 0xc1; i.reg = 1; i.imm = 16; i.opsize = 2; i.rm_reg = 0; i.has_rex = 1;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.flags == 1 && seen.kind == 1 && seen.count == 16);

    /* A count that masks to 0 (imm=32 on a byte) changes no flags. */
    reset(); i.op = 0xc0; i.reg = 0; i.imm = 32; i.opsize = 1; i.rm_reg = 0; i.has_rex = 1;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.flags == 0);

    /* Memory CL shifts preserve the EA path and use scratch storage. */
    reset(); state.output_flags_dead = 0; i.op = 0xd3; i.reg = 5; i.opsize = 8; i.is_mem = 1; memory = 1;
    HL_CHECK(hl_x86_lower_shift(&i, 100, &state) == TX_NEXT);
    HL_CHECK(seen.loads == 1 && seen.shifts >= 1 && seen.stores >= 1 && seen.flags > 0);
    return 0;
}
