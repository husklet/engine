#include "test.h"

#include <string.h>

#include "../../src/translator/guest/x86_64/address.h"

enum operation { ADDI, SUBI, MOVC, ADDR, LSR, MOVR, MOVZ, UXT, LCPU, LSCALED, LUNSCALED, LOAD, GUARD, BRANCH, PATCH };

typedef struct address_event {
    enum operation operation;
    int a, b, c, d, e;
    uint64_t value;
} address_event;

typedef struct address_capture {
    address_event events[32];
    int count;
} address_capture;

static address_event *record(void *context, enum operation operation) {
    address_capture *capture = context;
    address_event *event = &capture->events[capture->count++];
    memset(event, 0, sizeof(*event));
    event->operation = operation;
    return event;
}

static void addi(void *c, int d, int n, unsigned i, int sf, int sh) {
    address_event *e = record(c, ADDI);
    e->a = d;
    e->b = n;
    e->c = (int)i;
    e->d = sf;
    e->e = sh;
}

static void subi(void *c, int d, int n, unsigned i, int sf, int sh) {
    address_event *e = record(c, SUBI);
    e->a = d;
    e->b = n;
    e->c = (int)i;
    e->d = sf;
    e->e = sh;
}

static void movc(void *c, int d, uint64_t v) {
    address_event *e = record(c, MOVC);
    e->a = d;
    e->value = v;
}

static void addr(void *c, int d, int n, int m, int sf, int sh) {
    address_event *e = record(c, ADDR);
    e->a = d;
    e->b = n;
    e->c = m;
    e->d = sf;
    e->e = sh;
}

static void lsr(void *c, int d, int n, int sh, int sf) {
    address_event *e = record(c, LSR);
    e->a = d;
    e->b = n;
    e->c = sh;
    e->d = sf;
}

static void movr(void *c, int d, int n, int sf) {
    address_event *e = record(c, MOVR);
    e->a = d;
    e->b = n;
    e->c = sf;
}

static void movz(void *c, int d, uint32_t i, int sh) {
    address_event *e = record(c, MOVZ);
    e->a = d;
    e->b = (int)i;
    e->c = sh;
}

static void uxt(void *c, int d, int n, int b) {
    address_event *e = record(c, UXT);
    e->a = d;
    e->b = n;
    e->c = b;
}

static void lcpu(void *c, int t, int o) {
    address_event *e = record(c, LCPU);
    e->a = t;
    e->b = o;
}

static void lscaled(void *c, int w, int t, int n, unsigned o) {
    address_event *e = record(c, LSCALED);
    e->a = w;
    e->b = t;
    e->c = n;
    e->d = (int)o;
}

static void lunscaled(void *c, int w, int t, int n, int o) {
    address_event *e = record(c, LUNSCALED);
    e->a = w;
    e->b = t;
    e->c = n;
    e->d = o;
}

static void load(void *c, int w, int t, int n) {
    address_event *e = record(c, LOAD);
    e->a = w;
    e->b = t;
    e->c = n;
}

static void guard(void *c, int r, uint64_t s, uint64_t p) {
    address_event *e = record(c, GUARD);
    e->a = r;
    e->value = s;
    e->b = (int)p;
}

static uintptr_t branch(void *c) {
    record(c, BRANCH);
    return (uintptr_t)77;
}

static void patch(void *c, uintptr_t p, int r) {
    address_event *e = record(c, PATCH);
    e->value = p;
    e->a = r;
}

static const hl_x86_address_emitter emitter = {addi, subi,    movc,      addr, lsr,   movr,   movz, uxt,
                                               lcpu, lscaled, lunscaled, load, guard, branch, patch};

static hl_x86_address_state state(address_capture *capture) {
    hl_x86_address_state state = {capture, &emitter, 0, 0, 0, 64, 72, 1, 0};
    memset(capture, 0, sizeof(*capture));
    return state;
}

int main(void) {
    address_capture capture;
    hl_x86_address_state s = state(&capture);
    hl_x86_insn insn = {.m_hasbase = 1, .m_base = 3, .disp = 24, .len = 4};
    hl_x86_address_emit(&s, &insn, 100, 1);
    HL_CHECK(capture.count == 1 && capture.events[0].operation == ADDI && capture.events[0].a == 17 &&
             capture.events[0].b == 3 && capture.events[0].c == 24);

    s = state(&capture);
    insn.disp = -32;
    hl_x86_address_emit(&s, &insn, 100, 1);
    HL_CHECK(capture.count == 1 && capture.events[0].operation == SUBI && capture.events[0].c == 32);

    s = state(&capture);
    insn.m_hasindex = 1;
    insn.m_index = 5;
    insn.m_scale = 2;
    insn.disp = 0x12345;
    hl_x86_address_emit(&s, &insn, 100, 1);
    HL_CHECK(capture.count == 3 && capture.events[0].operation == ADDR && capture.events[0].c == 5 &&
             capture.events[1].operation == ADDI && capture.events[1].e == 1 && capture.events[2].operation == ADDI);

    s = state(&capture);
    memset(&insn, 0, sizeof(insn));
    insn.rip_rel = 1;
    insn.disp = -4;
    hl_x86_address_emit(&s, &insn, 100, 1);
    HL_CHECK(capture.count == 1 && capture.events[0].operation == MOVC && capture.events[0].value == 96);

    s = state(&capture);
    insn.rip_rel = 0;
    insn.m_hasbase = 1;
    insn.m_base = 2;
    insn.addr32 = 1;
    insn.seg = 1;
    hl_x86_address_emit(&s, &insn, 100, 1);
    HL_CHECK(capture.count == 4 && capture.events[1].operation == UXT && capture.events[2].operation == LCPU &&
             capture.events[2].b == 64 && capture.events[3].operation == ADDR);

    s = state(&capture);
    s.nonpie_lo = 0x400000;
    s.nonpie_hi = 0x500000;
    s.nonpie_bias = 0x100000000;
    memset(&insn, 0, sizeof(insn));
    insn.m_hasbase = 1;
    insn.m_base = 2;
    hl_x86_address_emit(&s, &insn, 100, 1);
    HL_CHECK(capture.count == 6 && capture.events[1].operation == LSR && capture.events[2].operation == BRANCH &&
             capture.events[3].operation == MOVC && capture.events[4].operation == ADDR);
    HL_CHECK(capture.events[5].operation == PATCH);

    s = state(&capture);
    insn = (hl_x86_insn){.m_hasbase = 1, .m_base = 6, .disp = 32, .len = 5};
    int rn = 0, off = 0;
    HL_CHECK(hl_x86_address_fold(&s, &insn, 8, &rn, &off) == 1 && rn == 6 && off == 32);
    insn.disp = -7;
    HL_CHECK(hl_x86_address_fold(&s, &insn, 8, &rn, &off) == 2);
    s.bus_active = 1;
    HL_CHECK(hl_x86_address_fold(&s, &insn, 8, &rn, &off) == 0);

    s = state(&capture);
    insn.disp = 32;
    hl_x86_address_load(&s, &insn, 100, 8, 9);
    HL_CHECK(capture.count == 1 && capture.events[0].operation == LSCALED);
    s = state(&capture);
    insn.m_hasindex = 1;
    hl_x86_address_load(&s, &insn, 100, 8, 9);
    HL_CHECK(capture.events[capture.count - 2].operation == GUARD &&
             capture.events[capture.count - 1].operation == LOAD);
    return EXIT_SUCCESS;
}
