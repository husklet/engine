// isafuzz_gen (aarch64) -- deterministic AArch64 instruction-sequence generator for differential
// ISA fuzzing of the engine's aarch64-Linux guest frontend.
//
// Given a seed it emits a self-contained AArch64 C source file whose body is one long
// straight-line randomized instruction sequence written as a single file-scope inline assembly
// block (so the compiler never reorders, allocates, or reinterprets any of it). The body seeds
// every usable general-purpose register and all 32 vector registers from constants baked into
// the source, runs the sequence, and dumps a canonical fixed-width state image: the GPRs, NZCV,
// FPSR, FPCR, all 32 V registers and an FNV-1a checksum of an 8 KiB scratch buffer.
//
// The SAME binary is then executed natively on the ARM64 host (reference) and under
// build/linux-production/hl-engine-linux-aarch64 (under test). Guest ISA == host ISA == same
// silicon, so any difference in the dump is unambiguously an engine bug -- there is no
// emulator-vs-emulator ambiguity and no second implementation's undefined behaviour to argue
// about. This is a strictly stronger oracle than the x86-64 side's qemu.
//
// Determinism: identical seed => byte-identical output file. The guest reads nothing from the
// environment (no time, no cpuid-style probes, no syscalls beyond the final printf) and no
// ADDRESS ever reaches the dump -- the two reserved registers x24/x25 hold every pointer the
// body needs and are neither dumped nor available as operands.
//
// WHERE THE BUGS ARE. The aarch64 frontend is a same-ISA transliterator: it copies most
// instructions verbatim and MANGLES only those naming a register the engine has stolen
// (x16, x17, x18, x28, x30 -- see is_stolen(), src/translator/guest/aarch64/cpu.h). The
// interesting surface is therefore (a) the stolen-register rewrite paths, (b) emit_fold_mem /
// emit_fold_advsimd_struct memory folding, (c) the handful of instructions the engine actually
// lowers (FEAT_I8MM MMLA, FEAT_BF16 BFCVT/BFDOT), (d) exclusives and CASP. The generator biases
// register selection HARD toward the stolen set for exactly this reason.
//
// usage: isafuzz_gen <seed> <out.c> [steps] [feature-flags...]
//        feature flags: +i8mm  +bf16  +dczva  +fpcr  +hot   (all off by default)

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

static uint32_t pick(uint32_t n) { return (uint32_t)(nx() % n); }

static int chance(uint32_t pct) { return pick(100) < pct; }

// ---------------------------------------------------------------- feature flags

static int f_i8mm, f_bf16, f_dczva, f_fpcr, f_reps = 1;

// ---------------------------------------------------------------- memory map of the guest area
//
// One 16 KiB page-aligned object. Only [0,8192) is hashed into the dump, so the save/seed/dump
// regions can never perturb the comparison; every generated memory operand is constructed to
// land inside [0,8192).

#define SCRATCH_BYTES 8192
#define OFF_VSEED 8192  /* 32 * 16 = 512 bytes of vector seed values */
#define OFF_SAVE 8704   /* 16 * 8  callee-saved + platform GPRs      */
#define OFF_FSAVE 8832  /* 8  * 8  d8..d15                           */
#define OFF_DUMPX 8896  /* 31 * 8                                    */
#define OFF_DUMPF 9152  /* nzcv, fpsr, fpcr                          */
#define OFF_DUMPV 9184  /* 32 * 16                                   */
#define AREA_BYTES 16384

// ---------------------------------------------------------------- registers
//
// x25 = scratch base (invariant for the whole body), x24 = writeback/aux base (re-established by
// every step that needs it). Neither is ever an operand of a generated instruction and neither is
// dumped, which is what keeps addresses out of the comparison. x31 (sp) is likewise off-limits.
// EVERYTHING else -- crucially x16, x17, x18, x28 and x30, the registers the engine steals -- is
// a normal member of the pool.

static const int POOL[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                           15, 16, 17, 18, 19, 20, 21, 22, 23, 26, 27, 28, 29, 30};
enum { NPOOL = (int)(sizeof POOL / sizeof POOL[0]) };

// The engine-stolen set. Weighted up because these are the only registers whose lowering is not
// a verbatim copy.
static const int STOLEN[] = {16, 17, 18, 28, 30};
enum { NSTOLEN = 5 };

static int rreg(void) {
    if (chance(38)) return STOLEN[pick(NSTOLEN)];
    return POOL[pick(NPOOL)];
}

// A register distinct from `a` (and from `b`, pass -1 to ignore).
static int rreg2(int a, int b) {
    for (int i = 0; i < 64; i++) {
        int r = rreg();
        if (r != a && r != b) return r;
    }
    return (a == 0 || b == 0) ? 1 : 0;
}

// Even-numbered pool register r such that r+1 is also usable (never 24/25/31): the pair forms
// for CASP and for LDP/STP-style consecutive requirements.
static int rpair(void) {
    static const int P[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 26, 28};
    return P[pick((uint32_t)(sizeof P / sizeof P[0]))];
}

// vector register
static int vreg(void) { return (int)pick(32); }

static int vreg_lim(int lim) { return (int)pick((uint32_t)lim + 1); }

// ---------------------------------------------------------------- output buffer

static char *out;
static size_t out_len, out_cap;

static void raw(const char *s) {
    size_t n = strlen(s);
    if (out_len + n + 1 > out_cap) {
        out_cap = (out_len + n + 1) * 2 + 65536;
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

static void rawf(const char *fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    raw(line);
}

// Instructions are accumulated per generated STEP and flushed as a single source line. That
// granularity is what makes delta-debugging sound: a step's guard instructions (the index clamp
// in front of a scaled memory operand, the `add x24,x25,#off` that establishes a writeback base,
// the branch target of a b.cond skip) are inseparable from the instruction they protect, so the
// minimizer can never delete a guard and turn a real divergence into a wild-memory artifact.
static char step_buf[16384];
static size_t step_len;

static void asmf(const char *fmt, ...) {
    char line[1024];
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

static void step_flush(void) {
    if (!step_len) return;
    rawf("    \"  %s\\n\"\n", step_buf);
    step_len = 0;
    step_buf[0] = 0;
}

// Emit a fixed (non-step) instruction directly as its own line.
static void fixed(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rawf("    \"  %s\\n\"\n", line);
}

static int labelno;

// ---------------------------------------------------------------- operand helpers

static const char *X(int r) {
    static char b[8][8];
    static int i;
    i = (i + 1) & 7;
    if (r == 31)
        snprintf(b[i], 8, "xzr");
    else
        snprintf(b[i], 8, "x%d", r);
    return b[i];
}

static const char *W(int r) {
    static char b[8][8];
    static int i;
    i = (i + 1) & 7;
    if (r == 31)
        snprintf(b[i], 8, "wzr");
    else
        snprintf(b[i], 8, "w%d", r);
    return b[i];
}

// sf: 1 = 64-bit, 0 = 32-bit
static const char *R(int sf, int r) { return sf ? X(r) : W(r); }

static const char *CONDS[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};

// "al"/"nv" are useless for csel-family (they degenerate) but perfectly legal; keep them out of
// the conditional-select pool so every emitted csel actually reads NZCV.
static const char *rcond(void) { return CONDS[pick(14)]; }

// Valid AArch64 logical (bitmask) immediates. Curated so the assembler always accepts them and
// so the set covers single bits, runs, and the classic alternating patterns.
static const uint64_t LIMM64[] = {
    0x1u,
    0x3u,
    0x7u,
    0xfu,
    0xffu,
    0xffffu,
    0xffffffffu,
    0x7fffffffffffffffULL,
    0x8000000000000000ULL,
    0xfffffffffffffffeULL,
    0x5555555555555555ULL,
    0xaaaaaaaaaaaaaaaaULL,
    0x3333333333333333ULL,
    0xf0f0f0f0f0f0f0f0ULL,
    0x0f0f0f0f0f0f0f0fULL,
    0xff00ff00ff00ff00ULL,
    0x00000000ffff0000ULL,
    0xffffffff00000000ULL,
    0x000f000f000f000fULL,
    0x3f3f3f3f3f3f3f3fULL,
};
static const uint32_t LIMM32[] = {
    0x1u,        0x3u,        0x7u,        0xfu,        0xffu,       0xffffu,     0x7fffffffu,
    0x80000000u, 0xfffffffeu, 0x55555555u, 0xaaaaaaaau, 0x33333333u, 0xf0f0f0f0u, 0x0f0f0f0fu,
    0xff00ff00u, 0x000f000fu, 0x3f3f3f3fu, 0xfff00000u, 0x0000ff00u, 0xc0000003u,
};

static void limm(char *buf, size_t n, int sf) {
    if (sf)
        snprintf(buf, n, "#0x%llx", (unsigned long long)LIMM64[pick((uint32_t)(sizeof LIMM64 / sizeof LIMM64[0]))]);
    else
        snprintf(buf, n, "#0x%x", LIMM32[pick((uint32_t)(sizeof LIMM32 / sizeof LIMM32[0]))]);
}

// Shifted-register operand suffix, e.g. ", lsl #7". Empty ~40% of the time.
static void shiftsfx(char *buf, size_t n, int sf, int allow_ror) {
    if (chance(40)) {
        buf[0] = 0;
        return;
    }
    static const char *S[4] = {"lsl", "lsr", "asr", "ror"};
    int k = (int)pick(allow_ror ? 4 : 3);
    snprintf(buf, n, ", %s #%u", S[k], pick(sf ? 64 : 32));
}

// Extended-register operand suffix, e.g. ", sxth #3". The register named with it is w for
// uxtb/uxth/uxtw/sxtb/sxth/sxtw and x for uxtx/sxtx -- returns 1 when the operand must be an X.
static int extsfx(char *buf, size_t n, int sf) {
    static const char *E[8] = {"uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};
    int k = (int)pick(sf ? 8 : 7);
    if (!sf && (k == 3 || k == 7)) k = 2;
    unsigned amt = pick(5);
    if (amt)
        snprintf(buf, n, "%s #%u", E[k], amt);
    else
        snprintf(buf, n, "%s", E[k]);
    return sf && (k == 3 || k == 7);
}

// ---------------------------------------------------------------- memory operand construction
//
// Every access must land inside [0, SCRATCH_BYTES). Three shapes:
//   base+imm      : x25 + a bounded, correctly-scaled displacement
//   base+reg      : x25 + a pool register CLAMPED to 0..63 by an inseparable `and` in the same
//                   step, optionally with an extend and the natural scale
//   writeback     : x24 established from x25 in the same step, then pre/post-index

// scaled unsigned-immediate displacement for an access of `width` bytes
static int disp_scaled(int width) {
    int slots = (SCRATCH_BYTES - 64) / width;
    if (slots > 4096) slots = 4096;
    return (int)pick((uint32_t)slots) * width;
}

// Build "[x25, #d]" style operand; returns via buf.
static void mem_imm(char *buf, size_t n, int width) {
    snprintf(buf, n, "[x25, #%d]", disp_scaled(width));
}

// Build a register-offset operand, emitting the clamp into the current step first.
static void mem_reg(char *buf, size_t n, int width, int avoid) {
    int idx = rreg2(avoid, -1);
    asmf("and %s, %s, #0x3f", X(idx), X(idx));
    int lg = width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : width == 8 ? 3 : 4;
    int form = (int)pick(4);
    if (form == 0)
        snprintf(buf, n, "[x25, %s]", X(idx)); /* implicit lsl #0 */
    else if (form == 1 && lg)
        snprintf(buf, n, "[x25, %s, lsl #%d]", X(idx), lg);
    else if (form == 2)
        snprintf(buf, n, "[x25, %s, uxtw%s]", W(idx), lg ? (lg == 1 ? " #1" : lg == 2 ? " #2" : lg == 3 ? " #3" : " #4") : "");
    else
        snprintf(buf, n, "[x25, %s, sxtw%s]", W(idx), lg ? (lg == 1 ? " #1" : lg == 2 ? " #2" : lg == 3 ? " #3" : " #4") : "");
}

// Build a pre/post-index operand on x24, emitting the base setup into the current step first.
// Base is placed at 2048 so a signed simm9 writeback can never leave the scratch region.
static void mem_wb(char *buf, size_t n, int width, int *is_pre) {
    (void)width;
    asmf("add x24, x25, #%d", 2048 + (int)pick(64) * 16);
    int imm = (int)pick(129) - 64;
    *is_pre = chance(50);
    if (*is_pre)
        snprintf(buf, n, "[x24, #%d]!", imm);
    else
        snprintf(buf, n, "[x24], #%d", imm);
}

// A naturally-aligned base+imm operand for atomics / exclusives (these fault when unaligned).
static void mem_aligned(char *buf, size_t n, int width) {
    snprintf(buf, n, "[x25, #%d]", (int)pick((uint32_t)((SCRATCH_BYTES - 64) / width)) * width);
}

// x24 pointing at a naturally-aligned scratch slot (atomics take no displacement).
static void base_aligned(int width) {
    asmf("add x24, x25, #%d", (int)pick((uint32_t)(4032 / width)) * width);
}

// ---------------------------------------------------------------- vector arrangement helpers

static const char *ARR_INT[6] = {"8b", "16b", "4h", "8h", "2s", "4s"};
static const char *ARR_INTQ[3] = {"16b", "8h", "4s"};      /* full-width only */
static const char *ARR_ALL[7] = {"8b", "16b", "4h", "8h", "2s", "4s", "2d"};
static const char *ARR_FP[3] = {"2s", "4s", "2d"};

static const char *arr_int(void) { return ARR_INT[pick(6)]; }
static const char *arr_all(void) { return ARR_ALL[pick(7)]; }
static const char *arr_fp(void) { return ARR_FP[pick(3)]; }

// element size in bits of an arrangement string
static int arr_esize(const char *a) {
    char c = a[strlen(a) - 1];
    return c == 'b' ? 8 : c == 'h' ? 16 : c == 's' ? 32 : 64;
}
static int arr_lanes(const char *a) {
    int n = atoi(a);
    return n;
}

// ---------------------------------------------------------------- instruction classes

static void gen_addsub(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg(), m = rreg();
    const char *op;
    int form = (int)pick(4);
    int S = chance(45);
    switch (pick(4)) {
    case 0: op = S ? "adds" : "add"; break;
    case 1: op = S ? "subs" : "sub"; break;
    case 2: op = S ? "adds" : "add"; break;
    default: op = S ? "subs" : "sub"; break;
    }
    if (form == 0) { /* immediate, with optional lsl #12 */
        unsigned imm = pick(4096);
        asmf("%s %s, %s, #%u%s", op, R(sf, d), R(sf, n1), imm, chance(25) ? ", lsl #12" : "");
    } else if (form == 1) { /* shifted register */
        char sh[32];
        shiftsfx(sh, sizeof sh, sf, 0);
        asmf("%s %s, %s, %s%s", op, R(sf, d), R(sf, n1), R(sf, m), sh);
    } else if (form == 2) { /* extended register */
        char ex[32];
        int need_x = extsfx(ex, sizeof ex, sf);
        asmf("%s %s, %s, %s, %s", op, R(sf, d), R(sf, n1), need_x ? X(m) : W(m), ex);
    } else { /* negate / compare aliases (Rd or Rn = zr) */
        if (chance(50))
            asmf("%s %s, %s, %s", S ? "subs" : "sub", R(sf, d), R(sf, 31), R(sf, m));
        else
            asmf("%s %s, %s, %s", S ? "adds" : "add", R(sf, 31), R(sf, n1), R(sf, m));
    }
}

static void gen_adcsbc(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg(), m = rreg();
    static const char *OPS[4] = {"adc", "adcs", "sbc", "sbcs"};
    asmf("%s %s, %s, %s", OPS[pick(4)], R(sf, d), R(sf, n1), R(sf, m));
}

static void gen_logic(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg(), m = rreg();
    static const char *OPS[8] = {"and", "orr", "eor", "bic", "orn", "eon", "ands", "bics"};
    const char *op = OPS[pick(8)];
    if (chance(35) && (!strcmp(op, "and") || !strcmp(op, "orr") || !strcmp(op, "eor") || !strcmp(op, "ands"))) {
        char b[32];
        limm(b, sizeof b, sf);
        asmf("%s %s, %s, %s", op, R(sf, d), R(sf, n1), b);
        return;
    }
    char sh[32];
    shiftsfx(sh, sizeof sh, sf, 1);
    asmf("%s %s, %s, %s%s", op, R(sf, d), R(sf, n1), R(sf, m), sh);
}

static void gen_movwide(void) {
    int sf = (int)pick(2);
    int d = rreg();
    static const char *OPS[3] = {"movz", "movn", "movk"};
    int k = (int)pick(3);
    unsigned sh = pick(sf ? 4u : 2u) * 16u;
    asmf("%s %s, #0x%x, lsl #%u", OPS[k], R(sf, d), (unsigned)pick(65536), sh);
}

static void gen_bitfield(void) {
    int sf = (int)pick(2);
    int bits = sf ? 64 : 32;
    int d = rreg(), n1 = rreg();
    int lsb = (int)pick((uint32_t)bits);
    int width = 1 + (int)pick((uint32_t)(bits - lsb));
    switch (pick(8)) {
    case 0: asmf("ubfx %s, %s, #%d, #%d", R(sf, d), R(sf, n1), lsb, width); break;
    case 1: asmf("sbfx %s, %s, #%d, #%d", R(sf, d), R(sf, n1), lsb, width); break;
    case 2: asmf("bfi %s, %s, #%d, #%d", R(sf, d), R(sf, n1), lsb, width); break;
    case 3: asmf("bfxil %s, %s, #%d, #%d", R(sf, d), R(sf, n1), lsb, width); break;
    case 4: asmf("ubfiz %s, %s, #%d, #%d", R(sf, d), R(sf, n1), lsb, width); break;
    case 5: asmf("sbfiz %s, %s, #%d, #%d", R(sf, d), R(sf, n1), lsb, width); break;
    case 6: {
        static const char *E[6] = {"sxtb", "sxth", "uxtb", "uxth", "sxtw", "sxtw"};
        int k = (int)pick(sf ? 6 : 4);
        if (k >= 4)
            asmf("sxtw %s, %s", X(d), W(n1));
        else
            asmf("%s %s, %s", E[k], R(sf, d), W(n1));
        break;
    }
    default: {
        static const char *S[3] = {"lsl", "lsr", "asr"};
        asmf("%s %s, %s, #%d", S[pick(3)], R(sf, d), R(sf, n1), lsb);
        break;
    }
    }
}

static void gen_extr(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg(), m = rreg();
    int lsb = (int)pick((uint32_t)(sf ? 64 : 32));
    if (chance(30))
        asmf("ror %s, %s, #%d", R(sf, d), R(sf, n1), lsb);
    else
        asmf("extr %s, %s, %s, #%d", R(sf, d), R(sf, n1), R(sf, m), lsb);
}

static void gen_dp1(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg();
    static const char *OPS[6] = {"rbit", "rev16", "rev", "clz", "cls", "rev32"};
    int k = (int)pick(6);
    if (k == 5 && !sf) k = 2; /* rev32 is 64-bit only */
    asmf("%s %s, %s", OPS[k], R(sf, d), R(sf, n1));
}

static void gen_dp2(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg(), m = rreg();
    static const char *OPS[6] = {"udiv", "sdiv", "lslv", "lsrv", "asrv", "rorv"};
    asmf("%s %s, %s, %s", OPS[pick(6)], R(sf, d), R(sf, n1), R(sf, m));
}

static void gen_dp3(void) {
    int d = rreg(), n1 = rreg(), m = rreg(), a = rreg();
    switch (pick(8)) {
    case 0: asmf("madd %s, %s, %s, %s", X(d), X(n1), X(m), X(a)); break;
    case 1: asmf("msub %s, %s, %s, %s", X(d), X(n1), X(m), X(a)); break;
    case 2: asmf("madd %s, %s, %s, %s", W(d), W(n1), W(m), W(a)); break;
    case 3: asmf("msub %s, %s, %s, %s", W(d), W(n1), W(m), W(a)); break;
    case 4: asmf("smulh %s, %s, %s", X(d), X(n1), X(m)); break;
    case 5: asmf("umulh %s, %s, %s", X(d), X(n1), X(m)); break;
    case 6: {
        static const char *L[4] = {"smaddl", "umaddl", "smsubl", "umsubl"};
        asmf("%s %s, %s, %s, %s", L[pick(4)], X(d), W(n1), W(m), X(a));
        break;
    }
    default: {
        static const char *L[2] = {"smull", "umull"};
        asmf("%s %s, %s, %s", L[pick(2)], X(d), W(n1), W(m));
        break;
    }
    }
}

static void gen_ccmp(void) {
    int sf = (int)pick(2);
    int n1 = rreg(), m = rreg();
    const char *op = chance(50) ? "ccmp" : "ccmn";
    unsigned nzcv = pick(16);
    if (chance(50))
        asmf("%s %s, #%u, #%u, %s", op, R(sf, n1), pick(32), nzcv, rcond());
    else
        asmf("%s %s, %s, #%u, %s", op, R(sf, n1), R(sf, m), nzcv, rcond());
}

static void gen_csel(void) {
    int sf = (int)pick(2);
    int d = rreg(), n1 = rreg(), m = rreg();
    switch (pick(8)) {
    case 0: asmf("csel %s, %s, %s, %s", R(sf, d), R(sf, n1), R(sf, m), rcond()); break;
    case 1: asmf("csinc %s, %s, %s, %s", R(sf, d), R(sf, n1), R(sf, m), rcond()); break;
    case 2: asmf("csinv %s, %s, %s, %s", R(sf, d), R(sf, n1), R(sf, m), rcond()); break;
    case 3: asmf("csneg %s, %s, %s, %s", R(sf, d), R(sf, n1), R(sf, m), rcond()); break;
    case 4: asmf("cset %s, %s", R(sf, d), rcond()); break;
    case 5: asmf("csetm %s, %s", R(sf, d), rcond()); break;
    case 6: asmf("cinc %s, %s, %s", R(sf, d), R(sf, n1), rcond()); break;
    default: asmf("cneg %s, %s, %s", R(sf, d), R(sf, n1), rcond()); break;
    }
}

// A conditional branch skipping one or two instructions. The whole construct -- branch, skipped
// body and label -- lives in ONE step so the minimizer can never separate them.
static void gen_branch(void) {
    int lab = ++labelno;
    int sf = (int)pick(2);
    int t = rreg(), d = rreg(), n1 = rreg(), m = rreg();
    switch (pick(4)) {
    case 0: asmf("b.%s .Lfz%d", rcond(), lab); break;
    case 1: asmf("cbz %s, .Lfz%d", R(sf, t), lab); break;
    case 2: asmf("cbnz %s, .Lfz%d", R(sf, t), lab); break;
    default:
        asmf("%s %s, #%u, .Lfz%d", chance(50) ? "tbz" : "tbnz", R(sf, t), pick(sf ? 64 : 32), lab);
        break;
    }
    asmf("eor %s, %s, %s", R(sf, d), R(sf, n1), R(sf, m));
    if (chance(50)) asmf("adds %s, %s, %s", R(sf, d), R(sf, d), R(sf, m));
    asmf(".Lfz%d:", lab);
}

// adr/adrp reach the dump only as a DIFFERENCE of two program-counter-relative values, which is a
// link-time constant. The absolute value is never dumped, so the load bias a non-PIE image would
// introduce cannot produce a false divergence -- while the adr/adrp write path (which the frontend
// special-cases for stolen destinations) is still fully exercised.
static void gen_adr(void) {
    int a = rreg(), b = rreg2(a, -1);
    int lab = ++labelno;
    if (chance(50)) {
        asmf("adr %s, .Lfz%d", X(a), lab);
        asmf("adr %s, .Lfz%d + 8", X(b), lab);
    } else {
        asmf("adrp %s, .Lfz%d", X(a), lab);
        asmf("adrp %s, .Lfz%d + 4096", X(b), lab);
    }
    asmf(".Lfz%d:", lab);
    asmf("sub %s, %s, %s", X(a), X(b), X(a));
    asmf("mov %s, #0", X(b));
}

static void gen_load(void) {
    int sf = (int)pick(2);
    int t = rreg();
    static const struct {
        const char *mn;
        int width;
        int is64; /* 0 = W dest, 1 = X dest, 2 = W or X (sign-extending) */
    } L[] = {
        {"ldrb", 1, 0}, {"ldrh", 2, 0}, {"ldr", 4, 0}, {"ldr", 8, 1},
        {"ldrsb", 1, 2}, {"ldrsh", 2, 2}, {"ldrsw", 4, 1},
    };
    int k = (int)pick(7);
    int width = L[k].width;
    int is_x = L[k].is64 == 1 || (L[k].is64 == 2 && sf);
    char op[64];
    switch (pick(4)) {
    case 0: mem_imm(op, sizeof op, width); break;
    case 1: mem_reg(op, sizeof op, width, t); break;
    case 2: {
        int pre;
        mem_wb(op, sizeof op, width, &pre);
        break;
    }
    default:
        asmf("ldur%s %s, [x25, #%d]", L[k].mn + 3, is_x ? X(t) : W(t), (int)pick(256));
        return;
    }
    asmf("%s %s, %s", L[k].mn, is_x ? X(t) : W(t), op);
}

static void gen_store(void) {
    int t = rreg();
    static const struct {
        const char *mn;
        int width;
        int is64;
    } S[] = {{"strb", 1, 0}, {"strh", 2, 0}, {"str", 4, 0}, {"str", 8, 1}};
    int k = (int)pick(4);
    int width = S[k].width;
    char op[64];
    switch (pick(4)) {
    case 0: mem_imm(op, sizeof op, width); break;
    case 1: mem_reg(op, sizeof op, width, t); break;
    case 2: {
        int pre;
        mem_wb(op, sizeof op, width, &pre);
        break;
    }
    default:
        asmf("stur%s %s, [x25, #%d]", S[k].mn + 3, S[k].is64 ? X(t) : W(t), (int)pick(256));
        return;
    }
    asmf("%s %s, %s", S[k].mn, S[k].is64 ? X(t) : W(t), op);
}

static void gen_pair(void) {
    int a = rreg(), b = rreg2(a, -1);
    int sf = (int)pick(2);
    int load = chance(55);
    int sw = load && sf && chance(25); /* ldpsw: X destinations, WORD scaling */
    int width = sw ? 4 : (sf ? 8 : 4);
    char op[64];
    int shape = (int)pick(3);
    if (shape == 0) {
        snprintf(op, sizeof op, "[x25, #%d]", (int)pick(60) * width);
    } else {
        asmf("add x24, x25, #%d", 2048 + (int)pick(64) * 16);
        int imm = ((int)pick(33) - 16) * width;
        snprintf(op, sizeof op, shape == 1 ? "[x24, #%d]!" : "[x24], #%d", imm);
    }
    if (sw)
        asmf("ldpsw %s, %s, %s", X(a), X(b), op);
    else if (load)
        asmf("ldp %s, %s, %s", R(sf, a), R(sf, b), op);
    else
        asmf("stp %s, %s, %s", R(sf, a), R(sf, b), op);
}

// LSE atomic read-modify-writes and CAS, in every acquire/release flavour.
static void gen_atomic(void) {
    int sf = (int)pick(2);
    int width = sf ? 8 : 4;
    static const char *RMW[8] = {"ldadd", "ldclr", "ldeor",  "ldset",
                                 "ldsmax", "ldsmin", "ldumax", "ldumin"};
    static const char *ORD[4] = {"", "a", "l", "al"};
    const char *ord = ORD[pick(4)];
    int s = rreg(), t = rreg2(s, -1);
    switch (pick(4)) {
    case 0:
        base_aligned(width);
        asmf("%s%s %s, %s, [x24]", RMW[pick(8)], ord, R(sf, s), R(sf, t));
        break;
    case 1:
        base_aligned(width);
        asmf("swp%s %s, %s, [x24]", ord, R(sf, s), R(sf, t));
        break;
    case 2:
        base_aligned(width);
        asmf("cas%s %s, %s, [x24]", ord, R(sf, s), R(sf, t));
        break;
    default: {
        /* CASP: consecutive even-numbered pairs, and the two pairs must be distinct. */
        int p = rpair(), q = rpair();
        for (int i = 0; i < 16 && q == p; i++) q = rpair();
        if (q == p) q = (p == 0) ? 2 : 0;
        base_aligned(16);
        asmf("casp%s %s, %s, %s, %s, [x24]", ord, R(sf, p), R(sf, p + 1), R(sf, q), R(sf, q + 1));
        break;
    }
    }
}

// Bounded exclusive-monitor sequences. The store-exclusive status register is architecturally allowed
// to report a SPURIOUS failure, so the retry loop is what makes the final state deterministic: the loop
// only exits with the status == 0 and the memory holding the computed value.
//
// The shapes here are deliberately the exact idioms try_lse_atomic() pattern-matches (translate.c): the
// swap loop, the five register RMW loops (add/orr/eor/and/sub), the add-immediate loop and the CAS loop,
// plus near-misses that must NOT be upgraded. That matcher is the densest hand-written code in the
// frontend, and a mismatch between the loop it replaces and the single LSE op it emits is invisible to
// any whole-program fixture.
static void gen_exclusive(void) {
    int lab = ++labelno;
    int sf = (int)pick(2);
    int width = sf ? 8 : 4;
    int t = rreg(), s = rreg2(t, -1), v = rreg2(t, s), s2 = rreg2(t, s);
    static const char *LD[2] = {"ldxr", "ldaxr"};
    static const char *ST[2] = {"stxr", "stlxr"};
    int k = (int)pick(2);
    int shape = (int)pick(8);
    base_aligned(shape == 7 ? 16 : width);
    if (shape == 7) {
        /* exclusive PAIR (ldxp/stxp): never an LSE-upgrade candidate */
        int a = rpair(), b = rpair();
        for (int i = 0; i < 16 && b == a; i++) b = rpair();
        if (b == a) b = (a == 0) ? 2 : 0;
        while (s == b || s == b + 1) s = rreg2(t, -1);
        asmf(".Lfz%d:", lab);
        asmf("%s %s, %s, [x24]", k ? "ldaxp" : "ldxp", R(sf, a), R(sf, a + 1));
        asmf("%s %s, %s, %s, [x24]", k ? "stlxp" : "stxp", W(s), R(sf, b), R(sf, b + 1));
        asmf("cbnz %s, .Lfz%d", W(s), lab);
        return;
    }
    if (shape == 6) {
        /* CAS loop: ldxr Wt; cmp Wt,Wexp; b.ne out; stxr Ws,Wnew; cbnz Ws,loop; out: */
        int out = ++labelno;
        asmf(".Lfz%d:", lab);
        asmf("%s %s, [x24]", LD[k], R(sf, t));
        asmf("cmp %s, %s", R(sf, t), R(sf, v));
        asmf("b.ne .Lfz%d", out);
        asmf("%s %s, %s, [x24]", ST[k], W(s), R(sf, s2));
        asmf("cbnz %s, .Lfz%d", W(s), lab);
        asmf(".Lfz%d:", out);
        return;
    }
    if (shape == 5) {
        /* swap loop */
        asmf(".Lfz%d:", lab);
        asmf("%s %s, [x24]", LD[k], R(sf, t));
        asmf("%s %s, %s, [x24]", ST[k], W(s), R(sf, v));
        asmf("cbnz %s, .Lfz%d", W(s), lab);
        return;
    }
    if (shape == 4) {
        /* fetch-add of a small constant */
        asmf(".Lfz%d:", lab);
        asmf("%s %s, [x24]", LD[k], R(sf, t));
        asmf("add %s, %s, #%u", R(sf, s2), R(sf, t), pick(4096));
        asmf("%s %s, %s, [x24]", ST[k], W(s), R(sf, s2));
        asmf("cbnz %s, .Lfz%d", W(s), lab);
        return;
    }
    /* register RMW: add / orr / eor / and / sub. The operand order is randomised because the matcher
       accepts either for the commutative ops and only Rn==Wt for sub. */
    static const char *RMW[5] = {"add", "orr", "eor", "and", "sub"};
    int op = shape;
    int swapped = op != 4 && chance(40);
    asmf(".Lfz%d:", lab);
    asmf("%s %s, [x24]", LD[k], R(sf, t));
    if (swapped)
        asmf("%s %s, %s, %s", RMW[op], R(sf, s2), R(sf, v), R(sf, t));
    else
        asmf("%s %s, %s, %s", RMW[op], R(sf, s2), R(sf, t), R(sf, v));
    asmf("%s %s, %s, [x24]", ST[k], W(s), R(sf, s2));
    asmf("cbnz %s, .Lfz%d", W(s), lab);
}

// Load-acquire / store-release and the RCpc forms.
static void gen_acqrel(void) {
    int sf = (int)pick(2);
    int width = sf ? 8 : 4;
    int t = rreg();
    base_aligned(width);
    static const char *LD[3] = {"ldar", "ldapr", "ldar"};
    if (chance(50))
        asmf("%s %s, [x24]", LD[pick(3)], R(sf, t));
    else
        asmf("stlr %s, [x24]", R(sf, t));
}

// ---------------------------------------------------------------- SIMD

static void gen_simd_int3(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    static const char *OPS[24] = {"add",  "sub",  "mul",  "mla",  "mls",  "sqadd", "uqadd", "sqsub",
                                  "uqsub", "shadd", "uhadd", "srhadd", "urhadd", "shsub", "uhsub",
                                  "smax", "smin", "umax", "umin", "sabd", "uabd", "saba", "uaba",
                                  "sqdmulh"};
    int k = (int)pick(24);
    const char *op = OPS[k];
    /* no 2d form for any of these (arr_int excludes it); sqdmulh additionally has no byte form */
    const char *a = (k == 23) ? ARR_INT[2 + pick(4)] : arr_int();
    asmf("%s v%d.%s, v%d.%s, v%d.%s", op, d, a, n1, a, m, a);
}

static void gen_simd_logic(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    static const char *OPS[7] = {"and", "orr", "eor", "bic", "orn", "bsl", "bit"};
    const char *op = OPS[pick(7)];
    const char *a = chance(50) ? "8b" : "16b";
    if (chance(15)) {
        asmf("bif v%d.%s, v%d.%s, v%d.%s", d, a, n1, a, m, a);
        return;
    }
    asmf("%s v%d.%s, v%d.%s, v%d.%s", op, d, a, n1, a, m, a);
}

static void gen_simd_cmp(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    static const char *OPS[6] = {"cmeq", "cmge", "cmgt", "cmhi", "cmhs", "cmtst"};
    const char *a = arr_all();
    if (chance(35)) {
        static const char *Z[5] = {"cmeq", "cmge", "cmgt", "cmle", "cmlt"};
        asmf("%s v%d.%s, v%d.%s, #0", Z[pick(5)], d, a, n1, a);
        return;
    }
    asmf("%s v%d.%s, v%d.%s, v%d.%s", OPS[pick(6)], d, a, n1, a, m, a);
}

static void gen_simd_shift(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    const char *a = arr_all();
    int es = arr_esize(a);
    switch (pick(6)) {
    case 0: asmf("shl v%d.%s, v%d.%s, #%u", d, a, n1, a, pick((uint32_t)es)); break;
    case 1: asmf("%s v%d.%s, v%d.%s, #%u", chance(50) ? "sshr" : "ushr", d, a, n1, a, 1 + pick((uint32_t)es - 1)); break;
    case 2: asmf("%s v%d.%s, v%d.%s, #%u", chance(50) ? "srshr" : "urshr", d, a, n1, a, 1 + pick((uint32_t)es - 1)); break;
    case 3:
        if (chance(50))
            asmf("sri v%d.%s, v%d.%s, #%u", d, a, n1, a, 1 + pick((uint32_t)es - 1));
        else
            asmf("sli v%d.%s, v%d.%s, #%u", d, a, n1, a, pick((uint32_t)es));
        break;
    case 4: {
        static const char *V[4] = {"sshl", "ushl", "sqshl", "uqshl"};
        asmf("%s v%d.%s, v%d.%s, v%d.%s", V[pick(4)], d, a, n1, a, m, a);
        break;
    }
    default: {
        static const char *V[3] = {"sqshl", "uqshl", "sqshlu"};
        asmf("%s v%d.%s, v%d.%s, #%u", V[pick(3)], d, a, n1, a, pick((uint32_t)es));
        break;
    }
    }
}

static void gen_simd_widen(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    static const char *NARROW[3] = {"8b", "4h", "2s"};
    static const char *WIDE[3] = {"8h", "4s", "2d"};
    int k = (int)pick(3);
    const char *nn = NARROW[k], *ww = WIDE[k];
    switch (pick(6)) {
    case 0: {
        static const char *L[6] = {"saddl", "uaddl", "ssubl", "usubl", "smull", "umull"};
        asmf("%s v%d.%s, v%d.%s, v%d.%s", L[pick(6)], d, ww, n1, nn, m, nn);
        break;
    }
    case 1: {
        static const char *L[4] = {"saddw", "uaddw", "ssubw", "usubw"};
        asmf("%s v%d.%s, v%d.%s, v%d.%s", L[pick(4)], d, ww, n1, ww, m, nn);
        break;
    }
    case 2: asmf("%s v%d.%s, v%d.%s", chance(50) ? "sxtl" : "uxtl", d, ww, n1, nn); break;
    case 3: asmf("xtn v%d.%s, v%d.%s", d, nn, n1, ww); break;
    case 4: {
        static const char *L[3] = {"sqxtn", "uqxtn", "sqxtun"};
        asmf("%s v%d.%s, v%d.%s", L[pick(3)], d, nn, n1, ww);
        break;
    }
    default: {
        static const char *L[4] = {"addhn", "raddhn", "subhn", "rsubhn"};
        asmf("%s v%d.%s, v%d.%s, v%d.%s", L[pick(4)], d, nn, n1, ww, m, ww);
        break;
    }
    }
}

static void gen_simd_pair_red(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    switch (pick(4)) {
    case 0: {
        static const char *P[6] = {"addp", "smaxp", "sminp", "umaxp", "uminp", "addp"};
        const char *a = arr_int();
        asmf("%s v%d.%s, v%d.%s, v%d.%s", P[pick(6)], d, a, n1, a, m, a);
        break;
    }
    case 1: {
        static const char *P[4] = {"saddlp", "uaddlp", "sadalp", "uadalp"};
        static const char *NARROW[3] = {"16b", "8h", "4s"};
        static const char *WIDE[3] = {"8h", "4s", "2d"};
        int k = (int)pick(3);
        asmf("%s v%d.%s, v%d.%s", P[pick(4)], d, WIDE[k], n1, NARROW[k]);
        break;
    }
    case 2: {
        static const char *V[5] = {"addv", "smaxv", "sminv", "umaxv", "uminv"};
        static const char *A[3] = {"16b", "8h", "4s"};
        static const char *S[3] = {"b", "h", "s"};
        int k = (int)pick(3);
        asmf("%s %s%d, v%d.%s", V[pick(5)], S[k], d, n1, A[k]);
        break;
    }
    default: {
        static const char *A[3] = {"16b", "8h", "4s"};
        static const char *S[3] = {"h", "s", "d"};
        int k = (int)pick(3);
        asmf("%s %s%d, v%d.%s", chance(50) ? "saddlv" : "uaddlv", S[k], d, n1, A[k]);
        break;
    }
    }
}

static void gen_simd_perm(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    switch (pick(5)) {
    case 0: {
        static const char *P[6] = {"zip1", "zip2", "uzp1", "uzp2", "trn1", "trn2"};
        const char *a = arr_all();
        asmf("%s v%d.%s, v%d.%s, v%d.%s", P[pick(6)], d, a, n1, a, m, a);
        break;
    }
    case 1: {
        int q = chance(60);
        const char *a = q ? "16b" : "8b";
        asmf("ext v%d.%s, v%d.%s, v%d.%s, #%u", d, a, n1, a, m, a, pick(q ? 16u : 8u));
        break;
    }
    case 2: {
        /* TBL/TBX with a 1..4 register consecutive table */
        int len = 1 + (int)pick(4);
        int base = (int)pick((uint32_t)(33 - len));
        const char *a = chance(50) ? "16b" : "8b";
        char list[64];
        int p = 0;
        p += snprintf(list + p, sizeof list - p, "{");
        for (int i = 0; i < len; i++)
            p += snprintf(list + p, sizeof list - p, "%sv%d.16b", i ? ", " : "", base + i);
        snprintf(list + p, sizeof list - p, "}");
        asmf("%s v%d.%s, %s, v%d.%s", chance(50) ? "tbl" : "tbx", d, a, list, m, a);
        break;
    }
    case 3: {
        static const char *U[6] = {"rev16", "rev32", "rev64", "cnt", "rbit", "rev64"};
        int k = (int)pick(6);
        const char *a;
        if (k == 0 || k == 3 || k == 4)
            a = chance(50) ? "16b" : "8b";
        else if (k == 1)
            a = ARR_INT[pick(4)]; /* 8b/16b/4h/8h */
        else
            a = ARR_INT[pick(6)];
        asmf("%s v%d.%s, v%d.%s", U[k], d, a, n1, a);
        break;
    }
    default: {
        static const char *U[6] = {"abs", "neg", "cls", "clz", "sqabs", "sqneg"};
        int k = (int)pick(6);
        const char *a = (k == 2 || k == 3) ? ARR_INT[pick(6)] : arr_all();
        asmf("%s v%d.%s, v%d.%s", U[k], d, a, n1, a);
        break;
    }
    }
}

// Cross-boundary vector<->GPR moves. UMOV/SMOV write a GPR and DUP/INS(general) read one, so a
// stolen register named here must be mangled -- this is one of the frontend's hand-written cases.
static void gen_simd_gpr(void) {
    int v = vreg(), r = rreg();
    switch (pick(5)) {
    case 0: {
        static const char *A[4] = {"b", "h", "s", "d"};
        static const int N[4] = {16, 8, 4, 2};
        int k = (int)pick(4);
        if (k == 3)
            asmf("umov %s, v%d.d[%u]", X(r), v, pick(2));
        else
            asmf("umov %s, v%d.%s[%u]", W(r), v, A[k], pick((uint32_t)N[k]));
        break;
    }
    case 1: {
        static const char *A[3] = {"b", "h", "s"};
        static const int N[3] = {16, 8, 4};
        int k = (int)pick(3);
        if (k == 2)
            asmf("smov %s, v%d.s[%u]", X(r), v, pick(4));
        else
            asmf("smov %s, v%d.%s[%u]", chance(50) ? X(r) : W(r), v, A[k], pick((uint32_t)N[k]));
        break;
    }
    case 2: {
        static const char *A[4] = {"8b", "16b", "4h", "8h"};
        static const char *B[3] = {"2s", "4s", "2d"};
        if (chance(50))
            asmf("dup v%d.%s, %s", v, A[pick(4)], W(r));
        else {
            const char *a = B[pick(3)];
            asmf("dup v%d.%s, %s", v, a, strcmp(a, "2d") ? W(r) : X(r));
        }
        break;
    }
    case 3: {
        static const char *A[4] = {"b", "h", "s", "d"};
        static const int N[4] = {16, 8, 4, 2};
        int k = (int)pick(4);
        asmf("ins v%d.%s[%u], %s", v, A[k], pick((uint32_t)N[k]), k == 3 ? X(r) : W(r));
        break;
    }
    default: {
        static const char *A[4] = {"b", "h", "s", "d"};
        static const int N[4] = {16, 8, 4, 2};
        int k = (int)pick(4);
        int n1 = vreg();
        asmf("ins v%d.%s[%u], v%d.%s[%u]", v, A[k], pick((uint32_t)N[k]), n1, A[k], pick((uint32_t)N[k]));
        break;
    }
    }
}

static void gen_simd_movi(void) {
    int d = vreg();
    switch (pick(4)) {
    case 0:
        asmf("movi v%d.%s, #0x%x", d, chance(50) ? "16b" : "8b", pick(256));
        break;
    case 1: {
        static const char *A[4] = {"4h", "8h", "2s", "4s"};
        int k = (int)pick(4);
        unsigned sh = (k < 2) ? pick(2) * 8 : pick(4) * 8;
        asmf("%s v%d.%s, #0x%x, lsl #%u", chance(50) ? "movi" : "mvni", d, A[k], pick(256), sh);
        break;
    }
    case 2: {
        static const char *A[4] = {"4h", "8h", "2s", "4s"};
        int k = (int)pick(4);
        unsigned sh = (k < 2) ? pick(2) * 8 : pick(4) * 8;
        asmf("%s v%d.%s, #0x%x, lsl #%u", chance(50) ? "orr" : "bic", d, A[k], pick(256), sh);
        break;
    }
    default: asmf("movi v%d.2d, #0x%s", d, chance(50) ? "ff00ff00ff00ff00" : "00ffff0000ffff00"); break;
    }
}

static void gen_simd_fp(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    const char *a = arr_fp();
    switch (pick(6)) {
    case 0: {
        static const char *OPS[12] = {"fadd", "fsub", "fmul",   "fdiv",   "fmax",  "fmin",
                                      "fmaxnm", "fminnm", "fmla", "fmls", "fabd", "fmulx"};
        asmf("%s v%d.%s, v%d.%s, v%d.%s", OPS[pick(12)], d, a, n1, a, m, a);
        break;
    }
    case 1: {
        static const char *OPS[8] = {"fabs",   "fneg",   "fsqrt",  "frecpe",
                                     "frsqrte", "frinta", "frintn", "frintz"};
        asmf("%s v%d.%s, v%d.%s", OPS[pick(8)], d, a, n1, a);
        break;
    }
    case 2: {
        static const char *OPS[5] = {"fcmeq", "fcmge", "fcmgt", "facge", "facgt"};
        if (chance(35)) {
            static const char *Z[5] = {"fcmeq", "fcmge", "fcmgt", "fcmle", "fcmlt"};
            asmf("%s v%d.%s, v%d.%s, #0.0", Z[pick(5)], d, a, n1, a);
        } else
            asmf("%s v%d.%s, v%d.%s, v%d.%s", OPS[pick(5)], d, a, n1, a, m, a);
        break;
    }
    case 3: {
        static const char *OPS[4] = {"fcvtzs", "fcvtzu", "scvtf", "ucvtf"};
        if (chance(40))
            asmf("%s v%d.%s, v%d.%s, #%u", OPS[pick(4)], d, a, n1, a,
                 1 + pick((uint32_t)arr_esize(a) - 1));
        else
            asmf("%s v%d.%s, v%d.%s", OPS[pick(4)], d, a, n1, a);
        break;
    }
    case 4: {
        /* widen / narrow between binary32 and binary64, and to binary16 */
        switch (pick(4)) {
        case 0: asmf("fcvtl v%d.2d, v%d.2s", d, n1); break;
        case 1: asmf("fcvtn v%d.2s, v%d.2d", d, n1); break;
        case 2: asmf("fcvtxn v%d.2s, v%d.2d", d, n1); break;
        default: asmf("fcvtl v%d.4s, v%d.4h", d, n1); break;
        }
        break;
    }
    default: {
        static const char *OPS[4] = {"faddp", "fmaxp", "fminp", "fmaxnmp"};
        if (chance(40)) {
            static const char *V[4] = {"fmaxv", "fminv", "fmaxnmv", "fminnmv"};
            asmf("%s s%d, v%d.4s", V[pick(4)], d, n1);
        } else
            asmf("%s v%d.%s, v%d.%s, v%d.%s", OPS[pick(4)], d, a, n1, a, m, a);
        break;
    }
    }
}

// Scalar FP, including the NZCV-setting compares and the FP<->GPR conversions the frontend has to
// classify by hand (they sit in the SIMD encoding box but name a general-purpose register).
static void gen_fp_scalar(void) {
    int d = vreg(), n1 = vreg(), m = vreg(), r = rreg();
    int dbl = (int)pick(2);
    char t = dbl ? 'd' : 's';
    switch (pick(7)) {
    case 0: {
        static const char *OPS[10] = {"fadd", "fsub",   "fmul",   "fdiv", "fmax",
                                      "fmin", "fmaxnm", "fminnm", "fabd", "fmulx"};
        asmf("%s %c%d, %c%d, %c%d", OPS[pick(10)], t, d, t, n1, t, m);
        break;
    }
    case 1: {
        static const char *OPS[4] = {"fmadd", "fmsub", "fnmadd", "fnmsub"};
        int a2 = vreg();
        asmf("%s %c%d, %c%d, %c%d, %c%d", OPS[pick(4)], t, d, t, n1, t, m, t, a2);
        break;
    }
    case 2:
        if (chance(50))
            asmf("%s %c%d, %c%d", chance(50) ? "fcmp" : "fcmpe", t, n1, t, m);
        else
            asmf("%s %c%d, #0.0", chance(50) ? "fcmp" : "fcmpe", t, n1);
        break;
    case 3:
        if (chance(50))
            asmf("%s %c%d, %c%d, #%u, %s", chance(50) ? "fccmp" : "fccmpe", t, n1, t, m, pick(16), rcond());
        else
            asmf("fcsel %c%d, %c%d, %c%d, %s", t, d, t, n1, t, m, rcond());
        break;
    case 4: {
        /* FP <-> GPR: these read or write a GENERAL-PURPOSE register */
        static const char *CV[8] = {"fcvtzs", "fcvtzu", "fcvtas", "fcvtau",
                                    "fcvtms", "fcvtmu", "fcvtps", "fcvtpu"};
        switch (pick(4)) {
        case 0: asmf("%s %s, %c%d", CV[pick(8)], chance(50) ? X(r) : W(r), t, n1); break;
        case 1: asmf("%s %c%d, %s", chance(50) ? "scvtf" : "ucvtf", t, d, chance(50) ? X(r) : W(r)); break;
        case 2:
            if (dbl)
                asmf("fmov %s, d%d", X(r), n1);
            else
                asmf("fmov %s, s%d", W(r), n1);
            break;
        default:
            if (dbl)
                asmf("fmov d%d, %s", d, X(r));
            else
                asmf("fmov s%d, %s", d, W(r));
            break;
        }
        break;
    }
    case 5: {
        /* fixed-point conversions also cross the boundary */
        if (chance(50))
            asmf("fcvtzs %s, %c%d, #%u", X(r), t, n1, 1 + pick(63));
        else
            asmf("scvtf %c%d, %s, #%u", t, d, X(r), 1 + pick(63));
        break;
    }
    default: {
        static const char *U[8] = {"fabs",   "fneg",   "fsqrt",  "frinta",
                                   "frintn", "frintp", "frintm", "frintx"};
        if (chance(30))
            asmf("fcvt %c%d, %c%d", dbl ? 's' : 'd', d, t, n1);
        else
            asmf("%s %c%d, %c%d", U[pick(8)], t, d, t, n1);
        break;
    }
    }
}

// AdvSIMD load/store STRUCTURE ops -- the emit_fold_advsimd_struct path, including the register
// post-index form whose increment operand is a GPR (a classic field-classification trap).
static void gen_simd_struct(void) {
    int nreg = 1 + (int)pick(4);
    int base = (int)pick((uint32_t)(33 - nreg));
    static const char *A[6] = {"8b", "16b", "4h", "8h", "2s", "4s"};
    const char *a = A[pick(6)];
    int lanes = arr_lanes(a);
    int es = arr_esize(a) / 8;
    int bytes = nreg * lanes * es;
    int load = chance(55);
    (void)bytes;
    bytes = nreg * lanes * es;
    char list[128];
    int p = snprintf(list, sizeof list, "{");
    for (int i = 0; i < nreg; i++)
        p += snprintf(list + p, sizeof list - p, "%sv%d.%s", i ? ", " : "", base + i, a);
    snprintf(list + p, sizeof list - p, "}");

    asmf("add x24, x25, #%d", (int)pick(120) * 32);
    int form = (int)pick(3);
    if (form == 0)
        asmf("%s%d %s, [x24]", load ? "ld" : "st", nreg, list);
    else if (form == 1)
        asmf("%s%d %s, [x24], #%d", load ? "ld" : "st", nreg, list, bytes);
    else {
        int inc = rreg();
        asmf("mov %s, #32", X(inc));
        asmf("%s%d %s, [x24], %s", load ? "ld" : "st", nreg, list, X(inc));
    }
}

// LD1R / LD1..LD4 single-structure-to-lane.
static void gen_simd_struct_lane(void) {
    static const char *A[4] = {"b", "h", "s", "d"};
    static const int N[4] = {16, 8, 4, 2};
    int k = (int)pick(4);
    int nreg = 1 + (int)pick(4);
    int base = (int)pick((uint32_t)(33 - nreg));
    int es = 1 << k;
    asmf("add x24, x25, #%d", (int)pick(120) * 32);
    if (chance(35)) {
        static const char *AR[4] = {"16b", "8h", "4s", "2d"};
        asmf("ld1r {v%d.%s}, [x24]", base, AR[k]);
        return;
    }
    char list[128];
    int p = snprintf(list, sizeof list, "{");
    for (int i = 0; i < nreg; i++)
        p += snprintf(list + p, sizeof list - p, "%sv%d.%s", i ? ", " : "", base + i, A[k]);
    snprintf(list + p, sizeof list - p, "}");
    int load = chance(55);
    if (chance(50))
        asmf("%s%d %s[%u], [x24]", load ? "ld" : "st", nreg, list, pick((uint32_t)N[k]));
    else
        asmf("%s%d %s[%u], [x24], #%d", load ? "ld" : "st", nreg, list, pick((uint32_t)N[k]), nreg * es);
}

// Vector loads and stores of whole registers (ldr q / str q / ldp q / ldur q).
static void gen_simd_ldst(void) {
    int v = vreg(), v2 = vreg();
    static const char sz[5] = {'b', 'h', 's', 'd', 'q'};
    int k = (int)pick(5);
    int width = 1 << k;
    switch (pick(4)) {
    case 0: asmf("%s %c%d, [x25, #%d]", chance(50) ? "ldr" : "str", sz[k], v, disp_scaled(width)); break;
    case 1: {
        char op[64];
        mem_reg(op, sizeof op, width, -1);
        asmf("%s %c%d, %s", chance(50) ? "ldr" : "str", sz[k], v, op);
        break;
    }
    case 2: {
        int pre;
        char op[64];
        mem_wb(op, sizeof op, width, &pre);
        asmf("%s %c%d, %s", chance(50) ? "ldr" : "str", sz[k], v, op);
        break;
    }
    default: {
        if (v2 == v) v2 = (v + 1) & 31;
        int w = k < 2 ? 4 : width;
        char c = k < 2 ? 's' : sz[k];
        asmf("%s %c%d, %c%d, [x25, #%d]", chance(50) ? "ldp" : "stp", c, v, c, v2, (int)pick(30) * w * 2);
        break;
    }
    }
}

// Optional classes ------------------------------------------------------------------------

static void gen_i8mm(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    static const char *OPS[3] = {"smmla", "ummla", "usmmla"};
    asmf("%s v%d.4s, v%d.16b, v%d.16b", OPS[pick(3)], d, n1, m);
}

static void gen_bf16(void) {
    int d = vreg(), n1 = vreg(), m = vreg();
    if (chance(40))
        asmf("bfcvt h%d, s%d", d, n1);
    else
        asmf("bfdot v%d.4s, v%d.8h, v%d.8h", d, n1, m);
}

static void gen_dczva(void) {
    asmf("add x24, x25, #%d", (int)pick(60) * 64);
    asmf("dc zva, x24");
}

static void gen_fpcr(void) {
    int r = rreg();
    static const unsigned MODES[4] = {0x0u, 0x400000u, 0x800000u, 0xc00000u};
    unsigned v = MODES[pick(4)];
    if (chance(25)) v |= 0x1000000u; /* FZ */
    if (chance(20)) v |= 0x2000000u; /* DN */
    asmf("mov %s, #0x%x", X(r), v & 0xffff);
    asmf("movk %s, #0x%x, lsl #16", X(r), (v >> 16) & 0xffff);
    asmf("msr fpcr, %s", X(r));
}

static void gen_mrs(void) {
    int r = rreg();
    if (chance(50)) {
        asmf("mrs %s, nzcv", X(r));
        asmf("and %s, %s, #0xf0000000", X(r), X(r));
    } else {
        asmf("mrs %s, fpsr", X(r));
        asmf("and %s, %s, #0xff", X(r), X(r));
    }
}

// ---------------------------------------------------------------- class table

typedef void (*genfn)(void);

struct klass {
    genfn fn;
    int weight;
    const char *name;
};

static struct klass KLASS[] = {
    {gen_addsub, 90, "addsub"},
    {gen_adcsbc, 30, "adcsbc"},
    {gen_logic, 60, "logic"},
    {gen_movwide, 20, "movwide"},
    {gen_bitfield, 55, "bitfield"},
    {gen_extr, 25, "extr"},
    {gen_dp1, 30, "dp1"},
    {gen_dp2, 35, "dp2"},
    {gen_dp3, 40, "dp3"},
    {gen_ccmp, 35, "ccmp"},
    {gen_csel, 45, "csel"},
    {gen_branch, 40, "branch"},
    {gen_adr, 12, "adr"},
    {gen_load, 70, "load"},
    {gen_store, 55, "store"},
    {gen_pair, 45, "ldstp"},
    {gen_atomic, 45, "atomic"},
    {gen_exclusive, 55, "exclusive"},
    {gen_acqrel, 18, "acqrel"},
    {gen_simd_int3, 45, "simd-int"},
    {gen_simd_logic, 25, "simd-logic"},
    {gen_simd_cmp, 25, "simd-cmp"},
    {gen_simd_shift, 30, "simd-shift"},
    {gen_simd_widen, 30, "simd-widen"},
    {gen_simd_pair_red, 25, "simd-reduce"},
    {gen_simd_perm, 30, "simd-perm"},
    {gen_simd_gpr, 40, "simd-gpr"},
    {gen_simd_movi, 18, "simd-movi"},
    {gen_simd_fp, 45, "simd-fp"},
    {gen_fp_scalar, 50, "fp-scalar"},
    {gen_simd_struct, 35, "simd-struct"},
    {gen_simd_struct_lane, 25, "simd-struct-lane"},
    {gen_simd_ldst, 40, "simd-ldst"},
    {gen_mrs, 12, "mrs"},
};
enum { NKLASS = (int)(sizeof KLASS / sizeof KLASS[0]) };

// ---------------------------------------------------------------- seeds
//
// Dense random mixed with the corner values a translator most plausibly mishandles.

static uint64_t seed_gpr(void) {
    static const uint64_t CORNER[] = {0,
                                      1,
                                      UINT64_C(0xffffffffffffffff),
                                      UINT64_C(0x8000000000000000),
                                      UINT64_C(0x7fffffffffffffff),
                                      UINT64_C(0x80000000),
                                      UINT64_C(0x7fffffff),
                                      UINT64_C(0xffffffff),
                                      UINT64_C(0x100000000),
                                      UINT64_C(0xffffffff00000000),
                                      UINT64_C(0x5555555555555555),
                                      UINT64_C(0xaaaaaaaaaaaaaaaa),
                                      UINT64_C(0x8080808080808080),
                                      UINT64_C(0x00000000000000ff),
                                      UINT64_C(0xff),
                                      UINT64_C(0x8000)};
    if (chance(45)) return CORNER[pick((uint32_t)(sizeof CORNER / sizeof CORNER[0]))];
    return nx();
}

static uint64_t seed_fp(void) {
    static const uint64_t CORNER[] = {
        UINT64_C(0x0000000000000000), /* +0            */
        UINT64_C(0x8000000000000000), /* -0            */
        UINT64_C(0x3ff0000000000000), /* 1.0           */
        UINT64_C(0xbff0000000000000), /* -1.0          */
        UINT64_C(0x7ff0000000000000), /* +inf          */
        UINT64_C(0xfff0000000000000), /* -inf          */
        UINT64_C(0x7ff8000000000000), /* qNaN          */
        UINT64_C(0x7ff0000000000001), /* sNaN          */
        UINT64_C(0x000fffffffffffff), /* max denormal  */
        UINT64_C(0x0000000000000001), /* min denormal  */
        UINT64_C(0x0010000000000000), /* min normal    */
        UINT64_C(0x7fefffffffffffff), /* max normal    */
        UINT64_C(0x7f8000017f800000), /* two f32 sNaNs */
        UINT64_C(0x7fc0000000800000), /* f32 qNaN|denorm */
        UINT64_C(0x0000000180000001), /* f32 denorm/-denorm */
        UINT64_C(0x3f80000047ffffff), /* f32 1.0 / big */
    };
    if (chance(55)) return CORNER[pick((uint32_t)(sizeof CORNER / sizeof CORNER[0]))];
    return nx();
}

// ---------------------------------------------------------------- program emission

static void emit_seed_gpr(int r, uint64_t v) {
    fixed("movz %s, #0x%x, lsl #48", X(r), (unsigned)((v >> 48) & 0xffff));
    fixed("movk %s, #0x%x, lsl #32", X(r), (unsigned)((v >> 32) & 0xffff));
    fixed("movk %s, #0x%x, lsl #16", X(r), (unsigned)((v >> 16) & 0xffff));
    fixed("movk %s, #0x%x", X(r), (unsigned)(v & 0xffff));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <seed> <out.c> [steps] [+i8mm +bf16 +dczva +fpcr +hot]\n", argv[0]);
        return 2;
    }
    uint64_t seed = strtoull(argv[1], NULL, 0);
    const char *path = argv[2];
    int steps = argc > 3 ? atoi(argv[3]) : 200;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "+i8mm")) f_i8mm = 1;
        else if (!strcmp(argv[i], "+bf16")) f_bf16 = 1;
        else if (!strcmp(argv[i], "+dczva")) f_dczva = 1;
        else if (!strcmp(argv[i], "+fpcr")) f_fpcr = 1;
        else if (!strcmp(argv[i], "+hot")) f_reps = 64;
        else {
            fprintf(stderr, "unknown feature flag %s\n", argv[i]);
            return 2;
        }
    }
    rng_state = seed * UINT64_C(0x2545F4914F6CDD1D) + 0x9E3779B9u;

    raw("/* generated by tests/fuzz/isa/aarch64/isafuzz_gen.c -- do not edit */\n");
    rawf("/* seed %llu, steps %d */\n", (unsigned long long)seed, steps);
    raw("#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n\n");
    rawf("#define REPS %d\n", f_reps);
    rawf("#define AREA_BYTES %d\n", AREA_BYTES);
    rawf("#define SCRATCH_BYTES %d\n", SCRATCH_BYTES);
    rawf("#define OFF_VSEED %d\n#define OFF_DUMPX %d\n#define OFF_DUMPF %d\n#define OFF_DUMPV %d\n",
         OFF_VSEED, OFF_DUMPX, OFF_DUMPF, OFF_DUMPV);
    raw("static unsigned char area[AREA_BYTES] __attribute__((aligned(4096)));\n\n");

    /* deterministic scratch fill */
    raw("static const uint64_t scratch_seed[SCRATCH_BYTES / 8] = {\n");
    for (int i = 0; i < SCRATCH_BYTES / 8; i++)
        rawf("  0x%016llxULL,%s", (unsigned long long)(chance(35) ? seed_fp() : seed_gpr()),
             (i % 4 == 3) ? "\n" : "");
    raw("};\n\n");

    raw("static const uint64_t vseed[64] = {\n");
    for (int i = 0; i < 64; i++)
        rawf("  0x%016llxULL,%s", (unsigned long long)seed_fp(), (i % 4 == 3) ? "\n" : "");
    raw("};\n\n");

    raw("extern void isafuzz_body(void *area);\n\n");
    raw("__asm__(\n");
    raw("    \".arch armv8.4-a+lse+rcpc+dotprod+i8mm+bf16\\n\"\n");
    raw("    \".text\\n\"\n");
    raw("    \".balign 16\\n\"\n");
    raw("    \".globl isafuzz_body\\n\"\n");
    raw("    \".type isafuzz_body, %function\\n\"\n");
    raw("    \"isafuzz_body:\\n\"\n");

    /* ---- prologue: preserve everything the AAPCS64 caller expects back, plus x16..x18 ---- */
    fixed("str x%d, [x0, #%d]", 19, OFF_SAVE + 0);
    fixed("str x%d, [x0, #%d]", 20, OFF_SAVE + 8);
    fixed("str x%d, [x0, #%d]", 21, OFF_SAVE + 16);
    fixed("str x%d, [x0, #%d]", 22, OFF_SAVE + 24);
    fixed("str x%d, [x0, #%d]", 23, OFF_SAVE + 32);
    fixed("str x%d, [x0, #%d]", 24, OFF_SAVE + 40);
    fixed("str x%d, [x0, #%d]", 25, OFF_SAVE + 48);
    fixed("str x%d, [x0, #%d]", 26, OFF_SAVE + 56);
    fixed("str x%d, [x0, #%d]", 27, OFF_SAVE + 64);
    fixed("str x%d, [x0, #%d]", 28, OFF_SAVE + 72);
    fixed("str x%d, [x0, #%d]", 29, OFF_SAVE + 80);
    fixed("str x%d, [x0, #%d]", 30, OFF_SAVE + 88);
    fixed("str x%d, [x0, #%d]", 16, OFF_SAVE + 96);
    fixed("str x%d, [x0, #%d]", 17, OFF_SAVE + 104);
    fixed("str x%d, [x0, #%d]", 18, OFF_SAVE + 112);
    fixed("str d%d, [x0, #%d]", 8, OFF_FSAVE + 0);
    fixed("str d%d, [x0, #%d]", 9, OFF_FSAVE + 8);
    fixed("str d%d, [x0, #%d]", 10, OFF_FSAVE + 16);
    fixed("str d%d, [x0, #%d]", 11, OFF_FSAVE + 24);
    fixed("str d%d, [x0, #%d]", 12, OFF_FSAVE + 32);
    fixed("str d%d, [x0, #%d]", 13, OFF_FSAVE + 40);
    fixed("str d%d, [x0, #%d]", 14, OFF_FSAVE + 48);
    fixed("str d%d, [x0, #%d]", 15, OFF_FSAVE + 56);
    fixed("mov x25, x0");
    fixed("mov x24, x25");

    /* ---- seed the vector file from the constant table ---- */
    for (int v = 0; v < 32; v++) fixed("ldr q%d, [x25, #%d]", v, OFF_VSEED + v * 16);
    /* ---- seed the general-purpose file ---- */
    for (int i = 0; i < NPOOL; i++) emit_seed_gpr(POOL[i], seed_gpr());
    /* ---- seed NZCV deterministically ---- */
    fixed("movz x0, #0x%x, lsl #16", (unsigned)((pick(16) << 12) & 0xffff));
    fixed("msr nzcv, x0");
    fixed("msr fpsr, xzr");
    emit_seed_gpr(0, seed_gpr());

    /* ---- body ---- */
    raw("    /* BODY-BEGIN */\n");

    int total = 0;
    for (int i = 0; i < NKLASS; i++) total += KLASS[i].weight;
    struct klass opt[4];
    int nopt = 0;
    int opt_total = 0;
    if (f_i8mm) opt[nopt++] = (struct klass){gen_i8mm, 30, "i8mm"};
    if (f_bf16) opt[nopt++] = (struct klass){gen_bf16, 30, "bf16"};
    if (f_dczva) opt[nopt++] = (struct klass){gen_dczva, 12, "dczva"};
    if (f_fpcr) opt[nopt++] = (struct klass){gen_fpcr, 12, "fpcr"};
    for (int i = 0; i < nopt; i++) opt_total += opt[i].weight;

    for (int s = 0; s < steps; s++) {
        if (nopt && chance(12)) {
            int t = (int)pick((uint32_t)opt_total), i = 0;
            while (t >= opt[i].weight) t -= opt[i++].weight;
            opt[i].fn();
        } else {
            int t = (int)pick((uint32_t)total), i = 0;
            while (t >= KLASS[i].weight) t -= KLASS[i++].weight;
            KLASS[i].fn();
        }
        step_flush();
    }

    raw("    /* BODY-END */\n");

    /* ---- dump ---- */
    for (int r = 0; r <= 30; r++) fixed("str x%d, [x25, #%d]", r, OFF_DUMPX + r * 8);
    fixed("mrs x0, nzcv");
    fixed("str x0, [x25, #%d]", OFF_DUMPF);
    fixed("mrs x0, fpsr");
    fixed("str x0, [x25, #%d]", OFF_DUMPF + 8);
    fixed("mrs x0, fpcr");
    fixed("str x0, [x25, #%d]", OFF_DUMPF + 16);
    for (int v = 0; v < 32; v++) fixed("str q%d, [x25, #%d]", v, OFF_DUMPV + v * 16);

    /* ---- epilogue: FPCR must be put back or libc's printf inherits a fuzzed rounding mode ---- */
    fixed("msr fpcr, xzr");
    fixed("ldr x%d, [x25, #%d]", 19, OFF_SAVE + 0);
    fixed("ldr x%d, [x25, #%d]", 20, OFF_SAVE + 8);
    fixed("ldr x%d, [x25, #%d]", 21, OFF_SAVE + 16);
    fixed("ldr x%d, [x25, #%d]", 22, OFF_SAVE + 24);
    fixed("ldr x%d, [x25, #%d]", 23, OFF_SAVE + 32);
    fixed("ldr x%d, [x25, #%d]", 26, OFF_SAVE + 56);
    fixed("ldr x%d, [x25, #%d]", 27, OFF_SAVE + 64);
    fixed("ldr x%d, [x25, #%d]", 28, OFF_SAVE + 72);
    fixed("ldr x%d, [x25, #%d]", 29, OFF_SAVE + 80);
    fixed("ldr x%d, [x25, #%d]", 30, OFF_SAVE + 88);
    fixed("ldr x%d, [x25, #%d]", 16, OFF_SAVE + 96);
    fixed("ldr x%d, [x25, #%d]", 17, OFF_SAVE + 104);
    fixed("ldr x%d, [x25, #%d]", 18, OFF_SAVE + 112);
    fixed("ldr d%d, [x25, #%d]", 8, OFF_FSAVE + 0);
    fixed("ldr d%d, [x25, #%d]", 9, OFF_FSAVE + 8);
    fixed("ldr d%d, [x25, #%d]", 10, OFF_FSAVE + 16);
    fixed("ldr d%d, [x25, #%d]", 11, OFF_FSAVE + 24);
    fixed("ldr d%d, [x25, #%d]", 12, OFF_FSAVE + 32);
    fixed("ldr d%d, [x25, #%d]", 13, OFF_FSAVE + 40);
    fixed("ldr d%d, [x25, #%d]", 14, OFF_FSAVE + 48);
    fixed("ldr d%d, [x25, #%d]", 15, OFF_FSAVE + 56);
    fixed("ldr x24, [x25, #%d]", OFF_SAVE + 40);
    fixed("ldr x25, [x25, #%d]", OFF_SAVE + 48);
    fixed("ret");
    raw("    \".size isafuzz_body, . - isafuzz_body\\n\"\n");
    raw(");\n\n");

    /* ---- driver ---- */
    raw(
        "int main(void) {\n"
        "    /* REPS>1 makes every generated block HOT, so the run also crosses the engine's tier-2\n"
        "       specialization / trace-formation paths. Each repetition re-seeds memory AND registers\n"
        "       (the seeding lives inside isafuzz_body), so every iteration is identical and the dump\n"
        "       stays a pure function of the seed. */\n"
        "    for (int rep = 0; rep < REPS; rep++) {\n"
        "        memcpy(area, scratch_seed, SCRATCH_BYTES);\n"
        "        memcpy(area + OFF_VSEED, vseed, sizeof vseed);\n"
        "        isafuzz_body(area);\n"
        "    }\n"
        "    const uint64_t *x = (const uint64_t *)(area + OFF_DUMPX);\n"
        "    for (int r = 0; r <= 30; r++) {\n"
        "        if (r == 24 || r == 25) continue;   /* reserved bases: hold addresses */\n"
        "        printf(\"X%02d %016llx\\n\", r, (unsigned long long)x[r]);\n"
        "    }\n"
        "    const uint64_t *f = (const uint64_t *)(area + OFF_DUMPF);\n"
        "    printf(\"NZCV %08llx\\n\", (unsigned long long)(f[0] & 0xf0000000ULL));\n"
        "    printf(\"FPSR %08llx\\n\", (unsigned long long)(f[1] & 0x0800009fULL));\n"
        "    printf(\"FPCR %08llx\\n\", (unsigned long long)(f[2] & 0x07ffff00ULL));\n"
        "    const unsigned char *v = area + OFF_DUMPV;\n"
        "    for (int i = 0; i < 32; i++) {\n"
        "        printf(\"V%02d \", i);\n"
        "        for (int b = 15; b >= 0; b--) printf(\"%02x\", v[i * 16 + b]);\n"
        "        printf(\"\\n\");\n"
        "    }\n"
        "    uint64_t h = 1469598103934665603ULL;\n"
        "    for (int i = 0; i < SCRATCH_BYTES; i++) { h ^= area[i]; h *= 1099511628211ULL; }\n"
        "    printf(\"MEM %016llx\\n\", (unsigned long long)h);\n"
        "    return 0;\n"
        "}\n");

    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return 2;
    }
    fwrite(out, 1, out_len, fp);
    fclose(fp);
    return 0;
}
