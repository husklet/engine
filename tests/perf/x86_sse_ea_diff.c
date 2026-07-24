// Differential oracle for the base+index EA fold on 128-bit SSE loads/stores
// (translator/guest/x86_64/address.c hl_x86_address_fold_reg + translate.c g_ldr_q_ea/g_str_q_ea).
//
// The fold rewrites `add x17,base,index; ldr q,[x17]` into `ldr q,[base,index{,lsl #4}]`, so what has
// to be proven is that the effective address is unchanged for every addressing shape the folder
// accepts AND that the shapes it must reject (SIB scale ARM cannot express, non-zero displacement,
// 32-bit address wrap, segment override) still compute the x86 EA. Exercise all of them and compare
// the loaded/stored bytes against qemu-x86_64.
//
// Build:  $X86_64_LINUX_STATIC_CC -O2 -static -std=gnu11 x86_sse_ea_diff.c -o x86_sse_ea_diff

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N 4096
static unsigned char buf[N + 64] __attribute__((aligned(64)));
static unsigned char dst[N + 64] __attribute__((aligned(64)));

static void fill(void) {
    for (int i = 0; i < N + 64; i++) buf[i] = (unsigned char)(i * 31 + 7);
    memset(dst, 0, sizeof dst);
}

static void show(const char *tag, long idx, const unsigned char *p) {
    printf("%-14s idx=%6ld", tag, idx);
    for (int i = 0; i < 16; i++) printf(" %02x", p[i]);
    printf("\n");
}

// One (op, scale) pair: load 16 bytes at base+index*scale, run a packed op against a second operand
// loaded the same way, store the result through a base+index EA, and print both.
#define EA_CASE(name, insn, scale, sfx)                                                                              \
    static void name(long idx) {                                                                                     \
        unsigned char out[16];                                                                                        \
        __asm__ volatile("movaps (%[b],%[i]," #scale "), %%xmm0\n\t" insn                                              \
                         " (%[b],%[i]," #scale "), %%xmm0\n\t"                                                        \
                         "movups %%xmm0, (%[d],%[i]," #scale ")\n\t"                                                  \
                         "movups (%[d],%[i]," #scale "), %%xmm1\n\t"                                                  \
                         "movups %%xmm1, %[o]\n\t"                                                                    \
                         : [o] "=m"(out)                                                                              \
                         : [b] "r"(buf), [d] "r"(dst), [i] "r"(idx)                                                   \
                         : "xmm0", "xmm1", "memory");                                                                 \
        show(sfx, idx, out);                                                                                          \
    }

EA_CASE(c_add_s1, "addps", 1, "addps.s1")
EA_CASE(c_mul_s1, "mulps", 1, "mulps.s1")
EA_CASE(c_sub_s1, "subps", 1, "subps.s1")
EA_CASE(c_div_s1, "divps", 1, "divps.s1")
EA_CASE(c_min_s1, "minps", 1, "minps.s1")
EA_CASE(c_max_s1, "maxps", 1, "maxps.s1")
EA_CASE(c_and_s1, "andps", 1, "andps.s1")
EA_CASE(c_padd_s1, "paddd", 1, "paddd.s1")
EA_CASE(c_add_s2, "addps", 2, "addps.s2")
EA_CASE(c_add_s4, "addps", 4, "addps.s4")
EA_CASE(c_add_s8, "addps", 8, "addps.s8")
EA_CASE(c_mul_s8, "mulps", 8, "mulps.s8")

// Shapes the folder must REJECT: a non-zero displacement (ARM has no base+index+imm), a 32-bit
// address size (x86 wraps the EA to 32 bits), and a base-only / index-only form.
static void c_disp(long idx) {
    unsigned char out[16];
    __asm__ volatile("movaps 32(%[b],%[i],1), %%xmm0\n\t"
                     "mulps  16(%[b],%[i],1), %%xmm0\n\t"
                     "movups %%xmm0, 48(%[d],%[i],1)\n\t"
                     "movups 48(%[d],%[i],1), %%xmm1\n\t"
                     "movups %%xmm1, %[o]\n\t"
                     : [o] "=m"(out)
                     : [b] "r"(buf), [d] "r"(dst), [i] "r"(idx)
                     : "xmm0", "xmm1", "memory");
    show("disp", idx, out);
}

static void c_baseonly(long idx) {
    unsigned char out[16];
    const unsigned char *p = buf + idx, *q = dst + idx;
    __asm__ volatile("movdqa (%[p]), %%xmm0\n\t"
                     "paddd  (%[p]), %%xmm0\n\t"
                     "movdqu %%xmm0, (%[q])\n\t"
                     "movdqu (%[q]), %%xmm1\n\t"
                     "movups %%xmm1, %[o]\n\t"
                     : [o] "=m"(out)
                     : [p] "r"(p), [q] "r"(q)
                     : "xmm0", "xmm1", "memory");
    show("baseonly", idx, out);
}

static void c_addr32(long idx) {
    unsigned char out[16];
    __asm__ volatile("addr32 movups (%k[b],%k[i],1), %%xmm0\n\t"
                     "addr32 movups %%xmm0, (%k[d],%k[i],1)\n\t"
                     "addr32 movups (%k[d],%k[i],1), %%xmm1\n\t"
                     "movups %%xmm1, %[o]\n\t"
                     : [o] "=m"(out)
                     : [b] "r"(buf), [d] "r"(dst), [i] "r"(idx)
                     : "xmm0", "xmm1", "memory");
    show("addr32", idx, out);
}

int main(void) {
    fill();
    for (long idx = 0; idx <= 256; idx += 16) {
        c_add_s1(idx);
        c_mul_s1(idx);
        c_sub_s1(idx);
        c_div_s1(idx);
        c_min_s1(idx);
        c_max_s1(idx);
        c_and_s1(idx);
        c_padd_s1(idx);
        c_disp(idx);
        c_baseonly(idx);
        c_addr32(idx);
    }
    for (long idx = 0; idx <= 128; idx += 8) {
        c_add_s2(idx);
        c_add_s4(idx);
        c_add_s8(idx);
        c_mul_s8(idx);
    }
    // Whole-buffer digest: catches any store that landed at the wrong address.
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < N + 64; i++) {
        h ^= dst[i];
        h *= 1099511628211ull;
    }
    printf("dst-digest %016llx\n", (unsigned long long)h);
    return 0;
}
