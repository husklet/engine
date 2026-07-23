// isafuzz_gen -- deterministic x86-64 instruction-sequence generator for differential ISA fuzzing.
//
// Given a seed it emits a self-contained x86-64 C source file whose body is one long
// straight-line randomized instruction sequence written as file-scope basic inline assembly
// (so the compiler never reorders, allocates, or reinterprets any of it). After the sequence
// the program dumps a canonical fixed-width state image: the 14 general-purpose registers the
// sequence is allowed to touch, RFLAGS masked down to the bits that are architecturally defined
// at that point, MXCSR, all sixteen XMM registers, and an FNV-1a checksum of the scratch buffer.
//
// The same generated program is executed under qemu-x86_64 (reference) and under the engine
// (under test); any difference in the dump is a translator divergence.
//
// Determinism: identical seed => byte-identical output file. No time, no environment, no libc
// randomness. Everything the guest observes is baked into the generated source.
//
// Undefined flags are not fenced away -- the generator tracks, per emitted instruction, which of
// CF/PF/ZF/SF/OF are architecturally defined, gates flag consumers on that, and folds the final
// definedness into a constant mask that the generated program applies before printing RFLAGS.
// AF is always masked out. That keeps every *defined* flag bit under test while never comparing
// a bit the architecture leaves open.
//
// usage: isafuzz_gen <seed> <out.c> [steps] [feature-flags...]
//        feature flags: +x87  +bmi  (both off by default)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- rng

static uint64_t rng_state;

static uint64_t nx(void) {
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint32_t pick(uint32_t n) {
    return (uint32_t)(nx() % n);
}

static int chance(uint32_t pct) {
    return pick(100) < pct;
}

// ---------------------------------------------------------------- registers

// r15 is pinned to the scratch base and rsp is the live stack; neither is ever an operand of a
// generated instruction, and neither is dumped.
static const int POOL[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

enum { NPOOL = (int)(sizeof POOL / sizeof POOL[0]) };

static const char *R64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                              "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *R32[16] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                              "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *R16[16] = {"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                              "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *R8[16] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                             "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};

static int rreg(void) {
    return POOL[pick(NPOOL)];
}

// size codes: 0 = 8-bit, 1 = 16-bit, 2 = 32-bit, 3 = 64-bit
static const char SUF[4] = {'b', 'w', 'l', 'q'};
static const int BITS[4] = {8, 16, 32, 64};

static const char *rn(int sz, int r) {
    switch (sz) {
    case 0: return R8[r];
    case 1: return R16[r];
    case 2: return R32[r];
    default: return R64[r];
    }
}

// ---------------------------------------------------------------- output buffer

static char *out;
static size_t out_len, out_cap;

static void raw(const char *s) {
    size_t n = strlen(s);
    if (out_len + n + 1 > out_cap) {
        out_cap = (out_len + n + 1) * 2 + 4096;
        out = realloc(out, out_cap);
        if (!out) {
            fprintf(stderr, "oom\n");
            exit(2);
        }
    }
    memcpy(out + out_len, s, n);
    out_len += n;
    out[out_len] = 0;
}

// Instructions are accumulated per generated STEP and flushed as a single source line.
// That granularity is what makes delta-debugging sound: a step's guard instructions (the index
// clamp in front of a scaled memory operand, the non-zero divisor setup in front of a div, the
// `or $1` in front of a bsf) are inseparable from the instruction they protect, so the minimizer
// can never delete a guard and turn a real divergence into a wild-memory or #DE artifact.
static char step_buf[8192];
static size_t step_len;

static void asmf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (step_len + n + 8 >= sizeof step_buf) return;
    if (step_len) {
        memcpy(step_buf + step_len, "\\n  ", 4);
        step_len += 4;
    }
    memcpy(step_buf + step_len, line, n);
    step_len += n;
    step_buf[step_len] = 0;
}

// Flush the accumulated step as one line of the generated asm block.
static void step_flush(void) {
    if (!step_len) return;
    char wrapped[9216];
    snprintf(wrapped, sizeof wrapped, "    \"  %s\\n\"\n", step_buf);
    raw(wrapped);
    step_len = 0;
    step_buf[0] = 0;
}

// ---------------------------------------------------------------- flag definedness tracking

enum { F_CF = 1, F_PF = 2, F_ZF = 4, F_SF = 8, F_OF = 16, F_ALL = 31 };

static int fdef; // bitmask of architecturally-defined arithmetic flags right now

static void fset(int defined_bits, int touched_bits) {
    // touched_bits are written by the instruction; those in defined_bits become defined, the
    // rest become undefined. Untouched bits keep their previous definedness.
    fdef = (fdef & ~touched_bits) | (defined_bits & touched_bits);
}

static void fall(void) {
    fdef = F_ALL;
}

static void fnone(void) {
    fdef = 0;
}

// ---------------------------------------------------------------- scratch memory operands

// scratch is 8192 bytes; base displacements stay <= 4000 and the optional scaled index adds at
// most 63*8 = 504, so every access is comfortably in bounds.
#define SCRATCH_BYTES 8192

// Build a memory operand string. If `aligned16` the operand is guaranteed 16-byte aligned
// (base+disp only, disp a multiple of 16). Otherwise an index form may be used, in which case a
// clamping `and` is emitted first (which is itself a defined flag producer).
static void memop(char *buf, size_t bufsz, int aligned16, int width) {
    if (aligned16) {
        int disp = (int)pick(240) * 16;
        snprintf(buf, bufsz, "%d(%%r15)", disp);
        return;
    }
    if (chance(30)) {
        int idx = rreg();
        int scale = 1 << pick(4);
        int disp = (int)pick(3000);
        disp -= disp % (width ? width : 1);
        asmf("andl $0x3f, %%%s", R32[idx]);
        fall(); // and: CF=0 OF=0, PF/ZF/SF from result
        snprintf(buf, bufsz, "%d(%%r15,%%%s,%d)", disp, R64[idx], scale);
        return;
    }
    int disp = (int)pick(4000);
    disp -= disp % (width ? width : 1);
    snprintf(buf, bufsz, "%d(%%r15)", disp);
}

// Naturally-aligned base+displacement operand, no index. Used for the atomic RMW forms
// (xchg-with-memory, lock add/xadd/cmpxchg): the engine lowers those to ARM LSE atomics, which
// fault on an unaligned address, whereas x86 permits an unaligned locked access ("split lock").
// That is a real divergence -- see the campaign report -- but it is a single known issue, so the
// corpus keeps these operands aligned rather than rediscovering it on every other seed.
static void memop_aligned(char *buf, size_t bufsz, int width) {
    int disp = (int)pick(4000 / width) * width;
    snprintf(buf, bufsz, "%d(%%r15)", disp);
}

// ---------------------------------------------------------------- condition codes

struct cc {
    const char *name;
    int needs;
};
static const struct cc CCS[] = {
    {"o", F_OF},
    {"no", F_OF},
    {"b", F_CF},
    {"ae", F_CF},
    {"e", F_ZF},
    {"ne", F_ZF},
    {"be", F_CF | F_ZF},
    {"a", F_CF | F_ZF},
    {"s", F_SF},
    {"ns", F_SF},
    {"p", F_PF},
    {"np", F_PF},
    {"l", F_SF | F_OF},
    {"ge", F_SF | F_OF},
    {"le", F_ZF | F_SF | F_OF},
    {"g", F_ZF | F_SF | F_OF},
};

enum { NCCS = (int)(sizeof CCS / sizeof CCS[0]) };

// Return a condition code whose inputs are all currently defined, or NULL if none is.
static const char *usable_cc(void) {
    int tries = 24;
    while (tries--) {
        const struct cc *c = &CCS[pick(NCCS)];
        if ((c->needs & ~fdef) == 0) return c->name;
    }
    for (int i = 0; i < NCCS; i++)
        if ((CCS[i].needs & ~fdef) == 0) return CCS[i].name;
    return NULL;
}

// ---------------------------------------------------------------- instruction classes

static int label_counter;
static int want_x87, want_bmi;

static const char *ALU3[] = {"add", "sub", "adc", "sbb", "and", "or", "xor", "cmp"};

// ADC/SBB (and RCL/RCR) READ CF. Picking one while CF is architecturally undefined would make the
// program's result undefined too, and the resulting "divergence" would be a harness artifact rather
// than a translator bug -- so the carry consumers are only ever chosen when CF is defined.
static const char *pick_alu(void) {
    int i = (int)pick(8);
    if ((i == 2 || i == 3) && !(fdef & F_CF)) i = (int)pick(2); // -> add/sub
    return ALU3[i];
}

static int alu_defines_all(const char *op) {
    (void)op;
    return 1; // every op in ALU3 fully defines CF/PF/ZF/SF/OF (AF is masked out globally)
}

static void gen_alu_rr(void) {
    int sz = (int)pick(4);
    const char *op = pick_alu();
    int a = rreg(), b = rreg();
    asmf("%s%c %%%s, %%%s", op, SUF[sz], rn(sz, a), rn(sz, b));
    (void)alu_defines_all(op);
    fall();
}

static void gen_alu_ri(void) {
    int sz = (int)pick(4);
    const char *op = pick_alu();
    int a = rreg();
    long long imm;
    switch (pick(4)) {
    case 0: imm = (long long)(int8_t)pick(256); break;
    case 1: imm = (long long)(int16_t)pick(65536); break;
    case 2: imm = (long long)(int32_t)(uint32_t)nx(); break;
    default: imm = (long long)(int8_t)(pick(2) ? 1 : -1); break;
    }
    if (sz == 0)
        imm = (int8_t)imm;
    else if (sz == 1)
        imm = (int16_t)imm;
    else
        imm = (int32_t)imm;
    asmf("%s%c $%lld, %%%s", op, SUF[sz], imm, rn(sz, a));
    fall();
}

static void gen_alu_rm(int store) {
    int sz = (int)pick(4);
    const char *op = pick_alu();
    int a = rreg();
    char m[64];
    memop(m, sizeof m, 0, BITS[sz] / 8);
    if (store && strcmp(op, "cmp") != 0)
        asmf("%s%c %%%s, %s", op, SUF[sz], rn(sz, a), m);
    else
        asmf("%s%c %s, %%%s", op, SUF[sz], m, rn(sz, a));
    fall();
}

static void gen_unary(void) {
    int sz = (int)pick(4);
    int a = rreg();
    switch (pick(4)) {
    case 0:
        asmf("inc%c %%%s", SUF[sz], rn(sz, a));
        fset(F_ALL, F_PF | F_ZF | F_SF | F_OF); // CF untouched
        break;
    case 1:
        asmf("dec%c %%%s", SUF[sz], rn(sz, a));
        fset(F_ALL, F_PF | F_ZF | F_SF | F_OF);
        break;
    case 2:
        asmf("neg%c %%%s", SUF[sz], rn(sz, a));
        fall();
        break;
    default:
        asmf("not%c %%%s", SUF[sz], rn(sz, a)); // no flags at all
        break;
    }
}

static void gen_shift(void) {
    static const char *SH[] = {"shl", "shr", "sar", "rol", "ror", "rcl", "rcr"};
    int sz = (int)pick(4);
    int oi = (int)pick(7);
    if (oi >= 5 && !(fdef & F_CF)) oi = (int)pick(5); // rcl/rcr read CF -- only when it is defined
    const char *op = SH[oi];
    int rotate = op[0] == 'r';
    int through_carry = (strcmp(op, "rcl") == 0 || strcmp(op, "rcr") == 0);
    int a = rreg();
    int cmask = (sz == 3) ? 63 : 31;
    int form = (int)pick(10);

    if (form < 3) {
        // by-1 form: OF is defined
        asmf("%s%c %%%s", op, SUF[sz], rn(sz, a));
        if (rotate)
            fset(F_ALL, F_CF | F_OF);
        else
            fset(F_ALL, F_CF | F_PF | F_ZF | F_SF | F_OF);
        (void)through_carry;
        return;
    }
    if (form < 7) {
        // by-immediate
        int imm = (int)pick(256);
        // RCL/RCR at 8/16-bit width reduce the 5-bit count MODULO 9 / 17, so a masked count of 9
        // (or 17) is architecturally a NO-OP. qemu-user 10.2 gets that wrong -- it zeroes the
        // operand and clears CF -- while the engine is correct, so the corpus keeps the immediate
        // inside one rotation period for those widths instead of encoding the oracle's bug.
        if (through_carry && sz < 2) imm %= (sz == 0) ? 9 : 17;
        int eff = imm & cmask;
        asmf("%s%c $%d, %%%s", op, SUF[sz], imm, rn(sz, a));
        if (eff == 0) return; // flags entirely unchanged
        int of_defined = (eff == 1) ? F_OF : 0;
        if (rotate)
            fset(F_CF | of_defined, F_CF | F_OF);
        else
            fset(F_CF | F_PF | F_ZF | F_SF | of_defined, F_CF | F_PF | F_ZF | F_SF | F_OF);
        return;
    }
    if (form < 9) {
        // by-CL with a count the generator knows statically -- exercises the dynamic-count
        // lowering path while keeping flag definedness analyzable.
        int imm = (int)pick(256);
        int eff = imm & cmask;
        asmf("movb $%d, %%cl", imm);
        asmf("%s%c %%cl, %%%s", op, SUF[sz], rn(sz, a));
        if (eff == 0) return;
        int of_defined = (eff == 1) ? F_OF : 0;
        if (rotate)
            fset(F_CF | of_defined, F_CF | F_OF);
        else
            fset(F_CF | F_PF | F_ZF | F_SF | of_defined, F_CF | F_PF | F_ZF | F_SF | F_OF);
        return;
    }
    // truly dynamic count: conservatively mark everything it could touch undefined
    asmf("%s%c %%cl, %%%s", op, SUF[sz], rn(sz, a));
    if (rotate)
        fset(0, F_CF | F_OF);
    else
        fset(0, F_ALL);
}

static void gen_shld_shrd(void) {
    int sz = 2 + (int)pick(2); // 32 or 64 only
    int a = rreg(), b = rreg();
    const char *op = pick(2) ? "shld" : "shrd";
    int cmask = (sz == 3) ? 63 : 31;
    if (chance(60)) {
        int imm = (int)pick(64);
        // A masked count of 0 makes SHLD/SHRD a documented no-operation -- nothing is written at
        // all. qemu-user still writes the destination back, which for the 32-bit form zero-extends
        // bits 63:32; the engine (correctly) leaves them alone. Oracle disagreement, not a
        // translator bug, so keep the count out of the corpus.
        if ((imm & cmask) == 0) imm |= 1;
        asmf("%s%c $%d, %%%s, %%%s", op, SUF[sz], imm, rn(sz, a), rn(sz, b));
        fset(F_CF | F_PF | F_ZF | F_SF, F_ALL); // OF undefined for shld/shrd (Intel: undefined)
    } else {
        int imm = (int)pick(256);
        if ((imm & cmask) == 0) imm |= 1; // same no-operation caveat as the immediate form
        asmf("movb $%d, %%cl", imm);
        asmf("%s%c %%cl, %%%s, %%%s", op, SUF[sz], rn(sz, a), rn(sz, b));
        fnone();
    }
}

static void gen_muldiv(void) {
    int sz = 2 + (int)pick(2); // 32/64 for the wide paths
    int a = rreg(), b = rreg();
    switch (pick(6)) {
    case 0: // one-operand unsigned multiply: rdx:rax <- rax * r
        asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, 0));
        asmf("mul%c %%%s", SUF[sz], rn(sz, b == 0 || b == 2 ? 1 : b));
        fset(F_CF | F_OF, F_ALL);
        break;
    case 1: // one-operand signed multiply
        asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, 0));
        asmf("imul%c %%%s", SUF[sz], rn(sz, b == 0 || b == 2 ? 1 : b));
        fset(F_CF | F_OF, F_ALL);
        break;
    case 2: // two-operand
        asmf("imul%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, b));
        fset(F_CF | F_OF, F_ALL);
        break;
    case 3: { // three-operand
        int imm = (int)(int32_t)(uint32_t)nx();
        asmf("imul%c $%d, %%%s, %%%s", SUF[sz], imm, rn(sz, a), rn(sz, b));
        fset(F_CF | F_OF, F_ALL);
        break;
    }
    case 4:                                                      // unsigned divide, guarded so #DE is impossible
        asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, 0)); // rax <- dividend
        asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, b), rn(sz, 1)); // rcx <- divisor
        asmf("shr%c $1, %%%s", SUF[sz], rn(sz, 1));
        asmf("inc%c %%%s", SUF[sz], rn(sz, 1)); // divisor now in [1, 2^(n-1)]
        asmf("xorl %%edx, %%edx");              // clear high half => quotient always fits
        asmf("div%c %%%s", SUF[sz], rn(sz, 1));
        fnone();
        break;
    default: // signed divide, guarded
        asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, 0));
        asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, b), rn(sz, 1));
        asmf("shr%c $1, %%%s", SUF[sz], rn(sz, 1));
        asmf("inc%c %%%s", SUF[sz], rn(sz, 1)); // divisor in [1, 2^(n-1)], never 0 and never -1
        asmf(sz == 3 ? "cqto" : "cltd");        // sign-extend rax into rdx:rax
        asmf("idiv%c %%%s", SUF[sz], rn(sz, 1));
        fnone();
        break;
    }
}

static void gen_muldiv8(void) {
    // 8/16-bit divide edges, guarded so the quotient always fits.
    int a = rreg(), b = rreg();
    if (chance(50)) {
        asmf("movq %%%s, %%rax", R64[a]);
        asmf("movzbl %%al, %%eax"); // dividend < 256
        asmf("movb %%%s, %%cl", R8[b]);
        asmf("shrb $1, %%cl");
        asmf("incb %%cl"); // divisor in [1,128] => quotient <= 255
        asmf("divb %%cl");
        fnone();
    } else {
        asmf("movq %%%s, %%rax", R64[a]);
        asmf("movzwl %%ax, %%eax");
        asmf("movw %%%s, %%cx", R16[b]);
        asmf("shrw $1, %%cx");
        asmf("incw %%cx");
        asmf("xorl %%edx, %%edx");
        asmf("divw %%cx");
        fnone();
    }
}

static void gen_bitop(void) {
    int sz = 2 + (int)pick(2);
    int a = rreg(), b = rreg();
    switch (pick(8)) {
    case 0:
    case 1:
    case 2:
    case 3: {
        static const char *BT[] = {"bt", "bts", "btr", "btc"};
        const char *op = BT[pick(4)];
        if (chance(50)) {
            asmf("%s%c $%d, %%%s", op, SUF[sz], (int)pick(BITS[sz]), rn(sz, a));
        } else {
            asmf("and%c $%d, %%%s", SUF[sz], BITS[sz] - 1, rn(sz, b));
            fall();
            asmf("%s%c %%%s, %%%s", op, SUF[sz], rn(sz, b), rn(sz, a));
        }
        fset(F_CF, F_ALL); // CF defined; OF/SF/PF undefined
        break;
    }
    case 4:
    case 5: { // bsf/bsr -- source forced non-zero so the destination is defined
        const char *op = pick(2) ? "bsf" : "bsr";
        asmf("or%c $1, %%%s", SUF[sz], rn(sz, a));
        fall();
        asmf("%s%c %%%s, %%%s", op, SUF[sz], rn(sz, a), rn(sz, b));
        fset(F_ZF, F_ALL);
        break;
    }
    case 6:
        asmf("popcnt%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, b));
        fall(); // popcnt: CF=OF=SF=PF=AF=0, ZF from result -- all defined
        break;
    default:
        if (want_bmi) {
            const char *op = pick(2) ? "tzcnt" : "lzcnt";
            asmf("%s%c %%%s, %%%s", op, SUF[sz], rn(sz, a), rn(sz, b));
            fset(F_CF | F_ZF, F_ALL);
        } else {
            asmf("popcnt%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, b));
            fall();
        }
        break;
    }
}

static void gen_movx(void) {
    int a = rreg(), b = rreg();
    char m[64];
    switch (pick(10)) {
    case 0: asmf("movsbl %%%s, %%%s", R8[a], R32[b]); break;
    case 1: asmf("movsbq %%%s, %%%s", R8[a], R64[b]); break;
    case 2: asmf("movswl %%%s, %%%s", R16[a], R32[b]); break;
    case 3: asmf("movswq %%%s, %%%s", R16[a], R64[b]); break;
    case 4: asmf("movslq %%%s, %%%s", R32[a], R64[b]); break;
    case 5: asmf("movzbl %%%s, %%%s", R8[a], R32[b]); break;
    case 6: asmf("movzwl %%%s, %%%s", R16[a], R32[b]); break;
    case 7:
        memop(m, sizeof m, 0, 1);
        asmf("movsbq %s, %%%s", m, R64[b]);
        break;
    case 8:
        memop(m, sizeof m, 0, 2);
        asmf("movzwq %s, %%%s", m, R64[b]);
        break;
    default: {
        int bs = 2 + (int)pick(2);
        asmf("bswap%c %%%s", SUF[bs], rn(bs, a));
    } break;
    }
    // none of the above touch flags
}

static void gen_mov(void) {
    int sz = (int)pick(4);
    int a = rreg(), b = rreg();
    char m[64];
    switch (pick(6)) {
    case 0: asmf("mov%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, b)); break;
    case 1:
        memop(m, sizeof m, 0, BITS[sz] / 8);
        asmf("mov%c %s, %%%s", SUF[sz], m, rn(sz, b));
        break;
    case 2:
        memop(m, sizeof m, 0, BITS[sz] / 8);
        asmf("mov%c %%%s, %s", SUF[sz], rn(sz, a), m);
        break;
    case 3: {
        long long imm = (long long)(int32_t)(uint32_t)nx();
        if (sz == 0)
            imm = (int8_t)imm;
        else if (sz == 1)
            imm = (int16_t)imm;
        asmf("mov%c $%lld, %%%s", SUF[sz], imm, rn(sz, b));
        break;
    }
    case 4: asmf("movabsq $%lld, %%%s", (long long)nx(), R64[b]); break;
    default: {
        int idx = rreg();
        asmf("andl $0x3f, %%%s", R32[idx]);
        fall();
        asmf("leaq %d(%%r15,%%%s,%d), %%%s", (int)pick(64), R64[idx], 1 << pick(4), R64[b]);
        // lea result is a scratch pointer; scrub it back to a value so the dump stays
        // address-independent (r15 is identical under both oracles, but keep it honest).
        asmf("subq %%r15, %%%s", R64[b]);
        fall();
        break;
    }
    }
}

static void gen_bytereg(void) {
    // legacy high-byte registers and narrow writes into wide registers -- the partial-register
    // merge semantics the translator has to model by hand.
    static const char *HI[] = {"ah", "ch", "dh", "bh"};
    static const int HILOW[] = {0, 1, 2, 3};
    int h = (int)pick(4);
    int b = rreg();
    switch (pick(6)) {
    case 0: asmf("movb %%%s, %%%s", HI[h], R8[pick(4)]); break;
    case 1:
        asmf("addb %%%s, %%%s", HI[h], HI[(h + 1) & 3]);
        fall();
        break;
    case 2: {
        static const int NOREX[7] = {0, 1, 2, 3, 5, 6, 7};
        asmf("movzbl %%%s, %%%s", HI[h], R32[NOREX[pick(7)]]);
    } break;
    case 3:
        asmf("xorb $%d, %%%s", (int)pick(256), HI[h]);
        fall();
        break;
    case 4:
        asmf("movw $%d, %%%s", (int)pick(65536), R16[b]); // 16-bit write preserves upper 48
        break;
    default: asmf("movb $%d, %%%s", (int)pick(256), R8[HILOW[h]]); break;
    }
}

static void gen_xchg_cmpxchg(void) {
    int sz = 2 + (int)pick(2);
    int a = rreg(), b = rreg();
    char m[64];
    switch (pick(6)) {
    case 0: asmf("xchg%c %%%s, %%%s", SUF[sz], rn(sz, a), rn(sz, b)); break;
    case 1:
        memop_aligned(m, sizeof m, BITS[sz] / 8);
        asmf("xchg%c %%%s, %s", SUF[sz], rn(sz, a), m);
        break;
    case 2:
        // cmpxchg reg form: compares rax with dest, ZF/CF/... defined like cmp.
        // Pinned to the 64-bit width on purpose: for a 32-bit REGISTER destination the two
        // oracles disagree about the failing-comparison path (the SDM's `DEST <- DEST` write
        // zero-extends bits 63:32 on hardware, qemu-user preserves them). That is an oracle
        // question, not a translator bug, so it is kept out of the corpus.
        asmf("cmpxchgq %%%s, %%%s", R64[a == 0 ? 1 : a], R64[b == 0 ? 3 : b]);
        fall();
        break;
    case 3:
        memop_aligned(m, sizeof m, BITS[sz] / 8);
        asmf("lock cmpxchg%c %%%s, %s", SUF[sz], rn(sz, a == 0 ? 1 : a), m);
        fall();
        break;
    case 4:
        memop_aligned(m, sizeof m, BITS[sz] / 8);
        asmf("lock xadd%c %%%s, %s", SUF[sz], rn(sz, a), m);
        fall();
        break;
    default:
        memop_aligned(m, sizeof m, BITS[sz] / 8);
        asmf("lock add%c %%%s, %s", SUF[sz], rn(sz, a), m);
        fall();
        break;
    }
}

static void gen_consumer(void) {
    const char *cc = usable_cc();
    if (!cc) {
        gen_alu_rr();
        return;
    }
    int a = rreg(), b = rreg();
    char m[64];
    switch (pick(6)) {
    case 0: asmf("set%s %%%s", cc, R8[a]); break;
    case 1:
        memop(m, sizeof m, 0, 1);
        asmf("set%s %s", cc, m);
        break;
    case 2:
    case 3: {
        int sz = 2 + (int)pick(2);
        asmf("cmov%s %%%s, %%%s", cc, rn(sz, a), rn(sz, b));
        break;
    }
    case 4: {
        // forward conditional skip over a short randomized block -- forces the lazy-flag
        // machinery to materialize NZCV at a branch boundary.
        int lbl = label_counter++;
        asmf("j%s .Lfz%d", cc, lbl);
        int n = 1 + (int)pick(3);
        for (int i = 0; i < n; i++) {
            int sz = (int)pick(4);
            asmf("%s%c %%%s, %%%s", pick_alu(), SUF[sz], rn(sz, rreg()), rn(sz, rreg()));
        }
        // Definedness after the join is the intersection of the two paths: the taken path
        // preserves the pre-branch flags, the fallthrough path fully defines them. So the
        // merged definedness is exactly the pre-branch definedness -- leave fdef alone.
        asmf(".Lfz%d:", lbl);
        break;
    }
    default:
        if (fdef & F_CF) {
            int sz = 2 + (int)pick(2);
            asmf("%s%c %%%s, %%%s", pick(2) ? "adc" : "sbb", SUF[sz], rn(sz, a), rn(sz, b));
            fall();
        } else {
            asmf("set%s %%%s", cc, R8[a]);
        }
        break;
    }
}

// ---------------------------------------------------------------- SSE

static int xreg(void) {
    return (int)pick(16);
}

static void gen_sse_int(void) {
    static const char *OPS[] = {
        "paddb",     "paddw",     "paddd",     "paddq",      "psubb",     "psubw",     "psubd",     "psubq",
        "paddsb",    "paddsw",    "paddusb",   "paddusw",    "psubsb",    "psubsw",    "psubusb",   "psubusw",
        "pand",      "pandn",     "por",       "pxor",       "pcmpeqb",   "pcmpeqw",   "pcmpeqd",   "pcmpeqq",
        "pcmpgtb",   "pcmpgtw",   "pcmpgtd",   "pcmpgtq",    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
        "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq", "packsswb",  "packssdw",  "packuswb",  "packusdw",
        "pmullw",    "pmulhw",    "pmulhuw",   "pmuludq",    "pmulld",    "pavgb",     "pavgw",     "pmaxub",
        "pminub",    "pmaxsw",    "pminsw",    "pmaxsb",     "pminsb",    "pmaxsd",    "pminsd",    "pmaxud",
        "pminud",    "pmaxuw",    "pminuw",    "psadbw",     "pshufb",    "pmaddwd",   "pmaddubsw", "phaddw",
        "phaddd",    "phsubw",    "phsubd",    "pmulhrsw",   "psignb",    "psignw",    "psignd",    "pabsb",
        "pabsw",     "pabsd",     "pmovsxbw",  "pmovsxwd",   "pmovsxdq",  "pmovzxbw",  "pmovzxwd",  "pmovzxdq",
        "pmovsxbd",  "pmovzxbd",
    };

    enum { NOPS = (int)(sizeof OPS / sizeof OPS[0]) };

    int a = xreg(), b = xreg();
    const char *op = OPS[pick(NOPS)];
    if (chance(25)) {
        char m[64];
        memop(m, sizeof m, 1, 16);
        asmf("%s %s, %%xmm%d", op, m, b);
    } else {
        asmf("%s %%xmm%d, %%xmm%d", op, a, b);
    }
}

static void gen_sse_shift_shuf(void) {
    int a = xreg(), b = xreg();
    switch (pick(9)) {
    case 0: {
        static const char *S[] = {"psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"};
        asmf("%s $%d, %%xmm%d", S[pick(8)], (int)pick(72), b);
        break;
    }
    case 1: {
        static const char *S[] = {"psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"};
        asmf("%s %%xmm%d, %%xmm%d", S[pick(8)], a, b);
        break;
    }
    case 2: asmf("pslldq $%d, %%xmm%d", (int)pick(20), b); break;
    case 3: asmf("psrldq $%d, %%xmm%d", (int)pick(20), b); break;
    case 4: asmf("pshufd $%d, %%xmm%d, %%xmm%d", (int)pick(256), a, b); break;
    case 5: asmf("pshuflw $%d, %%xmm%d, %%xmm%d", (int)pick(256), a, b); break;
    case 6: asmf("pshufhw $%d, %%xmm%d, %%xmm%d", (int)pick(256), a, b); break;
    case 7: asmf("palignr $%d, %%xmm%d, %%xmm%d", (int)pick(32), a, b); break;
    default: asmf("pblendw $%d, %%xmm%d, %%xmm%d", (int)pick(256), a, b); break;
    }
}

static void gen_sse_fp(void) {
    static const char *OPS[] = {
        "addps",    "addpd",    "addss",    "addsd",    "subps",    "subpd",    "subss",   "subsd",   "mulps",
        "mulpd",    "mulss",    "mulsd",    "divps",    "divpd",    "divss",    "divsd",   "minps",   "minpd",
        "minss",    "minsd",    "maxps",    "maxpd",    "maxss",    "maxsd",    "sqrtps",  "sqrtpd",  "sqrtss",
        "sqrtsd",   "andps",    "andpd",    "andnps",   "andnpd",   "orps",     "orpd",    "xorps",   "xorpd",
        "unpcklps", "unpcklpd", "unpckhps", "unpckhpd", "addsubps", "addsubpd", "haddps",  "haddpd",  "hsubps",
        "hsubpd",   "dpps",     "dppd",     "blendps",  "blendpd",  "roundps",  "roundpd", "roundss", "roundsd",
    };

    enum { NOPS = (int)(sizeof OPS / sizeof OPS[0]) };

    int a = xreg(), b = xreg();
    const char *op = OPS[pick(NOPS)];
    // ops taking an imm8
    if (!strcmp(op, "dpps") || !strcmp(op, "dppd") || !strcmp(op, "blendps") || !strcmp(op, "blendpd") ||
        !strncmp(op, "round", 5)) {
        asmf("%s $%d, %%xmm%d, %%xmm%d", op, (int)pick(16), a, b);
        return;
    }
    asmf("%s %%xmm%d, %%xmm%d", op, a, b);
}

static void gen_sse_misc(void) {
    int a = xreg(), b = xreg();
    int g = rreg();
    char m[64];
    switch (pick(14)) {
    case 0: asmf("cmpps $%d, %%xmm%d, %%xmm%d", (int)pick(8), a, b); break;
    case 1: asmf("cmppd $%d, %%xmm%d, %%xmm%d", (int)pick(8), a, b); break;
    case 2: asmf("cmpss $%d, %%xmm%d, %%xmm%d", (int)pick(8), a, b); break;
    case 3: asmf("cmpsd $%d, %%xmm%d, %%xmm%d", (int)pick(8), a, b); break;
    case 4: asmf("shufps $%d, %%xmm%d, %%xmm%d", (int)pick(256), a, b); break;
    case 5: asmf("shufpd $%d, %%xmm%d, %%xmm%d", (int)pick(4), a, b); break;
    case 6: asmf("pmovmskb %%xmm%d, %%%s", a, R32[g]); break;
    case 7: asmf("movmskps %%xmm%d, %%%s", a, R32[g]); break;
    case 8: asmf("movmskpd %%xmm%d, %%%s", a, R32[g]); break;
    case 9: {
        static const char *C[] = {"cvtdq2ps", "cvtps2dq", "cvttps2dq", "cvtdq2pd", "cvtps2pd",
                                  "cvtpd2ps", "cvtss2sd", "cvtsd2ss",  "cvtpd2dq", "cvttpd2dq"};
        asmf("%s %%xmm%d, %%xmm%d", C[pick(10)], a, b);
        break;
    }
    case 10: asmf("cvtsi2sdq %%%s, %%xmm%d", R64[g], b); break;
    case 11:
        // cvttsd2si on an out-of-range or NaN source yields the architectural "integer
        // indefinite" value; that is defined, so it stays in the comparison.
        asmf("cvttsd2si %%xmm%d, %%%s", a, R64[g]);
        break;
    case 12:
        asmf("%s %%xmm%d, %%xmm%d", pick(2) ? "ucomisd" : "comisd", a, b);
        fall(); // ZF/PF/CF from compare, OF/SF/AF cleared
        break;
    default:
        memop(m, sizeof m, 1, 16);
        asmf("ptest %s, %%xmm%d", m, b);
        fall(); // ZF/CF defined, others cleared
        break;
    }
}

static void gen_sse_mov(void) {
    int a = xreg(), b = xreg();
    int g = rreg();
    char m[64];
    switch (pick(12)) {
    case 0: asmf("movdqa %%xmm%d, %%xmm%d", a, b); break;
    case 1: asmf("movaps %%xmm%d, %%xmm%d", a, b); break;
    case 2:
        memop(m, sizeof m, 1, 16);
        asmf("movdqa %s, %%xmm%d", m, b);
        break;
    case 3:
        memop(m, sizeof m, 1, 16);
        asmf("movdqa %%xmm%d, %s", a, m);
        break;
    case 4:
        memop(m, sizeof m, 0, 1);
        asmf("movdqu %s, %%xmm%d", m, b);
        break;
    case 5:
        memop(m, sizeof m, 0, 1);
        asmf("movdqu %%xmm%d, %s", a, m);
        break;
    case 6: asmf("movd %%%s, %%xmm%d", R32[g], b); break;
    case 7: asmf("movq %%%s, %%xmm%d", R64[g], b); break;
    case 8: asmf("movd %%xmm%d, %%%s", a, R32[g]); break;
    case 9: asmf("movq %%xmm%d, %%%s", a, R64[g]); break;
    case 10:
        memop(m, sizeof m, 0, 8);
        asmf("movhps %s, %%xmm%d", m, b);
        break;
    default:
        memop(m, sizeof m, 0, 8);
        asmf("movlps %%xmm%d, %s", a, m);
        break;
    }
}

static void gen_sse_str(void) {
    int a = xreg(), b = xreg();
    // pcmpistri writes ECX and fully defines CF/ZF/SF/OF (AF=PF=0)
    asmf("pcmpistri $%d, %%xmm%d, %%xmm%d", (int)pick(64), a, b);
    fall();
}

static void gen_x87(void) {
    char m1[64], m2[64];
    memop(m1, sizeof m1, 1, 8);
    memop(m2, sizeof m2, 1, 8);
    static const char *OPS[] = {"faddl", "fsubl", "fmull", "fdivl", "fsubrl", "fdivrl"};
    asmf("fldl %s", m1);
    asmf("fldl %s", m2);
    asmf("%s %s", OPS[pick(6)], m1);
    if (chance(30)) asmf("fsqrt");
    if (chance(30)) asmf("fabs");
    if (chance(20)) asmf("fchs");
    asmf("fstpl %s", m2);
    asmf("fstpl %s", m1);
}

// ---------------------------------------------------------------- program assembly

struct wclass {
    void (*fn)(void);
    int weight;
};

static void gen_alu_rm_load(void) {
    gen_alu_rm(0);
}

static void gen_alu_rm_store(void) {
    gen_alu_rm(1);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <seed> <out.c> [steps] [+x87] [+bmi]\n", argv[0]);
        return 2;
    }
    uint64_t seed = strtoull(argv[1], NULL, 0);
    const char *outpath = argv[2];
    int steps = (argc > 3) ? atoi(argv[3]) : 200;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "+x87")) want_x87 = 1;
        if (!strcmp(argv[i], "+bmi")) want_bmi = 1;
    }
    rng_state = seed * UINT64_C(0x2545F4914F6CDD1D) + UINT64_C(0x9E3779B97F4A7C15);

    struct wclass classes[] = {
        {gen_alu_rr, 12},        {gen_alu_ri, 10},      {gen_alu_rm_load, 6}, {gen_alu_rm_store, 6},
        {gen_unary, 6},          {gen_shift, 11},       {gen_shld_shrd, 3},   {gen_muldiv, 6},
        {gen_muldiv8, 2},        {gen_bitop, 7},        {gen_movx, 6},        {gen_mov, 8},
        {gen_bytereg, 5},        {gen_xchg_cmpxchg, 4}, {gen_consumer, 14},   {gen_sse_int, 12},
        {gen_sse_shift_shuf, 6}, {gen_sse_fp, 10},      {gen_sse_misc, 8},    {gen_sse_mov, 7},
        {gen_sse_str, 2},        {gen_x87, 0},
    };

    enum { NCLASS = (int)(sizeof classes / sizeof classes[0]) };

    if (want_x87) classes[NCLASS - 1].weight = 4;
    int total = 0;
    for (int i = 0; i < NCLASS; i++)
        total += classes[i].weight;

    // ---- header
    char buf[4096];
    raw("// GENERATED by tests/fuzz/isa/x86_64/isafuzz_gen.c -- do not edit by hand.\n");
    snprintf(buf, sizeof buf, "// seed=%llu steps=%d\n", (unsigned long long)seed, steps);
    raw(buf);
    raw("#include <stdio.h>\n#include <stdint.h>\n\n");
    snprintf(buf, sizeof buf, "#define SCRATCH_BYTES %d\n\n", SCRATCH_BYTES);
    raw(buf);
    raw("unsigned char scratch[SCRATCH_BYTES] __attribute__((aligned(64)));\n");
    raw("unsigned long init_gpr[16];\n");
    raw("unsigned char init_xmm[16][16] __attribute__((aligned(16)));\n");
    raw("unsigned long out_gpr[16];\n");
    raw("unsigned char out_xmm[16][16] __attribute__((aligned(16)));\n");
    raw("unsigned long out_flags;\n");
    raw("unsigned int out_mxcsr;\n");
    raw("void fuzz_run(void);\n\n");

    // ---- seeded initial state, emitted as data so the guest reads nothing from outside
    raw("const unsigned long INIT_GPR[16] __attribute__((aligned(16))) = {\n");
    uint64_t initg[16];
    for (int i = 0; i < 16; i++) {
        // mix of dense random values and interesting corner constants
        static const uint64_t CORNERS[] = {
            0,
            1,
            UINT64_C(0xFFFFFFFFFFFFFFFF),
            UINT64_C(0x8000000000000000),
            UINT64_C(0x7FFFFFFFFFFFFFFF),
            UINT64_C(0x80000000),
            UINT64_C(0x7FFFFFFF),
            UINT64_C(0xFFFFFFFF),
            0xFF,
            0x80,
            0x7F,
            UINT64_C(0xFFFF),
            UINT64_C(0x8000),
        };
        initg[i] = chance(35) ? CORNERS[pick(13)] : nx();
        snprintf(buf, sizeof buf, "  0x%016llxUL,\n", (unsigned long long)initg[i]);
        raw(buf);
    }
    raw("};\n\n");

    raw("const unsigned char INIT_XMM[16][16] __attribute__((aligned(16))) = {\n");
    for (int i = 0; i < 16; i++) {
        unsigned char v[16];
        int mode = (int)pick(4);
        for (int j = 0; j < 16; j++)
            v[j] = (unsigned char)pick(256);
        if (mode == 0) {
            // pack with interesting float/double bit patterns (NaNs, infinities, zeros,
            // denormals, and ordinary magnitudes)
            static const uint64_t FPD[] = {
                UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x7FF0000000000000),
                UINT64_C(0xFFF0000000000000), UINT64_C(0x7FF8000000000000), UINT64_C(0xFFF8000000000000),
                UINT64_C(0x7FF0000000000001), UINT64_C(0xFFF0000000000001), UINT64_C(0x0000000000000001),
                UINT64_C(0x000FFFFFFFFFFFFF), UINT64_C(0x3FF0000000000000), UINT64_C(0xBFF0000000000000),
                UINT64_C(0x4059000000000000), UINT64_C(0xC08F400000000000),
            };
            for (int half = 0; half < 2; half++) {
                uint64_t d = FPD[pick(14)];
                for (int k = 0; k < 8; k++)
                    v[half * 8 + k] = (unsigned char)(d >> (8 * k));
            }
        } else if (mode == 1) {
            static const uint32_t FPS[] = {
                0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u,
                0xFF800001u, 0x00000001u, 0x007FFFFFu, 0x3F800000u, 0xBF800000u, 0x42C80000u, 0xC47A0000u,
            };
            for (int q = 0; q < 4; q++) {
                uint32_t f = FPS[pick(14)];
                for (int k = 0; k < 4; k++)
                    v[q * 4 + k] = (unsigned char)(f >> (8 * k));
            }
        } else if (mode == 2) {
            static const unsigned char BYTES[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55, 0xAA};
            for (int j = 0; j < 16; j++)
                v[j] = BYTES[pick(7)];
        }
        raw("  {");
        for (int j = 0; j < 16; j++) {
            snprintf(buf, sizeof buf, "0x%02x%s", v[j], j == 15 ? "" : ",");
            raw(buf);
        }
        raw("},\n");
    }
    raw("};\n\n");

    raw("const unsigned char INIT_SCRATCH[256] = {\n  ");
    for (int i = 0; i < 256; i++) {
        snprintf(buf, sizeof buf, "0x%02x,%s", (unsigned)pick(256), (i % 16 == 15) ? "\n  " : "");
        raw(buf);
    }
    raw("\n};\n\n");

    // ---- the assembly body
    raw("__asm__(\n");
    raw("    \".text\\n\"\n");
    raw("    \".globl fuzz_run\\n\"\n");
    raw("    \".type fuzz_run,@function\\n\"\n");
    raw("    \"fuzz_run:\\n\"\n");
#define ASM1(...)                                                                                                      \
    do {                                                                                                               \
        asmf(__VA_ARGS__);                                                                                             \
        step_flush();                                                                                                  \
    } while (0)
    ASM1("push %%rbx");
    ASM1("push %%rbp");
    ASM1("push %%r12");
    ASM1("push %%r13");
    ASM1("push %%r14");
    ASM1("push %%r15");
    // load the initial architectural state
    for (int i = 0; i < 16; i++)
        ASM1("movdqa INIT_XMM+%d(%%rip), %%xmm%d", i * 16, i);
    ASM1("leaq scratch(%%rip), %%r15");
    for (int i = 0; i < NPOOL; i++)
        ASM1("movq INIT_GPR+%d(%%rip), %%%s", POOL[i] * 8, R64[POOL[i]]);
    {
        // deterministic starting RFLAGS: reserved bit 1 set, IF set, arithmetic bits seeded
        unsigned f = 0x202u | (unsigned)(nx() & 0x8D5u);
        ASM1("pushq $0x%x", f);
        ASM1("popfq");
    }
    fall();

    raw("    \"/* BODY-BEGIN */\\n\"\n");
    for (int i = 0; i < steps; i++) {
        int r = (int)pick((uint32_t)total);
        for (int c = 0; c < NCLASS; c++) {
            if (r < classes[c].weight) {
                classes[c].fn();
                break;
            }
            r -= classes[c].weight;
        }
        step_flush();
    }
    raw("    \"/* BODY-END */\\n\"\n");

    // ---- state capture (mov to memory never disturbs flags, so RAX is banked first)
    ASM1("movq %%rax, out_gpr+0(%%rip)");
    ASM1("pushfq");
    ASM1("popq out_flags(%%rip)");
    for (int i = 0; i < NPOOL; i++) {
        if (POOL[i] == 0) continue;
        ASM1("movq %%%s, out_gpr+%d(%%rip)", R64[POOL[i]], POOL[i] * 8);
    }
    for (int i = 0; i < 16; i++)
        ASM1("movups %%xmm%d, out_xmm+%d(%%rip)", i, i * 16);
    ASM1("stmxcsr out_mxcsr(%%rip)");
    if (want_x87) ASM1("fninit"); // leave the x87 stack clean for the printf that follows
    ASM1("pop %%r15");
    ASM1("pop %%r14");
    ASM1("pop %%r13");
    ASM1("pop %%r12");
    ASM1("pop %%rbp");
    ASM1("pop %%rbx");
    ASM1("ret");
    raw("    \".size fuzz_run,.-fuzz_run\\n\"\n");
    raw(");\n\n");

    // ---- driver
    snprintf(buf, sizeof buf, "#define FLAG_MASK 0x%03xUL\n",
             (unsigned)(((fdef & F_CF) ? 0x001u : 0) | ((fdef & F_PF) ? 0x004u : 0) | ((fdef & F_ZF) ? 0x040u : 0) |
                        ((fdef & F_SF) ? 0x080u : 0) | ((fdef & F_OF) ? 0x800u : 0)));
    raw(buf);
    raw("\n");
    raw("static const char *const GPRN[16] = {\"RAX\",\"RCX\",\"RDX\",\"RBX\",\"RSP\",\"RBP\",\"RSI\",\"RDI\",\n"
        "  \"R8 \",\"R9 \",\"R10\",\"R11\",\"R12\",\"R13\",\"R14\",\"R15\"};\n");
    raw("static const int DUMPED[14] = {0,1,2,3,5,6,7,8,9,10,11,12,13,14};\n\n");
    raw("int main(void) {\n");
    raw("  for (int i = 0; i < 16; i++) init_gpr[i] = INIT_GPR[i];\n");
    raw("  for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) init_xmm[i][j] = INIT_XMM[i][j];\n");
    raw("  for (int i = 0; i < SCRATCH_BYTES; i++) scratch[i] = INIT_SCRATCH[(i * 7 + (i >> 4)) & 255];\n");
    raw("  fuzz_run();\n");
    raw("  for (int k = 0; k < 14; k++) {\n");
    raw("    int i = DUMPED[k];\n");
    raw("    printf(\"%s %016lx\\n\", GPRN[i], out_gpr[i]);\n");
    raw("  }\n");
    raw("  printf(\"FLG %016lx\\n\", out_flags & FLAG_MASK);\n");
    raw("  printf(\"MXC %08x\\n\", out_mxcsr);\n");
    raw("  for (int i = 0; i < 16; i++) {\n");
    raw("    printf(\"XMM%-2d \", i);\n");
    raw("    for (int j = 15; j >= 0; j--) printf(\"%02x\", out_xmm[i][j]);\n");
    raw("    printf(\"\\n\");\n");
    raw("  }\n");
    raw("  { unsigned long h = 1469598103934665603UL;\n");
    raw("    for (int i = 0; i < SCRATCH_BYTES; i++) { h ^= scratch[i]; h *= 1099511628211UL; }\n");
    raw("    printf(\"MEM %016lx\\n\", h); }\n");
    raw("  return 0;\n");
    raw("}\n");

    FILE *f = fopen(outpath, "w");
    if (!f) {
        perror(outpath);
        return 2;
    }
    fwrite(out, 1, out_len, f);
    fclose(f);
    return 0;
}
