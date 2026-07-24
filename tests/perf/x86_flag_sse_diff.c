// Differential flag oracle for the "SSE is flag-transparent" liveness classification
// (translator/guest/x86_64/lower/trace.c, x86_two_byte_flagfree).
//
// The change lets the translator's flag-liveness scanners walk THROUGH SSE/MMX instructions, which
// makes it elide (a) the per-iteration cpu->nzcv spill in a tier-2 self-loop whose body contains SSE
// and (b) the PF/AF substrate of an integer producer whose next PF/AF writer is on the far side of
// some SSE code. Both are only sound if every architectural flag survives the intervening SSE
// instructions bit-for-bit.
//
// So: run an integer flag PRODUCER, then a slab of SSE work, then read the full RFLAGS
// (CF/PF/AF/ZF/SF/OF) with pushfq -- across operand patterns chosen to hit every flag boundary
// (zero results, sign flips, carry in/out, INT_MIN/INT_MAX overflow, parity, and the bit-3->4
// half-carry that drives AF). Bit-compare the output against qemu-x86_64.
//
// Build:  $X86_64_LINUX_STATIC_CC -O2 -static -std=gnu11 x86_flag_sse_diff.c -o x86_flag_sse_diff
// Run:    qemu-x86_64 ./x86_flag_sse_diff > a; hl-engine ./x86_flag_sse_diff > b; cmp a b

#include <stdint.h>
#include <stdio.h>

#define FLAGMASK 0x8D5u // OF(11) SF(7) ZF(6) AF(4) PF(2) CF(0)

// A slab of SSE the flag state has to survive: packed FP arith (the float_simd loop's mulps/addps),
// packed integer arith/logic/shift, shuffles and moves -- i.e. representatives of every range in
// x86_two_byte_flagfree. Deliberately opaque to the compiler so it cannot be sunk or hoisted.
#define SSE_SLAB                                                                                                     \
    "movaps %[sa], %%xmm6\n\t"                                                                                       \
    "mulps  %[sb], %%xmm6\n\t"                                                                                       \
    "addps  %[sa], %%xmm6\n\t"                                                                                       \
    "subps  %[sb], %%xmm6\n\t"                                                                                       \
    "divps  %[sb], %%xmm6\n\t"                                                                                       \
    "sqrtps %%xmm6, %%xmm7\n\t"                                                                                      \
    "minps  %%xmm7, %%xmm6\n\t"                                                                                      \
    "maxps  %%xmm7, %%xmm6\n\t"                                                                                      \
    "andps  %%xmm7, %%xmm6\n\t"                                                                                      \
    "orps   %%xmm7, %%xmm6\n\t"                                                                                      \
    "xorps  %%xmm7, %%xmm6\n\t"                                                                                      \
    "movdqa %[sa], %%xmm5\n\t"                                                                                       \
    "paddd  %[sb], %%xmm5\n\t"                                                                                       \
    "psubd  %[sb], %%xmm5\n\t"                                                                                       \
    "pand   %[sb], %%xmm5\n\t"                                                                                       \
    "por    %[sb], %%xmm5\n\t"                                                                                       \
    "pxor   %[sb], %%xmm5\n\t"                                                                                       \
    "pcmpeqd %[sb], %%xmm5\n\t"                                                                                      \
    "pcmpgtd %[sb], %%xmm5\n\t"                                                                                      \
    "pslld  $3, %%xmm5\n\t"                                                                                          \
    "psrld  $2, %%xmm5\n\t"                                                                                          \
    "punpcklbw %%xmm6, %%xmm5\n\t"                                                                                    \
    "pshufd $0x1b, %%xmm5, %%xmm5\n\t"                                                                                \
    "shufps $0x4e, %%xmm6, %%xmm6\n\t"                                                                                \
    "cvtdq2ps %%xmm5, %%xmm4\n\t"                                                                                     \
    "movaps %%xmm4, %[out]\n\t"

typedef struct {
    float f[4];
} v4;
static v4 g_sa = {{1.5f, -2.25f, 3.0f, 0.5f}};
static v4 g_sb = {{-0.75f, 4.0f, 1.25f, -8.0f}};
static v4 g_sink;

// Every 64-bit operand pattern worth testing: zero, one, sign boundaries, byte/word/dword
// boundaries, the bit-3/bit-4 half-carry cases that AF is computed from, and parity sources.
static const uint64_t PAT[] = {
    0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000002ull, 0x0000000000000003ull,
    0x0000000000000007ull, 0x0000000000000008ull, 0x0000000000000009ull, 0x000000000000000Full,
    0x0000000000000010ull, 0x000000000000007Full, 0x0000000000000080ull, 0x00000000000000FFull,
    0x0000000000007FFFull, 0x0000000000008000ull, 0x000000000000FFFFull, 0x000000007FFFFFFFull,
    0x0000000080000000ull, 0x00000000FFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
    0xFFFFFFFFFFFFFFFFull, 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 0x5555555555555555ull,
    0xAAAAAAAAAAAAAAAAull, 0x000000000000000Eull, 0x0000000000000011ull, 0x00000000FFFFFFF0ull,
};
#define NPAT ((int)(sizeof PAT / sizeof PAT[0]))

// One producer/consumer pair: run `insn` on (a,b) with carry-in `ci`, then the SSE slab, then read
// RFLAGS. `res` is the (possibly written) destination.
#define GEN(name, insn, decl)                                                                                        \
    static void name(uint64_t a, uint64_t b, int ci, uint64_t *res, uint64_t *fl) {                                  \
        uint64_t r = a, f;                                                                                           \
        v4 out;                                                                                                      \
        __asm__ volatile("cmp $1, %[ci]\n\t"                                                                          \
                         "cmc\n\t" /* set CF = ci */                                                                 \
                         insn "\n\t" SSE_SLAB "pushfq\n\t"                                                            \
                         "pop %[f]\n\t"                                                                              \
                         : [r] "+r"(r), [f] "=&r"(f), [out] "=m"(out)                                                \
                         : [b] decl(b), [ci] "r"((uint64_t)ci), [sa] "m"(g_sa), [sb] "m"(g_sb)                        \
                         : "xmm4", "xmm5", "xmm6", "xmm7", "cc");                                                     \
        g_sink = out;                                                                                                \
        *res = r;                                                                                                    \
        *fl = f & FLAGMASK;                                                                                          \
    }

GEN(t_add64, "add %[b], %[r]", "r")
GEN(t_sub64, "sub %[b], %[r]", "r")
GEN(t_cmp64, "cmp %[b], %[r]", "r")
GEN(t_adc64, "adc %[b], %[r]", "r")
GEN(t_sbb64, "sbb %[b], %[r]", "r")
GEN(t_and64, "and %[b], %[r]", "r")
GEN(t_or64, "or %[b], %[r]", "r")
GEN(t_xor64, "xor %[b], %[r]", "r")
GEN(t_test64, "test %[b], %[r]", "r")
GEN(t_add32, "add %k[b], %k[r]", "r")
GEN(t_sub32, "sub %k[b], %k[r]", "r")
GEN(t_cmp32, "cmp %k[b], %k[r]", "r")
GEN(t_adc32, "adc %k[b], %k[r]", "r")
GEN(t_sbb32, "sbb %k[b], %k[r]", "r")
GEN(t_and32, "and %k[b], %k[r]", "r")
GEN(t_xor32, "xor %k[b], %k[r]", "r")
GEN(t_add16, "add %w[b], %w[r]", "r")
GEN(t_sub16, "sub %w[b], %w[r]", "r")
GEN(t_add8, "add %b[b], %b[r]", "q")
GEN(t_sub8, "sub %b[b], %b[r]", "q")
GEN(t_adc8, "adc %b[b], %b[r]", "q")
GEN(t_sbb8, "sbb %b[b], %b[r]", "q")
GEN(t_cmp8, "cmp %b[b], %b[r]", "q")
GEN(t_neg64, "neg %[r]", "r")
GEN(t_neg32, "neg %k[r]", "r")
GEN(t_inc64, "inc %[r]", "r")
GEN(t_dec64, "dec %[r]", "r")
GEN(t_inc8, "inc %b[r]", "q")
GEN(t_dec8, "dec %b[r]", "q")
GEN(t_addi64, "add $0x10, %[r]", "r")
GEN(t_cmpi64, "cmp $0x4000, %[r]", "r")
GEN(t_addi32, "add $0x10, %k[r]", "r")
GEN(t_cmpi32, "cmp $0x4000, %k[r]", "r")
GEN(t_addi8, "add $0x0f, %b[r]", "q")
GEN(t_cmpi8, "cmp $0x08, %b[r]", "q")

struct tc {
    const char *name;
    void (*fn)(uint64_t, uint64_t, int, uint64_t *, uint64_t *);
};
static const struct tc TESTS[] = {
    {"add64", t_add64},   {"sub64", t_sub64},   {"cmp64", t_cmp64},   {"adc64", t_adc64},   {"sbb64", t_sbb64},
    {"and64", t_and64},   {"or64", t_or64},     {"xor64", t_xor64},   {"test64", t_test64}, {"add32", t_add32},
    {"sub32", t_sub32},   {"cmp32", t_cmp32},   {"adc32", t_adc32},   {"sbb32", t_sbb32},   {"and32", t_and32},
    {"xor32", t_xor32},   {"add16", t_add16},   {"sub16", t_sub16},   {"add8", t_add8},     {"sub8", t_sub8},
    {"adc8", t_adc8},     {"sbb8", t_sbb8},     {"cmp8", t_cmp8},     {"neg64", t_neg64},   {"neg32", t_neg32},
    {"inc64", t_inc64},   {"dec64", t_dec64},   {"inc8", t_inc8},     {"dec8", t_dec8},     {"addi64", t_addi64},
    {"cmpi64", t_cmpi64}, {"addi32", t_addi32}, {"cmpi32", t_cmpi32}, {"addi8", t_addi8},   {"cmpi8", t_cmpi8},
};
#define NTEST ((int)(sizeof TESTS / sizeof TESTS[0]))

// The tier-2 self-loop shape the elision targets: SSE body + add/cmp/jne back edge. After the loop
// exits, every flag must still be the one the final cmp produced (the spill is now on the exit edge).
static uint64_t loop_flags(uint64_t start, uint64_t limit, uint64_t step, uint64_t *outi) {
    uint64_t i = start, f;
    v4 out;
    __asm__ volatile("1:\n\t" SSE_SLAB "add %[st], %[i]\n\t"
                     "cmp %[lim], %[i]\n\t"
                     "jne 1b\n\t"
                     "pushfq\n\t"
                     "pop %[f]\n\t"
                     : [i] "+r"(i), [f] "=&r"(f), [out] "=m"(out)
                     : [st] "r"(step), [lim] "r"(limit), [sa] "m"(g_sa), [sb] "m"(g_sb)
                     : "xmm4", "xmm5", "xmm6", "xmm7", "cc");
    g_sink = out;
    *outi = i;
    return f & FLAGMASK;
}

int main(void) {
    for (int t = 0; t < NTEST; t++)
        for (int i = 0; i < NPAT; i++)
            for (int j = 0; j < NPAT; j++)
                for (int ci = 0; ci < 2; ci++) {
                    uint64_t r, f;
                    TESTS[t].fn(PAT[i], PAT[j], ci, &r, &f);
                    printf("%-7s a=%016llx b=%016llx ci=%d -> r=%016llx fl=%04llx\n", TESTS[t].name,
                           (unsigned long long)PAT[i], (unsigned long long)PAT[j], ci, (unsigned long long)r,
                           (unsigned long long)f);
                }
    for (int i = 0; i < NPAT; i++) {
        uint64_t oi, f = loop_flags(0, PAT[i] & 0xFF, 1, &oi);
        printf("loop    lim=%016llx -> i=%016llx fl=%04llx\n", (unsigned long long)(PAT[i] & 0xFF),
               (unsigned long long)oi, (unsigned long long)f);
    }
    return 0;
}
