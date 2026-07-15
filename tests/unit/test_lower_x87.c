#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/lower/primitives.h"
#include "../../src/translator/guest/x86_64/lower/x87.h"

typedef struct calls {
    unsigned raw, loads, stores, runtime_loads, runtime_stores, runtime_pushes, runtime_tops, exits;
    int destination, source, index, delta;
    uint64_t constant, rip, reason;
    uint32_t instruction;
} calls;
static calls seen;

static void reset_calls(void) { memset(&seen, 0, sizeof(seen)); }

void emit32(uint32_t instruction) { seen.raw++; seen.instruction = instruction; }
void g_ldr_d(int destination, int address) { seen.loads++; seen.destination = destination; seen.source = address; }
void g_str_d(int source, int address) { seen.stores++; seen.source = source; seen.destination = address; }
void e_fp_ld(int destination, int index) { seen.runtime_loads++; seen.destination = destination; seen.index = index; }
void e_fp_st(int source, int index) { seen.runtime_stores++; seen.source = source; seen.index = index; }
void e_fp_push(int source) { seen.runtime_pushes++; seen.source = source; }
void e_fp_settop(int delta) { seen.runtime_tops++; seen.delta = delta; }
void e_movconst(int destination, uint64_t value) { seen.destination = destination; seen.constant = value; }
void e_str(int source, int base, int offset) { (void)base; (void)offset; seen.stores++; seen.source = source; }
void emit_exit_const(uint64_t rip, uint64_t reason) { seen.exits++; seen.rip = rip; seen.reason = reason; }

void e_addi(int d, int s, unsigned i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_bfi(int d, int s, int l, int w, int sf) { (void)d; (void)s; (void)l; (void)w; (void)sf; }
void e_csel(int d, int t, int f, int c, int sf) { (void)d; (void)t; (void)f; (void)c; (void)sf; }
void e_cset(int d, int c, int sf) { (void)d; (void)c; (void)sf; }
void e_fcom_setfpsw(int l, int r) { (void)l; (void)r; }
void e_fmov_from_d(int d, int s) { (void)d; (void)s; }
void e_fmov_to_d(int d, int s) { (void)d; (void)s; }
void e_ldr(int d, int b, int o) { (void)d; (void)b; (void)o; }
void e_lsl_i(int d, int s, int i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_lsr_i(int d, int s, int i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_rrr(uint32_t i, int d, int l, int r, int sf, int sh) { (void)i; (void)d; (void)l; (void)r; (void)sf; (void)sh; }
void e_subi(int d, int s, unsigned i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_subi_s(int d, int s, unsigned i, int sf) { (void)d; (void)s; (void)i; (void)sf; }
void e_sxt(int d, int s, int w) { (void)d; (void)s; (void)w; }

int main(void) {
    hl_x86_x87_reset();
    reset_calls();
    hl_x86_x87_load(18, 3);
    HL_CHECK(seen.runtime_loads == 1 && seen.loads == 0 && seen.destination == 18 && seen.index == 3);

    hl_x86_x87_anchor(0);
    reset_calls();
    hl_x86_x87_push(16);
    HL_CHECK(seen.runtime_pushes == 0 && seen.raw == 1 && seen.stores == 1 && seen.source == 16);
    reset_calls();
    hl_x86_x87_load(17, 1);
    HL_CHECK(seen.runtime_loads == 0 && seen.raw == 1 && seen.loads == 1 && seen.destination == 17);

    reset_calls();
    hl_x86_x87_materialize();
    HL_CHECK(seen.constant == 7 && seen.stores == 1);
    reset_calls();
    hl_x86_x87_materialize();
    HL_CHECK(seen.stores == 0);

    hl_x86_x87_adjust_top(2);
    reset_calls();
    hl_x86_x87_drop();
    HL_CHECK(seen.constant == 1 && seen.stores == 1 && !hl_x86_x87_known());
    reset_calls();
    hl_x86_x87_adjust_top(-1);
    HL_CHECK(seen.runtime_tops == 1 && seen.delta == -1);

    hl_x86_x87_anchor(0);
    reset_calls();
    hl_x86_x87_round();
    HL_CHECK(seen.loads == 1 && seen.stores == 1 && seen.runtime_loads == 0 && seen.raw == 3);

    reset_calls();
    hl_x86_x87_function(X87_FSIN, UINT64_C(0x1234));
    HL_CHECK(seen.exits == 1 && seen.rip == UINT64_C(0x1234) && seen.reason == R_X87FUNC);
    HL_CHECK(!hl_x86_x87_known());
    return 0;
}
