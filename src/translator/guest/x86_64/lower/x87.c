// translator/guest/x86_64 -- x87 translate-time stack-top (fptop) tracking.
// The baseline x87 path keeps ST(0..7) in cpu->st[] with the live top in cpu->fptop, and every
// ST(i) touch recomputes the wrapped slot at runtime (e_st_addr: ldr fptop; add #i; and #7; add
// base; add idx,lsl#3 -- 5 insns) while each push/pop does a ldr/modify/str of cpu->fptop.
//
// When the absolute top is statically known at translate time we instead:
//   * resolve ST(i) to its concrete shadow-stack slot and address it with ONE `add xa,x28,#off`,
//   * keep push/pop as pure translate-time bookkeeping (no cpu->fptop traffic), writing the shadow
//     back to cpu->fptop only when guest state may escape (a faulting guest memory access, a C-helper
//     exit, or any non-x87 instruction / block boundary) -- exactly as lazy flags spill to cpu->nzcv.
// Storage stays cpu->st[] at double precision, the ops/condition codes/fpsw paths are untouched, so
// results are bit-identical to the baseline; only the addressing of ST(i) and the timing of the
// cpu->fptop store change, and the escape-point materialize keeps cpu->fptop observably current.
//
// ===== H11 (KNOWN GAP, NOT fixed here): x87 stack is 64-bit double, not 80-bit extended ==========
// The architectural x87 register file is 80-bit extended precision (64-bit explicit mantissa, 15-bit
// exponent). This engine carries ST(0..7) as IEEE-754 binary64 in cpu->st[] (see hl_x86_x87_load/hl_x86_x87_store here
// and hl_x86_x87_load_ext80/hl_x86_x87_store_ext80_pop in x87state.c). Precisely what that loses:
//   * mantissa: 64 explicit bits -> 52 -> every D8-DF arithmetic op (fadd/fmul/fsub/fdiv/fsqrt/frndint/
//     fprem/fscale/fxtract above) and every transcendental (x87_func) rounds each intermediate to 53
//     significant bits instead of 64, so long chains of x87 math and C `long double` computations drift
//     in the low ~11 bits vs a real FPU;
//   * exponent: 15-bit (range ~1e+-4932) -> 11-bit (~1e+-308), so values with |exp| beyond binary64's
//     range flush to 0/Inf where a real x87 would keep them (fscale clamps to the binary64 exponent);
//   * round-trips: FLD m80 / FSTP m80 (the x87state.c helpers) narrow to double on load and re-widen a
//     53-bit value on store, so an 80-bit value written by the guest and read back loses its tail;
//     `printf("%Lf", ...)`, C `long double`, and 80-bit `fldt/fstpt` object files see the drift.
// A true fix needs an 80-bit (or software-emulated ext80) carrier for cpu->st[] plus reworking every op
// and both m80 converters -- a large, cross-cutting change spanning ops.c (outside this file's
// cluster). It is NOT attempted here, and cannot be cheaply approximated on this host: the macOS/arm64
// build ABI makes `long double` == `double` (64-bit), so widening the host carrier to `long double`
// would buy nothing. Related: #248/#249. Everything below is deliberately double-precision-limited.
//
// The top is "known" only after a `finit` anchors it (top=0) within an unbroken run of x87
// instructions; any non-x87 instruction ends the run (materialize + drop to the runtime model), and
// any x87 op we cannot statically track falls back to the baseline helpers. NOX87OPT forces the
// runtime-top path everywhere -> byte-identical to the pre-opt engine.
#include "x87.h"

#include "../cpu.h"
#include "../encoding.h"
#include "primitives.h"
#include "x87_stack.h"

static struct hl_x87_stack g_fp_stack;

int hl_x86_x87_optimized(void) {
    return 1;
}

void hl_x86_x87_reset(void) {
    hl_x87_stack_reset(&g_fp_stack);
}

void hl_x86_x87_anchor(unsigned top) {
    hl_x87_stack_anchor(&g_fp_stack, top);
}

// &cpu->st[shadow slot i] -> xdst, single add (OFF_ST + slot*8 fits the add imm12).
static void fp_slot_addr(int xdst, int i) {
    unsigned off = (unsigned)OFF_ST + hl_x87_stack_slot(&g_fp_stack, i) * 8u;
    emit32(0x91000000u | (off << 10) | (28u << 5) | (unsigned)xdst); // add xdst, x28, #off
}

// Make cpu->fptop reflect the shadow (idempotent). Keeps the shadow known.
void hl_x86_x87_materialize(void) {
    if (hl_x87_stack_known(&g_fp_stack) && hl_x87_stack_dirty(&g_fp_stack)) {
        e_movconst(16, (uint64_t)hl_x87_stack_top(&g_fp_stack));
        e_str(16, 28, OFF_FPTOP);
        hl_x87_stack_materialized(&g_fp_stack);
    }
}

// Leave the static-top model (run boundary / untrackable op): spill the shadow, go runtime-top.
void hl_x86_x87_drop(void) {
    hl_x86_x87_materialize();
    hl_x87_stack_drop(&g_fp_stack);
}

int hl_x86_x87_known(void) {
    return hl_x87_stack_known(&g_fp_stack);
}

#define FP_STATIC (hl_x86_x87_optimized() && hl_x86_x87_known())

void hl_x86_x87_load(int vd, int i) { // vd = ST(i)
    if (FP_STATIC) {
        fp_slot_addr(17, i);
        g_ldr_d(vd, 17);
    } else
        e_fp_ld(vd, i);
}

void hl_x86_x87_store(int vs, int i) { // ST(i) = vs
    if (FP_STATIC) {
        fp_slot_addr(17, i);
        g_str_d(vs, 17);
    } else
        e_fp_st(vs, i);
}

void hl_x86_x87_push(int vs) { // push vs -> ST(0)  (top -= 1)
    if (FP_STATIC) {
        hl_x87_stack_push(&g_fp_stack);
        fp_slot_addr(17, 0);
        g_str_d(vs, 17);
    } else
        e_fp_push(vs);
}

void hl_x86_x87_adjust_top(int delta) { // top += delta  (pop = +1)
    if (FP_STATIC) {
        hl_x87_stack_adjust(&g_fp_stack, delta);
    } else
        e_fp_settop(delta);
}

#undef FP_STATIC

// ===== x87 D9 Fx remainder / scale / extract group (computed on host doubles) ===========
// These ops have no SSE counterpart; emulate them on f64 with the same write-back-to-cpu->st[]
// and FPSW condition-code conventions as the inline D8-DF arithmetic. Scratch: GP x16/x17/x19/x21,
// FP v16/v17/v18 (v0..v15 are guest xmm; v16+ are free). The
// hl_x86_x87_load/hl_x86_x87_store/hl_x86_x87_push/hl_x86_x87_adjust_top calls keep the translate-time static-top
// shadow consistent exactly like the surrounding ops.

// x87 GENERATED-NaN sign fixup -- the scalar-double analogue of emit_dnan_pre/post (SSE).
// x87 invalid operations (fsqrt of a negative, 0/0, inf/inf, inf-inf, 0*inf) deliver the QNaN
// floating-point INDEFINITE with the sign bit SET (0xFFF8000000000000); ARM's FADD/FSUB/FMUL/FDIV/
// FSQRT deliver the DEFAULT NaN with the sign CLEAR -- identical payload, opposite sign. A NaN
// PROPAGATED from an input keeps that input's sign on both ISAs, so only a GENERATED NaN may be
// stamped: "result is NaN AND no input was NaN". PRE runs while both inputs are still live (the
// ARM forms are destructive, so the mask cannot be built afterwards); POST runs on the result.
// Scratch: v22/v23, which nothing in the D8-DF lowering (v16/v17/v18/v20 only) uses.
#define FCMEQd(d, n, m) emit32(0x5E60E400u | ((m) << 16) | ((n) << 5) | (d)) /* FCMEQ Dd,Dn,Dm */

void hl_x86_x87_dnan_pre(int n, int m) {
    FCMEQd(22, n, n);                                  // d22 = (n == n)   (all-ones iff n is NOT NaN)
    FCMEQd(23, m, m);                                  // d23 = (m == m)
    emit32(0x0E201C00u | (23 << 16) | (22 << 5) | 22); // AND v22.8b, v22, v23  -> both inputs ordered
}

void hl_x86_x87_dnan_post(int d) {
    FCMEQd(23, d, d);                                    // d23 = (result == result)
    emit32(0x0E601C00u | (23 << 16) | (22 << 5) | 22);   // BIC v22.8b -> ordered inputs AND NaN result
    emit32(0x4F005400u | (127u << 16) | (22 << 5) | 22); // SHL v22.2d, v22, #63 -> the sign bit, or 0
    emit32(0x0EA01C00u | (22 << 16) | ((d) << 5) | (d)); // ORR v_d.8b -> stamp x86's negative indefinite
}

// FPREM (round-to-zero) / FPREM1 (round-to-nearest-even): ST0 = ST0 - ST1*Q, Q = round(ST0/ST1).
// The reduction is completed in one fused step, so C2<-0 ("reduction complete"). FPREM also publishes
// the low three bits of |Q| (C0<-Q2, C3<-Q1, C1<-Q0); FPREM1 leaves the quotient bits cleared -- both
// matching qemu's helper_fprem and the `do { fprem } while (C2)` loop libc wraps around fmod/remainder.
void hl_x86_x87_remainder(int ieee) {
    hl_x86_x87_load(18, 0);                                         // d18 = ST0
    hl_x86_x87_load(16, 1);                                         // d16 = ST1
    hl_x86_x87_dnan_pre(18, 16);                                    // generated NaN (x/0, inf%y) -> negative
    emit32(0x1E601800u | (16 << 16) | (18 << 5) | 17);              // fdiv  d17, d18, d16
    emit32((ieee ? 0x1E644000u : 0x1E65C000u) | (17 << 5) | 17);    // frintn/frintz d17, d17  (= Q)
    emit32(0x1F408000u | (16 << 16) | (18 << 10) | (17 << 5) | 18); // fmsub d18, d17, d16, d18 (ST0-Q*ST1)
    hl_x86_x87_dnan_post(18);
    hl_x86_x87_store(18, 0);
    e_ldr(16, 28, OFF_FPSW);
    e_movconst(19, ~(uint64_t)0x4700);
    e_rrr(A_AND, 16, 16, 19, 1, 0);           // clear C0/C1/C2/C3 (C2 stays 0 -> reduction complete)
    if (!ieee) {                              // FPREM: quotient bits from the magnitude of Q
        emit32(0x1E60C000u | (17 << 5) | 17); // fabs   d17, d17  (|Q|)
        emit32(0x9E780000u | (17 << 5) | 17); // fcvtzs x17, d17  (|Q| as integer)
        e_bfi(16, 17, 9, 1, 1);               // C1 (bit 9)  <- Q bit0
        e_lsr_i(19, 17, 1, 1);
        e_bfi(16, 19, 14, 1, 1); // C3 (bit 14) <- Q bit1
        e_lsr_i(19, 17, 2, 1);
        e_bfi(16, 19, 8, 1, 1); // C0 (bit 8)  <- Q bit2
    }
    e_str(16, 28, OFF_FPSW);
}

// FSCALE: ST0 = ST0 * 2^trunc(ST1). Build 2^n straight into the double exponent field; clamping the
// biased exponent to [0,2047] gives +0.0 on underflow and +Inf on overflow, matching scalbn.
void hl_x86_x87_scale(void) {
    hl_x86_x87_load(18, 0);      // d18 = ST0
    hl_x86_x87_load(16, 1);      // d16 = ST1
    hl_x86_x87_dnan_pre(18, 16); // inf*0 -> x86's NEGATIVE indefinite, not ARM's default NaN
    // FSCALE of a NON-FINITE or ZERO ST0 is the IDENTITY (scalbn(+-inf,n)=+-inf, scalbn(+-0,n)=+-0,
    // scalbn(NaN,n)=NaN) for EVERY n. The exponent clamp below cannot express that: a large negative
    // ST1 clamps 2^n to +0.0, so inf*0 came out as the indefinite instead of inf (and symmetrically a
    // large positive ST1 turned +-0 into a NaN via 0*inf). Capture ST0 and the "not finite, or zero"
    // predicate now, and select ST0 back over the product at the end.
    emit32(0x1E60C000u | (18 << 5) | 19); // fabs  d19, d18            (|ST0|)
    e_movconst(20, 0x7ff0000000000000ull);
    e_fmov_to_d(20, 20);                               // d20 = +inf
    emit32(0x7EE0E400u | (19 << 16) | (20 << 5) | 21); // FCMGT d21, d20, d19       (|ST0| < inf, ordered)
    emit32(0x2E205800u | (21 << 5) | 21);              // NOT   v21.8b              -> inf or NaN
    emit32(0x5EE0D800u | (19 << 5) | 20);              // FCMEQ d20, d19, #0.0      -> +-0
    emit32(0x0EA01C00u | (20 << 16) | (21 << 5) | 21); // ORR   v21.8b              -> "identity" mask
    emit32(0x0EA01C00u | (18 << 16) | (18 << 5) | 20); // MOV   v20.8b, v18         (save ST0)
    emit32(0x1E780000u | (16 << 5) | 16);              // fcvtzs w16, d16  (n = trunc(ST1), int32-saturating)
    e_sxt(16, 16, 4);                                  // sign-extend n to 64-bit
    e_addi(16, 16, 1023, 1);                           // biased exponent e = n + 1023
    e_movconst(19, 2047);
    e_subi_s(31, 16, 2047, 1);        // cmp e, #2047
    e_csel(16, 19, 16, 12 /*GT*/, 1); // e = (e > 2047) ? 2047 : e
    e_movconst(19, 0);
    e_subi_s(31, 16, 0, 1);                            // cmp e, #0
    e_csel(16, 19, 16, 11 /*LT*/, 1);                  // e = (e < 0)    ? 0    : e
    e_lsl_i(16, 16, 52, 1);                            // place e into the exponent field
    e_fmov_to_d(17, 16);                               // d17 = 2^n
    emit32(0x1E600800u | (17 << 16) | (18 << 5) | 18); // fmul d18, d18, d17
    emit32(0x2E601C00u | (18 << 16) | (20 << 5) | 21); // BSL v21.8b, v20(ST0), v18(prod) -> identity?ST0:prod
    emit32(0x0EA01C00u | (21 << 16) | (21 << 5) | 18); // MOV v18.8b, v21
    hl_x86_x87_dnan_post(18);
    hl_x86_x87_store(18, 0);
}

// FXTRACT: split ST0 into unbiased exponent and significand. ST0 <- significand (in [1,2) with ST0's
// sign), then the exponent is pushed so ST1 = exponent, ST0 = significand (normal operands).
void hl_x86_x87_extract(void) {
    hl_x86_x87_load(16, 0); // d16 = ST0
    e_fmov_from_d(16, 16);  // x16 = bit pattern
    e_lsr_i(17, 16, 52, 1);
    e_movconst(19, 0x7FF);
    e_rrr(A_AND, 17, 17, 19, 1, 0);       // exponent field
    e_subi(17, 17, 1023, 1);              // unbiased exponent (signed)
    emit32(0x9E620000u | (17 << 5) | 17); // scvtf d17, x17  (exponent -> double)
    e_movconst(19, ~(0x7FFULL << 52));
    e_rrr(A_AND, 16, 16, 19, 1, 0); // clear exponent field
    e_movconst(19, 1023ULL << 52);
    e_rrr(A_ORR, 16, 16, 19, 1, 0); // set exponent to bias -> significand in [1,2)
    e_fmov_to_d(18, 16);            // d18 = significand
    hl_x86_x87_store(17, 0);        // ST0 = exponent
    hl_x86_x87_push(18);            // push significand -> ST0 = significand, ST1 = exponent
}

// FRNDINT: round ST0 to an integral value using the CURRENT x87 rounding control (cpu->fpcw bits[11:10]).
// x87 has its OWN rounding domain, separate from SSE MXCSR (both share ARM FPCR.RMode), so a bare frintx
// under the live (SSE, default-nearest) FPCR ignored fldcw's RC -- floorl/ceill/truncl (glibc sets RC via
// fldcw around frndint) then all rounded to nearest, diverging from a real FPU. Round under a saved/restored
// FPCR whose RMode is derived from the x87 RC (the same two-bit swap as ldmxcsr / emit_x87_round_st0), so
// SSE rounding is untouched. Scratch x20/x21/x22/x23 (guest xmm is v0..v15; x20+ are free here).
void hl_x86_x87_round(void) {
    hl_x86_x87_load(16, 0);
    e_ldr(20, 28, OFF_FPCW);                                          // w20 = cpu->fpcw
    emit32(0x53000000u | (10u << 16) | (11u << 10) | (20 << 5) | 20); // ubfx w20,w20,#10,#2 -> RC (0..3)
    e_movconst(21, 1);
    e_rrr(A_AND, 22, 20, 21, 0, 0);       // w22 = RC & 1
    e_lsr_i(21, 20, 1, 0);                // w21 = RC >> 1
    e_rrr(A_ORR, 22, 21, 22, 0, 1);       // w22 = (RC>>1) | (RC&1)<<1 = ARM RMode (x87 RC bits swapped)
    emit32(0xD53B4400u | 23);             // mrs x23, fpcr  (save the live -- SSE -- rounding mode)
    e_movconst(21, 3u << 22);             // RMode mask
    e_rrr(A_BIC, 20, 23, 21, 1, 0);       // x20 = fpcr & ~RMode
    e_rrr(A_ORR, 20, 20, 22, 1, 22);      // x20 = | (ARM RMode << 22)
    emit32(0xD51B4400u | 20);             // msr fpcr, x20  (x87 rounding mode)
    emit32(0x1E67C000u | (16 << 5) | 16); // frinti d16, d16 (round to integral per FPCR.RMode)
    emit32(0xD51B4400u | 23);             // msr fpcr, x23  (restore SSE rounding mode)
    hl_x86_x87_store(16, 0);
}

// FTST: compare ST0 with 0.0 and set the FPSW condition codes (same path as fcom).
void hl_x86_x87_test(void) {
    hl_x86_x87_load(18, 0);
    e_movconst(16, 0);
    e_fmov_to_d(16, 16);    // d16 = 0.0
    e_fcom_setfpsw(18, 16); // ST0 : 0.0 -> C0/C2/C3
}

// x87 FSW exception flags (bits IE0/DE1/ZE2/OE3/UE4/PE5) mirror the SSE MXCSR exception bits and, like
// them, are projected lazily from the host FPSR cumulative flags at read time (the x87 arithmetic ops
// execute as host NEON just like SSE, so the real exceptions already accumulate in the host FPSR). The
// per-bit map (FSW bit i <- FPSR bit) is IE<-IOC(0) DE<-IDC(7) ZE<-DZC(1) OE<-OFC(2) UE<-UFC(3) PE<-IXC(4),
// identical to the MXCSR projection in translate.c. fnclex/finit clear the host FPSR sticky flags.
static const int g_fsw_fpsr_bit[6] = {0, 7, 1, 2, 3, 4};

// OR the host FPSR sticky exception flags into x16 (the in-progress FSW) at bits 0..5, then set ES(7)
// and B(15) if any raised exception is UNMASKED per the current FCW mask bits (FCW[0..5], 1 = masked).
// Scratch: x17/x20/x21/x22 -- deliberately NOT x19, which holds the store EA at the fnstenv/fnstsw-m16
// call sites (x16 also survives as the running FSW word the callers store afterward).
static void fp_project_exceptions(void) {
    emit32(0xD53B4420u | 22); // mrs x22, fpsr
    e_movconst(21, 0);        // exception accumulator (FSW bits 0..5)
    e_movconst(20, 1);
    for (int i = 0; i < 6; i++) {
        e_lsr_i(17, 22, g_fsw_fpsr_bit[i], 0);
        e_rrr(A_AND, 17, 17, 20, 0, 0);
        e_rrr(A_ORR, 21, 21, 17, 0, i); // x21 |= bit << i
    }
    e_rrr(A_ORR, 16, 16, 21, 0, 0); // FSW |= exceptions (sticky, bits 0..5)
    e_ldr(17, 28, OFF_FPCW);        // w17 = FCW (bits 0..5 are the exception masks)
    e_rrr(A_BIC, 17, 21, 17, 0, 0); // x17 = raised & ~masked = unmasked exceptions
    e_movconst(20, 0x3f);
    e_rrr(A_AND, 17, 17, 20, 0, 0); // keep only bits 0..5
    e_subi_s(31, 17, 0, 1);         // cmp x17, #0
    e_cset(17, 1 /*NE*/, 1);        // x17 = (any unmasked exception)
    e_bfi(16, 17, 7, 1, 1);         // ES (error summary, bit 7)
    e_bfi(16, 17, 15, 1, 1);        // B  (busy, bit 15, mirrors ES)
}

// FNSTSW / FSTSW: the x87 status word reports TOP-of-stack (cpu->fptop) in bits 11-13 ORed with the
// condition codes held in cpu->fpsw and the exception flags projected from the host FPSR -- qemu does the
// same, and code that follows FNSTSW with SAHF relies on it. Result -> x16 (clobbers x17/x19..x22). The
// shadow top is materialized first so cpu->fptop is current under the static-top optimization.
void hl_x86_x87_status(void) {
    hl_x86_x87_materialize();
    e_ldr(16, 28, OFF_FPSW);
    e_ldr(17, 28, OFF_FPTOP);
    e_bfi(16, 17, 11, 3, 1); // status[13:11] = TOP
    fp_project_exceptions();
}

// FNCLEX: clear the sticky exception flags (host FPSR IOC/DZC/OFC/UFC/IXC/IDC + the projected FSW/ES/B),
// leaving the condition codes and TOP intact. Clobbers x16/x17.
void hl_x86_x87_clear_exceptions(void) {
    emit32(0xD53B4420u | 16);       // mrs x16, fpsr
    e_movconst(17, 0x9f);           // IOC|DZC|OFC|UFC|IXC|IDC (bits 0-4,7)
    e_rrr(A_BIC, 16, 16, 17, 0, 0); // clear the host sticky flags
    emit32(0xD51B4420u | 16);       // msr fpsr, x16
}

// FXAM: classify ST0 and set the FPSW condition codes (C1 = sign, {C3,C2,C0} = class), per the x87
// spec. We keep no tag bits, so "empty" cannot be reported and every stored slot is read as its value;
// cpu->st[] is double precision, so 80-bit unsupported/pseudo-denormal forms cannot arise. Class codes
// {C3,C2,C0}: zero=100, NaN=001, Inf=011, denormal=110, normal=010. From the IEEE-754 fields this is
// C0=(exp==max), C3=(exp==0), C2=!(zero|NaN). Scratch: x16/x17/x19/x21/x22, v18.
void hl_x86_x87_classify(void) {
    hl_x86_x87_load(18, 0);
    e_fmov_from_d(16, 18);  // x16 = bit pattern of ST0
    e_lsr_i(21, 16, 63, 1); // x21 = sign            -> C1
    e_lsr_i(17, 16, 52, 1);
    e_movconst(19, 0x7FF);
    e_rrr(A_AND, 17, 17, 19, 1, 0); // x17 = exponent field
    e_movconst(19, (1ull << 52) - 1);
    e_rrr(A_AND, 16, 16, 19, 1, 0); // x16 = mantissa field
    e_subi_s(31, 17, 0, 1);
    e_cset(19, 0, 1); // x19 = (exp == 0)      -> C3
    e_subi_s(31, 17, 0x7FF, 1);
    e_cset(17, 0, 1); // x17 = (exp == max)    -> C0
    e_subi_s(31, 16, 0, 1);
    e_cset(16, 0, 1);               // x16 = (mantissa == 0)
    e_rrr(A_AND, 22, 19, 16, 1, 0); // x22 = zero = exp0 & mant0
    e_rrr(A_BIC, 16, 17, 16, 1, 0); // x16 = NaN  = expMax & ~mant0
    e_rrr(A_ORR, 22, 22, 16, 1, 0); // x22 = zero | NaN
    e_movconst(16, 1);
    e_rrr(A_EOR, 22, 22, 16, 1, 0); // x22 = C2 = !(zero | NaN)
    e_movconst(16, 0);
    e_bfi(16, 17, 8, 1, 1);  // C0 (bit 8)
    e_bfi(16, 21, 9, 1, 1);  // C1 (bit 9)
    e_bfi(16, 22, 10, 1, 1); // C2 (bit 10)
    e_bfi(16, 19, 14, 1, 1); // C3 (bit 14)
    e_str(16, 28, OFF_FPSW);
}

// x87 transcendentals (the D9 F0-FF subset: F2XM1/FYL2X/FPTAN/FPATAN/FYL2XP1/FSINCOS/FSIN/FCOS) have
// no ARM/SSE counterpart and need host libm, so they exit the block to the C helper x87_func(), which
// computes the op on the double-precision ST stack. cpu->x87_ea carries the X87_* selector. The block
// ends here (like the m80 fld/fstp helpers); the caller breaks out of translation afterwards.
void hl_x86_x87_function(int fn, uint64_t next) {
    hl_x86_x87_drop(); // the helper reads/writes cpu->st[] and cpu->fptop directly -> spill the shadow top
    e_movconst(16, (uint64_t)fn);
    e_str(16, 28, OFF_X87EA);
    emit_exit_const(next, R_X87FUNC);
}
