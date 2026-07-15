// translator/guest/x86_64/ops.c -- remaining x86-only block-exit helpers used by the production dispatcher.
#include "x87state.h"

// ---- W4-C: rep cmps/scas idiom (R_REPSTR) -------------------------------------------------
// One C round-trip does the entire (possibly REP/REPE/REPNE) compare/scan, then writes the exact
// x86 end-state (RCX/RSI/RDI and ZF/SF/CF/OF) back to the cpu struct. The descriptor (cpu->divop)
// carries the direction flag (DF, bit 11), taken from the runtime cpu->df when not statically known:
// DF=0 scans low->high (fast host memcmp/memchr paths), DF=1 (after `std`/popfq) scans high->low via
// the generic per-element loop.
// width-w (a-b) flags -> ARM NZCV, in the engine's borrow convention (stored C = NOT x86 CF),
// byte-identical to what do_alu()/SUBS produces for a normal cmp of the same width.
static void do_repstr(struct cpu *c) {
    uint64_t d = c->divop;
    int w = (int)(d & 0xff);
    int isscas = (d >> 8) & 1, isrepne = (d >> 9) & 1, isrep = (d >> 10) & 1, df = (d >> 11) & 1;
    uint64_t n = isrep ? c->r[RCX] : 1; // REP uses RCX; a bare cmps/scas does exactly one step
    if (n == 0) return;                 // REP with RCX==0: no element executed, flags+pointers UNCHANGED
    // W6A/non-PIE: a biased ET_EXEC guest pointer may still hold a low LINK address -- e.g. rdi loaded via
    // `mov edi,imm32` pointing at a baked .rodata string (node20's non-PIE argv/flag parser emits
    // `mov edi,<flagstr>; rep cmpsb`). This helper dereferences rsi/rdi DIRECTLY from C (memcmp/memchr/typed
    // loads), so a low link address must be rebased to the real high mapping first -- the single-access fault
    // path nonpie_fixup cannot serve a bulk memcmp. rep movs/stos already do this (repstr_g2h); cmps/scas did
    // not -> the low deref SIGSEGV'd (node:20 x86 `node --version`). Bias ONLY the dereferenced pointers;
    // the RSI/RDI write-back below advances the GUEST (unbiased) values. Inert for PIE/static-PIE (g_nonpie_lo
    // == 0 makes repstr_g2h the identity), so no behavior change for the common case.
    uint64_t gsi = c->r[RSI], gdi = c->r[RDI];             // guest (unbiased) pointers for the write-back
    uint64_t rsi = repstr_g2h(gsi), rdi = repstr_g2h(gdi); // host pointers for the dereferences
    int64_t step = df ? -(int64_t)w : (int64_t)w;          // DF=1 (std) scans high->low, DF=0 low->high
    uint64_t wmask = (w == 8) ? ~0ull : ((1ull << (8 * w)) - 1);
    uint64_t acc = c->r[RAX] & wmask; // scas accumulator (AL/AX/EAX/RAX)
    int stop_on_equal = isrepne;      // REPNE stops at first equal; REPE stops at first not-equal
    uint64_t k = 0, av = 0, bv = 0;
    if (!df && !norepcmp() && isrep && w == 1) { // ---- fast host scan (the lever; forward only) ----
        if (!isscas) {                           // rep cmps byte  -> memcmp-style first-difference scan
            const uint8_t *pa = (const uint8_t *)rsi, *pb = (const uint8_t *)rdi;
            if (!stop_on_equal) { // REPE: stop at first diff -> memcmp tests equality fast,
                if (memcmp(pa, pb, n) == 0)
                    k = n; // then a bounded scan locates the mismatch byte.
                else {
                    size_t i = 0;
                    while (pa[i] == pb[i])
                        i++;
                    k = i + 1;
                }
            } else { // REPNE: stop at first equal (rare)
                size_t i = 0;
                while (i < n && pa[i] != pb[i])
                    i++;
                k = (i < n) ? i + 1 : n;
            }
            av = pa[k - 1];
            bv = pb[k - 1];
        } else { // scas byte: REPNE -> memchr (strlen/memchr), REPE -> run-scan
            const uint8_t *pb = (const uint8_t *)rdi;
            uint8_t cc = (uint8_t)acc;
            if (stop_on_equal) {
                const uint8_t *hit = memchr(pb, cc, n);
                k = hit ? (uint64_t)(hit - pb) + 1 : n;
            } else {
                size_t i = 0;
                while (i < n && pb[i] == cc)
                    i++;
                k = (i < n) ? i + 1 : n;
            }
            av = acc;
            bv = pb[k - 1];
        }
    } else if (!df && !norepcmp() && isrep && !isscas) { // rep cmps word/dword/qword (forward)
        if (!stop_on_equal) {                            // REPE: memcmp tests equality fast, then locate the element
            if (memcmp((void *)rsi, (void *)rdi, n * (size_t)w) == 0)
                k = n;
            else {
                size_t i = 0;
                while (hl_x86_operand_read(rsi + i * w, w) == hl_x86_operand_read(rdi + i * w, w))
                    i++;
                k = i + 1;
            }
        } else {
            size_t i = 0;
            while (i < n && hl_x86_operand_read(rsi + i * w, w) != hl_x86_operand_read(rdi + i * w, w))
                i++;
            k = (i < n) ? i + 1 : n;
        }
        av = hl_x86_operand_read(rsi + (k - 1) * w, w);
        bv = hl_x86_operand_read(rdi + (k - 1) * w, w);
    } else if (!df && !norepcmp() && isrep) { // rep scas word/dword/qword: typed loop (forward)
        size_t i = 0;
        if (stop_on_equal)
            while (i < n && (hl_x86_operand_read(rdi + i * w, w) & wmask) != acc)
                i++;
        else
            while (i < n && (hl_x86_operand_read(rdi + i * w, w) & wmask) == acc)
                i++;
        k = (i < n) ? i + 1 : n;
        av = acc;
        bv = hl_x86_operand_read(rdi + (k - 1) * w, w);
    } else {                                   // generic per-element loop: NOREPCMP oracle, bare
        for (;;) {                             // cmps/scas, OR any DF=1 (backward) scan
            uint64_t off = k * (uint64_t)step; // signed stride (forward +w, backward -w), modular
            if (isscas) {
                av = acc;
                bv = hl_x86_operand_read(rdi + off, w);
            } else {
                av = hl_x86_operand_read(rsi + off, w);
                bv = hl_x86_operand_read(rdi + off, w);
            }
            k++;
            int eq = ((av & wmask) == (bv & wmask));
            if (k >= n) break;
            if (stop_on_equal ? eq : !eq) break;
        }
    }
    if (isrep) c->r[RCX] = n - k;
    if (!isscas) c->r[RSI] = gsi + k * (uint64_t)step; // advance the GUEST pointers (not the host-biased ones)
    c->r[RDI] = gdi + k * (uint64_t)step;
    c->nzcv = hl_x86_sub_nzcv(av, bv, w);
    g_repstr_n++;
    g_repstr_elems += k;
}

// ---- RCL/RCR by CL (R_RCL) ---------------------------------------------------------------
// Rotate-through-carry with a runtime count. Descriptor in cpu->divop:
//   [0:8]=w(1/2/4/8)  bit8=rcr  bit9=is_mem  bit10=hi8(byte high reg AH/CH/DH/BH)  [16:21]=r/m reg.
// A memory operand's already-host-biased effective address is in cpu->x87_ea. Carry-in/out use cpu->nzcv
// in the engine's borrow convention (stored ARM C = NOT x86 CF) -- byte-identical to the inline
// emit_rcl_rcr() constant-count path and the membank flag ABI, so the following block reloads flags
// verbatim. x86: count is masked to 5 bits (operand size 8/16/32) or 6 bits (64), then MOD (width+1);
// a masked count of 0 changes NO flags; OF is defined only when the masked count is exactly 1.
// CPUID emulation. We advertise EXACTLY the feature set the engine actually translates (legacy-SSE in
// emit.c + the 0F38/0F3A SSSE3/SSE4/AES/PCLMUL/SHA/CRC32/MOVBE and BMI lanes in avx.c do_sse3b/do_avx),
// mirroring a real x86-64 baseline. We deliberately do NOT advertise AVX/AVX2/FMA/F16C/XSAVE/OSXSAVE: those
// are gated on YMM state being enabled in XCR0, but our xgetbv reports only x87+SSE (translate.c), so a
// conformant guest would correctly decline them anyway -- advertising them would only mislead.
// Coverage: leaf 0 (max std leaf 7 + vendor), leaf 1 (EDX/ECX baseline feature bits), leaf 7 subleaf 0
// (BMI1/BMI2/ERMS/SHA + FSRM), and the extended range 0x80000000..0x80000008 (LM/SYSCALL/NX/RDTSCP/LAHF,
// the 48-byte brand string, invariant TSC, and address sizes). The exact bit-for-bit set is mirrored into
// /proc/cpuinfo's `flags:` line (os/linux/container/vfs.c cpuinfo_x86_block) so CPUID and /proc agree.

// x87 transcendentals (R_X87FUNC): the D9 F0-FF subset has no ARM/SSE counterpart, so it is computed
// here on the double-precision ST stack via host libm. cpu->x87_ea carries the X87_* selector. We
// track no tag bits, so C1 (stack over/underflow) is cleared on success; C2 (argument out of range,
// |x| >= 2^63) is set for the trig ops exactly as the hardware does, leaving the operand untouched.
#if 0 /* Compiled independently in x87math.c. */
static void x87_push_d(struct cpu *c, double v) {
    c->fptop = (c->fptop - 1) & 7;
    c->st[c->fptop & 7] = v;
}

static void x87_func(struct cpu *c) {
    double st0 = c->st[c->fptop & 7];
    double st1 = c->st[(c->fptop + 1) & 7];
    c->fpsw &= ~0x4700ull; // clear C0/C1/C2/C3 (bits 8/9/10/14)
    switch (c->x87_ea) {
    case X87_F2XM1: // ST0 = 2^ST0 - 1
        c->st[c->fptop & 7] = exp2(st0) - 1.0;
        break;
    case X87_FYL2X: // ST1 = ST1 * log2(ST0); pop -> result in ST0
        c->st[(c->fptop + 1) & 7] = st1 * log2(st0);
        c->fptop = (c->fptop + 1) & 7;
        break;
    case X87_FPATAN: // ST1 = atan2(ST1, ST0); pop
        c->st[(c->fptop + 1) & 7] = atan2(st1, st0);
        c->fptop = (c->fptop + 1) & 7;
        break;
    case X87_FYL2XP1: // ST1 = ST1 * log2(ST0 + 1); pop
        c->st[(c->fptop + 1) & 7] = st1 * log2(st0 + 1.0);
        c->fptop = (c->fptop + 1) & 7;
        break;
    case X87_FPTAN: // ST0 = tan(ST0); push 1.0
        if (fabs(st0) >= 0x1p63) {
            c->fpsw |= 0x400;
            break;
        }
        c->st[c->fptop & 7] = tan(st0);
        x87_push_d(c, 1.0);
        break;
    case X87_FSINCOS: // ST0 = sin(ST0); push cos -> ST0=cos, ST1=sin
        if (fabs(st0) >= 0x1p63) {
            c->fpsw |= 0x400;
            break;
        }
        c->st[c->fptop & 7] = sin(st0);
        x87_push_d(c, cos(st0));
        break;
    case X87_FSIN:
        if (fabs(st0) >= 0x1p63) {
            c->fpsw |= 0x400;
            break;
        }
        c->st[c->fptop & 7] = sin(st0);
        break;
    case X87_FCOS:
        if (fabs(st0) >= 0x1p63) {
            c->fpsw |= 0x400;
            break;
        }
        c->st[c->fptop & 7] = cos(st0);
        break;
    }
}
#endif
