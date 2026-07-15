// translator/guest/x86_64/ops.c -- x86-only block-exit helpers the dispatcher invokes: cpuid emulation
// and the 80-bit x87 load/store (which need a C round-trip, not inline codegen).
#include <math.h> // unity consumers in avx.c use isnan

// ---- W4-C: rep cmps/scas idiom (R_REPSTR) -------------------------------------------------
// One C round-trip does the entire (possibly REP/REPE/REPNE) compare/scan, then writes the exact
// x86 end-state (RCX/RSI/RDI and ZF/SF/CF/OF) back to the cpu struct. The descriptor (cpu->divop)
// carries the direction flag (DF, bit 11), taken from the runtime cpu->df when not statically known:
// DF=0 scans low->high (fast host memcmp/memchr paths), DF=1 (after `std`/popfq) scans high->low via
// the generic per-element loop.
// width-w (a-b) flags -> ARM NZCV, in the engine's borrow convention (stored C = NOT x86 CF),
// byte-identical to what do_alu()/SUBS produces for a normal cmp of the same width.
static uint64_t repstr_nzcv(uint64_t a, uint64_t b, int w) {
    int bits = 8 * w;
    uint64_t mask = (w == 8) ? ~0ull : ((1ull << bits) - 1);
    uint64_t ua = a & mask, ub = b & mask, r = (ua - ub) & mask;
    uint64_t N = r >> (bits - 1);
    uint64_t Z = (r == 0);
    uint64_t C = (ua >= ub); // ARM carry = NO borrow
    // signed overflow of (a-b): sign(a)!=sign(b) AND sign(r)!=sign(a). Bitwise -> works at all widths
    // incl. 64-bit (a naive INT64 negation form was the one bug the qemu oracle caught -- see W4-C md §5).
    uint64_t V = (((ua ^ ub) & (ua ^ r)) >> (bits - 1)) & 1;
    return (N << 31) | (Z << 30) | (C << 29) | (V << 28);
}

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
    c->nzcv = repstr_nzcv(av, bv, w);
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

// x87 80-bit extended <-> double conversion (done in C for reliability; libm-free).
// We emulate the ST stack at double precision, so this loses the 80-bit mantissa tail.
static void x87_fld_m80(struct cpu *c) {
    uint8_t *ea = (uint8_t *)c->x87_ea;
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, ea, 8);
    memcpy(&se, ea + 8, 2);
    int s = se >> 15, e = se & 0x7fff;
    double d;
    if (sig == 0 && e == 0)
        d = 0.0;
    else if (e == 0x7fff) { // ext80 Inf/NaN -> double Inf/NaN (was: NaN silently became Inf)
        uint64_t frac = sig & ((1ull << 63) - 1);        // significand below the explicit integer bit
        uint64_t db = ((uint64_t)s << 63) | (0x7ffull << 52) | (frac ? (1ull << 51) : 0); // NaN keeps quiet bit
        memcpy(&d, &db, 8);
    } else {
        d = (double)sig;        // ~2^63 (ucvtf)
        int k = e - 16383 - 63; // scale exponent
        uint64_t db;
        memcpy(&db, &d, 8);
        int de = (int)((db >> 52) & 0x7ff) + k;
        if (de <= 0)
            d = 0.0;
        else if (de >= 0x7ff) {
            db = (db & (1ull << 63)) | (0x7ffull << 52);
            memcpy(&d, &db, 8);
        } else {
            db = (db & ~(0x7ffull << 52)) | ((uint64_t)de << 52);
            memcpy(&d, &db, 8);
        }
        if (s) d = -d;
    }
    c->fptop = (c->fptop - 1) & 7;
    c->st[c->fptop & 7] = d;
}

static void x87_fstp_m80(struct cpu *c) {
    uint8_t *ea = (uint8_t *)c->x87_ea;
    double d = c->st[c->fptop & 7];
    c->fptop = (c->fptop + 1) & 7;
    uint64_t db;
    memcpy(&db, &d, 8);
    int s = (int)(db >> 63), de = (int)((db >> 52) & 0x7ff);
    uint64_t dm = db & ((1ull << 52) - 1);
    uint64_t sig;
    uint16_t se;
    if (de == 0) { // double zero (or subnormal, flushed): ext80 zero, sign preserved
        sig = 0;
        se = (uint16_t)(s ? 0x8000 : 0);
    } else if (de == 0x7ff) { // double Inf/NaN -> ext80 Inf/NaN (exp all-ones), NOT a rebiased finite value.
        // (Was: e80 = 0x7ff-1023+16383 = 0x43FF, silently writing ~2^1024 instead of Inf.) Integer bit set;
        // frac carries the mantissa so a QNaN stays quiet and Inf (dm==0) writes 0x8000000000000000.
        sig = (1ull << 63) | (dm << 11);
        se = (uint16_t)((s << 15) | 0x7fff);
    } else {
        sig = (1ull << 63) | (dm << 11);
        int e80 = de - 1023 + 16383;
        se = (uint16_t)((s << 15) | (e80 & 0x7fff));
    }
    memcpy(ea, &sig, 8);
    memcpy(ea + 8, &se, 2);
}

// Pure double<->ext80 conversions (same math as x87_fld_m80/x87_fstp_m80, without the stack push/pop) for
// the fxsave/fxrstor x87-register-DATA area. The ST stack is modeled at double precision, so an ext80 value
// with a mantissa tail beyond binary64 is preserved only to double precision (the documented arm64 limit).
static void x87_double_to_m80(double d, uint8_t *ea) {
    uint64_t db;
    memcpy(&db, &d, 8);
    int s = (int)(db >> 63), de = (int)((db >> 52) & 0x7ff);
    uint64_t dm = db & ((1ull << 52) - 1), sig;
    uint16_t se;
    if (de == 0) {
        sig = 0;
        se = (uint16_t)(s ? 0x8000 : 0);
    } else if (de == 0x7ff) {
        sig = (1ull << 63) | (dm << 11);
        se = (uint16_t)((s << 15) | 0x7fff);
    } else {
        sig = (1ull << 63) | (dm << 11);
        se = (uint16_t)((s << 15) | ((de - 1023 + 16383) & 0x7fff));
    }
    memcpy(ea, &sig, 8);
    memcpy(ea + 8, &se, 2);
}
static double x87_m80_to_double(const uint8_t *ea) {
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, ea, 8);
    memcpy(&se, ea + 8, 2);
    int s = se >> 15, e = se & 0x7fff;
    double d;
    if (sig == 0 && e == 0)
        d = 0.0;
    else if (e == 0x7fff) {
        uint64_t frac = sig & ((1ull << 63) - 1);
        uint64_t db = ((uint64_t)s << 63) | (0x7ffull << 52) | (frac ? (1ull << 51) : 0);
        memcpy(&d, &db, 8);
    } else {
        d = (double)sig;
        int k = e - 16383 - 63;
        uint64_t db;
        memcpy(&db, &d, 8);
        int de = (int)((db >> 52) & 0x7ff) + k;
        if (de <= 0)
            d = 0.0;
        else if (de >= 0x7ff) {
            db = (db & (1ull << 63)) | (0x7ffull << 52);
            memcpy(&d, &db, 8);
        } else {
            db = (db & ~(0x7ffull << 52)) | ((uint64_t)de << 52);
            memcpy(&d, &db, 8);
        }
        if (s) d = -d;
    }
    return d;
}

// fxsave/fxrstor x87-register-DATA + FSW (R_FXSAVE/R_FXRSTOR). The XMM lanes, MXCSR and FCW are handled by
// the inline emitter; this C tail fills the parts that need the modeled x87 stack (c->st[]/fptop/fpsw). The
// FXSAVE area base is stashed in c->x87_ea. Physical register j lives at offset 32 + j*16 (10 bytes ext80);
// FSW@2 carries C0-C3 + the top-of-stack pointer; the abridged FTW@4 marks registers valid (empty-tag
// state is not modeled, so all 8 are reported present -- exact for the common save-all-then-restore round trip).
static void do_fxsave(struct cpu *c) {
    uint8_t *p = (uint8_t *)c->x87_ea;
    uint32_t mxcsr = 0x1f80;
#if defined(__aarch64__)
    uint64_t fpcr, fpsr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    uint32_t arm_rc = (uint32_t)((fpcr >> 22) & 3u);
    uint32_t x86_rc = ((arm_rc & 1u) << 1) | ((arm_rc >> 1) & 1u);
    static const unsigned fpsr_bit[6] = {0, 7, 1, 2, 3, 4};
    mxcsr |= x86_rc << 13;
    for (unsigned bit = 0; bit < 6; ++bit) mxcsr |= (uint32_t)((fpsr >> fpsr_bit[bit]) & 1u) << bit;
#endif
    memcpy(p, &c->fpcw, 2);
    uint16_t fsw = (uint16_t)((c->fpsw & 0x4700) | ((c->fptop & 7) << 11));
    memcpy(p + 2, &fsw, 2); // FSW: C0-C3 (bits 8/9/10/14) + TOP (bits 11-13)
    p[4] = 0xff;            // abridged FTW: all registers reported valid (see note above)
    p[5] = 0;
    for (int j = 0; j < 8; j++)
        x87_double_to_m80(c->st[j], p + 32 + j * 16);
    memcpy(p + 24, &mxcsr, sizeof(mxcsr));
    memcpy(p + 160, c->v, 16 * 16);
}
static void do_fxrstor(struct cpu *c) {
    const uint8_t *p = (const uint8_t *)c->x87_ea;
    uint16_t fsw;
    uint16_t fcw;
    uint32_t mxcsr;
    memcpy(&fcw, p, sizeof(fcw));
    memcpy(&fsw, p + 2, 2);
    memcpy(&mxcsr, p + 24, sizeof(mxcsr));
    c->fpcw = fcw;
    c->fpsw = fsw & 0x4700;    // restore C0-C3
    c->fptop = (fsw >> 11) & 7; // restore TOP
    for (int j = 0; j < 8; j++)
        c->st[j] = x87_m80_to_double(p + 32 + j * 16);
    memcpy(c->v, p + 160, 16 * 16);
#if defined(__aarch64__)
    uint64_t fpcr, fpsr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    uint32_t x86_rc = (mxcsr >> 13) & 3u;
    uint32_t arm_rc = ((x86_rc & 1u) << 1) | ((x86_rc >> 1) & 1u);
    fpcr = (fpcr & ~(UINT64_C(3) << 22)) | (uint64_t)arm_rc << 22;
    static const unsigned fpsr_bit[6] = {0, 7, 1, 2, 3, 4};
    fpsr &= ~UINT64_C(0x9f);
    for (unsigned bit = 0; bit < 6; ++bit) fpsr |= (uint64_t)((mxcsr >> bit) & 1u) << fpsr_bit[bit];
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
#endif
}

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
