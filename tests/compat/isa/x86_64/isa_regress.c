/* x86-64 instruction-lowering regression corpus.
 *
 * Every check below is a translator divergence that the differential ISA fuzzer
 * (tests/fuzz/isa/x86_64) found against qemu-x86_64 and that has since been fixed. The golden
 * output is the reference oracle's, so a regression in any of these lowerings shows up as an
 * exact stdout mismatch.
 *
 * Deterministic by construction: fixed inputs, no environment, no time, no addresses printed.
 * Built as x86_64-linux-gnu-gcc -O2 -static -no-pie (see the Makefile rule).
 */

#include <stdio.h>
#include <string.h>

unsigned char xmm_out[16] __attribute__((aligned(16)));
unsigned int A4[4] __attribute__((aligned(16)));
unsigned int B4[4] __attribute__((aligned(16)));
unsigned long long A2[2] __attribute__((aligned(16)));
unsigned long long B2[2] __attribute__((aligned(16)));
unsigned short mem16 = 0x1234;
unsigned char scratch[64] __attribute__((aligned(16)));

static void show(const char *tag) {
    printf("%-18s ", tag);
    for (int i = 15; i >= 0; i--)
        printf("%02x", xmm_out[i]);
    printf("\n");
}

#define VRUN(setup, body)                                                                                              \
    do {                                                                                                               \
        __asm__ volatile(setup body "\n movdqa %%xmm0, xmm_out(%%rip)" : : : "xmm0", "xmm1", "xmm2", "memory");        \
    } while (0)

#define LOAD4 "movdqa A4(%%rip), %%xmm0\n movdqa B4(%%rip), %%xmm1\n"
#define LOAD2 "movdqa A2(%%rip), %%xmm0\n movdqa B2(%%rip), %%xmm1\n"

/* ---------------------------------------------------------------- scalar SSE upper lanes */
/* CMPSS/CMPSD, the scalar arithmetic, the scalar min/max, CVTSI2SD, CVTSS2SD and CVTSD2SS all
 * write ONLY the low element; bits 127:64 (127:32 for the ss forms) are architecturally
 * preserved. The ARM scalar forms zero them, so each needed an explicit merge. */
static void scalar_upper_lanes(void) {
    A2[0] = 0x3ff0000000000000ULL;
    A2[1] = 0x1122334455667788ULL;
    B2[0] = 0x4000000000000000ULL;
    B2[1] = 0x99aabbccddeeff00ULL;
    VRUN(LOAD2, "addsd %%xmm1,%%xmm0");
    show("addsd");
    VRUN(LOAD2, "subss %%xmm1,%%xmm0");
    show("subss");
    VRUN(LOAD2, "mulss %%xmm1,%%xmm0");
    show("mulss");
    VRUN(LOAD2, "divsd %%xmm1,%%xmm0");
    show("divsd");
    VRUN(LOAD2, "sqrtsd %%xmm1,%%xmm0");
    show("sqrtsd");
    VRUN(LOAD2, "minsd %%xmm1,%%xmm0");
    show("minsd");
    VRUN(LOAD2, "maxss %%xmm1,%%xmm0");
    show("maxss");
    VRUN(LOAD2, "cmpsd $4,%%xmm1,%%xmm0");
    show("cmpsd-neq");
    VRUN(LOAD2, "cmpss $0,%%xmm1,%%xmm0");
    show("cmpss-eq");
    VRUN(LOAD2, "cvtss2sd %%xmm1,%%xmm0");
    show("cvtss2sd");
    VRUN(LOAD2, "cvtsd2ss %%xmm1,%%xmm0");
    show("cvtsd2ss");
    VRUN(LOAD2, "movq $0x123456789, %%rax\n cvtsi2sdq %%rax,%%xmm0");
    show("cvtsi2sdq");
}

/* ---------------------------------------------------------------- packed conversions */
/* Legacy (non-VEX) CVTPS2PD / CVTPD2PS used to fall into the SCALAR 0F 5A arm and produced a
 * single converted element plus garbage. */
static void packed_converts(void) {
    A2[0] = 0x4055000000000000ULL; /* 84.0            */
    A2[1] = 0xc07e000000000000ULL; /* -480.0          */
    B2[0] = 0x42c8000041200000ULL; /* floats 10.0, 100.0 */
    B2[1] = 0x4048000040000000ULL; /* floats 2.0, 3.125  */
    VRUN(LOAD2, "cvtpd2ps %%xmm0,%%xmm0");
    show("cvtpd2ps");
    VRUN(LOAD2, "cvtps2pd %%xmm1,%%xmm0");
    show("cvtps2pd");
}

/* ---------------------------------------------------------------- SSE NaN selection */
/* x86 selects SRC1 when SRC1 is a QNaN, otherwise SRC2 if that is a NaN, otherwise SRC1. The
 * (QNaN, QNaN) pair -- the common one, since every propagated NaN is quiet -- used to come out
 * as SRC2. Also: horizontal ops add the ODD lane FIRST, and a GENERATED NaN carries x86's
 * negative indefinite sign. */
static void sse_nan(void) {
    static const unsigned int NANS[][2] = {
        {0xffffffffu, 0x7fc00001u}, /* qnan , qnan */
        {0xffffffffu, 0x7f800002u}, /* qnan , snan */
        {0xff800001u, 0x7fc00001u}, /* snan , qnan */
        {0xff800001u, 0x7f800002u}, /* snan , snan */
        {0xff800001u, 0x40000000u}, /* snan , value */
        {0x40000000u, 0x7f800002u}, /* value, snan */
    };
    for (unsigned i = 0; i < sizeof NANS / sizeof NANS[0]; i++) {
        A4[0] = NANS[i][0];
        A4[1] = A4[2] = A4[3] = 0x3f800000u;
        B4[0] = NANS[i][1];
        B4[1] = B4[2] = B4[3] = 0x40000000u;
        char tag[32];
        snprintf(tag, sizeof tag, "subps-nan%u", i);
        VRUN(LOAD4, "subps %%xmm1,%%xmm0");
        show(tag);
        snprintf(tag, sizeof tag, "mulps-nan%u", i);
        VRUN(LOAD4, "mulps %%xmm1,%%xmm0");
        show(tag);
    }
    /* generated NaN (inf + -inf) must carry the x86 negative indefinite sign */
    A4[0] = 0x7f800000u;
    A4[1] = 0xff800000u;
    A4[2] = 0x7f800000u;
    A4[3] = 0x7f800000u;
    B4[0] = 0x41000000u;
    B4[1] = 0x41200000u;
    B4[2] = 0x41400000u;
    B4[3] = 0x41600000u;
    VRUN(LOAD4, "haddps %%xmm1,%%xmm0");
    show("haddps-geninf");
    VRUN(LOAD4, "hsubps %%xmm1,%%xmm0");
    show("hsubps-geninf");
    VRUN(LOAD4, "addsubps %%xmm1,%%xmm0");
    show("addsubps-geninf");
    /* addsub must not sign-flip a NaN operand on its subtracting lanes */
    A4[0] = 0x3f800000u;
    A4[1] = 0x3f800000u;
    A4[2] = 0x3f800000u;
    A4[3] = 0x3f800000u;
    B4[0] = 0x7fc00001u;
    B4[1] = 0x7fc00002u;
    B4[2] = 0x7fc00003u;
    B4[3] = 0x7fc00004u;
    VRUN(LOAD4, "addsubps %%xmm1,%%xmm0");
    show("addsubps-nan");
    A2[0] = 0x3ff0000000000000ULL;
    A2[1] = 0x3ff0000000000000ULL;
    B2[0] = 0x7ff8000000000001ULL;
    B2[1] = 0x7ff8000000000002ULL;
    VRUN(LOAD2, "addsubpd %%xmm1,%%xmm0");
    show("addsubpd-nan");
    /* horizontal add pairs (high, low) with the HIGH lane as the first operand */
    A2[0] = 0xfff8000000000000ULL;
    A2[1] = 0x7ff8000000000000ULL;
    B2[0] = 0x3ff0000000000000ULL;
    B2[1] = 0x4000000000000000ULL;
    VRUN(LOAD2, "haddpd %%xmm1,%%xmm0");
    show("haddpd-nanpair");
}

/* ---------------------------------------------------------------- comisd/ucomisd flags */
/* x86 clears OF (and SF and AF) for every (U)COMISS/(U)COMISD. ARM FCMP reports "unordered" by
 * SETTING V, which is exactly where this engine keeps x86 OF -- so an unordered compare used to
 * leave OF=1 and mis-steer jo/seto/cmovo and the signed SF!=OF conditions. */
static void comis_flags(void) {
    static const unsigned long long PAIRS[][2] = {
        {0x7ff8000000000000ULL, 0x3ff0000000000000ULL}, /* NaN vs 1.0 -> unordered */
        {0x3ff0000000000000ULL, 0x7ff8000000000000ULL}, /* 1.0 vs NaN -> unordered */
        {0x3ff0000000000000ULL, 0x4000000000000000ULL}, /* 1.0 < 2.0             */
        {0x4000000000000000ULL, 0x4000000000000000ULL}, /* equal                 */
    };
    for (unsigned i = 0; i < 4; i++) {
        unsigned long ovf = 0, flags = 0;
        A2[0] = PAIRS[i][0];
        B2[0] = PAIRS[i][1];
        __asm__ volatile("movsd A2(%%rip), %%xmm0\n movsd B2(%%rip), %%xmm1\n"
                         "ucomisd %%xmm1, %%xmm0\n"
                         "seto %%al\n movzbl %%al, %%eax\n movq %%rax, %0\n"
                         "pushfq\n popq %1"
                         : "=r"(ovf), "=r"(flags)
                         :
                         : "rax", "xmm0", "xmm1", "cc", "memory");
        printf("ucomisd%-11u of=%lu flags=%04lx\n", i, ovf, flags & 0x8d5u);
    }
}

/* ---------------------------------------------------------------- 16-bit partial writes */
/* A 16-bit destination write PRESERVES bits 63:16; only the 32-bit forms zero-extend. */
static void narrow_writes(void) {
    unsigned long r = 0, r2 = 0;
#define SEED "movabsq $0xaaaabbbbccccdddd, %%rax\n movabsq $0x5555666677778888, %%rbx\n"
#define ONE(tag, body)                                                                                                 \
    do {                                                                                                               \
        __asm__ volatile(SEED body "\n movq %%rax, %0" : "=r"(r) : : "rax", "rbx", "rcx", "cc", "memory");             \
        printf("%-18s %016lx\n", tag, r);                                                                              \
    } while (0)
    ONE("movw-mem", "movw mem16(%%rip), %%ax");
    ONE("movw-imm", "movw $0x77, %%ax");
    ONE("movw-regreg", "movw %%bx, %%ax");
    ONE("cmovew-mem", "xorl %%ecx, %%ecx\n cmovew mem16(%%rip), %%ax");
    ONE("cmovew-reg", "xorl %%ecx, %%ecx\n cmovew %%bx, %%ax");
    ONE("popcntw", "popcntw mem16(%%rip), %%ax");
    ONE("bsfw", "bsfw %%bx, %%ax");
    ONE("bsrw", "bsrw %%bx, %%ax");
    ONE("xchgw-reg", "xchgw %%bx, %%ax");
    ONE("xchgw-mem", "xchgw mem16(%%rip), %%ax");
    ONE("leaw", "leaw 4(%%rbx), %%ax");
    ONE("pushpopw", "pushq %%rbx\n popw %%ax\n addq $6, %%rsp");
    /* the 16-bit push/pop pair must move RSP by 2, not 8 */
    __asm__ volatile("movq %%rsp, %%rcx\n pushw %%bx\n popw %%dx\n subq %%rsp, %%rcx\n movq %%rcx, %0"
                     : "=r"(r2)
                     :
                     : "rbx", "rcx", "rdx", "memory");
    printf("%-18s %016lx\n", "pushw-rsp-delta", r2);
#undef ONE
}

/* ---------------------------------------------------------------- cmpxchg / xadd */
/* The 8- and 16-bit REGISTER forms used to be UNIMPL (a hard engine abort), and the flags of
 * every CMPXCHG width were computed as (dest - accumulator) instead of (accumulator - dest). */
static void cmpxchg_xadd(void) {
    unsigned long a, b, d, f;
#define CX(tag, body)                                                                                                  \
    do {                                                                                                               \
        __asm__ volatile("movabsq $0xaaaabbbbccccdddd, %%rax\n"                                                        \
                         "movabsq $0x5555666677778888, %%rbx\n"                                                        \
                         "movabsq $0x1111222233334444, %%rdx\n" body                                                   \
                         "\n movq %%rax,%0\n movq %%rbx,%1\n movq %%rdx,%2\n pushfq\n popq %3"                         \
                         : "=r"(a), "=r"(b), "=r"(d), "=r"(f)                                                          \
                         :                                                                                             \
                         : "rax", "rbx", "rdx", "cc", "memory");                                                       \
        printf("%-18s %016lx %016lx %016lx %04lx\n", tag, a, b, d, f & 0x8d5u);                                        \
    } while (0)
    CX("cmpxchgb-ne", "cmpxchgb %%bl, %%dl");
    CX("cmpxchgb-eq", "movb %%dl, %%al\n cmpxchgb %%bl, %%dl");
    CX("cmpxchgb-hi8", "cmpxchgb %%ah, %%bh");
    CX("cmpxchgw-ne", "cmpxchgw %%bx, %%dx");
    CX("cmpxchgw-eq", "movw %%dx, %%ax\n cmpxchgw %%bx, %%dx");
    /* The 32-bit REGISTER form is deliberately absent: on the failing-comparison path the SDM's
     * `DEST <- DEST` write zero-extends bits 63:32 on hardware, while qemu-user preserves them.
     * That is an oracle disagreement, not a translator property, so it is not pinned here. */
    CX("cmpxchgq", "cmpxchgq %%rbx, %%rdx");
    CX("xaddb", "xaddb %%bl, %%dl");
    CX("xaddb-hi8", "xaddb %%ah, %%bh");
    CX("xaddw", "xaddw %%bx, %%dx");
    CX("xaddw-same", "xaddw %%ax, %%ax");
    CX("xaddq", "xaddq %%rbx, %%rdx");
#undef CX
}

/* ---------------------------------------------------------------- popcnt flags */
/* POPCNT sets ZF from the SOURCE and clears OF, SF, AF, CF and PF -- all of them. */
static void popcnt_flags(void) {
    static const unsigned long SRC[] = {0, 1, 0x8000000000000000ULL, 0xffffffffffffffffULL, 0x0f0f};
    for (unsigned i = 0; i < 5; i++) {
        unsigned long np = 0, res = 0, flags = 0;
        __asm__ volatile("popcntq %3, %0\n setnp %%cl\n movzbl %%cl, %%ecx\n movq %%rcx, %1\n"
                         "pushfq\n popq %2"
                         : "=r"(res), "=r"(np), "=r"(flags)
                         : "r"(SRC[i])
                         : "rcx", "cc");
        printf("popcnt%-12u %2lu np=%lu flags=%04lx\n", i, res, np, flags & 0x8d5u);
    }
}

/* ---------------------------------------------------------------- deferred-flag clobbers */
/* None of these SSE instructions is an x86 flag producer, but each one's lowering writes the
 * host NZCV (a count clamp, a default-NaN probe, an out-of-range probe). With the guest's flags
 * still deferred in that register, the following integer consumer read manufactured flags. */
static void sse_flag_clobber(void) {
    unsigned long r = 0;
    memset(scratch, 0x5a, sizeof scratch);
    /* variable SSE shift between an 8-bit ADC and a carry consumer */
    __asm__ volatile("movabsq $0x00000000000000f0, %%rbx\n movabsq $0x0000000000000020, %%r8\n"
                     "stc\n adcb %%r8b, %%bl\n"
                     "movdqa A4(%%rip), %%xmm5\n movdqa B4(%%rip), %%xmm7\n psraw %%xmm7, %%xmm5\n"
                     "movb $8, %%cl\n rclb %%cl, %%bl\n movq %%rbx, %0"
                     : "=r"(r)
                     :
                     : "rbx", "rcx", "r8", "xmm5", "xmm7", "cc", "memory");
    printf("%-18s %016lx\n", "adc-psraw-rcl", r);
    /* scalar SSE default-NaN probe between a compare and a jcc */
    A2[0] = 0xfff0000000000000ULL;
    B2[0] = 0x7ff0000000000000ULL;
    __asm__ volatile("movq $1, %%rbx\n cmpq $2, %%rbx\n" /* SF=1 */
                     "movsd A2(%%rip), %%xmm0\n movsd B2(%%rip), %%xmm1\n addsd %%xmm1, %%xmm0\n"
                     "movq $0, %%rbx\n js 1f\n movq $7, %%rbx\n 1:\n movq %%rbx, %0"
                     : "=r"(r)
                     :
                     : "rbx", "xmm0", "xmm1", "cc", "memory");
    printf("%-18s %016lx\n", "cmp-addsd-js", r);
    /* cvttsd2si out-of-range probe between a compare and a jcc */
    A2[0] = 0x7ff8000000000000ULL;
    __asm__ volatile("movq $1, %%rbx\n cmpq $2, %%rbx\n" /* SF=1 */
                     "movsd A2(%%rip), %%xmm0\n cvttsd2si %%xmm0, %%rdi\n"
                     "movq $0, %%rbx\n js 1f\n movq $9, %%rbx\n 1:\n movq %%rbx, %0"
                     : "=r"(r)
                     :
                     : "rbx", "rdi", "xmm0", "cc", "memory");
    printf("%-18s %016lx\n", "cmp-cvttsd2si-js", r);
}

int main(void) {
    scalar_upper_lanes();
    packed_converts();
    sse_nan();
    comis_flags();
    narrow_writes();
    cmpxchg_xadd();
    popcnt_flags();
    sse_flag_clobber();
    return 0;
}
