// translator/guest/x86_64/interp.c -- the x86-64 guest backend for hosts that are NOT AArch64: decode and
// execute x86-64 rather than emit ARM64. core/target/x86_64.c forks on HL_HOST_CPU_AARCH64 and textually
// includes this file into that unity TU in place of emit.c + translate.c + cache.c, so it must define
// exactly the names those three defined and no more. Background: docs/amd64-host.md.
//
// Legal because core/dispatch.c run_guest's whole contract is `code = translate_block(G_PC(c));
// run_block(c, code);`, with c->reason, c->rip and all guest state final in *c on return -- `code` need not
// be machine code. struct cpu and the ARM-NZCV EFLAGS substrate stay as the JIT leaves them; ptrace GETREGS,
// the rt_sigframe builder and the checkpoint image read them.
//
// EXTENSION POINTS: interp_step_one_byte() (one-byte map + group /reg) and interp_step_two_byte() (0F map)
// return STEP_NEXT or STEP_END; add a class there and drop its interp_undefined() route. Host-neutral C
// already covers VEX/EVEX (R_AVX), 0F38/0F3A (R_SSE3B) and x87 m80/transcendental/fxsave (R_X87*, R_FX*);
// legacy SSE, the 0F map proper, has none -- the largest gap. Guest memory goes ONLY through
// interp_load/interp_store/interp_locked_rmw.

#include <math.h> // x87 on the double-precision ST stack
#include <setjmp.h>

#if defined(HL_HOST_CPU_X86_64)
// Baseline on x86-64. xmmintrin.h supplies the guest's MXCSR (_mm_getcsr/_mm_setcsr).
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#include "../../identity.h"
#include "decoder.h"

// For the setter declarations engine_global_init needs; its hl_x86_rep_movs/stos stay unused. The string ops
// run element-at-a-time below: those per-element pointer/counter updates are what make a fault mid-string
// architecturally correct. Link fallout: docs/amd64-host-findings.md 3.3.
#include "lower/repstr.h"

// ---- The seam: names the JIT files own, which the rest of the TU needs.

// A biased ET_EXEC's Go type section and V8 embedded-blob code base, in LOW link coordinates; set by
// linux_abi/x86.c. g_nonpie_lo/hi/bias are declared above this include, in core/target/x86_64.c.
static uint64_t g_nonpie_types_lo, g_nonpie_types_hi;
static uint64_t g_nonpie_blob_code;

// Must match guest/x86_64/cache.c, or switching host CPU silently relocates the guest image.
#define PC_IMG_BASE 0x0000040000000000ull    // 4 TB
#define PC_INTERP_BASE 0x0000048000000000ull // 4.5 TB

// No emitted block to inline clock_gettime into, so the fast-clock lever and cpu->fastclk_* stay off. Must
// still EXIST; core/target/x86_64.c reports them at exit.
static int g_fastsys;
static uint64_t g_fast_count;

static void s1_calibrate(void) {
    // Nothing to measure; clock syscalls take the R_SYSCALL exit, as after a failure.
    g_fastsys = 0;
}

// abi.h's G_SMC_UNMAP. No instruction bytes are cached, so a stale DECODE is impossible; dropping the map
// entry only forces a fresh fetch, and a truthful fault if the range is now unmapped.
static void jit86_drop_range_translations(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    uint64_t range[1][2];
    range[0][0] = lo;
    range[0][1] = hi;
    if (map_invalidate_source_ranges((const uint64_t(*)[2])range, 1)) {
        memset(g_ibtc, 0, sizeof g_ibtc);
        memset(g_xibtc, 0, sizeof g_xibtc);
    }
}

// abi.h's G_THREAD_START_FLUSH / G_SHARED_MAP_BARRIERS. No block here has its x86-TSO barriers elided, so
// there is nothing to flush. Must return nonzero -- the clone path reads 0 as a clone failure.
static int hl_x86_flush_for_thread_start(void) {
    return 1;
}

static int hl_x86_force_barriers_for_shared(void) {
    return 1;
}

// Never called (G_BLOCK_ALIGN is a compile-time 0) but must exist for that dead branch to type-check, and
// aborts rather than appending an ARM64 nop into the descriptor arena. Non-static: lower/primitives.h.
void emit32(uint32_t instruction) {
    (void)instruction;
    fprintf(stderr, "[hl] emit32() called on a " HL_HOST_CPU_NAME " host: the interpreter backend emits no\n"
                    "     instructions, so a caller believes the code arena holds machine code. G_BLOCK_ALIGN\n"
                    "     is 0 precisely so the shared dispatcher's alignment pad never reaches here.\n");
    abort();
}

// ---- The fault model. *cpu is already authoritative, so capture is trivial; escaping is not -- the
// faulting access is a memcpy several C frames deep and must be ABANDONED, not resumed past. run_block arms
// a sigsetjmp pad (savemask=1: it is entered FROM a handler with that signal blocked) and marks every guest
// access; claiming a fault only when marked is what keeps an engine-side null dereference reportable.

static __thread sigjmp_buf g_interp_fault_pad;
static __thread int g_interp_pad_armed;             // a run_block landing pad is live
static __thread volatile int g_interp_guest_access; // a guest access is in flight
// The cpu run_block armed the pad for; the ledger check below has no siginfo.
static __thread struct cpu *g_interp_pad_cpu;

// ---- The past-EOF SIGBUS ledger. mem.c re-maps the past-EOF tail of a MAP_PRIVATE file mapping as
// anonymous zero, so the host never raises BUS_ADRERR and the translator owes the guest SIGBUS out of
// mem.c's ledger (core/bus.h). Takes the GUEST address -- what the guest handler compares si_addr against --
// and rebases both ways, since the ledger stores HOST addresses. jit_guest_bus_fault returns the FIRST
// past-EOF byte, so an access straddling the boundary reports it as Linux does.
static void interp_bus_ledger_check(uint64_t guest_address, uint64_t length) {
    // Inert when the ledger is empty.
    if (!jit_guest_bus_active()) return;
    uint64_t host = hl_x86_guest_pointer(guest_address);
    uint64_t host_fault = jit_guest_bus_fault(host, length);
    if (host_fault == 0) return;
    struct cpu *cpu = g_interp_pad_cpu;
    if (cpu == NULL || !g_interp_pad_armed) return; // no pad: unreachable from run_block
    uint64_t guest_fault = host_fault - (host - guest_address);
    cpu->fault_addr = guest_fault;
    cpu->bus_ea = guest_fault;
    cpu->reason = R_BUS;
    g_interp_guest_access = 0;
    siglongjmp(g_interp_fault_pad, 1);
}

static inline void interp_access_begin(uint64_t guest_address, uint64_t length) {
    interp_bus_ledger_check(guest_address, length);
    g_interp_guest_access = 1;
    __atomic_signal_fence(__ATOMIC_SEQ_CST); // the marker must be visible to a handler BEFORE the access
}

static inline void interp_access_end(void) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    g_interp_guest_access = 0;
}

int interp_signal_capture(struct cpu *cpu, void *native_context);
void interp_signal_resume(struct cpu *cpu, void *native_context);

// 1 only for a fault the GUEST caused, i.e. one inside a marked access.
int interp_signal_capture(struct cpu *cpu, void *native_context) {
    (void)native_context; // *cpu is already authoritative
    if (cpu == NULL || !g_interp_pad_armed || !g_interp_guest_access) return 0;
    return 1;
}

// The delivery path has already queued the signal and set R_BRANCH; just return from run_block.
void interp_signal_resume(struct cpu *cpu, void *native_context) {
    (void)cpu;
    (void)native_context;
    if (!g_interp_pad_armed) return; // not ours; the caller re-raises
    siglongjmp(g_interp_fault_pad, 1);
}

// ---- Guest memory. Guest VA == host VA except that a non-PIE ET_EXEC's low link range is served at
// +g_nonpie_bias (hl_x86_guest_pointer); no software TLB, no page table. Everything goes through memcpy
// because UNALIGNED ACCESS MUST WORK and a cast through uint64_t* is UB (a real fault on some hosts). No
// byte swapping: guest and every host so far are little-endian. Nothing is emitted, so the guest sees the
// host's memory ordering -- x86-64 host TSO IS guest TSO, hence the empty fence below.

static inline void interp_tso_fence(void) {
#if !defined(HL_HOST_CPU_X86_64)
    // Around EVERY access: an interpreter cannot know which one is the synchronising one.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static uint64_t interp_load(uint64_t guest_address, int width) {
    uint64_t value = 0;
    const void *host = (const void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_access_begin(guest_address, (uint64_t)width);
    memcpy(&value, host, (size_t)width);
    interp_access_end();
    interp_tso_fence();
    return value;
}

static void interp_store(uint64_t guest_address, int width, uint64_t value) {
    void *host = (void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_tso_fence();
    interp_access_begin(guest_address, (uint64_t)width);
    memcpy(host, &value, (size_t)width);
    interp_access_end();
    // Stores into an emulated MAP_SHARED mapping (or an executable alias) must be queued for
    // jit86_smc_commit before a syscall lets a peer observe them.
    if (jit86_store_alias_observation_active()) jit86_store_alias_changed(guest_address, (uint64_t)width);
}

// Operands wider than a general register. ONE interp_access_begin/end pair spans the whole transfer, so a
// fault inside a straddling 16-byte access abandons the instruction with no architectural change -- hence
// callers read operands into locals FIRST and commit after the last access.
static void interp_load_bytes(uint64_t guest_address, void *destination, unsigned length) {
    const void *host = (const void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_access_begin(guest_address, length);
    memcpy(destination, host, length);
    interp_access_end();
    interp_tso_fence();
}

static void interp_store_bytes(uint64_t guest_address, const void *source, unsigned length) {
    void *host = (void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_tso_fence();
    interp_access_begin(guest_address, length);
    memcpy(host, source, length);
    interp_access_end();
    if (jit86_store_alias_observation_active()) jit86_store_alias_changed(guest_address, (uint64_t)length);
}

// The address CALL pushes is guest-visible: DWARF FDE lookup, dladdr and unwinding need a biased ET_EXEC's
// LINK address; the dispatcher rebiases on the RET. Go and V8 are the exceptions -- their code metadata is
// rebased high, so their walkers need high return PCs. Must stay byte-for-byte translate.c's.
static uint64_t interp_call_return_pc(uint64_t pc) {
    if (g_nonpie_lo && !g_nonpie_types_lo && !g_nonpie_blob_code && pc >= g_nonpie_lo + g_nonpie_bias &&
        pc < g_nonpie_hi + g_nonpie_bias)
        return pc - g_nonpie_bias;
    return pc;
}

static uint64_t interp_ea(const struct cpu *cpu, const struct insn *insn, uint64_t next);

// A non-PIE rip-relative LEA must yield the LOW link address: it MATERIALISES a pointer compared against
// the image's baked LOW pointers, and a HIGH value silently disagrees -- glibc's __malloc_fork_lock_parent
// then self-deadlocks on main_arena.mutex. Un-biases materialisation only; ACCESSES stay rebiased by
// hl_x86_guest_pointer. Guards as in lower/mov.c (docs/amd64-host-findings.md 3.11): 64-bit opsize (32-bit
// truncates to the low value anyway); rip-relative; target inside the link range; Go images narrowed to the
// type section, since code LEAs (`LEAQ asyncPreempt(SB)`) need HIGH for findfunc.
static uint64_t interp_lea_value(const struct cpu *cpu, const struct insn *insn, uint64_t next) {
    if (insn->opsize == 8 && insn->rip_rel && g_nonpie_lo) {
        uint64_t link_target = (next - g_nonpie_bias) + (uint64_t)insn->disp;
        uint64_t range_lo = g_nonpie_types_lo ? g_nonpie_types_lo : g_nonpie_lo;
        uint64_t range_hi = g_nonpie_types_lo ? g_nonpie_types_hi : g_nonpie_hi;
        if (link_target >= range_lo && link_target < range_hi) return link_target;
    }
    return interp_ea(cpu, insn, next);
}

// ---- The flag substrate: x86 EFLAGS on ARM NZCV in cpu->nzcv plus side lanes, fixed by the checkpoint
// format and signal.c's converters.
//   bit 31 N = SF, bit 30 Z = ZF, bit 28 V = OF, bit 29 C = NOT x86 CF (ARM's borrow convention)
//   cpu->pf      a BYTE whose EVEN PARITY is x86 PF; ops store the low byte of the result
//   cpu->af      (a ^ b ^ result) of the last add/sub-shaped op; x86 AF is bit 4. Logical ops store 0.
//   cpu->df      x86 DF, 0 = forward. Runtime state, so a cross-block `std` is honoured.
//   cpu->idflag  x86 RFLAGS.ID (bit 21), the bit 32-bit CPUID probes flip.
// The C inversion is the classic way an x86 interpreter passes its own tests and fails on hardware.

#define NZ_N (UINT64_C(1) << 31)
#define NZ_Z (UINT64_C(1) << 30)
#define NZ_C (UINT64_C(1) << 29)
#define NZ_V (UINT64_C(1) << 28)

static uint64_t interp_mask(int width) {
    return width == 8 ? UINT64_MAX : ((UINT64_C(1) << (8 * width)) - 1);
}

static unsigned interp_msb(uint64_t value, int width) {
    return (unsigned)((value >> (8 * width - 1)) & 1);
}

static void interp_flags_nzcv(struct cpu *cpu, unsigned sf, unsigned zf, unsigned x86_cf, unsigned of) {
    uint64_t nzcv = 0;
    if (sf) nzcv |= NZ_N;
    if (zf) nzcv |= NZ_Z;
    if (!x86_cf) nzcv |= NZ_C; // stored C is the INVERSE of x86 CF
    if (of) nzcv |= NZ_V;
    cpu->nzcv = nzcv;
}

static unsigned interp_cf(const struct cpu *cpu) {
    return (unsigned)(((cpu->nzcv >> 29) & 1) ^ 1);
}

static void interp_set_cf(struct cpu *cpu, unsigned x86_cf) {
    if (x86_cf)
        cpu->nzcv &= ~NZ_C;
    else
        cpu->nzcv |= NZ_C;
}

static unsigned interp_pf(const struct cpu *cpu) {
    return (unsigned)(__builtin_parity((unsigned)(cpu->pf & 0xff)) ^ 1); // even parity -> PF=1
}

static uint64_t interp_alu_add(struct cpu *cpu, uint64_t a, uint64_t b, unsigned carry_in, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned __int128 wide = (unsigned __int128)(a & m) + (b & m) + carry_in;
    uint64_t result = (uint64_t)wide & m;
    unsigned cf = (unsigned)((wide >> bits) & 1);
    // OF: both inputs agree in sign and the result disagrees with them.
    unsigned of = (unsigned)((((a ^ result) & (b ^ result)) >> (bits - 1)) & 1);
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, cf, of);
    cpu->pf = result & 0xff;
    cpu->af = a ^ b ^ result;
    return result;
}

static uint64_t interp_alu_sub(struct cpu *cpu, uint64_t a, uint64_t b, unsigned borrow_in, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned __int128 wide = (unsigned __int128)(a & m) - (b & m) - borrow_in;
    uint64_t result = (uint64_t)wide & m;
    unsigned cf = (unsigned)((wide >> bits) & 1); // the subtraction went negative -> borrow
    // OF: the inputs disagree in sign and the result disagrees with the minuend.
    unsigned of = (unsigned)((((a ^ b) & (a ^ result)) >> (bits - 1)) & 1);
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, cf, of);
    cpu->pf = result & 0xff;
    cpu->af = a ^ b ^ result;
    return result;
}

// AND/OR/XOR/TEST: CF and OF clear, AF undefined (0, matching the JIT).
static void interp_flags_logic(struct cpu *cpu, uint64_t result, int width) {
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, 0, 0);
    cpu->pf = result & 0xff;
    cpu->af = 0;
}

// ADD/SUB of 1 except that CF is left UNTOUCHED.
static uint64_t interp_alu_incdec(struct cpu *cpu, uint64_t a, int decrement, int width) {
    unsigned cf = interp_cf(cpu);
    uint64_t result = decrement ? interp_alu_sub(cpu, a, 1, 0, width) : interp_alu_add(cpu, a, 1, 0, width);
    interp_set_cf(cpu, cf);
    return result;
}

// Indexed by the low nibble of a Jcc/SETcc/CMOVcc opcode.
static int interp_cond(const struct cpu *cpu, int code) {
    unsigned cf = interp_cf(cpu);
    unsigned zf = (unsigned)((cpu->nzcv >> 30) & 1);
    unsigned sf = (unsigned)((cpu->nzcv >> 31) & 1);
    unsigned of = (unsigned)((cpu->nzcv >> 28) & 1);
    switch (code & 0xf) {
    case 0x0: return (int)of;                 // O
    case 0x1: return (int)!of;                // NO
    case 0x2: return (int)cf;                 // B / NAE / C
    case 0x3: return (int)!cf;                // AE / NB / NC
    case 0x4: return (int)zf;                 // E / Z
    case 0x5: return (int)!zf;                // NE / NZ
    case 0x6: return (int)(cf || zf);         // BE / NA
    case 0x7: return (int)(!cf && !zf);       // A / NBE
    case 0x8: return (int)sf;                 // S
    case 0x9: return (int)!sf;                // NS
    case 0xa: return (int)interp_pf(cpu);     // P / PE
    case 0xb: return (int)!interp_pf(cpu);    // NP / PO
    case 0xc: return (int)(sf != of);         // L / NGE
    case 0xd: return (int)(sf == of);         // GE / NL
    case 0xe: return (int)(zf || (sf != of)); // LE / NG
    default: return (int)(!zf && (sf == of)); // G / NLE
    }
}

// IF reads 1 -- a guest always observes interrupts enabled. Mirrors the JIT's pushfq lowering bit for bit.
static uint64_t interp_read_rflags(const struct cpu *cpu) {
    uint64_t flags = hl_x86_signal_nzcv_to_eflags(cpu->nzcv);
    flags |= UINT64_C(1) << 9;                     // IF
    flags |= (cpu->df & 1) << 10;                  // DF
    if (interp_pf(cpu)) flags |= UINT64_C(1) << 2; // PF
    flags |= ((cpu->af >> 4) & 1) << 4;            // AF
    flags |= (cpu->idflag & 1) << 21;              // ID
    return flags;
}

static void interp_write_rflags(struct cpu *cpu, uint64_t flags) {
    cpu->nzcv = hl_x86_signal_eflags_to_nzcv(flags);
    cpu->df = (flags >> 10) & 1;
    cpu->idflag = (flags >> 21) & 1;
    cpu->af = ((flags >> 4) & 1) << 4;
    // 0 has even parity (PF=1), 1 has odd (PF=0).
    cpu->pf = ((flags >> 2) & 1) ^ 1u;
}

// ---- Register and r/m operand access.

// WITHOUT REX, byte register numbers 4..7 name AH/CH/DH/BH, not the low byte of rSP/rBP/rSI/rDI.
static int interp_hi8(const struct insn *insn, int number, int width) {
    return width == 1 && !insn->has_rex && number >= 4 && number <= 7;
}

static uint64_t interp_reg_read(const struct cpu *cpu, const struct insn *insn, int number, int width) {
    if (interp_hi8(insn, number, width)) return (cpu->r[number - 4] >> 8) & 0xff;
    return cpu->r[number] & interp_mask(width);
}

static void interp_reg_write(struct cpu *cpu, const struct insn *insn, int number, int width, uint64_t value) {
    if (interp_hi8(insn, number, width)) {
        cpu->r[number - 4] = (cpu->r[number - 4] & ~UINT64_C(0xff00)) | ((value & 0xff) << 8);
        return;
    }
    switch (width) {
    // A byte or word write MERGES into the surrounding bits.
    case 1: cpu->r[number] = (cpu->r[number] & ~UINT64_C(0xff)) | (value & 0xff); break;
    case 2: cpu->r[number] = (cpu->r[number] & ~UINT64_C(0xffff)) | (value & 0xffff); break;
    // A 32-bit write ZERO-EXTENDS; stale high bits surface later as a wild pointer.
    case 4: cpu->r[number] = value & UINT64_C(0xffffffff); break;
    default: cpu->r[number] = value; break;
    }
}

// In GUEST coordinates: LEA must yield what the guest computes; the rebias happens at dereference.
static uint64_t interp_ea(const struct cpu *cpu, const struct insn *insn, uint64_t next) {
    uint64_t address;
    if (insn->rip_rel) {
        // Measured from the END of the instruction, hence `next`.
        address = next + (uint64_t)insn->disp;
    } else {
        address = 0;
        if (insn->m_hasbase) address += cpu->r[insn->m_base];
        if (insn->m_hasindex) address += cpu->r[insn->m_index] << insn->m_scale;
        address += (uint64_t)insn->disp;
    }
    // 0x67: modulo 2^32, so one mask at the end suffices.
    if (insn->addr32) address &= UINT64_C(0xffffffff);
    if (insn->seg == 1)
        address += cpu->fs_base; // %fs: TLS (arch_prctl SET_FS)
    else if (insn->seg == 2)
        address += cpu->gs_base;
    return address;
}

typedef struct interp_operand {
    int is_memory;
    uint64_t address; // valid when is_memory
    int number;       // register number when !is_memory
} interp_operand;

static interp_operand interp_rm(const struct cpu *cpu, const struct insn *insn, uint64_t next) {
    interp_operand operand;
    operand.is_memory = insn->is_mem;
    operand.address = insn->is_mem ? interp_ea(cpu, insn, next) : 0;
    operand.number = insn->is_mem ? 0 : insn->rm_reg;
    return operand;
}

static uint64_t interp_rm_read(const struct cpu *cpu, const struct insn *insn, const interp_operand *operand,
                               int width) {
    if (operand->is_memory) return interp_load(operand->address, width);
    return interp_reg_read(cpu, insn, operand->number, width);
}

static void interp_rm_write(struct cpu *cpu, const struct insn *insn, const interp_operand *operand, int width,
                            uint64_t value) {
    if (operand->is_memory)
        interp_store(operand->address, width, value);
    else
        interp_reg_write(cpu, insn, operand->number, width, value);
}

// ---- Atomic read-modify-write for LOCK-prefixed instructions (and memory XCHG, implicitly locked). The
// unaligned split-lock case, which x86 permits and ARM's LSE atomics refuse, falls back to cmpxchg.c's
// hashed spinlock.

#define INTERP_SPLIT_LOCKS 256
static _Atomic unsigned g_interp_split_lock[INTERP_SPLIT_LOCKS];

enum interp_rmw_kind {
    RMW_ADD,
    RMW_OR,
    RMW_ADC,
    RMW_SBB,
    RMW_AND,
    RMW_SUB,
    RMW_XOR,
    RMW_CMP, // LOCK CMP is legal and read-only
    RMW_NOT,
    RMW_NEG,
    RMW_INC,
    RMW_DEC,
    RMW_XCHG,
    RMW_BTS,
    RMW_BTR,
    RMW_BTC
};

static uint64_t interp_rmw_apply(enum interp_rmw_kind kind, uint64_t old, uint64_t operand, unsigned carry_in,
                                 int width) {
    uint64_t m = interp_mask(width);
    switch (kind) {
    case RMW_ADD: return (old + operand) & m;
    case RMW_OR: return (old | operand) & m;
    case RMW_ADC: return (old + operand + carry_in) & m;
    case RMW_SBB: return (old - operand - carry_in) & m;
    case RMW_AND: return (old & operand) & m;
    case RMW_SUB: return (old - operand) & m;
    case RMW_XOR: return (old ^ operand) & m;
    case RMW_CMP: return old & m;
    case RMW_NOT: return (~old) & m;
    case RMW_NEG: return (UINT64_C(0) - old) & m;
    case RMW_INC: return (old + 1) & m;
    case RMW_DEC: return (old - 1) & m;
    case RMW_BTS: return (old | operand) & m;
    case RMW_BTR: return (old & ~operand) & m;
    case RMW_BTC: return (old ^ operand) & m;
    default: return operand & m; // RMW_XCHG
    }
}

// Returns the PRE-image, so flags match what a non-locked path would have seen.
static uint64_t interp_locked_rmw(uint64_t guest_address, int width, enum interp_rmw_kind kind, uint64_t operand,
                                  unsigned carry_in) {
    uint64_t host_address = hl_x86_guest_pointer(guest_address);
    void *pointer = (void *)(uintptr_t)host_address;
    uint64_t old = 0;
    if ((host_address & (uint64_t)(width - 1)) == 0) {
        interp_access_begin(guest_address, (uint64_t)width);
        switch (width) {
        case 1: {
            unsigned char *p = pointer;
            unsigned char expected = __atomic_load_n(p, __ATOMIC_SEQ_CST), desired;
            do {
                desired = (unsigned char)interp_rmw_apply(kind, expected, operand, carry_in, width);
            } while (!__atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            old = expected;
            break;
        }
        case 2: {
            unsigned short *p = pointer;
            unsigned short expected = __atomic_load_n(p, __ATOMIC_SEQ_CST), desired;
            do {
                desired = (unsigned short)interp_rmw_apply(kind, expected, operand, carry_in, width);
            } while (!__atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            old = expected;
            break;
        }
        case 4: {
            uint32_t *p = pointer;
            uint32_t expected = __atomic_load_n(p, __ATOMIC_SEQ_CST), desired;
            do {
                desired = (uint32_t)interp_rmw_apply(kind, expected, operand, carry_in, width);
            } while (!__atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            old = expected;
            break;
        }
        default: {
            uint64_t *p = pointer;
            uint64_t expected = __atomic_load_n(p, __ATOMIC_SEQ_CST), desired;
            do {
                desired = interp_rmw_apply(kind, expected, operand, carry_in, width);
            } while (!__atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            old = expected;
            break;
        }
        }
        interp_access_end();
    } else {
        // Hashed: the same bytes serialise, unrelated sites do not contend.
        unsigned hash = (unsigned)((host_address >> 3) & (INTERP_SPLIT_LOCKS - 1));
        _Atomic unsigned *lock = &g_interp_split_lock[hash];
        uint64_t next_value;
        while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE))
            ; // an exchange always makes forward progress
        interp_access_begin(guest_address, (uint64_t)width);
        memcpy(&old, pointer, (size_t)width);
        next_value = interp_rmw_apply(kind, old, operand, carry_in, width);
        memcpy(pointer, &next_value, (size_t)width);
        interp_access_end();
        __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
    }
    if (kind != RMW_CMP && jit86_store_alias_observation_active())
        jit86_store_alias_changed(guest_address, (uint64_t)width);
    return old & interp_mask(width);
}

// ---- The block descriptor.

// One descriptor per translated guest PC, holding no decoded instructions: run_block re-decodes from guest
// memory every execution, which makes self-modifying guest code coherent by construction. Bump-allocated
// from the shared CODE ARENA (g_cp), not malloc'd, because the flush, the stop-the-world generation
// rotation, jit_publish_code, arena reclamation and jit_resolve_rw_code all reason about arena membership.
// The magic word and gpc make a stale pointer -- from a reclaimed arena, or host code out of a JIT-written
// persistent cache -- fail loudly at the first execution.
#define INTERP_BLOCK_MAGIC UINT64_C(0x496e74657270426b) // "InterpBk"

struct interp_block {
    uint64_t magic;
    uint64_t gpc;
    uint64_t generation; // diagnostic
};

// Must return a distinct non-NULL pointer per guest PC: non-NULL from map_host() suppresses re-translation.
static void *translate_block(uint64_t gpc) {
    // Pick up writes made through another MAP_SHARED alias before reading an emulated executable view.
    uint64_t source_page = gpc & ~UINT64_C(0xfff);
    filemap_refresh_emulated(source_page, source_page + UINT64_C(0x1000));
    HL_LOGF(&g_jit_log, HL_LOG_TAG_TRANSLATE, "isa=x86_64 interp guest_pc=%#llx", (unsigned long long)gpc);
    while ((uintptr_t)g_cp & 15)
        g_cp++;
    struct interp_block *block = (struct interp_block *)g_cp;
    g_cp += sizeof *block;
    block->magic = INTERP_BLOCK_MAGIC;
    block->gpc = gpc;
    block->generation = g_cache_gen;
    // host == body (no prologue to skip); SOURCE range [gpc, gpc+1) so SMC invalidation finds it by address.
    map_put(gpc, gpc, gpc + 1, block, block);
    return block;
}

// No emitted back-edge to fold and no in-cache counter, so R_TIER2 is unreachable (interp_dispatch.h
// normalizes it to R_BRANCH). core/dispatch.c calls this unconditionally.
static void tier2_promote(uint64_t gpc) {
    (void)gpc;
}

// An ENGINE GAP, not a guest fault: a class this backend cannot execute stops the run loudly with reason 99
// (interp_dispatch.h -> exit 70), whereas a guest-caused #UD is a SIGILL delivered by interp_guest_trap --
// never route one here. cpu->rip stays EXACT (the JIT writes a 0xDEAD marker); bytes come through the guest
// fetch path, so reporting next to an unmapped page cannot itself fault.
static int interp_undefined(struct cpu *cpu, const struct insn *insn, uint64_t pc, const char *class_name) {
    uint8_t bytes[16] = {0};
    char text[96];
    int length = (insn->len > 0 && insn->len <= 15) ? insn->len : 8;
    int used = 0;
    const char *map = insn->vex ? (insn->evex ? "EVEX" : "VEX")
                      : insn->map3 == 2 ? "0F38"
                      : insn->map3 == 3 ? "0F3A"
                      : insn->two       ? "0F"
                                        : "1B";
    if (hl_guest_fetch_exec(pc, bytes, (size_t)length) != 0) length = 0;
    for (int index = 0; index < length && used < (int)sizeof text - 4; index++)
        used += snprintf(text + used, sizeof text - (size_t)used, "%02x ", bytes[index]);
    if (used > 0) text[used - 1] = 0;
    fprintf(stderr,
            "[hl] interp: unimplemented %s at rip=%llx (image+%llx)\n"
            "     bytes: %s | map=%s op=0x%02x modrm=0x%02x reg=%d rm=%d opsize=%d len=%d prefixes:%s%s%s%s%s%s\n",
            class_name, (unsigned long long)pc, (unsigned long long)(pc - g_loadbase), text, map, insn->op,
            insn->has_modrm ? insn->modrm : 0, insn->reg, insn->rm_reg, insn->opsize, insn->len,
            insn->has_rex ? " REX" : "", insn->p66 ? " 66" : "", insn->rep ? " F3" : "", insn->repne ? " F2" : "",
            insn->lock ? " LOCK" : "", insn->seg == 1 ? " FS" : insn->seg == 2 ? " GS" : "");
    HL_LOGF(&g_jit_log, HL_LOG_TAG_TRANSLATE, "isa=x86_64 interp unimplemented=%s guest_pc=%#llx op=%#x", class_name,
            (unsigned long long)pc, (unsigned)insn->op);
    cpu->rip = pc;
    cpu->reason = 99;
    return 1; // STEP_END
}

// ---- The interpreter.

enum { STEP_NEXT = 0, STEP_END = 1 };

// Guest trap signal, as the JIT's emit_guest_signal: divop = (signo | si_code<<8), rip = the handler's PC.
static int interp_guest_trap(struct cpu *cpu, uint64_t rip, int signo, int si_code) {
    cpu->divop = (uint64_t)((signo & 0xff) | ((si_code & 0xff) << 8));
    cpu->rip = rip;
    cpu->reason = R_TRAP;
    return STEP_END;
}

static int interp_exit(struct cpu *cpu, uint64_t rip, uint64_t reason) {
    cpu->rip = rip;
    cpu->reason = reason;
    return STEP_END;
}

// Push/pop default to 64-bit in long mode; insn->opsize follows REX.W, so ask here instead.
static int interp_stack_width(const struct insn *insn) {
    return insn->p66 ? 2 : 8;
}

static void interp_push(struct cpu *cpu, uint64_t value, int width) {
    cpu->r[RSP] -= (uint64_t)width;
    interp_store(cpu->r[RSP], width, value);
}

static uint64_t interp_pop(struct cpu *cpu, int width) {
    uint64_t value = interp_load(cpu->r[RSP], width);
    cpu->r[RSP] += (uint64_t)width;
    return value;
}

// Flag traps: a zero effective count changes NO flags; the count masks to 5 bits (6 at 64-bit), so
// `shlb $8` shifts by 8 and yields 0; ROL/ROR touch only CF (plus OF at count==1), and write CF whenever
// the MASKED count is nonzero, even if count%width == 0.
static uint64_t interp_shift(struct cpu *cpu, int kind, uint64_t value, unsigned count_raw, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned count = count_raw & (width == 8 ? 63u : 31u);
    uint64_t original = value & m;
    uint64_t result = original;
    if (kind == 0 || kind == 1) { // ROL / ROR
        unsigned rotate = count % bits;
        if (rotate != 0)
            result = (kind == 0 ? ((original << rotate) | (original >> (bits - rotate)))
                                : ((original >> rotate) | (original << (bits - rotate)))) &
                     m;
        if (count != 0) {
            unsigned cf = kind == 0 ? (unsigned)(result & 1) : interp_msb(result, width);
            interp_set_cf(cpu, cf);
            if (count == 1) {
                unsigned of = kind == 0 ? (interp_msb(result, width) ^ cf)
                                        : (interp_msb(result, width) ^ (unsigned)((result >> (bits - 2)) & 1));
                cpu->nzcv = (cpu->nzcv & ~NZ_V) | ((uint64_t)of << 28);
            }
        }
        return result;
    }
    if (count == 0) return original; // no flags change
    unsigned cf, of = 0;
    if (kind == 4) { // SHL / SAL
        result = (count < bits ? (original << count) : 0) & m;
        cf = count <= bits ? (unsigned)((original >> (bits - count)) & 1) : 0u;
        of = interp_msb(result, width) ^ cf;
    } else if (kind == 5) { // SHR
        result = count < bits ? (original >> count) : 0;
        cf = count <= bits ? (unsigned)((original >> (count - 1)) & 1) : 0u;
        of = interp_msb(original, width);
    } else { // SAR (kind 7)
        int64_t signed_value = (int64_t)(original << (64 - bits)) >> (64 - bits);
        unsigned shift = count < bits ? count : bits - 1;
        result = (uint64_t)(signed_value >> shift) & m;
        cf = (unsigned)((signed_value >> (count > bits ? bits - 1 : count - 1)) & 1);
        of = 0; // SAR can never overflow
    }
    // Flags x86 leaves UNDEFINED are written exactly as the JIT writes them, everywhere in this file.
    // For shifts that means AF untouched.
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, cf, of);
    cpu->pf = result & 0xff;
    return result;
}

// SHLD/SHRD: `fill` supplies the shifted-in bits.
static uint64_t interp_double_shift(struct cpu *cpu, int right, uint64_t value, uint64_t fill, unsigned count_raw,
                                    int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned count = count_raw & (width == 8 ? 63u : 31u);
    uint64_t result, cf;
    if (count == 0 || count > bits) return value & m; // count > width: x86 leaves the result undefined
    if (right) {
        cf = (value >> (count - 1)) & 1;
        result = count == bits ? (fill & m) : ((((value & m) >> count) | ((fill & m) << (bits - count))) & m);
    } else {
        cf = (value >> (bits - count)) & 1;
        result = count == bits ? (fill & m) : ((((value & m) << count) | ((fill & m) >> (bits - count))) & m);
    }
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, (unsigned)cf,
                      count == 1 ? (interp_msb(result, width) ^ interp_msb(value, width)) : 0u);
    cpu->pf = result & 0xff;
    return result;
}

// DIV / IDIV. #DE for a zero divisor and for a too-wide quotient both exit divop == 0, R_DIV/R_IDIV, at
// the FAULTING PC -- the JIT's convention. The 64-bit form is left to the dispatcher's 128/64 division.
static int interp_divide(struct cpu *cpu, uint64_t divisor, int width, int is_signed, uint64_t pc, uint64_t next) {
    uint64_t reason = is_signed ? R_IDIV : R_DIV;
    if (divisor == 0) {
        cpu->divop = 0;
        return interp_exit(cpu, pc, reason);
    }
    if (width == 8) {
        cpu->divop = divisor;
        return interp_exit(cpu, next, reason); // 128/64 in the dispatcher, quotient overflow included
    }
    unsigned bits = (unsigned)(8 * width);
    uint64_t m = interp_mask(width);
    if (!is_signed) {
        // At byte width the dividend is AX alone, not DX:AX.
        uint64_t dividend = width == 1 ? (cpu->r[RAX] & 0xffff)
                                       : (((cpu->r[RDX] & m) << bits) | (cpu->r[RAX] & m));
        uint64_t quotient = dividend / (divisor & m);
        uint64_t remainder = dividend % (divisor & m);
        if (quotient > m) {
            cpu->divop = 0;
            return interp_exit(cpu, pc, reason);
        }
        if (width == 1) {
            cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | (quotient & 0xff) | ((remainder & 0xff) << 8);
        } else {
            uint64_t q = quotient & m, r = remainder & m;
            cpu->r[RAX] = width == 4 ? q : ((cpu->r[RAX] & ~m) | q);
            cpu->r[RDX] = width == 4 ? r : ((cpu->r[RDX] & ~m) | r);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }
    int64_t signed_divisor = (int64_t)((divisor & m) << (64 - bits)) >> (64 - bits);
    int64_t dividend;
    if (width == 1) {
        dividend = (int64_t)(int16_t)(uint16_t)(cpu->r[RAX] & 0xffff);
    } else if (width == 2) {
        dividend = (int64_t)(int32_t)(uint32_t)(((cpu->r[RDX] & 0xffff) << 16) | (cpu->r[RAX] & 0xffff));
    } else {
        dividend = (int64_t)(((cpu->r[RDX] & 0xffffffff) << 32) | (cpu->r[RAX] & 0xffffffff));
    }
    if (signed_divisor == -1 && dividend == INT64_MIN) { // cannot happen for width<8, but be explicit
        cpu->divop = 0;
        return interp_exit(cpu, pc, reason);
    }
    int64_t quotient = dividend / signed_divisor;
    int64_t remainder = dividend % signed_divisor;
    int64_t low = (int64_t)(((uint64_t)quotient & m) << (64 - bits)) >> (64 - bits);
    if (low != quotient) { // quotient too wide -> #DE
        cpu->divop = 0;
        return interp_exit(cpu, pc, reason);
    }
    if (width == 1) {
        cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | ((uint64_t)quotient & 0xff) |
                      (((uint64_t)remainder & 0xff) << 8);
    } else {
        uint64_t q = (uint64_t)quotient & m, r = (uint64_t)remainder & m;
        cpu->r[RAX] = width == 4 ? q : ((cpu->r[RAX] & ~m) | q);
        cpu->r[RDX] = width == 4 ? r : ((cpu->r[RDX] & ~m) | r);
    }
    cpu->rip = next;
    return STEP_NEXT;
}

// MUL / IMUL (widening). CF=OF when the high half is significant; N=Z=0, as the JIT's e_mul_set_oc.
static void interp_widening_multiply(struct cpu *cpu, const struct insn *insn, uint64_t source, int width,
                                     int is_signed) {
    uint64_t m = interp_mask(width);
    uint64_t low, high;
    unsigned overflow;
    if (is_signed) {
        __int128 a = (__int128)(int64_t)((cpu->r[RAX] & m) << (64 - 8 * width)) >> (64 - 8 * width);
        __int128 b = (__int128)(int64_t)((source & m) << (64 - 8 * width)) >> (64 - 8 * width);
        __int128 product = a * b;
        low = (uint64_t)product & m;
        high = (uint64_t)((unsigned __int128)product >> (8 * width)) & m;
        int64_t sign_extended_low = (int64_t)(low << (64 - 8 * width)) >> (64 - 8 * width);
        overflow = (unsigned)((__int128)sign_extended_low != product);
    } else {
        unsigned __int128 product = (unsigned __int128)(cpu->r[RAX] & m) * (source & m);
        low = (uint64_t)product & m;
        high = (uint64_t)(product >> (8 * width)) & m;
        overflow = high != 0;
    }
    if (width == 1) {
        // MUL r/m8 lands the whole 16-bit product in AX.
        cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | (low & 0xff) | ((high & 0xff) << 8);
    } else {
        interp_reg_write(cpu, insn, RAX, width, low);
        interp_reg_write(cpu, insn, RDX, width, high);
    }
    interp_flags_nzcv(cpu, 0, 0, overflow, overflow);
}

// Two/three-operand IMUL: CF=OF report that the untruncated product did not fit; SF/ZF from the result.
static uint64_t interp_imul_truncating(struct cpu *cpu, uint64_t a, uint64_t b, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    __int128 sa = (__int128)(int64_t)((a & m) << (64 - bits)) >> (64 - bits);
    __int128 sb = (__int128)(int64_t)((b & m) << (64 - bits)) >> (64 - bits);
    __int128 product = sa * sb;
    uint64_t result = (uint64_t)product & m;
    int64_t sign_extended = (int64_t)(result << (64 - bits)) >> (64 - bits);
    unsigned overflow = (unsigned)((__int128)sign_extended != product);
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, overflow, overflow);
    cpu->pf = result & 0xff;
    return result;
}

static int interp_step_one_byte(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next);
static int interp_step_two_byte(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next);
// x87 (D8..DF) is implemented below, with the other FP.
static int interp_step_x87(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next);

static int interp_step(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    // VEX/EVEX -> avx.c, which advances rip itself, so rip must name THIS instruction on the way out.
    if (insn->vex) {
        // Trap, not gap: EVEX mm==0 and 0x62's legacy BOUND are both #UD here, so SIGILL to the guest.
        if (insn->evex && insn->vex_map == 0) return interp_guest_trap(cpu, pc, 4, 2);
        return interp_exit(cpu, pc, R_AVX);
    }
    if (insn->map3) return interp_exit(cpu, pc, R_SSE3B);
    if (insn->two) return interp_step_two_byte(cpu, insn, pc, next);
    return interp_step_one_byte(cpu, insn, pc, next);
}

// Every guest control transfer ends the block, keeping run_guest's per-iteration work (signal poll,
// safepoints) at block granularity.
static void interp_execute(struct cpu *cpu) {
    for (;;) {
        uint64_t pc = cpu->rip; // a fault below reports precisely this PC
        struct insn insn;
        if (hl_x86_decode(pc, &insn) < 0) {
            // Fetch failed the executable-mapping check: a guest fault, not an engine crash.
            (void)interp_guest_trap(cpu, pc, 11, 2);
            return;
        }
        if (interp_step(cpu, &insn, pc, pc + (uint64_t)insn.len) == STEP_END) return;
    }
}

// run_block / block_return -- the symbols core/dispatch.c and the fault path name. On AArch64 they are
// trampolines into emitted code; here run_block IS the interpreter, and block_return stays address-taken
// for sigframe_resume_dispatch but aborts.
//
// STATIC is load-bearing: the dual archive links BOTH target objects and namespace.h does not cover these
// two, so an external definition collides at link time (findings 3.7). Every caller is in this TU.
static void run_block(struct cpu *cpu, void *code);
static void block_return(void);

static void run_block(struct cpu *cpu, void *code) {
    struct interp_block *block = (struct interp_block *)code;
    if (block == NULL || block->magic != INTERP_BLOCK_MAGIC) {
        fprintf(stderr,
                "[hl] interp: block descriptor at %p is not one of ours (magic=%llx, expected %llx) for rip=%llx.\n"
                "     A persistent cache written by the ARM64 backend, or a reclaimed arena generation, is the\n"
                "     only way to reach this.\n",
                code, (unsigned long long)(block ? block->magic : 0), (unsigned long long)INTERP_BLOCK_MAGIC,
                (unsigned long long)cpu->rip);
        abort();
    }
    // Guest-fault landing pad. savemask=1: the jump arrives from a handler with the fault signal blocked.
    int previous = g_interp_pad_armed;
    struct cpu *previous_cpu = g_interp_pad_cpu;
    if (sigsetjmp(g_interp_fault_pad, 1) != 0) {
        // A guest access was abandoned; both routes already set cpu->reason and left cpu->rip on it.
        g_interp_guest_access = 0;
        g_interp_pad_armed = previous;
        g_interp_pad_cpu = previous_cpu;
        return;
    }
    g_interp_pad_armed = 1;
    g_interp_pad_cpu = cpu;
    interp_execute(cpu);
    g_interp_pad_armed = previous;
    g_interp_pad_cpu = previous_cpu;
}

static void block_return(void) {
    fprintf(stderr, "[hl] interp: block_return() entered on a " HL_HOST_CPU_NAME " host. Only translated ARM64\n"
                    "     blocks branch here and this backend emits none -- so its address was baked into\n"
                    "     something that then ran (a stale persistent-cache image or a mis-relocated exit).\n");
    abort();
}

// The one-byte opcode map.

// The eight ALU kinds in x86 opcode order.
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

static const enum interp_rmw_kind g_alu_rmw[8] = {RMW_ADD, RMW_OR, RMW_ADC, RMW_SBB,
                                                  RMW_AND, RMW_SUB, RMW_XOR, RMW_CMP};

// *store is cleared for CMP, which discards its result.
static uint64_t interp_alu_kind(struct cpu *cpu, int kind, uint64_t a, uint64_t b, int width, int *store) {
    uint64_t result;
    *store = 1;
    switch (kind) {
    case ALU_ADD: return interp_alu_add(cpu, a, b, 0, width);
    case ALU_ADC: return interp_alu_add(cpu, a, b, interp_cf(cpu), width);
    case ALU_SBB: return interp_alu_sub(cpu, a, b, interp_cf(cpu), width);
    case ALU_SUB: return interp_alu_sub(cpu, a, b, 0, width);
    case ALU_CMP:
        *store = 0;
        return interp_alu_sub(cpu, a, b, 0, width);
    case ALU_OR: result = (a | b) & interp_mask(width); break;
    case ALU_AND: result = (a & b) & interp_mask(width); break;
    default: result = (a ^ b) & interp_mask(width); break; // ALU_XOR
    }
    interp_flags_logic(cpu, result, width);
    return result;
}

// ALU with an r/m destination; the LOCK path is shared by all five encodings that reach it.
static void interp_alu_to_rm(struct cpu *cpu, struct insn *insn, const interp_operand *operand, int kind, int width,
                             uint64_t source) {
    int store;
    if (insn->lock && operand->is_memory && kind != ALU_CMP) {
        // Flags from the pre-image: a locked op and its unlocked twin must agree.
        unsigned carry = (kind == ALU_ADC || kind == ALU_SBB) ? interp_cf(cpu) : 0u;
        uint64_t old = interp_locked_rmw(operand->address, width, g_alu_rmw[kind], source, carry);
        (void)interp_alu_kind(cpu, kind, old, source, width, &store);
        return;
    }
    uint64_t old = interp_rm_read(cpu, insn, operand, width);
    uint64_t result = interp_alu_kind(cpu, kind, old, source, width, &store);
    if (store) interp_rm_write(cpu, insn, operand, width, result);
}

static int interp_step_one_byte(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;

    // 00..3F ALU block by op & 7: 0 r/m8,r8  1 r/m,r  2 r8,r/m8  3 r,r/m  4 AL,imm8  5 eAX,imm; 6/7 (segment
    // push/pop, BCD) are invalid in long mode and fall through.
    if (op < 0x40 && (op & 7) <= 5) {
        int kind = op >> 3;
        int form = op & 7;
        int width = (form == 0 || form == 2 || form == 4) ? 1 : insn->opsize;
        if (form == 4 || form == 5) {
            uint64_t old = interp_reg_read(cpu, insn, RAX, width);
            int store;
            uint64_t result = interp_alu_kind(cpu, kind, old, (uint64_t)insn->imm, width, &store);
            if (store) interp_reg_write(cpu, insn, RAX, width, result);
        } else if (form == 2 || form == 3) {
            interp_operand operand = interp_rm(cpu, insn, next);
            uint64_t source = interp_rm_read(cpu, insn, &operand, width);
            uint64_t old = interp_reg_read(cpu, insn, insn->reg, width);
            int store;
            uint64_t result = interp_alu_kind(cpu, kind, old, source, width, &store);
            if (store) interp_reg_write(cpu, insn, insn->reg, width, result);
        } else {
            interp_operand operand = interp_rm(cpu, insn, next);
            interp_alu_to_rm(cpu, insn, &operand, kind, width, interp_reg_read(cpu, insn, insn->reg, width));
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    switch (op) {
    // PUSH / POP register
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57: {
        int width = interp_stack_width(insn);
        interp_push(cpu, cpu->r[(op & 7) | (insn->rexB << 3)], width);
        cpu->rip = next;
        return STEP_NEXT;
    }
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F: {
        int width = interp_stack_width(insn);
        int number = (op & 7) | (insn->rexB << 3);
        uint64_t value = interp_pop(cpu, width);
        // `pop rsp` must take the POPPED value: write the register after interp_pop advanced rsp.
        if (width == 2)
            cpu->r[number] = (cpu->r[number] & ~UINT64_C(0xffff)) | (value & 0xffff);
        else
            cpu->r[number] = value;
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVSXD
    case 0x63: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, 4);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         insn->opsize == 8 ? (uint64_t)(int64_t)(int32_t)(uint32_t)source : source);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // PUSH imm, sign-extended
    case 0x68:
    case 0x6A:
        interp_push(cpu, (uint64_t)insn->imm, interp_stack_width(insn));
        cpu->rip = next;
        return STEP_NEXT;

    // IMUL r, r/m, imm
    case 0x69:
    case 0x6B: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, insn->opsize);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         interp_imul_truncating(cpu, source, (uint64_t)insn->imm, insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // Jcc rel8
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F:
        // Both edges end the block: the fall-through too, or a guest loop never reaches the safepoints.
        cpu->rip = interp_cond(cpu, op & 0xf) ? next + (uint64_t)insn->imm : next;
        cpu->reason = R_BRANCH;
        return STEP_END;

    // Group 1: ALU r/m, imm
    case 0x80:
    case 0x81:
    case 0x83: {
        int width = (op == 0x80) ? 1 : insn->opsize;
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_alu_to_rm(cpu, insn, &operand, insn->reg & 7, width, (uint64_t)insn->imm);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // TEST r/m, r
    case 0x84:
    case 0x85: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t left = interp_rm_read(cpu, insn, &operand, width);
        uint64_t right = interp_reg_read(cpu, insn, insn->reg, width);
        interp_flags_logic(cpu, (left & right) & interp_mask(width), width);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // XCHG r/m, r: IMPLICITLY locked on memory, LOCK prefix or not -- hence the unconditional locked_rmw.
    case 0x86:
    case 0x87: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t reg_value = interp_reg_read(cpu, insn, insn->reg, width);
        if (operand.is_memory) {
            uint64_t old = interp_locked_rmw(operand.address, width, RMW_XCHG, reg_value, 0);
            interp_reg_write(cpu, insn, insn->reg, width, old);
        } else {
            uint64_t old = interp_reg_read(cpu, insn, operand.number, width);
            interp_reg_write(cpu, insn, operand.number, width, reg_value);
            interp_reg_write(cpu, insn, insn->reg, width, old);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOV r/m<-r, then r<-r/m
    case 0x88:
    case 0x89: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, width, interp_reg_read(cpu, insn, insn->reg, width));
        cpu->rip = next;
        return STEP_NEXT;
    }
    case 0x8A:
    case 0x8B: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_reg_write(cpu, insn, insn->reg, width, interp_rm_read(cpu, insn, &operand, width));
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0x8D: {
        if (!insn->is_mem) return interp_undefined(cpu, insn, pc, "LEA with a register operand (#UD encoding)");
        // LEA is an ADDRESS computation, never interp_load, and stays in the GUEST's pointer domain: a
        // rip-relative LEA in a biased ET_EXEC must yield the LOW link address (findings 3.11).
        interp_reg_write(cpu, insn, insn->reg, insn->opsize, interp_lea_value(cpu, insn, next));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // POP r/m
    case 0x8F: {
        int width = interp_stack_width(insn);
        if ((insn->reg & 7) != 0) return interp_undefined(cpu, insn, pc, "group 1A opcode other than POP r/m");
        uint64_t value = interp_pop(cpu, width);
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, width, value);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // NOP and XCHG rAX, r
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {
        int number = (op & 7) | (insn->rexB << 3);
        // 0x90 without REX.B is XCHG rAX,rAX = NOP, as are 0x66 90 and F3 90 PAUSE.
        if (number != RAX) {
            int width = insn->opsize;
            uint64_t a = interp_reg_read(cpu, insn, RAX, width);
            uint64_t b = interp_reg_read(cpu, insn, number, width);
            interp_reg_write(cpu, insn, RAX, width, b);
            interp_reg_write(cpu, insn, number, width, a);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // CBW/CWDE/CDQE, then CWD/CDQ/CQO
    case 0x98: {
        int width = insn->opsize;
        uint64_t value;
        if (width == 2)
            value = (uint64_t)(int64_t)(int8_t)(uint8_t)(cpu->r[RAX] & 0xff); // CBW: AL -> AX
        else if (width == 4)
            value = (uint64_t)(int64_t)(int16_t)(uint16_t)(cpu->r[RAX] & 0xffff); // CWDE: AX -> EAX
        else
            value = (uint64_t)(int64_t)(int32_t)(uint32_t)(cpu->r[RAX] & 0xffffffff); // CDQE: EAX -> RAX
        interp_reg_write(cpu, insn, RAX, width, value);
        cpu->rip = next;
        return STEP_NEXT;
    }
    case 0x99: {
        int width = insn->opsize;
        uint64_t sign = interp_msb(cpu->r[RAX] & interp_mask(width), width) ? UINT64_MAX : 0;
        interp_reg_write(cpu, insn, RDX, width, sign);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // FWAIT: no x87 exception is ever pending here
    case 0x9B:
        cpu->rip = next;
        return STEP_NEXT;

    // PUSHFQ / POPFQ / SAHF / LAHF
    case 0x9C:
        interp_push(cpu, interp_read_rflags(cpu), interp_stack_width(insn));
        cpu->rip = next;
        return STEP_NEXT;
    case 0x9D:
        interp_write_rflags(cpu, interp_pop(cpu, interp_stack_width(insn)));
        cpu->rip = next;
        return STEP_NEXT;
    case 0x9E: { // SAHF: AH -> SF,ZF,AF,PF,CF (the reserved bits are ignored)
        uint64_t ah = (cpu->r[RAX] >> 8) & 0xff;
        // Preserve OF: SAHF writes only the low byte; OF is bit 11.
        unsigned of = (unsigned)((cpu->nzcv >> 28) & 1);
        interp_flags_nzcv(cpu, (unsigned)((ah >> 7) & 1), (unsigned)((ah >> 6) & 1), (unsigned)(ah & 1), of);
        cpu->pf = ((ah >> 2) & 1) ^ 1u; // PF lane holds a byte whose EVEN parity is PF
        cpu->af = ((ah >> 4) & 1) << 4;
        cpu->rip = next;
        return STEP_NEXT;
    }
    case 0x9F: { // LAHF: SF,ZF,0,AF,0,PF,1,CF -> AH
        uint64_t flags = interp_read_rflags(cpu);
        cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xff00)) | ((flags & 0xd5) | 0x02) << 8;
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOV to/from a moffs address
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3: {
        int width = (op & 1) ? insn->opsize : 1;
        uint64_t address = (uint64_t)insn->imm;
        if (insn->seg == 1)
            address += cpu->fs_base;
        else if (insn->seg == 2)
            address += cpu->gs_base;
        if (op <= 0xA1)
            interp_reg_write(cpu, insn, RAX, width, interp_load(address, width));
        else
            interp_store(address, width, interp_reg_read(cpu, insn, RAX, width));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVS / STOS / LODS
    case 0xA4:
    case 0xA5:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD: {
        int width = (op & 1) ? insn->opsize : 1;
        int movs = (op == 0xA4 || op == 0xA5);
        int lods = (op == 0xAC || op == 0xAD);
        int backward = (int)(cpu->df & 1);
        uint64_t step = backward ? (UINT64_C(0) - (uint64_t)width) : (uint64_t)width;
        // One element at a time, updating RCX/RSI/RDI after EACH: a fault mid-string must leave them
        // partially advanced so a guest handler can resume.
        // TODO(amd64-host): use the memcpy path for the forward non-overlapping case once it links.
        if (insn->rep && cpu->r[RCX] == 0) { // REP with RCX==0 executes nothing at all
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (insn->rep) {
            if (movs)
                hl_x86_count_rep_movs();
            else if (!lods)
                hl_x86_count_rep_stos();
        }
        uint64_t iterations = insn->rep ? cpu->r[RCX] : 1;
        while (iterations != 0) {
            if (movs) {
                interp_store(cpu->r[RDI], width, interp_load(cpu->r[RSI], width));
                cpu->r[RSI] += step;
                cpu->r[RDI] += step;
            } else if (lods) {
                interp_reg_write(cpu, insn, RAX, width, interp_load(cpu->r[RSI], width));
                cpu->r[RSI] += step;
            } else { // STOS
                interp_store(cpu->r[RDI], width, cpu->r[RAX] & interp_mask(width));
                cpu->r[RDI] += step;
            }
            if (!insn->rep) break;
            cpu->r[RCX]--;
            iterations = cpu->r[RCX];
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // CMPS / SCAS: handed whole to hl_x86_rep_compare via R_REPSTR; descriptor layout is rep.c's.
    case 0xA6:
    case 0xA7:
    case 0xAE:
    case 0xAF: {
        int width = (op & 1) ? insn->opsize : 1;
        int is_scas = (op == 0xAE || op == 0xAF);
        uint64_t descriptor = (uint64_t)width | ((uint64_t)(is_scas != 0) << 8) |
                              ((uint64_t)(insn->repne != 0) << 9) |
                              ((uint64_t)((insn->rep || insn->repne) != 0) << 10) | ((cpu->df & 1) << 11);
        cpu->divop = descriptor;
        return interp_exit(cpu, next, R_REPSTR);
    }

    // TEST AL/eAX, imm
    case 0xA8:
    case 0xA9: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_flags_logic(cpu, interp_reg_read(cpu, insn, RAX, width) & (uint64_t)insn->imm & interp_mask(width),
                           width);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOV r, imm
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
    case 0xB7:
        interp_reg_write(cpu, insn, (op & 7) | (insn->rexB << 3), 1, (uint64_t)insn->imm);
        cpu->rip = next;
        return STEP_NEXT;
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD:
    case 0xBE:
    case 0xBF:
        interp_reg_write(cpu, insn, (op & 7) | (insn->rexB << 3), insn->opsize, (uint64_t)insn->imm);
        cpu->rip = next;
        return STEP_NEXT;

    // Group 2: C0/C1 by imm8, D0/D1 by 1, D2/D3 by CL. /2 and /3 are RCL/RCR, reduced modulo width+1.
    case 0xC0:
    case 0xC1:
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {
        int kind = insn->reg & 7;
        if (kind == 6) kind = 4; // /6 SHL is SAL
        int width = (op & 1) ? insn->opsize : 1;
        int by_cl = (op == 0xD2 || op == 0xD3);
        int by_one = (op == 0xD0 || op == 0xD1);
        interp_operand operand = interp_rm(cpu, insn, next);
        if (kind == 2 || kind == 3) { // RCL / RCR
            uint64_t descriptor = (uint64_t)width | ((uint64_t)(kind == 3) << 8);
            if (operand.is_memory) {
                // rotate.c dereferences this directly: hand it the REBASED address.
                cpu->x87_ea = hl_x86_guest_pointer(operand.address);
                descriptor |= UINT64_C(1) << 9;
            } else {
                int high_byte = interp_hi8(insn, operand.number, width);
                descriptor |= ((uint64_t)(high_byte ? 1 : 0) << 10) |
                              ((uint64_t)((high_byte ? operand.number - 4 : operand.number) & 0x1f) << 16);
            }
            if (!by_cl) {
                // The helper counts from cpu->r[RCX], which a constant-count RCL/RCR must not clobber.
                unsigned count = (unsigned)(by_one ? 1 : (insn->imm & (width == 8 ? 63 : 31)));
                unsigned bits = (unsigned)(8 * width) + 1u;
                unsigned effective = count % bits;
                uint64_t m = interp_mask(width);
                uint64_t value = interp_rm_read(cpu, insn, &operand, width);
                unsigned carry = interp_cf(cpu);
                for (unsigned iteration = 0; iteration < effective; iteration++) {
                    if (kind == 2) { // RCL: CF <- msb, value <<= 1 | old CF
                        unsigned msb = interp_msb(value, width);
                        value = ((value << 1) | carry) & m;
                        carry = msb;
                    } else { // RCR: CF <- lsb, value >>= 1 with old CF entering at the top
                        unsigned lsb = (unsigned)(value & 1);
                        value = ((value >> 1) | ((uint64_t)carry << (8 * width - 1))) & m;
                        carry = lsb;
                    }
                }
                if (count != 0) {
                    interp_set_cf(cpu, carry);
                    if (count == 1) {
                        unsigned of = kind == 2 ? (interp_msb(value, width) ^ carry)
                                                : (interp_msb(value, width) ^
                                                   (unsigned)((value >> (8 * width - 2)) & 1));
                        cpu->nzcv = (cpu->nzcv & ~NZ_V) | ((uint64_t)of << 28);
                    }
                }
                interp_rm_write(cpu, insn, &operand, width, value);
                cpu->rip = next;
                return STEP_NEXT;
            }
            // hl_x86_rotate_carry decodes cpu->divop, so publishing it is not optional: a stale divop
            // reads as width 0 and the rotate silently does nothing.
            cpu->divop = descriptor;
            return interp_exit(cpu, next, R_RCL);
        }
        unsigned count = (unsigned)(by_cl ? (cpu->r[RCX] & 0xff) : (by_one ? 1u : (unsigned)(insn->imm & 0xff)));
        uint64_t value = interp_rm_read(cpu, insn, &operand, width);
        uint64_t result = interp_shift(cpu, kind, value, count, width);
        // A zero count still writes: a 32-bit register zero-extends, memory is rewritten unchanged.
        interp_rm_write(cpu, insn, &operand, width, result);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // RET and RET imm16
    case 0xC2:
    case 0xC3: {
        int width = interp_stack_width(insn);
        uint64_t target = interp_pop(cpu, width);
        if (op == 0xC2) cpu->r[RSP] += (uint64_t)(uint16_t)insn->imm;
        cpu->dbg_ibsrc = pc; // guest PC of the last indirect branch
        cpu->rip = target;
        cpu->reason = R_BRANCH;
        return STEP_END;
    }

    // MOV r/m, imm
    case 0xC6:
    case 0xC7: {
        if ((insn->reg & 7) != 0) return interp_undefined(cpu, insn, pc, "group 11 opcode other than MOV r/m,imm");
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, width, (uint64_t)insn->imm);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // LEAVE
    case 0xC9: {
        int width = interp_stack_width(insn);
        cpu->r[RSP] = cpu->r[RBP];
        cpu->r[RBP] = interp_pop(cpu, width);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // INT3: #BP -> SIGTRAP at the instruction AFTER the trap
    case 0xCC:
        return interp_guest_trap(cpu, next, 5 /*SIGTRAP*/, 1 /*TRAP_BRKPT*/);

    // LOOP / LOOPE / LOOPNE and JRCXZ
    case 0xE0:
    case 0xE1:
    case 0xE2: {
        // The counter is RCX, or ECX under a 0x67 address-size override, which also zero-extends.
        uint64_t counter = insn->addr32 ? ((cpu->r[RCX] - 1) & UINT64_C(0xffffffff)) : (cpu->r[RCX] - 1);
        cpu->r[RCX] = counter;
        int zf = (int)((cpu->nzcv >> 30) & 1);
        int take = counter != 0 && (op == 0xE2 || (op == 0xE1 ? zf : !zf));
        cpu->rip = take ? next + (uint64_t)insn->imm : next;
        cpu->reason = R_BRANCH;
        return STEP_END;
    }
    case 0xE3: {
        uint64_t counter = insn->addr32 ? (cpu->r[RCX] & UINT64_C(0xffffffff)) : cpu->r[RCX];
        cpu->rip = counter == 0 ? next + (uint64_t)insn->imm : next;
        cpu->reason = R_BRANCH;
        return STEP_END;
    }

    // CALL rel32
    case 0xE8:
        // interp_call_return_pc, not `next`: a biased non-PIE must push the LOW link address.
        interp_push(cpu, interp_call_return_pc(next), interp_stack_width(insn));
        cpu->rip = next + (uint64_t)insn->imm;
        cpu->reason = R_BRANCH;
        return STEP_END;

    // JMP rel32 / rel8
    case 0xE9:
    case 0xEB:
        cpu->rip = next + (uint64_t)insn->imm;
        cpu->reason = R_BRANCH;
        return STEP_END;

    // CMC / CLC / STC / CLD / STD
    case 0xF5:
        interp_set_cf(cpu, interp_cf(cpu) ^ 1u);
        cpu->rip = next;
        return STEP_NEXT;
    case 0xF8:
        interp_set_cf(cpu, 0);
        cpu->rip = next;
        return STEP_NEXT;
    case 0xF9:
        interp_set_cf(cpu, 1);
        cpu->rip = next;
        return STEP_NEXT;
    case 0xFC:
        cpu->df = 0;
        cpu->rip = next;
        return STEP_NEXT;
    case 0xFD:
        cpu->df = 1;
        cpu->rip = next;
        return STEP_NEXT;

    // Group 3
    case 0xF6:
    case 0xF7: {
        int width = (op & 1) ? insn->opsize : 1;
        int sub = insn->reg & 7;
        interp_operand operand = interp_rm(cpu, insn, next);
        switch (sub) {
        case 0:
        case 1: { // TEST r/m, imm
            uint64_t value = interp_rm_read(cpu, insn, &operand, width);
            interp_flags_logic(cpu, value & (uint64_t)insn->imm & interp_mask(width), width);
            break;
        }
        case 2: { // NOT: no flags at all
            if (insn->lock && operand.is_memory) {
                (void)interp_locked_rmw(operand.address, width, RMW_NOT, 0, 0);
            } else {
                uint64_t value = interp_rm_read(cpu, insn, &operand, width);
                interp_rm_write(cpu, insn, &operand, width, (~value) & interp_mask(width));
            }
            break;
        }
        case 3: { // NEG: flags exactly as SUB(0, value), so CF = (value != 0)
            uint64_t old;
            if (insn->lock && operand.is_memory)
                old = interp_locked_rmw(operand.address, width, RMW_NEG, 0, 0);
            else
                old = interp_rm_read(cpu, insn, &operand, width);
            uint64_t result = interp_alu_sub(cpu, 0, old, 0, width);
            if (!(insn->lock && operand.is_memory)) interp_rm_write(cpu, insn, &operand, width, result);
            break;
        }
        case 4: // MUL
        case 5: // IMUL (widening)
            interp_widening_multiply(cpu, insn, interp_rm_read(cpu, insn, &operand, width), width, sub == 5);
            break;
        case 6: // DIV
        case 7: // IDIV
            return interp_divide(cpu, interp_rm_read(cpu, insn, &operand, width), width, sub == 7, pc, next);
        default: break;
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // Group 4/5
    case 0xFE:
    case 0xFF: {
        int sub = insn->reg & 7;
        interp_operand operand = interp_rm(cpu, insn, next);
        if (sub == 0 || sub == 1) { // INC / DEC -- x86 leaves CF untouched
            int width = (op == 0xFE) ? 1 : insn->opsize;
            uint64_t old;
            if (insn->lock && operand.is_memory) {
                old = interp_locked_rmw(operand.address, width, sub == 0 ? RMW_INC : RMW_DEC, 0, 0);
                (void)interp_alu_incdec(cpu, old, sub == 1, width);
            } else {
                old = interp_rm_read(cpu, insn, &operand, width);
                interp_rm_write(cpu, insn, &operand, width, interp_alu_incdec(cpu, old, sub == 1, width));
            }
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (op == 0xFE) return interp_undefined(cpu, insn, pc, "group 4 opcode other than INC/DEC r/m8");
        int width = interp_stack_width(insn);
        if (sub == 2) { // CALL r/m (near, indirect)
            uint64_t target = interp_rm_read(cpu, insn, &operand, width);
            interp_push(cpu, interp_call_return_pc(next), width);
            cpu->dbg_ibsrc = pc;
            cpu->rip = target;
            cpu->reason = R_BRANCH;
            return STEP_END;
        }
        if (sub == 4) { // JMP r/m (near, indirect)
            uint64_t target = interp_rm_read(cpu, insn, &operand, width);
            cpu->dbg_ibsrc = pc;
            cpu->rip = target;
            cpu->reason = R_BRANCH;
            return STEP_END;
        }
        if (sub == 6) { // PUSH r/m
            interp_push(cpu, interp_rm_read(cpu, insn, &operand, width), width);
            cpu->rip = next;
            return STEP_NEXT;
        }
        // /3 CALLF and /5 JMPF: no segments here, and Linux never issues them.
        return interp_undefined(cpu, insn, pc, "far CALL/JMP through a segment descriptor");
    }

    // HLT: privileged -> #GP, which Linux reports as SIGSEGV/SI_KERNEL with si_addr 0, not SEGV_ACCERR.
    case 0xF4:
        return interp_guest_trap(cpu, pc, 11 /*SIGSEGV*/, 128 /*SI_KERNEL*/);

    default: break;
    }

    if (op >= 0xD8 && op <= 0xDF) return interp_step_x87(cpu, insn, pc, next);
    if (op == 0xD7) return interp_undefined(cpu, insn, pc, "TODO(amd64-host): XLATB");

    // Rest of the map, split trap-versus-gap. INTO is invalid in 64-bit mode: #UD, not a gap.
    if (op == 0xCE) return interp_guest_trap(cpu, pc, 4 /*SIGILL*/, 2 /*ILL_ILLOPN*/);

    // Port I/O at CPL 3 with IOPL 0: always #GP(0), with no implementation to add.
    if (op == 0x6C || op == 0x6D || op == 0x6E || op == 0x6F || op == 0xE4 || op == 0xE5 || op == 0xE6 || op == 0xE7 ||
        op == 0xEC || op == 0xED || op == 0xEE || op == 0xEF)
        return interp_guest_trap(cpu, pc, 11 /*SIGSEGV*/, 128 /*SI_KERNEL*/);

    // INT imm8/IRET/RETF stay REPORTED: `int $0x80` is Linux's 32-bit syscall gate -- a gap, not a fault.
    if (op == 0xCD || op == 0xCF || op == 0xCA || op == 0xCB)
        return interp_undefined(cpu, insn, pc, "software interrupt / far return (INT/IRET/RETF)");
    return interp_undefined(cpu, insn, pc, "one-byte opcode");
}

// LEGACY SSE / SSE2 (the 0F map), implemented here because only this map has no C emulator.
//
// Scope: data movement, bitwise logic, integer SIMD. FP arithmetic is reported instead; it needs an
// authoritative MXCSR (rounding mode, DAZ/FTZ, sticky flags) that only x87state.c owns.
//
// UPPER-BITS RULE, the opposite of the AArch64 one: legacy (non-VEX) SSE writes the low 128 bits and LEAVES
// BITS 128 AND ABOVE UNTOUCHED (VEX zeroes them). vhi[]/vz[] hold the AVX upper state, so interp_xmm_put
// must never clear vhi -- `pxor %xmm0,%xmm0` must not truncate ymm0.
//
// cpu->vdirty stays untouched: it only tells the JIT's R_SYSCALL exit to spill guest xmm out of host v
// registers, and cpu->v[] here IS the register file at every instruction boundary.

static void interp_xmm_get(const struct cpu *cpu, int number, uint8_t out[16]) {
    memcpy(out, &cpu->v[2 * number], 16);
}

static void interp_xmm_put(struct cpu *cpu, int number, const uint8_t in[16]) {
    memcpy(&cpu->v[2 * number], in, 16); // low 128 bits only -- see the upper-bits rule above
}

// Lane accessors. memcpy, not a cast: an odd lane offset (PINSRW word 3) stays defined.
static uint16_t interp_lane16(const uint8_t *p, int index) {
    uint16_t value;
    memcpy(&value, p + 2 * index, 2);
    return value;
}

static void interp_put16(uint8_t *p, int index, uint16_t value) {
    memcpy(p + 2 * index, &value, 2);
}

static uint32_t interp_lane32(const uint8_t *p, int index) {
    uint32_t value;
    memcpy(&value, p + 4 * index, 4);
    return value;
}

static void interp_put32(uint8_t *p, int index, uint32_t value) {
    memcpy(p + 4 * index, &value, 4);
}

static uint64_t interp_lane64(const uint8_t *p, int index) {
    uint64_t value;
    memcpy(&value, p + 8 * index, 8);
    return value;
}

static void interp_put64(uint8_t *p, int index, uint64_t value) {
    memcpy(p + 8 * index, &value, 8);
}

// F2/F3 outrank 0x66, as in hardware, so these tests are ordered rather than combined.
enum { SSE_NP = 0, SSE_66 = 1, SSE_F3 = 2, SSE_F2 = 3 };

static int interp_sse_prefix(const struct insn *insn) {
    if (insn->rep) return SSE_F3;
    if (insn->repne) return SSE_F2;
    if (insn->p66) return SSE_66;
    return SSE_NP;
}

// SSE r/m operand as `bytes` bytes: 16 packed, 4/8 scalar. `out`'s tail is zeroed for low-lane callers.
static void interp_sse_rm_get(struct cpu *cpu, const struct insn *insn, uint64_t next, unsigned bytes,
                              uint8_t out[16]) {
    memset(out, 0, 16);
    if (insn->is_mem)
        interp_load_bytes(interp_ea(cpu, insn, next), out, bytes);
    else
        memcpy(out, &cpu->v[2 * insn->rm_reg], bytes);
}

static void interp_sse_rm_put(struct cpu *cpu, const struct insn *insn, uint64_t next, unsigned bytes,
                              const uint8_t in[16]) {
    if (insn->is_mem)
        interp_store_bytes(interp_ea(cpu, insn, next), in, bytes);
    else
        memcpy(&cpu->v[2 * insn->rm_reg], in, bytes); // register destination: merge, upper lanes preserved
}

// MOVDQA/MOVAPS/MOVAPD/MOVNTDQ/MOVNTPS need a 16-byte-aligned memory operand and #GP(0) otherwise; honour
// the fault, a guest can depend on it. Linux delivers #GP as SIGSEGV/SI_KERNEL/si_addr 0.
static int interp_sse_unaligned(const struct cpu *cpu, const struct insn *insn, uint64_t next) {
    return insn->is_mem && (interp_ea(cpu, insn, next) & 15u) != 0;
}

static void interp_padd(uint8_t *d, const uint8_t *s, int lane) {
    for (int i = 0; i < 16 / lane; i++) {
        if (lane == 1)
            d[i] = (uint8_t)(d[i] + s[i]);
        else if (lane == 2)
            interp_put16(d, i, (uint16_t)(interp_lane16(d, i) + interp_lane16(s, i)));
        else if (lane == 4)
            interp_put32(d, i, interp_lane32(d, i) + interp_lane32(s, i));
        else
            interp_put64(d, i, interp_lane64(d, i) + interp_lane64(s, i));
    }
}

static void interp_psub(uint8_t *d, const uint8_t *s, int lane) {
    for (int i = 0; i < 16 / lane; i++) {
        if (lane == 1)
            d[i] = (uint8_t)(d[i] - s[i]);
        else if (lane == 2)
            interp_put16(d, i, (uint16_t)(interp_lane16(d, i) - interp_lane16(s, i)));
        else if (lane == 4)
            interp_put32(d, i, interp_lane32(d, i) - interp_lane32(s, i));
        else
            interp_put64(d, i, interp_lane64(d, i) - interp_lane64(s, i));
    }
}

static int32_t interp_sat_s8(int32_t v) {
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

static int32_t interp_sat_s16(int32_t v) {
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static int32_t interp_sat_u8(int32_t v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static int32_t interp_sat_u16(int32_t v) {
    return v < 0 ? 0 : v > 65535 ? 65535 : v;
}

static void interp_padds(uint8_t *d, const uint8_t *s, int lane, int subtract, int signed_form) {
    for (int i = 0; i < 16 / lane; i++) {
        if (lane == 1) {
            int32_t a = signed_form ? (int32_t)(int8_t)d[i] : (int32_t)d[i];
            int32_t b = signed_form ? (int32_t)(int8_t)s[i] : (int32_t)s[i];
            int32_t r = subtract ? a - b : a + b;
            d[i] = (uint8_t)(signed_form ? interp_sat_s8(r) : interp_sat_u8(r));
        } else {
            int32_t a = signed_form ? (int32_t)(int16_t)interp_lane16(d, i) : (int32_t)interp_lane16(d, i);
            int32_t b = signed_form ? (int32_t)(int16_t)interp_lane16(s, i) : (int32_t)interp_lane16(s, i);
            int32_t r = subtract ? a - b : a + b;
            interp_put16(d, i, (uint16_t)(signed_form ? interp_sat_s16(r) : interp_sat_u16(r)));
        }
    }
}

static void interp_pcmpeq(uint8_t *d, const uint8_t *s, int lane) {
    for (int i = 0; i < 16 / lane; i++) {
        if (lane == 1)
            d[i] = d[i] == s[i] ? 0xff : 0x00;
        else if (lane == 2)
            interp_put16(d, i, interp_lane16(d, i) == interp_lane16(s, i) ? 0xffffu : 0);
        else
            interp_put32(d, i, interp_lane32(d, i) == interp_lane32(s, i) ? 0xffffffffu : 0);
    }
}

// PCMPGT is SIGNED at every lane width; unsigned has no SSE2 encoding (string code uses PMINUB/PCMPEQB).
static void interp_pcmpgt(uint8_t *d, const uint8_t *s, int lane) {
    for (int i = 0; i < 16 / lane; i++) {
        if (lane == 1)
            d[i] = (int8_t)d[i] > (int8_t)s[i] ? 0xff : 0x00;
        else if (lane == 2)
            interp_put16(d, i, (int16_t)interp_lane16(d, i) > (int16_t)interp_lane16(s, i) ? 0xffffu : 0);
        else
            interp_put32(d, i, (int32_t)interp_lane32(d, i) > (int32_t)interp_lane32(s, i) ? 0xffffffffu : 0);
    }
}

// Per-lane shifts. A count >= the lane width gives zero (or a full sign fill), not a modulo -- x86's rule.
static void interp_pshift(uint8_t *d, int lane, unsigned count, int direction, int arithmetic) {
    unsigned bits = (unsigned)lane * 8u;
    for (int i = 0; i < 16 / lane; i++) {
        uint64_t value = lane == 2 ? interp_lane16(d, i) : lane == 4 ? interp_lane32(d, i) : interp_lane64(d, i);
        uint64_t result;
        if (arithmetic) {
            int64_t signed_value = lane == 2 ? (int64_t)(int16_t)value : (int64_t)(int32_t)value;
            result = (uint64_t)(signed_value >> (count >= bits ? bits - 1 : count));
        } else if (count >= bits) {
            result = 0;
        } else {
            result = direction ? (value >> count) : (value << count);
        }
        if (lane == 2)
            interp_put16(d, i, (uint16_t)result);
        else if (lane == 4)
            interp_put32(d, i, (uint32_t)result);
        else
            interp_put64(d, i, result);
    }
}

// PSLLDQ / PSRLDQ shift the WHOLE register by a BYTE count, not per-lane bits.
static void interp_pshift_bytes(uint8_t *d, unsigned count, int right) {
    uint8_t out[16] = {0};
    if (count < 16) {
        if (right)
            memcpy(out, d + count, 16 - count);
        else
            memcpy(out + count, d, 16 - count);
    }
    memcpy(d, out, 16);
}

// PUNPCK*: the destination lane comes first in each pair, which makes PUNPCKLBW with itself a broadcast.
static void interp_punpck(uint8_t *d, const uint8_t *s, int lane, int high) {
    uint8_t out[16];
    int lanes = 16 / lane;
    int base = high ? lanes / 2 : 0;
    for (int i = 0; i < lanes / 2; i++) {
        memcpy(out + (2 * i) * lane, d + (base + i) * lane, (size_t)lane);
        memcpy(out + (2 * i + 1) * lane, s + (base + i) * lane, (size_t)lane);
    }
    memcpy(d, out, 16);
}

// PACKSSWB / PACKUSWB / PACKSSDW: narrow with saturation, destination lanes then source lanes. `per` is
// 16/source_lane, NOT 8: with 8 the result's high half was left UNINITIALISED (stack garbage).
static void interp_pack(uint8_t *d, const uint8_t *s, int source_lane, int signed_result) {
    uint8_t out[16];
    int per = 16 / source_lane;
    for (int i = 0; i < per; i++) {
        if (source_lane == 2) {
            int32_t a = (int32_t)(int16_t)interp_lane16(d, i);
            int32_t b = (int32_t)(int16_t)interp_lane16(s, i);
            out[i] = (uint8_t)(signed_result ? interp_sat_s8(a) : interp_sat_u8(a));
            out[per + i] = (uint8_t)(signed_result ? interp_sat_s8(b) : interp_sat_u8(b));
        } else {
            int32_t a = (int32_t)interp_lane32(d, i);
            int32_t b = (int32_t)interp_lane32(s, i);
            interp_put16(out, i, (uint16_t)interp_sat_s16(a));
            interp_put16(out, per + i, (uint16_t)interp_sat_s16(b));
        }
    }
    memcpy(d, out, 16);
}

// STEP_SSE_UNHANDLED: not in the 0F SSE space; interp_step_two_byte diagnoses it.
enum { STEP_SSE_UNHANDLED = -1 };

static int interp_sse_is_float_arithmetic(uint8_t op) {
    if (op >= 0x58 && op <= 0x5F) return 1;                 // add/mul/cvt/sub/min/div/max
    if (op == 0x51 || op == 0x52 || op == 0x53) return 1;   // sqrt / rsqrt / rcp
    if (op == 0x2A || (op >= 0x2C && op <= 0x2F)) return 1; // cvtsi2s* / cvt*2si / ucomis* / comis*
    if (op == 0x5A || op == 0x5B) return 1;                 // cvtps2pd / cvtdq2ps
    if (op == 0xC2) return 1;                               // cmpps/cmppd/cmpss/cmpsd
    if (op == 0xE6) return 1;                               // cvtdq2pd / cvtpd2dq / cvttpd2dq
    if (op == 0x7C || op == 0x7D) return 1;                 // SSE3 hadd / hsub
    if (op == 0xD0) return 1;                               // SSE3 addsub
    return 0;
}

// SSE / SSE2 FLOATING POINT.
//
// There is no cpu->mxcsr: the guest MXCSR IS the host FP control register, because the host SSE unit
// executes the guest's FP (AArch64 projects onto FPCR/FPSR; see translate.c). So set nothing per operation
// -- run each guest instruction as the matching HOST instruction and let RC, DAZ/FZ, the sticky flags,
// x86's NaN generation and selection, MIN/MAX's non-IEEE ties and denormals come from hardware. Hence the
// _mm_* intrinsics (fixed operand order), not C `a + b`, which GCC may commute. Cost: engine FP between
// guest instructions can OR in a sticky flag the guest never raised.
//
// UPPER LANES: every scalar form MERGES, writing only the low 32 (SS) or 64 (SD) bits. _mm_*_ss/_sd have
// that shape; the one-argument unary intrinsics do not, hence `_mm_move_ss(a, _mm_sqrt_ss(b))`.

#if defined(HL_HOST_CPU_X86_64)

static int interp_fp_is_double(int prefix) {
    return prefix == SSE_66 || prefix == SSE_F2; // PD, SD
}

static int interp_fp_is_scalar(int prefix) {
    return prefix == SSE_F3 || prefix == SSE_F2; // SS, SD
}

// Bytes read from a MEMORY r/m operand: too many faults at the end of a mapping, too few substitutes zeros
// for real lanes. Trap: "F2/F3 means scalar" is the rule for the ARITHMETIC BLOCK ONLY, hence the list.
static unsigned interp_fp_source_bytes(uint8_t op, int prefix) {
    if (op == 0x5A && prefix == SSE_NP) return 8;  // CVTPS2PD: m64, two floats
    if (op == 0xE6 && prefix == SSE_F3) return 8;  // CVTDQ2PD: m64, two int32
    if (op == 0xE6 && prefix == SSE_F2) return 16; // CVTPD2DQ: PACKED despite the F2 prefix
    if (op == 0x5B) return 16;                     // CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ: 4-lane
    if (op == 0x7C || op == 0x7D || op == 0xD0) return 16; // HADD/HSUB/ADDSUB: packed under 66 AND F2
    if (!interp_fp_is_scalar(prefix)) return 16;
    return interp_fp_is_double(prefix) ? 8u : 4u;
}

static __m128 interp_fp_get_ps(const uint8_t image[16]) {
    __m128 value;
    memcpy(&value, image, 16);
    return value;
}

static __m128d interp_fp_get_pd(const uint8_t image[16]) {
    __m128d value;
    memcpy(&value, image, 16);
    return value;
}

static __m128i interp_fp_get_dq(const uint8_t image[16]) {
    __m128i value;
    memcpy(&value, image, 16);
    return value;
}

static void interp_fp_put_ps(uint8_t image[16], __m128 value) {
    memcpy(image, &value, 16);
}

static void interp_fp_put_pd(uint8_t image[16], __m128d value) {
    memcpy(image, &value, 16);
}

static void interp_fp_put_dq(uint8_t image[16], __m128i value) {
    memcpy(image, &value, 16);
}

// CMPPS/CMPSS predicate (imm8[2:0]): EQ/NEQ/UNORD/ORD are QUIET, LT/LE/NLT/NLE SIGNAL #IE on a QNaN.
static __m128 interp_fp_cmp_ps(__m128 a, __m128 b, unsigned predicate, int scalar) {
    switch (predicate & 7u) {
    case 0: return scalar ? _mm_cmpeq_ss(a, b) : _mm_cmpeq_ps(a, b);
    case 1: return scalar ? _mm_cmplt_ss(a, b) : _mm_cmplt_ps(a, b);
    case 2: return scalar ? _mm_cmple_ss(a, b) : _mm_cmple_ps(a, b);
    case 3: return scalar ? _mm_cmpunord_ss(a, b) : _mm_cmpunord_ps(a, b);
    case 4: return scalar ? _mm_cmpneq_ss(a, b) : _mm_cmpneq_ps(a, b);
    case 5: return scalar ? _mm_cmpnlt_ss(a, b) : _mm_cmpnlt_ps(a, b);
    case 6: return scalar ? _mm_cmpnle_ss(a, b) : _mm_cmpnle_ps(a, b);
    default: return scalar ? _mm_cmpord_ss(a, b) : _mm_cmpord_ps(a, b);
    }
}

static __m128d interp_fp_cmp_pd(__m128d a, __m128d b, unsigned predicate, int scalar) {
    switch (predicate & 7u) {
    case 0: return scalar ? _mm_cmpeq_sd(a, b) : _mm_cmpeq_pd(a, b);
    case 1: return scalar ? _mm_cmplt_sd(a, b) : _mm_cmplt_pd(a, b);
    case 2: return scalar ? _mm_cmple_sd(a, b) : _mm_cmple_pd(a, b);
    case 3: return scalar ? _mm_cmpunord_sd(a, b) : _mm_cmpunord_pd(a, b);
    case 4: return scalar ? _mm_cmpneq_sd(a, b) : _mm_cmpneq_pd(a, b);
    case 5: return scalar ? _mm_cmpnlt_sd(a, b) : _mm_cmpnlt_pd(a, b);
    case 6: return scalar ? _mm_cmpnle_sd(a, b) : _mm_cmpnle_pd(a, b);
    default: return scalar ? _mm_cmpord_sd(a, b) : _mm_cmpord_pd(a, b);
    }
}

// COMIS* (0F 2F) and UCOMIS* (0F 2E) are the only SSE instructions that write EFLAGS, and differ only in
// which NaN raises #IE (UCOMIS*: signalling only). ZF/PF/CF only (greater 000, less 001, equal 100,
// unordered 111), OF/SF/AF architecturally ZERO. setcc, not PUSHFQ: `pushfq` writes into the red zone.
static void interp_fp_comis_flags(struct cpu *cpu, unsigned char zf, unsigned char pf, unsigned char cf) {
    interp_flags_nzcv(cpu, 0 /*SF*/, zf, cf, 0 /*OF*/);
    // cpu->pf is a byte whose EVEN parity is x86 PF, so PF=1 (unordered) is byte 0 and PF=0 is 1.
    cpu->pf = pf ? 0u : 1u;
    cpu->af = 0;
}

#define INTERP_FP_COMIS(cpu, mnemonic, left, right)                                                                    \
    do {                                                                                                               \
        unsigned char zf_, pf_, cf_;                                                                                   \
        __asm__ volatile(mnemonic " %[b], %[a]\n\tsetz %[z]\n\tsetp %[p]\n\tsetc %[c]"                                 \
                         : [z] "=r"(zf_), [p] "=r"(pf_), [c] "=r"(cf_)                                                 \
                         : [a] "x"(left), [b] "x"(right)                                                               \
                         : "cc");                                                                                      \
        interp_fp_comis_flags((cpu), zf_, pf_, cf_);                                                                   \
    } while (0)

static int interp_step_sse_fp(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;
    int prefix = interp_sse_prefix(insn);
    int destination = insn->reg;
    int dbl = interp_fp_is_double(prefix);
    int scalar = interp_fp_is_scalar(prefix);
    unsigned source_bytes = interp_fp_source_bytes(op, prefix);
    uint8_t d[16], s[16];

    // 0x2A/0x2C/0x2D have MMX siblings; there is no mm[] register file, so report rather than alias to xmm.
    if ((op == 0x2A || op == 0x2C || op == 0x2D) && !scalar)
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX CVTPI2P*/CVTP*2PI (no mm[] register file)");
    // RSQRT and RCP are single-precision only; there is no RSQRTPD/RCPSD encoding to reach.
    if ((op == 0x52 || op == 0x53) && dbl) return interp_undefined(cpu, insn, pc, "reserved (no RSQRTPD/RCPPD)");

    switch (op) {
    // CVTSI2SS / CVTSI2SD (F3/F2 0F 2A): an INTEGER r/m, hence the ordinary integer path; destination merges.
    case 0x2A: {
        interp_operand operand = interp_rm(cpu, insn, next);
        int width = insn->rexW ? 8 : 4;
        uint64_t raw = interp_rm_read(cpu, insn, &operand, width);
        interp_xmm_get(cpu, destination, d);
        if (dbl) {
            __m128d a = interp_fp_get_pd(d);
            a = width == 8 ? _mm_cvtsi64_sd(a, (long long)raw) : _mm_cvtsi32_sd(a, (int)(uint32_t)raw);
            interp_fp_put_pd(d, a);
        } else {
            __m128 a = interp_fp_get_ps(d);
            a = width == 8 ? _mm_cvtsi64_ss(a, (long long)raw) : _mm_cvtsi32_ss(a, (int)(uint32_t)raw);
            interp_fp_put_ps(d, a);
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // CVTTSS2SI (0F 2C, truncating) and CVTSS2SI (0F 2D, per MXCSR.RC). ModRM.reg names a GPR here.
    case 0x2C:
    case 0x2D: {
        int width = insn->rexW ? 8 : 4;
        int64_t value;
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (dbl) {
            __m128d b = interp_fp_get_pd(s);
            if (width == 8)
                value = op == 0x2C ? _mm_cvttsd_si64(b) : _mm_cvtsd_si64(b);
            else
                value = op == 0x2C ? (int64_t)_mm_cvttsd_si32(b) : (int64_t)_mm_cvtsd_si32(b);
        } else {
            __m128 b = interp_fp_get_ps(s);
            if (width == 8)
                value = op == 0x2C ? _mm_cvttss_si64(b) : _mm_cvtss_si64(b);
            else
                value = op == 0x2C ? (int64_t)_mm_cvttss_si32(b) : (int64_t)_mm_cvtss_si32(b);
        }
        interp_reg_write(cpu, insn, destination, width, (uint64_t)value);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0x2E:
    case 0x2F: {
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, dbl ? 8u : 4u, s);
        if (dbl) {
            __m128d a = interp_fp_get_pd(d), b = interp_fp_get_pd(s);
            if (op == 0x2E)
                INTERP_FP_COMIS(cpu, "ucomisd", a, b);
            else
                INTERP_FP_COMIS(cpu, "comisd", a, b);
        } else {
            __m128 a = interp_fp_get_ps(d), b = interp_fp_get_ps(s);
            if (op == 0x2E)
                INTERP_FP_COMIS(cpu, "ucomiss", a, b);
            else
                INTERP_FP_COMIS(cpu, "comiss", a, b);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // SQRT (51), RSQRT (52), RCP (53). Unary; a scalar form's upper lanes come from the DESTINATION.
    case 0x51:
    case 0x52:
    case 0x53: {
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (dbl) {
            __m128d a = interp_fp_get_pd(d), b = interp_fp_get_pd(s);
            interp_fp_put_pd(d, scalar ? _mm_sqrt_sd(a, b) : _mm_sqrt_pd(b));
        } else {
            __m128 a = interp_fp_get_ps(d), b = interp_fp_get_ps(s);
            __m128 r = op == 0x51 ? _mm_sqrt_ps(b) : op == 0x52 ? _mm_rsqrt_ps(b) : _mm_rcp_ps(b);
            interp_fp_put_ps(d, scalar ? _mm_move_ss(a, r) : r);
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ADD (58) MUL (59) SUB (5C) MIN (5D) DIV (5E) MAX (5F) -- singly, since 0x5A/0x5B are conversions.
    case 0x58:
    case 0x59:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F: {
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (dbl) {
            __m128d a = interp_fp_get_pd(d), b = interp_fp_get_pd(s), r;
            switch (op) {
            case 0x58: r = scalar ? _mm_add_sd(a, b) : _mm_add_pd(a, b); break;
            case 0x59: r = scalar ? _mm_mul_sd(a, b) : _mm_mul_pd(a, b); break;
            case 0x5C: r = scalar ? _mm_sub_sd(a, b) : _mm_sub_pd(a, b); break;
            case 0x5D: r = scalar ? _mm_min_sd(a, b) : _mm_min_pd(a, b); break;
            case 0x5E: r = scalar ? _mm_div_sd(a, b) : _mm_div_pd(a, b); break;
            default: r = scalar ? _mm_max_sd(a, b) : _mm_max_pd(a, b); break;
            }
            interp_fp_put_pd(d, r);
        } else {
            __m128 a = interp_fp_get_ps(d), b = interp_fp_get_ps(s), r;
            switch (op) {
            case 0x58: r = scalar ? _mm_add_ss(a, b) : _mm_add_ps(a, b); break;
            case 0x59: r = scalar ? _mm_mul_ss(a, b) : _mm_mul_ps(a, b); break;
            case 0x5C: r = scalar ? _mm_sub_ss(a, b) : _mm_sub_ps(a, b); break;
            case 0x5D: r = scalar ? _mm_min_ss(a, b) : _mm_min_ps(a, b); break;
            case 0x5E: r = scalar ? _mm_div_ss(a, b) : _mm_div_ps(a, b); break;
            default: r = scalar ? _mm_max_ss(a, b) : _mm_max_ps(a, b); break;
            }
            interp_fp_put_ps(d, r);
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // 0F 5A: CVTPS2PD (np) / CVTPD2PS (66) / CVTSS2SD (F3) / CVTSD2SS (F2). The packed forms write the whole
    // destination (CVTPD2PS zeroes the upper 64 bits); the scalar forms merge.
    case 0x5A: {
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (prefix == SSE_NP)
            interp_fp_put_pd(d, _mm_cvtps_pd(interp_fp_get_ps(s)));
        else if (prefix == SSE_66)
            interp_fp_put_ps(d, _mm_cvtpd_ps(interp_fp_get_pd(s)));
        else if (prefix == SSE_F3)
            interp_fp_put_pd(d, _mm_cvtss_sd(interp_fp_get_pd(d), interp_fp_get_ps(s)));
        else
            interp_fp_put_ps(d, _mm_cvtsd_ss(interp_fp_get_ps(d), interp_fp_get_pd(s)));
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // 0F 5B: CVTDQ2PS (np) / CVTPS2DQ (66, per MXCSR.RC) / CVTTPS2DQ (F3, truncates). All 4-lane, 16 bytes.
    case 0x5B: {
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (prefix == SSE_NP)
            interp_fp_put_ps(d, _mm_cvtepi32_ps(interp_fp_get_dq(s)));
        else if (prefix == SSE_66)
            interp_fp_put_dq(d, _mm_cvtps_epi32(interp_fp_get_ps(s)));
        else if (prefix == SSE_F3)
            interp_fp_put_dq(d, _mm_cvttps_epi32(interp_fp_get_ps(s)));
        else
            return interp_undefined(cpu, insn, pc, "reserved (F2 0F 5B)");
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // 0F E6: CVTTPD2DQ (66) / CVTDQ2PD (F3, widens the low half) / CVTPD2DQ (F2). PD->DQ ZEROes the upper.
    case 0xE6: {
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (prefix == SSE_66)
            interp_fp_put_dq(d, _mm_cvttpd_epi32(interp_fp_get_pd(s)));
        else if (prefix == SSE_F3)
            interp_fp_put_pd(d, _mm_cvtepi32_pd(interp_fp_get_dq(s)));
        else if (prefix == SSE_F2)
            interp_fp_put_dq(d, _mm_cvtpd_epi32(interp_fp_get_pd(s)));
        else
            return interp_undefined(cpu, insn, pc, "reserved (0F E6 with no mandatory prefix)");
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // SSE3 horizontal add/sub (0F 7C/7D) and ADDSUBPS/PD (0F D0), from SSE2 shuffles rather than
    // _mm_hadd_ps: those need -msse3 and one binary ships per host OS/CPU pair, so HADDPS would SIGILL on a
    // pre-Prescott x86-64 (avx.c refuses -mf16c likewise). Exact, not approximate: the shuffles preserve
    // operand ORDER, which decides NaN selection. Prefix trap: 0x66 is PD and 0xF2 is PS, not scalar.
    case 0x7C:
    case 0x7D:
    case 0xD0: {
        int pd = prefix == SSE_66;
        if (!pd && prefix != SSE_F2) return interp_undefined(cpu, insn, pc, "reserved (SSE3 0F 7C/7D/D0 prefix)");
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        if (pd) {
            __m128d a = interp_fp_get_pd(d), b = interp_fp_get_pd(s), r;
            if (op == 0xD0) { // ADDSUBPD: lane0 subtracts, lane1 adds
                __m128d mask = _mm_castsi128_pd(_mm_set_epi64x(0, -1));
                r = _mm_or_pd(_mm_and_pd(mask, _mm_sub_pd(a, b)), _mm_andnot_pd(mask, _mm_add_pd(a, b)));
            } else {
                __m128d even = _mm_unpacklo_pd(a, b);
                __m128d odd = _mm_unpackhi_pd(a, b);
                r = op == 0x7C ? _mm_add_pd(even, odd) : _mm_sub_pd(even, odd);
            }
            interp_fp_put_pd(d, r);
        } else {
            __m128 a = interp_fp_get_ps(d), b = interp_fp_get_ps(s), r;
            if (op == 0xD0) { // ADDSUBPS: even lanes subtract, odd lanes add
                __m128 mask = _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, -1));
                r = _mm_or_ps(_mm_and_ps(mask, _mm_sub_ps(a, b)), _mm_andnot_ps(mask, _mm_add_ps(a, b)));
            } else {
                __m128 even = _mm_shuffle_ps(a, b, 0x88); // {a0, a2, b0, b2}
                __m128 odd = _mm_shuffle_ps(a, b, 0xdd);  // {a1, a3, b1, b3}
                r = op == 0x7C ? _mm_add_ps(even, odd) : _mm_sub_ps(even, odd);
            }
            interp_fp_put_ps(d, r);
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xC2: {
        unsigned predicate = (unsigned)insn->imm & 7u;
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, source_bytes, s);
        if (dbl)
            interp_fp_put_pd(d, interp_fp_cmp_pd(interp_fp_get_pd(d), interp_fp_get_pd(s), predicate, scalar));
        else
            interp_fp_put_ps(d, interp_fp_cmp_ps(interp_fp_get_ps(d), interp_fp_get_ps(s), predicate, scalar));
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    default: break;
    }
    return interp_undefined(cpu, insn, pc, "SSE floating-point opcode");
}

#else // !HL_HOST_CPU_X86_64

// Neither AArch64 nor x86-64: no host FP unit to be authoritative, and a software MXCSR would need a new
// cpu->mxcsr field, i.e. a checkpoint-format change. Report instead of computing plausible numbers.
static int interp_step_sse_fp(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    (void)next;
    return interp_undefined(cpu, insn, pc,
                            "SSE floating point on a non-x86-64 host (needs a software MXCSR: rounding, "
                            "DAZ/FZ and the sticky exception flags)");
}

#endif

// x87 -- the D8..DF ESC space.
//
// The state model is fixed elsewhere: `double st[8]`, `fptop`, `fpsw`, `fpcw` in struct cpu are a THREE-WAY
// ABI -- AArch64 emitters bake the offsets, checkpoints record sizeof(struct cpu), signal.c projects fpcw
// into the xsave area. Reproduce it; ST(i)'s precision cost is in lower/x87.c. On x86-64 the C double
// arithmetic below runs on SSE2 scalars, so the QNaN indefinite already carries x86's SET sign and
// FCOM/FUCOM's #IA distinction falls out of COMISD/UCOMISD. The ops round per the live MXCSR, not per
// x87's own FCW.RC, as on AArch64; FRNDINT and FIST/FISTP, where glibc's floorl/truncl and llrint make
// FCW.RC load-bearing, do honour it explicitly.
//
// ESC decode trap: /digit depends on the opcode byte AND on mod, the operand type rides on the opcode not
// a prefix, and the register forms of DC/DE SWAP the reverse-subtract/divide digits.

// ST(i) relative to fptop; the stack grows DOWNWARD.
static double interp_x87_get(const struct cpu *cpu, int index) {
    return cpu->st[(cpu->fptop + (uint64_t)(unsigned)index) & 7];
}

static void interp_x87_set(struct cpu *cpu, int index, double value) {
    cpu->st[(cpu->fptop + (uint64_t)(unsigned)index) & 7] = value;
}

static void interp_x87_push(struct cpu *cpu, double value) {
    cpu->fptop = (cpu->fptop - 1) & 7;
    cpu->st[cpu->fptop & 7] = value;
}

static void interp_x87_pop(struct cpu *cpu) {
    cpu->fptop = (cpu->fptop + 1) & 7;
}

// cpu->fpsw holds ONLY the condition codes (0x4700); the exception flags live in the host FPSW.
static void interp_x87_condition(struct cpu *cpu, unsigned c0, unsigned c1, unsigned c2, unsigned c3) {
    uint64_t status = cpu->fpsw & ~UINT64_C(0x4700);
    if (c0) status |= UINT64_C(1) << 8;
    if (c1) status |= UINT64_C(1) << 9;
    if (c2) status |= UINT64_C(1) << 10;
    if (c3) status |= UINT64_C(1) << 14;
    cpu->fpsw = status;
}

// C1 (FSW bit 9) is visible via FNSTSW/FXSAVE. Arithmetic ops write 0: no exact unrounded result.
static void interp_x87_c1(struct cpu *cpu, unsigned value) {
    if (value)
        cpu->fpsw |= UINT64_C(1) << 9;
    else
        cpu->fpsw &= ~(UINT64_C(1) << 9);
}

// A NaN compares false, so C1 = 0.
static unsigned interp_x87_rounded_up(double result, double original) {
    return (unsigned)(result > original);
}

// Not rint/nearbyint: those follow the live HOST rounding mode.
static double interp_round_half_even(double value) {
    double truncated;
    double fraction;
    double magnitude;
    if (!isfinite(value)) return value;
    truncated = trunc(value);
    fraction = value - truncated; // |value| >= 2^52 is already integral
    magnitude = fabs(fraction);
    if (magnitude > 0.5 || (magnitude == 0.5 && fmod(truncated, 2.0) != 0.0)) truncated += fraction > 0.0 ? 1.0 : -1.0;
    return truncated;
}

// Only for exceptions the host FP unit cannot raise itself: the FIST/FISTP out-of-range #IA.
static void interp_fp_raise(unsigned flags) {
#if defined(HL_HOST_CPU_X86_64)
    _mm_setcsr(_mm_getcsr() | (flags & 0x3fu));
#else
    (void)flags; // no host FP status word here
#endif
}

// x87 FSW and MXCSR exception bits are the SAME host bits here, so FNCLEX also clears the SSE flags.
static void interp_fp_clear_exceptions(void) {
#if defined(HL_HOST_CPU_X86_64)
    _mm_setcsr(_mm_getcsr() & ~0x3fu);
#endif
}

// TOP at 13:11, ES(7)/B(15) when a raised exception is UNMASKED per FCW. Must match hl_x86_fxsave.
static uint16_t interp_x87_status_word(const struct cpu *cpu) {
    uint16_t status = (uint16_t)((cpu->fpsw & 0x4700) | ((cpu->fptop & 7) << 11));
#if defined(HL_HOST_CPU_X86_64)
    uint16_t raised = (uint16_t)(_mm_getcsr() & 0x3fu); // same bit positions as MXCSR
    status |= raised;
    if (raised & (uint16_t)(~cpu->fpcw & 0x3f)) status |= (uint16_t)0x8080; // ES(7) + B(15)
#endif
    return status;
}

// COMISD raises #IE for ANY NaN, UCOMISD only for an sNaN -- the FCOM/FUCOM distinction. FSW codes are
// (C0,C2,C3) = (CF,PF,ZF), hence the EFLAGS shape.
static void interp_x87_compare_flags(double left, double right, int signalling, unsigned char *zf, unsigned char *pf,
                                     unsigned char *cf) {
#if defined(HL_HOST_CPU_X86_64)
    __m128d a = _mm_set_sd(left);
    __m128d b = _mm_set_sd(right);
    if (signalling)
        __asm__ volatile("comisd %[b], %[a]\n\tsetz %[z]\n\tsetp %[p]\n\tsetc %[c]"
                         : [z] "=r"(*zf), [p] "=r"(*pf), [c] "=r"(*cf)
                         : [a] "x"(a), [b] "x"(b)
                         : "cc");
    else
        __asm__ volatile("ucomisd %[b], %[a]\n\tsetz %[z]\n\tsetp %[p]\n\tsetc %[c]"
                         : [z] "=r"(*zf), [p] "=r"(*pf), [c] "=r"(*cf)
                         : [a] "x"(a), [b] "x"(b)
                         : "cc");
#else
    // No host compare: the signalling-vs-quiet #IE distinction is lost.
    (void)signalling;
    int unordered = isunordered(left, right);
    *pf = (unsigned char)(unordered ? 1 : 0);
    *zf = (unsigned char)((unordered || left == right) ? 1 : 0);
    *cf = (unsigned char)((unordered || left < right) ? 1 : 0);
#endif
}

static void interp_x87_compare_fpsw(struct cpu *cpu, double left, double right, int signalling) {
    unsigned char zf, pf, cf;
    interp_x87_compare_flags(left, right, signalling, &zf, &pf, &cf);
    interp_x87_condition(cpu, cf, 0, pf, zf);
}

static void interp_x87_compare_eflags(struct cpu *cpu, double left, double right, int signalling) {
    unsigned char zf, pf, cf;
    interp_x87_compare_flags(left, right, signalling, &zf, &pf, &cf);
    interp_flags_nzcv(cpu, 0, zf, cf, 0);
    cpu->pf = pf ? 0u : 1u; // EVEN parity of this byte is x86 PF
    cpu->af = 0;
    interp_x87_c1(cpu, 0); // FSW codes untouched; C1 is defined as 0
}

// Under x87 RC (FCW 11:10), not the live host mode.
static double interp_x87_round_integral(const struct cpu *cpu, double value) {
    unsigned rc = (unsigned)((cpu->fpcw >> 10) & 3u);
    if (!isfinite(value)) return value;
    switch (rc) {
    case 1: return floor(value);                   // -inf
    case 2: return ceil(value);                    // +inf
    case 3: return trunc(value);                   // zero
    default: return interp_round_half_even(value); // RC=00 default
    }
}

// Out of range (NaN and infinity included: all comparisons below false) gives INTEGER INDEFINITE.
static uint64_t interp_x87_to_integer(double value, int bytes, int *invalid) {
    double low = bytes == 2 ? -32768.0 : bytes == 4 ? -2147483648.0 : -9223372036854775808.0;
    double high = bytes == 2 ? 32767.0 : bytes == 4 ? 2147483647.0 : 9223372036854775808.0;
    // EXCLUSIVE: 2^63 is a double but not an int64.
    int in_range = bytes == 8 ? (value >= low && value < high) : (value >= low && value <= high);
    if (!in_range) {
        *invalid = 1;
        return bytes == 2 ? UINT64_C(0x8000) : bytes == 4 ? UINT64_C(0x80000000) : (UINT64_C(1) << 63);
    }
    *invalid = 0;
    return (uint64_t)(int64_t)value;
}

// FSUBR/FDIVR reversed, so the DC/DE digit swap is one decision.
static double interp_x87_arith(int kind, double destination, double source) {
    switch (kind) {
    case 0: return destination + source;  // FADD
    case 1: return destination * source;  // FMUL
    case 4: return destination - source;  // FSUB
    case 5: return source - destination;  // FSUBR
    case 6: return destination / source;  // FDIV
    default: return source / destination; // FDIVR
    }
}

// FXAM {C3,C2,C0}: zero=100, NaN=001, Inf=011, denormal=110, normal=010; C1 sign. No tag word: no 101.
static void interp_x87_classify(struct cpu *cpu) {
    uint64_t bits;
    double value = interp_x87_get(cpu, 0);
    memcpy(&bits, &value, sizeof bits);
    {
        unsigned sign = (unsigned)(bits >> 63);
        unsigned exponent_max = (unsigned)(((bits >> 52) & 0x7ff) == 0x7ff);
        unsigned exponent_zero = (unsigned)(((bits >> 52) & 0x7ff) == 0);
        unsigned mantissa_zero = (unsigned)((bits & ((UINT64_C(1) << 52) - 1)) == 0);
        unsigned is_zero = exponent_zero & mantissa_zero;
        unsigned is_nan = exponent_max & (unsigned)!mantissa_zero;
        interp_x87_condition(cpu, exponent_max, sign, (unsigned)!(is_zero | is_nan), exponent_zero);
    }
}

// ST0 <- unbiased exponent, then the significand (in [1,2), ST0's sign) is pushed. Like the JIT, no
// -inf/+inf/NaN for zero or non-finite.
static void interp_x87_extract(struct cpu *cpu) {
    uint64_t bits;
    double value = interp_x87_get(cpu, 0);
    double exponent;
    double significand;
    memcpy(&bits, &value, sizeof bits);
    exponent = (double)((int64_t)((bits >> 52) & 0x7ff) - 1023);
    bits = (bits & ~(UINT64_C(0x7ff) << 52)) | (UINT64_C(1023) << 52);
    memcpy(&significand, &bits, sizeof significand);
    interp_x87_set(cpu, 0, exponent);
    interp_x87_push(cpu, significand);
}

// Identity for non-finite or zero ST0 (scalbn's rule); the BIASED exponent clamps to [0,2047].
static double interp_x87_scale(double value, double exponent) {
    double power;
    int64_t biased;
    uint64_t bits;
    if (!isfinite(value) || value == 0.0) return value;
    if (isnan(exponent)) return value + exponent; // propagate ST1's NaN
    if (exponent > 4096.0)
        exponent = 4096.0;
    else if (exponent < -4096.0)
        exponent = -4096.0;
    biased = (int64_t)trunc(exponent) + 1023;
    if (biased > 2047)
        biased = 2047;
    else if (biased < 0)
        biased = 0;
    bits = (uint64_t)biased << 52;
    memcpy(&power, &bits, sizeof power);
    return value * power;
}

// Reduction completes in one step, so C2 <- 0, terminating libc's `do { fprem } while (C2)`. BOTH
// publish |Q|'s low three bits as C1/C3/C0; lower/x87.c and qemu wrongly clear them for FPREM1.
static void interp_x87_remainder(struct cpu *cpu, int ieee) {
    double st0 = interp_x87_get(cpu, 0);
    double st1 = interp_x87_get(cpu, 1);
    double result = ieee ? remainder(st0, st1) : fmod(st0, st1);
    unsigned c0 = 0, c1 = 0, c3 = 0;
    interp_x87_set(cpu, 0, result);
    if (isfinite(st0) && isfinite(st1) && st1 != 0.0) {
        double ratio = st0 / st1;
        double quotient = ieee ? interp_round_half_even(ratio) : trunc(ratio);
        if (fabs(quotient) < 9007199254740992.0) { // 2^53: beyond it the low bits are gone
            uint64_t magnitude = (uint64_t)fabs(quotient);
            c1 = (unsigned)(magnitude & 1u);
            c3 = (unsigned)((magnitude >> 1) & 1u);
            c0 = (unsigned)((magnitude >> 2) & 1u);
        }
    }
    interp_x87_condition(cpu, c0, c1, 0 /*C2: reduction complete*/, c3);
}

// FCW@0, FSW@4, FTW@8, pointers @12..24. No per-register tags: FTW all-empty, ignored on load.
static void interp_x87_store_environment(struct cpu *cpu, uint64_t address) {
    interp_store(address + 0, 2, cpu->fpcw & 0xffff);
    interp_store(address + 4, 2, interp_x87_status_word(cpu));
    interp_store(address + 8, 2, 0xffff);
    interp_store(address + 12, 4, 0);
    interp_store(address + 16, 4, 0);
    interp_store(address + 20, 4, 0);
    interp_store(address + 24, 4, 0);
}

static void interp_x87_load_environment(struct cpu *cpu, uint64_t address) {
    uint64_t control = interp_load(address + 0, 2);
    uint64_t status = interp_load(address + 4, 2);
    cpu->fpcw = control;
    cpu->fpsw = status & 0x4700; // condition codes only
    cpu->fptop = (status >> 11) & 7;
}

// Bit patterns: what a real FPU's 80-bit constant narrows to.
static const uint64_t g_x87_constants[8] = {
    UINT64_C(0x3FF0000000000000), // FLD1
    UINT64_C(0x400A934F0979A371), // FLDL2T
    UINT64_C(0x3FF71547652B82FE), // FLDL2E
    UINT64_C(0x400921FB54442D18), // FLDPI
    UINT64_C(0x3FD34413509F79FF), // FLDLG2
    UINT64_C(0x3FE62E42FEFA39EF), // FLDLN2
    UINT64_C(0x0000000000000000), // FLDZ
    UINT64_C(0x0000000000000000), // no EF form
};

static double interp_x87_load_f32(uint64_t address) {
    uint32_t bits = (uint32_t)interp_load(address, 4);
    float value;
    memcpy(&value, &bits, sizeof value);
    return (double)value;
}

static double interp_x87_load_f64(uint64_t address) {
    uint64_t bits = interp_load(address, 8);
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

// The one x87 store that can round.
static unsigned interp_x87_store_f32(uint64_t address, double value) {
    float narrowed = (float)value; // rounds per the live host mode, not FCW.RC
    uint32_t bits;
    memcpy(&bits, &narrowed, sizeof bits);
    interp_store(address, 4, bits);
    return interp_x87_rounded_up((double)narrowed, value);
}

static void interp_x87_store_f64(uint64_t address, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    interp_store(address, 8, bits); // exact: the ST carrier IS a double
}

// D8/DC m32/m64 float, DA/DE m32/m16 SIGNED int; destination always ST0.
static int interp_x87_memory_arith(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next, double source) {
    int kind = insn->reg & 7;
    double st0 = interp_x87_get(cpu, 0);
    if (kind == 2 || kind == 3) { // FCOM / FCOMP: signalling
        interp_x87_compare_fpsw(cpu, st0, source, 1);
        if (kind == 3) interp_x87_pop(cpu);
        cpu->rip = next;
        return STEP_NEXT;
    }
    (void)pc;
    interp_x87_set(cpu, 0, interp_x87_arith(kind, st0, source));
    interp_x87_c1(cpu, 0); // see interp_x87_c1
    cpu->rip = next;
    return STEP_NEXT;
}

static int interp_step_x87(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;
    // Neither the /digit nor the ST(i) selector is REX-extended: x87 has eight slots.
    int reg = insn->reg & 7;
    int rm = insn->rm & 7;

    if (insn->is_mem) {
        uint64_t address = interp_ea(cpu, insn, next);
        switch (op) {
        case 0xD8: return interp_x87_memory_arith(cpu, insn, pc, next, interp_x87_load_f32(address));
        case 0xDC: return interp_x87_memory_arith(cpu, insn, pc, next, interp_x87_load_f64(address));
        case 0xDA: return interp_x87_memory_arith(cpu, insn, pc, next, (double)(int32_t)interp_load(address, 4));
        case 0xDE: return interp_x87_memory_arith(cpu, insn, pc, next, (double)(int16_t)interp_load(address, 2));

        case 0xD9:
            switch (reg) {
            case 0: // FLD m32
                interp_x87_push(cpu, interp_x87_load_f32(address));
                interp_x87_c1(cpu, 0);
                break;
            case 2: // FST m32
            case 3: // FSTP m32
                interp_x87_c1(cpu, interp_x87_store_f32(address, interp_x87_get(cpu, 0)));
                if (reg == 3) interp_x87_pop(cpu);
                break;
            case 4: interp_x87_load_environment(cpu, address); break;    // FLDENV m28
            case 5: cpu->fpcw = interp_load(address, 2); break;          // FLDCW m16
            case 6: interp_x87_store_environment(cpu, address); break;   // FNSTENV m28
            case 7: interp_store(address, 2, cpu->fpcw & 0xffff); break; // FNSTCW m16
            default: return interp_undefined(cpu, insn, pc, "x87 D9 memory form");
            }
            cpu->rip = next;
            return STEP_NEXT;

        case 0xDB:
            switch (reg) {
            case 0: // FILD m32
                interp_x87_push(cpu, (double)(int32_t)interp_load(address, 4));
                interp_x87_c1(cpu, 0);
                break;
            case 1:   // FISTTP m32
            case 2:   // FIST m32
            case 3: { // FISTP m32
                int invalid = 0;
                // FISTTP truncates; FIST/FISTP round per FCW.
                double value = interp_x87_get(cpu, 0);
                double rounded = reg == 1 ? trunc(value) : interp_x87_round_integral(cpu, value);
                uint64_t stored = interp_x87_to_integer(rounded, 4, &invalid);
                if (invalid) interp_fp_raise(1u /*IE*/);
                interp_store(address, 4, stored);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                if (reg != 2) interp_x87_pop(cpu);
                break;
            }
            case 5: { // FLD m80 -- shared 80-bit converter
                uint8_t image[10];
                double value;
                interp_load_bytes(address, image, sizeof image);
                value = hl_x86_ext80_load(image);
                interp_x87_push(cpu, value);
                interp_x87_c1(cpu, 0);
                break;
            }
            case 7: { // FSTP m80
                uint8_t image[10];
                hl_x86_ext80_store(interp_x87_get(cpu, 0), image);
                interp_store_bytes(address, image, sizeof image);
                interp_x87_c1(cpu, 0);
                interp_x87_pop(cpu);
                break;
            }
            default: return interp_undefined(cpu, insn, pc, "x87 DB memory form");
            }
            cpu->rip = next;
            return STEP_NEXT;

        case 0xDD:
            switch (reg) {
            case 0: // FLD m64
                interp_x87_push(cpu, interp_x87_load_f64(address));
                interp_x87_c1(cpu, 0);
                break;
            case 1: { // FISTTP m64
                int invalid = 0;
                double value = interp_x87_get(cpu, 0);
                double rounded = trunc(value);
                uint64_t stored = interp_x87_to_integer(rounded, 8, &invalid);
                if (invalid) interp_fp_raise(1u);
                interp_store(address, 8, stored);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                interp_x87_pop(cpu);
                break;
            }
            case 2: // FST m64
            case 3: // FSTP m64
                interp_x87_store_f64(address, interp_x87_get(cpu, 0));
                interp_x87_c1(cpu, 0);
                if (reg == 3) interp_x87_pop(cpu);
                break;
            case 7: interp_store(address, 2, interp_x87_status_word(cpu)); break; // FNSTSW m16
            default:
                // FRSTOR/FNSAVE m108 also cover the eight 80-bit slots; wrong layout is silent.
                return interp_undefined(cpu, insn, pc, "x87 FNSAVE/FRSTOR m108 (DD /4,/6)");
            }
            cpu->rip = next;
            return STEP_NEXT;

        case 0xDF:
            switch (reg) {
            case 0: // FILD m16
                interp_x87_push(cpu, (double)(int16_t)interp_load(address, 2));
                interp_x87_c1(cpu, 0);
                break;
            case 1:   // FISTTP m16
            case 2:   // FIST m16
            case 3: { // FISTP m16
                int invalid = 0;
                double value = interp_x87_get(cpu, 0);
                double rounded = reg == 1 ? trunc(value) : interp_x87_round_integral(cpu, value);
                uint64_t stored = interp_x87_to_integer(rounded, 2, &invalid);
                if (invalid) interp_fp_raise(1u);
                interp_store(address, 2, stored);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                if (reg != 2) interp_x87_pop(cpu);
                break;
            }
            // FILD m64: the only x87 load that can round -- beyond 2^53 the carrier loses bits. C1 = 0.
            case 5:
                interp_x87_push(cpu, (double)(int64_t)interp_load(address, 8));
                interp_x87_c1(cpu, 0);
                break;
            case 7: { // FISTP m64
                int invalid = 0;
                double value = interp_x87_get(cpu, 0);
                double rounded = interp_x87_round_integral(cpu, value);
                uint64_t stored = interp_x87_to_integer(rounded, 8, &invalid);
                if (invalid) interp_fp_raise(1u);
                interp_store(address, 8, stored);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                interp_x87_pop(cpu);
                break;
            }
            default:
                // FBLD/FBSTP m80: packed BCD, unemitted.
                return interp_undefined(cpu, insn, pc, "x87 packed-BCD FBLD/FBSTP (DF /4,/6)");
            }
            cpu->rip = next;
            return STEP_NEXT;

        default: return interp_undefined(cpu, insn, pc, "x87 memory form");
        }
    }

    switch (op) {
    case 0xD8:
    case 0xDC:
    case 0xDE:
        if (reg == 2 || reg == 3) { // FCOM / FCOMP; DE D9 = FCOMPP, pops TWICE
            interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), 1);
            if (op == 0xDE && rm == 1) interp_x87_pop(cpu);
            if (reg == 3) interp_x87_pop(cpu);
            cpu->rip = next;
            return STEP_NEXT;
        }
        {
            // D8 writes ST0 reading ST(i); DC/DE write ST(i) reading ST0, with the REVERSE digits
            // SWAPPED between directions (D8 /4 FSUB vs DC /4 FSUBR, likewise /6): else b-a.
            int kind = reg;
            int target = op == 0xD8 ? 0 : rm;
            double destination = interp_x87_get(cpu, target);
            double source = op == 0xD8 ? interp_x87_get(cpu, rm) : interp_x87_get(cpu, 0);
            if (op != 0xD8 && (kind == 4 || kind == 5 || kind == 6 || kind == 7)) kind ^= 1;
            interp_x87_set(cpu, target, interp_x87_arith(kind, destination, source));
            interp_x87_c1(cpu, 0);               // see interp_x87_c1
            if (op == 0xDE) interp_x87_pop(cpu); // DE pops after writing
        }
        cpu->rip = next;
        return STEP_NEXT;

    case 0xD9:
        // Every arm writes C1 as 0; FXAM/FPREM/FPREM1 override.
        interp_x87_c1(cpu, 0);
        switch (reg) {
        case 0: interp_x87_push(cpu, interp_x87_get(cpu, rm)); break; // FLD ST(i)
        case 1: {                                                     // FXCH ST(i)
            double st0 = interp_x87_get(cpu, 0);
            interp_x87_set(cpu, 0, interp_x87_get(cpu, rm));
            interp_x87_set(cpu, rm, st0);
            break;
        }
        case 2:
            if (rm != 0) return interp_undefined(cpu, insn, pc, "x87 D9 /2 (only FNOP is defined)");
            break; // FNOP
        case 4:
            if (rm == 0)
                interp_x87_set(cpu, 0, -interp_x87_get(cpu, 0)); // FCHS: sign flip, NaNs too
            else if (rm == 1)
                interp_x87_set(cpu, 0, fabs(interp_x87_get(cpu, 0))); // FABS
            else if (rm == 4)
                interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), 0.0, 1); // FTST
            else if (rm == 5)
                interp_x87_classify(cpu); // FXAM
            else
                return interp_undefined(cpu, insn, pc, "x87 D9 /4 (reserved encoding)");
            break;
        case 5: { // FLD1 .. FLDZ
            double value;
            if (rm == 7) return interp_undefined(cpu, insn, pc, "x87 D9 EF (no such constant)");
            memcpy(&value, &g_x87_constants[rm], sizeof value);
            interp_x87_push(cpu, value);
            break;
        }
        case 6:
            // F0..F3 are transcendentals with no host FP instruction and exit to x87math.c.
            if (rm <= 3) {
                static const int selector[4] = {X87_F2XM1, X87_FYL2X, X87_FPTAN, X87_FPATAN};
                cpu->x87_ea = (uint64_t)selector[rm];
                return interp_exit(cpu, next, R_X87FUNC);
            }
            if (rm == 4)
                interp_x87_extract(cpu); // FXTRACT
            else if (rm == 5)
                interp_x87_remainder(cpu, 1); // FPREM1
            else if (rm == 6)
                cpu->fptop = (cpu->fptop - 1) & 7; // FDECSTP: rotate top only
            else
                cpu->fptop = (cpu->fptop + 1) & 7; // FINCSTP
            break;
        case 7:
            switch (rm) {
            case 0: interp_x87_remainder(cpu, 0); break;                         // FPREM
            case 2: interp_x87_set(cpu, 0, sqrt(interp_x87_get(cpu, 0))); break; // FSQRT
            case 4: { // FRNDINT: per FCW.RC, direction in C1
                double value = interp_x87_get(cpu, 0);
                double rounded = interp_x87_round_integral(cpu, value);
                interp_x87_set(cpu, 0, rounded);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                break;
            }
            case 5: // FSCALE
                interp_x87_set(cpu, 0, interp_x87_scale(interp_x87_get(cpu, 0), interp_x87_get(cpu, 1)));
                break;
            default: { // F9 FYL2XP1, FB FSINCOS, FE FSIN, FF FCOS
                static const int selector[8] = {0, X87_FYL2XP1, 0, X87_FSINCOS, 0, 0, X87_FSIN, X87_FCOS};
                cpu->x87_ea = (uint64_t)selector[rm];
                return interp_exit(cpu, next, R_X87FUNC);
            }
            }
            break;
        default: return interp_undefined(cpu, insn, pc, "x87 D9 register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    case 0xDA:
        if (reg <= 3) {
            // From the INTEGER EFLAGS, not the FSW; DB /0../3 are these negated.
            static const int condition[4] = {2 /*B*/, 4 /*E*/, 6 /*BE*/, 10 /*U (P)*/};
            if (interp_cond(cpu, condition[reg])) interp_x87_set(cpu, 0, interp_x87_get(cpu, rm));
        } else if (reg == 5 && rm == 1) { // FUCOMPP: pop twice
            interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, 1), 0);
            interp_x87_pop(cpu);
            interp_x87_pop(cpu);
        } else {
            return interp_undefined(cpu, insn, pc, "x87 DA register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    case 0xDB:
        if (reg <= 3) {
            static const int condition[4] = {3 /*NB*/, 5 /*NE*/, 7 /*NBE*/, 11 /*NU (NP)*/};
            if (interp_cond(cpu, condition[reg])) interp_x87_set(cpu, 0, interp_x87_get(cpu, rm));
        } else if (reg == 4 && rm == 2) { // FNCLEX: sticky exception flags only
            interp_fp_clear_exceptions();
        } else if (reg == 4 && rm == 3) { // FNINIT
            cpu->fptop = 0;
            cpu->fpsw = 0;
            cpu->fpcw = 0x037f; // nearest, 64-bit, all masked
            interp_fp_clear_exceptions();
        } else if (reg == 4) { // FNENI / FNDISI / FNSETPM: 8087 no-ops
            /* nothing */
        } else if (reg == 5 || reg == 6) { // FUCOMI /5 quiet, FCOMI /6 signalling
            interp_x87_compare_eflags(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), reg == 6);
        } else {
            return interp_undefined(cpu, insn, pc, "x87 DB register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    case 0xDD:
        switch (reg) {
        case 0: break; // FFREE: no tag word
        case 2:        // FST ST(i)
        case 3:        // FSTP ST(i)
            interp_x87_set(cpu, rm, interp_x87_get(cpu, 0));
            interp_x87_c1(cpu, 0);
            if (reg == 3) interp_x87_pop(cpu);
            break;
        case 4: // FUCOM: quiet, only sNaN raises #IA
        case 5: // FUCOMP ST(i)
            interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), 0);
            if (reg == 5) interp_x87_pop(cpu);
            break;
        default: return interp_undefined(cpu, insn, pc, "x87 DD register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    case 0xDF:
        if (reg == 4 && rm == 0) { // FNSTSW AX -- 16-bit, so RAX 63:16 are PRESERVED
            interp_reg_write(cpu, insn, RAX, 2, interp_x87_status_word(cpu));
        } else if (reg == 5 || reg == 6) { // FUCOMIP / FCOMIP: then pop
            interp_x87_compare_eflags(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), reg == 6);
            interp_x87_pop(cpu);
        } else {
            return interp_undefined(cpu, insn, pc, "x87 DF register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    default: return interp_undefined(cpu, insn, pc, "x87 register form");
    }
}

static int interp_step_sse(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;
    int prefix = interp_sse_prefix(insn);
    int destination = insn->reg; // the xmm destination in most forms

    // Native host FP under the guest's MXCSR.
    if (interp_sse_is_float_arithmetic(op)) return interp_step_sse_fp(cpu, insn, pc, next);

    // These have both MMX (no prefix) and SSE2 (0x66, xmm) encodings. No mm[] file is modelled, so MMX
    // forms are reported, not aliased.
    int integer_simd = (op >= 0x60 && op <= 0x6D) || op == 0x6E || op == 0x6F || (op >= 0x71 && op <= 0x76) ||
                       op == 0x7E || op == 0x7F || op == 0xD4 || op == 0xD5 || op == 0xD7 ||
                       (op >= 0xD1 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF) || (op >= 0xE0 && op <= 0xE5) ||
                       op == 0xE7 || (op >= 0xE8 && op <= 0xEF) || (op >= 0xF1 && op <= 0xFE) || op == 0x70 ||
                       op == 0xC4 || op == 0xC5;
    if (integer_simd && prefix == SSE_NP && op != 0x6F && op != 0x7E && op != 0x70)
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX form (no mm[] register file is modelled)");

    uint8_t d[16], s[16];

    switch (op) {
    case 0x10:
        if (prefix == SSE_F3) { // MOVSS: upper 96 bits ZEROED from memory, kept from a register
            interp_sse_rm_get(cpu, insn, next, 4, s);
            if (insn->is_mem) {
                memset(d, 0, 16);
                memcpy(d, s, 4);
            } else {
                interp_xmm_get(cpu, destination, d);
                memcpy(d, s, 4);
            }
        } else if (prefix == SSE_F2) { // MOVSD, at 64 bits
            interp_sse_rm_get(cpu, insn, next, 8, s);
            if (insn->is_mem) {
                memset(d, 0, 16);
                memcpy(d, s, 8);
            } else {
                interp_xmm_get(cpu, destination, d);
                memcpy(d, s, 8);
            }
        } else {
            interp_sse_rm_get(cpu, insn, next, 16, d); // unaligned permitted
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // MOVUPS/MOVUPD/MOVSS/MOVSD store
    case 0x11: {
        unsigned bytes = prefix == SSE_F3 ? 4u : prefix == SSE_F2 ? 8u : 16u;
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, bytes, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0x12:
    case 0x16: {
        int high = (op == 0x16);
        interp_xmm_get(cpu, destination, d);
        if (prefix == SSE_F3) {
            // Duplicate the EVEN (resp. ODD) single-precision lanes of a full m128; the F3 prefix alone
            // separates these from the MOVLPS/MOVHPS arm below, which reads 8 bytes.
            interp_sse_rm_get(cpu, insn, next, 16, s);
            for (int i = 0; i < 4; i++)
                interp_put32(d, i, interp_lane32(s, (i & ~1) | (high ? 1 : 0)));
        } else if (prefix == SSE_F2 && op == 0x12) { // MOVDDUP: low qword into both halves
            interp_sse_rm_get(cpu, insn, next, 8, s);
            memcpy(d + 0, s, 8);
            memcpy(d + 8, s, 8);
        } else if (insn->is_mem) { // MOVLPS low half, MOVHPS high half
            interp_sse_rm_get(cpu, insn, next, 8, s);
            memcpy(d + (high ? 8 : 0), s, 8);
        } else if (high) { // MOVLHPS: dest high := source LOW
            interp_xmm_get(cpu, insn->rm_reg, s);
            memcpy(d + 8, s + 0, 8);
        } else { // MOVHLPS: dest low := source HIGH
            interp_xmm_get(cpu, insn->rm_reg, s);
            memcpy(d + 0, s + 8, 8);
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVLPS/MOVLPD, MOVHPS/MOVHPD store
    case 0x13:
    case 0x17: {
        uint8_t half[16] = {0};
        interp_xmm_get(cpu, destination, d);
        memcpy(half, d + (op == 0x17 ? 8 : 0), 8);
        interp_sse_rm_put(cpu, insn, next, 8, half);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // Lane interleave: these ARE PUNPCK*DQ / PUNPCK*QDQ.
    case 0x14:
    case 0x15:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_punpck(d, s, prefix == SSE_66 ? 8 : 4, op == 0x15);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // MOVAPS/MOVAPD and the non-temporal stores
    case 0x28:
        if (interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_sse_rm_get(cpu, insn, next, 16, d);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0x29:
    case 0x2B:
        if (interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, 16, d);
        cpu->rip = next;
        return STEP_NEXT;

    // MOVMSKPS / MOVMSKPD
    case 0x50: {
        interp_xmm_get(cpu, insn->rm_reg, s);
        uint64_t mask = 0;
        if (prefix == SSE_66) {
            for (int i = 0; i < 2; i++)
                mask |= (uint64_t)((interp_lane64(s, i) >> 63) & 1) << i;
        } else {
            for (int i = 0; i < 4; i++)
                mask |= (uint64_t)((interp_lane32(s, i) >> 31) & 1) << i;
        }
        interp_reg_write(cpu, insn, destination, 4, mask); // 32-bit dest: zero-extends
        cpu->rip = next;
        return STEP_NEXT;
    }

    // Bitwise, so single/double carries no difference.
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int i = 0; i < 16; i++)
            d[i] = op == 0x54 ? (uint8_t)(d[i] & s[i])
                   : op == 0x55 ? (uint8_t)(~d[i] & s[i]) // ANDNPS: NOT dest, then AND
                   : op == 0x56 ? (uint8_t)(d[i] | s[i])
                                : (uint8_t)(d[i] ^ s[i]);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // PUNPCK* low and high
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x6C:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6D: {
        int lane = (op == 0x60 || op == 0x68) ? 1 : (op == 0x61 || op == 0x69) ? 2 : (op == 0x62 || op == 0x6A) ? 4 : 8;
        // NOT contiguous: the qword pair is at 0x6C (LOW) / 0x6D (HIGH), ABOVE the 0x68..0x6A group,
        // so a naive `op >= 0x68` turns PUNPCKLQDQ into PUNPCKHQDQ.
        int high = (op >= 0x68 && op <= 0x6A) || op == 0x6D;
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_punpck(d, s, lane, high);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // PACKSSWB / PACKUSWB / PACKSSDW
    case 0x63:
    case 0x67:
    case 0x6B:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_pack(d, s, op == 0x6B ? 4 : 2, op != 0x67);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // PCMPGTB/W/D
    case 0x64:
    case 0x65:
    case 0x66:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_pcmpgt(d, s, op == 0x64 ? 1 : op == 0x65 ? 2 : 4);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // An INTEGER r/m operand; REX.W selects 64-bit; upper bits ZEROED.
    case 0x6E: {
        interp_operand operand = interp_rm(cpu, insn, next);
        int width = insn->rexW ? 8 : 4;
        uint64_t value = interp_rm_read(cpu, insn, &operand, width);
        memset(d, 0, 16);
        memcpy(d, &value, (size_t)width);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVDQA (66) / MOVDQU (F3) load
    case 0x6F:
        if (prefix == SSE_NP)
            return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX MOVQ (no mm[] register file is modelled)");
        if (prefix == SSE_66 && interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_sse_rm_get(cpu, insn, next, 16, d);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // LDDQU: MOVDQU plus a micro-architectural hint. Memory only.
    case 0xF0:
        if (prefix != SSE_F2 || !insn->is_mem) return interp_undefined(cpu, insn, pc, "reserved (0F F0)");
        interp_sse_rm_get(cpu, insn, next, 16, d);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // MOVDQA (66) / MOVDQU (F3) store
    case 0x7F:
        if (prefix == SSE_NP)
            return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX MOVQ (no mm[] register file is modelled)");
        if (prefix == SSE_66 && interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, 16, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0x7E:
        if (prefix == SSE_F3) { // MOVQ load: upper 64 bits zeroed
            interp_sse_rm_get(cpu, insn, next, 8, s);
            memset(d, 0, 16);
            memcpy(d, s, 8);
            interp_xmm_put(cpu, destination, d);
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (prefix == SSE_66) { // MOVD/MOVQ to a GPR or memory
            interp_operand operand = interp_rm(cpu, insn, next);
            int width = insn->rexW ? 8 : 4;
            uint64_t value = 0;
            interp_xmm_get(cpu, destination, d);
            memcpy(&value, d, (size_t)width);
            interp_rm_write(cpu, insn, &operand, width, value);
            cpu->rip = next;
            return STEP_NEXT;
        }
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX MOVQ (no mm[] register file is modelled)");

    // PSHUFD (66) / PSHUFHW (F3) / PSHUFLW (F2)
    case 0x70: {
        interp_sse_rm_get(cpu, insn, next, 16, s);
        unsigned control = (unsigned)(insn->imm & 0xff);
        memset(d, 0, 16);
        if (prefix == SSE_66) {
            for (int i = 0; i < 4; i++)
                interp_put32(d, i, interp_lane32(s, (int)((control >> (2 * i)) & 3)));
        } else if (prefix == SSE_F3) { // PSHUFHW: the HIGH four words
            memcpy(d, s, 8);
            for (int i = 0; i < 4; i++)
                interp_put16(d, 4 + i, interp_lane16(s, 4 + (int)((control >> (2 * i)) & 3)));
        } else if (prefix == SSE_F2) { // PSHUFLW: the LOW four words
            memcpy(d + 8, s + 8, 8);
            for (int i = 0; i < 4; i++)
                interp_put16(d, i, interp_lane16(s, (int)((control >> (2 * i)) & 3)));
        } else {
            return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX PSHUFW (no mm[] register file is modelled)");
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ModRM.reg is the sub-opcode; the operand is the xmm in rm.
    case 0x71:
    case 0x72:
    case 0x73: {
        int sub = insn->reg & 7;
        unsigned count = (unsigned)(insn->imm & 0xff);
        int lane = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        interp_xmm_get(cpu, insn->rm_reg, d);
        if (op == 0x73 && (sub == 3 || sub == 7)) // PSRLDQ / PSLLDQ: whole-register bytes
            interp_pshift_bytes(d, count, sub == 3);
        else if (sub == 2)
            interp_pshift(d, lane, count, 1, 0); // PSRLW/D/Q
        else if (sub == 4 && op != 0x73)
            interp_pshift(d, lane, count, 1, 1); // PSRAW/D only: no PSRAQ, and 73 /4 is AVX-512
        else if (sub == 6)
            interp_pshift(d, lane, count, 0, 0); // PSLLW/D/Q
        else
            return interp_undefined(cpu, insn, pc, "unallocated SSE shift-group sub-opcode");
        interp_xmm_put(cpu, insn->rm_reg, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // PCMPEQB/W/D
    case 0x74:
    case 0x75:
    case 0x76:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_pcmpeq(d, s, op == 0x74 ? 1 : op == 0x75 ? 2 : 4);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // PINSRW and PEXTRW
    case 0xC4: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t value = interp_rm_read(cpu, insn, &operand, 2); // GPR low 16, or m16
        interp_xmm_get(cpu, destination, d);
        interp_put16(d, (int)(insn->imm & 7), (uint16_t)value);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xC5:
        interp_xmm_get(cpu, insn->rm_reg, s); // always an xmm register here
        interp_reg_write(cpu, insn, destination, 4, interp_lane16(s, (int)(insn->imm & 7)));
        cpu->rip = next;
        return STEP_NEXT;

    case 0xC6: {
        unsigned control = (unsigned)(insn->imm & 0xff);
        uint8_t out[16];
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        if (prefix == SSE_66) { // SHUFPD: one bit per qword
            interp_put64(out, 0, interp_lane64(d, (int)(control & 1)));
            interp_put64(out, 1, interp_lane64(s, (int)((control >> 1) & 1)));
        } else { // SHUFPS: low two dwords from dest, high two from src
            interp_put32(out, 0, interp_lane32(d, (int)(control & 3)));
            interp_put32(out, 1, interp_lane32(d, (int)((control >> 2) & 3)));
            interp_put32(out, 2, interp_lane32(s, (int)((control >> 4) & 3)));
            interp_put32(out, 3, interp_lane32(s, (int)((control >> 6) & 3)));
        }
        interp_xmm_put(cpu, destination, out);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // The count is the FULL low 64 bits: 0x100 is a count of 256, not 0.
    case 0xD1:
    case 0xD2:
    case 0xD3:
    case 0xE1:
    case 0xE2:
    case 0xF1:
    case 0xF2:
    case 0xF3: {
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        uint64_t raw = interp_lane64(s, 0);
        unsigned count = raw > 255 ? 255u : (unsigned)raw;
        int lane = (op == 0xD1 || op == 0xE1 || op == 0xF1) ? 2 : (op == 0xD2 || op == 0xE2 || op == 0xF2) ? 4 : 8;
        int arithmetic = (op == 0xE1 || op == 0xE2);
        int right = arithmetic || op == 0xD1 || op == 0xD2 || op == 0xD3;
        interp_pshift(d, lane, count, right, arithmetic);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVQ xmm/m64 store
    case 0xD6: {
        uint8_t half[16] = {0};
        interp_xmm_get(cpu, destination, d);
        memcpy(half, d, 8);
        if (insn->is_mem) {
            interp_store_bytes(interp_ea(cpu, insn, next), half, 8);
        } else {
            // MOVQ zeroes the upper 64 bits.
            interp_xmm_put(cpu, insn->rm_reg, half);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // PMOVMSKB: top bit of each of sixteen bytes
    case 0xD7: {
        interp_xmm_get(cpu, insn->rm_reg, s);
        uint64_t mask = 0;
        for (int i = 0; i < 16; i++)
            mask |= (uint64_t)((s[i] >> 7) & 1) << i;
        interp_reg_write(cpu, insn, destination, 4, mask);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVNTDQ: aligned store; the hint has no effect
    case 0xE7:
        if (interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, 16, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0xDB: // PAND
    case 0xDF: // PANDN
    case 0xEB: // POR
    case 0xEF: // PXOR
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int i = 0; i < 16; i++)
            d[i] = op == 0xDB ? (uint8_t)(d[i] & s[i])
                   : op == 0xDF ? (uint8_t)(~d[i] & s[i]) // PANDN: NOT dest, then AND
                   : op == 0xEB ? (uint8_t)(d[i] | s[i])
                                : (uint8_t)(d[i] ^ s[i]);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0xFC: // PADDB
    case 0xFD: // PADDW
    case 0xFE: // PADDD
    case 0xD4: // PADDQ
    case 0xF8: // PSUBB
    case 0xF9: // PSUBW
    case 0xFA: // PSUBD
    case 0xFB: // PSUBQ
    case 0xEC: // PADDSB
    case 0xED: // PADDSW
    case 0xDC: // PADDUSB
    case 0xDD: // PADDUSW
    case 0xE8: // PSUBSB
    case 0xE9: // PSUBSW
    case 0xD8: // PSUBUSB
    case 0xD9: // PSUBUSW
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        switch (op) {
        case 0xFC: interp_padd(d, s, 1); break;
        case 0xFD: interp_padd(d, s, 2); break;
        case 0xFE: interp_padd(d, s, 4); break;
        case 0xD4: interp_padd(d, s, 8); break;
        case 0xF8: interp_psub(d, s, 1); break;
        case 0xF9: interp_psub(d, s, 2); break;
        case 0xFA: interp_psub(d, s, 4); break;
        case 0xFB: interp_psub(d, s, 8); break;
        case 0xEC: interp_padds(d, s, 1, 0, 1); break;
        case 0xED: interp_padds(d, s, 2, 0, 1); break;
        case 0xDC: interp_padds(d, s, 1, 0, 0); break;
        case 0xDD: interp_padds(d, s, 2, 0, 0); break;
        case 0xE8: interp_padds(d, s, 1, 1, 1); break;
        case 0xE9: interp_padds(d, s, 2, 1, 1); break;
        case 0xD8: interp_padds(d, s, 1, 1, 0); break;
        default: interp_padds(d, s, 2, 1, 0); break; // 0xD9 PSUBUSW
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0xDA: // PMINUB
    case 0xDE: // PMAXUB
    case 0xEA: // PMINSW
    case 0xEE: // PMAXSW
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        if (op == 0xDA || op == 0xDE) {
            for (int i = 0; i < 16; i++)
                d[i] = (op == 0xDA) == (d[i] < s[i]) ? d[i] : s[i];
        } else {
            for (int i = 0; i < 8; i++) {
                int16_t a = (int16_t)interp_lane16(d, i), b = (int16_t)interp_lane16(s, i);
                interp_put16(d, i, (uint16_t)(((op == 0xEA) == (a < b)) ? a : b));
            }
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0xD5: // PMULLW: low 16 of each product
    case 0xE4: // PMULHUW: high 16, unsigned
    case 0xE5: // PMULHW: high 16, signed
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int i = 0; i < 8; i++) {
            uint32_t product;
            if (op == 0xE5)
                product = (uint32_t)(int32_t)((int16_t)interp_lane16(d, i) * (int32_t)(int16_t)interp_lane16(s, i));
            else
                product = (uint32_t)interp_lane16(d, i) * (uint32_t)interp_lane16(s, i);
            interp_put16(d, i, (uint16_t)(op == 0xD5 ? (product & 0xffff) : (product >> 16)));
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    case 0xF4: { // PMULUDQ: unsigned 32x32 -> 64
        uint8_t out[16];
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_put64(out, 0, (uint64_t)interp_lane32(d, 0) * (uint64_t)interp_lane32(s, 0));
        interp_put64(out, 1, (uint64_t)interp_lane32(d, 2) * (uint64_t)interp_lane32(s, 2));
        interp_xmm_put(cpu, destination, out);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xF5: { // PMADDWD: 16x16 products summed in pairs
        uint8_t out[16];
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int i = 0; i < 4; i++) {
            int32_t low = (int32_t)(int16_t)interp_lane16(d, 2 * i) * (int32_t)(int16_t)interp_lane16(s, 2 * i);
            int32_t high =
                (int32_t)(int16_t)interp_lane16(d, 2 * i + 1) * (int32_t)(int16_t)interp_lane16(s, 2 * i + 1);
            interp_put32(out, i, (uint32_t)(low + high));
        }
        interp_xmm_put(cpu, destination, out);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xF6: { // PSADBW: |differences| summed per half
        uint8_t out[16] = {0};
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int half = 0; half < 2; half++) {
            uint32_t total = 0;
            for (int i = 0; i < 8; i++) {
                int index = 8 * half + i;
                total += (uint32_t)(d[index] > s[index] ? d[index] - s[index] : s[index] - d[index]);
            }
            interp_put16(out, 4 * half, (uint16_t)total);
        }
        interp_xmm_put(cpu, destination, out);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xE0: // PAVGB
    case 0xE3: // PAVGW
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        if (op == 0xE0) {
            for (int i = 0; i < 16; i++)
                d[i] = (uint8_t)(((uint32_t)d[i] + (uint32_t)s[i] + 1u) >> 1);
        } else {
            for (int i = 0; i < 8; i++) {
                uint32_t sum = (uint32_t)interp_lane16(d, i) + (uint32_t)interp_lane16(s, i) + 1u;
                interp_put16(d, i, (uint16_t)(sum >> 1));
            }
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    default: break;
    }
    return STEP_SSE_UNHANDLED;
}

// The 0F (two-byte) opcode map.

// The 0F-map SSE/SSE2 space; a residual gap then reports as "SSE", not a bare opcode byte.
static int interp_is_legacy_sse(uint8_t op) {
    if (op >= 0x10 && op <= 0x17) return 1;
    if (op >= 0x28 && op <= 0x2F) return 1;
    if (op >= 0x50 && op <= 0x6D) return 1;
    if (op >= 0x6E && op <= 0x7F) return 1;
    if (op >= 0xD0 && op <= 0xFF) return 1;
    if (op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6) return 1;
    return 0;
}

static int interp_step_two_byte(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;

    // Jcc rel32 (0F 80..8F)
    if (op >= 0x80 && op <= 0x8F) {
        cpu->rip = interp_cond(cpu, op & 0xf) ? next + (uint64_t)insn->imm : next;
        cpu->reason = R_BRANCH;
        return STEP_END;
    }

    // SETcc r/m8 (0F 90..9F)
    if (op >= 0x90 && op <= 0x9F) {
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, 1, (uint64_t)(interp_cond(cpu, op & 0xf) ? 1 : 0));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // CMOVcc r, r/m (0F 40..4F)
    if (op >= 0x40 && op <= 0x4F) {
        interp_operand operand = interp_rm(cpu, insn, next);
        // Source read and destination written unconditionally; `cmovcc r32` zero-extends even without a move.
        uint64_t source = interp_rm_read(cpu, insn, &operand, insn->opsize);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         interp_cond(cpu, op & 0xf) ? source : interp_reg_read(cpu, insn, insn->reg, insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // BSWAP r (0F C8..CF)
    if (op >= 0xC8 && op <= 0xCF) {
        int number = (op & 7) | (insn->rexB << 3);
        uint64_t value = cpu->r[number];
        if (insn->opsize == 8)
            value = __builtin_bswap64(value);
        else
            value = __builtin_bswap32((uint32_t)value); // 32-bit form also zero-extends, via reg_write
        interp_reg_write(cpu, insn, number, insn->opsize == 8 ? 8 : 4, value);
        cpu->rip = next;
        return STEP_NEXT;
    }

    switch (op) {
    // SYSCALL (0F 05): rip is pre-advanced past the 0F 05 bytes, so R_SYSCALL in interp_dispatch.h adds none.
    case 0x05:
        return interp_exit(cpu, next, R_SYSCALL);

    // CPUID (0F A2)
    case 0xA2:
        return interp_exit(cpu, next, R_CPUID);

    // UD1/UD2 (0F B9/0F 0B): a trap, not an engine gap
    case 0x0B:
    case 0xB9:
        return interp_guest_trap(cpu, pc, 4 /*SIGILL*/, 2 /*ILL_ILLOPN*/);

    // Multi-byte NOPs and hints: 0F 1F, 0F 0D, 0F 18..1E (incl. ENDBR64). No register or memory effect.
    case 0x0D:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F:
        cpu->rip = next;
        return STEP_NEXT;

    // EMMS (0F 77): no MMX tag word exists in this model
    case 0x77:
        cpu->rip = next;
        return STEP_NEXT;

    case 0x01:
        if (insn->has_modrm && insn->modrm == 0xF9) { // RDTSCP: EDX:EAX = counter, ECX = TSC_AUX (0)
            uint64_t counter = now_ns();
            interp_reg_write(cpu, insn, RAX, 4, counter & UINT64_C(0xffffffff));
            interp_reg_write(cpu, insn, RDX, 4, counter >> 32);
            interp_reg_write(cpu, insn, RCX, 4, 0);
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (insn->has_modrm && insn->modrm == 0xD0) { // XGETBV(ecx=0): XCR0 = x87 + SSE, no AVX
            // Must match cpuid.c, which withholds AVX: disagreeing bits pick an unimplemented guest path.
            interp_reg_write(cpu, insn, RAX, 4, 3);
            interp_reg_write(cpu, insn, RDX, 4, 0);
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (insn->has_modrm && insn->modrm == 0xD5) { // XEND: no transaction is ever active -> NOP
            cpu->rip = next;
            return STEP_NEXT;
        }
        return interp_undefined(cpu, insn, pc, "0F 01 system instruction group");

    // RDTSC (0F 31): the engine's ns clock, not a host RDTSC -- host-neutral and matches clock_gettime.
    case 0x31: {
        uint64_t counter = now_ns();
        interp_reg_write(cpu, insn, RAX, 4, counter & UINT64_C(0xffffffff));
        interp_reg_write(cpu, insn, RDX, 4, counter >> 32);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVZX / MOVSX (0F B6/B7, BE/BF)
    case 0xB6:
    case 0xB7:
    case 0xBE:
    case 0xBF: {
        int source_width = (op & 1) ? 2 : 1;
        int signed_extend = (op >= 0xBE);
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t value = interp_rm_read(cpu, insn, &operand, source_width);
        if (signed_extend) {
            unsigned bits = (unsigned)(8 * source_width);
            value = (uint64_t)((int64_t)(value << (64 - bits)) >> (64 - bits));
        }
        interp_reg_write(cpu, insn, insn->reg, insn->opsize, value);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xAF: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, insn->opsize);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         interp_imul_truncating(cpu, interp_reg_read(cpu, insn, insn->reg, insn->opsize), source,
                                                insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // SHLD / SHRD (0F A4/A5/AC/AD)
    case 0xA4:
    case 0xA5:
    case 0xAC:
    case 0xAD: {
        int right = (op >= 0xAC);
        int width = insn->opsize;
        unsigned count = (op == 0xA4 || op == 0xAC) ? (unsigned)(insn->imm & 0xff) : (unsigned)(cpu->r[RCX] & 0xff);
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t value = interp_rm_read(cpu, insn, &operand, width);
        uint64_t fill = interp_reg_read(cpu, insn, insn->reg, width);
        interp_rm_write(cpu, insn, &operand, width, interp_double_shift(cpu, right, value, fill, count, width));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // BT/BTS/BTR/BTC: 0F A3/AB/B3/BB register-index, 0F BA /4../7 immediate. Only CF is written. A MEMORY
    // bit index is NOT masked; byte + bit-in-byte keeps a locked BTS a single-byte atomic.
    case 0xA3:
    case 0xAB:
    case 0xB3:
    case 0xBB:
    case 0xBA: {
        int immediate_form = (op == 0xBA);
        int sub = immediate_form ? (insn->reg & 7) : ((op >> 3) & 3) + 4;
        if (immediate_form && sub < 4) return interp_undefined(cpu, insn, pc, "0F BA group with /reg < 4");
        int width = insn->opsize;
        interp_operand operand = interp_rm(cpu, insn, next);
        int64_t index = immediate_form ? (int64_t)(insn->imm & (width == 8 ? 63 : 31))
                                       : (int64_t)interp_reg_read(cpu, insn, insn->reg, width);
        if (!immediate_form && width != 8) index = (int64_t)(int32_t)(uint32_t)(uint64_t)index;
        // sub: 4 = BT, 5 = BTS, 6 = BTR, 7 = BTC.
        enum interp_rmw_kind rmw = sub == 5 ? RMW_BTS : sub == 6 ? RMW_BTR : RMW_BTC;
        unsigned bit;
        uint64_t old;
        if (operand.is_memory) {
            int64_t byte_offset = index >> 3; // arithmetic: negative reaches below the EA
            uint64_t address = operand.address + (uint64_t)byte_offset;
            bit = (unsigned)(index & 7);
            if (sub == 4) {
                old = interp_load(address, 1);
            } else if (insn->lock) {
                old = interp_locked_rmw(address, 1, rmw, UINT64_C(1) << bit, 0);
            } else {
                old = interp_load(address, 1);
                interp_store(address, 1, interp_rmw_apply(rmw, old, UINT64_C(1) << bit, 0, 1));
            }
        } else {
            bit = (unsigned)((uint64_t)index & (width == 8 ? 63u : (unsigned)(8 * width - 1)));
            old = interp_reg_read(cpu, insn, operand.number, width);
            if (sub != 4)
                interp_reg_write(cpu, insn, operand.number, width,
                                 interp_rmw_apply(rmw, old, UINT64_C(1) << bit, 0, width));
        }
        interp_set_cf(cpu, (unsigned)((old >> bit) & 1));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // BSF (0F BC) / BSR (0F BD). F3 selects TZCNT/LZCNT, which differ: lzcnt counts leading ZEROS where
    // bsr returns a bit INDEX.
    case 0xBC:
    case 0xBD: {
        int width = insn->opsize;
        unsigned bits = (unsigned)(8 * width);
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, width) & interp_mask(width);
        if (insn->rep) { // TZCNT / LZCNT: CF = (source == 0), ZF = (result == 0)
            uint64_t result;
            if (source == 0)
                result = bits;
            else if (op == 0xBC)
                result = (uint64_t)__builtin_ctzll(source);
            else
                result = (uint64_t)(__builtin_clzll(source) - (64 - bits));
            interp_flags_nzcv(cpu, 0, result == 0, source == 0, 0);
            cpu->pf = result & 0xff;
            interp_reg_write(cpu, insn, insn->reg, width, result);
        } else { // BSF / BSR: ZF = (source == 0), destination UNDEFINED (unchanged) then
            if (source == 0) {
                interp_flags_nzcv(cpu, 0, 1, 0, 0);
                cpu->pf = 0xff;
            } else {
                uint64_t result = op == 0xBC ? (uint64_t)__builtin_ctzll(source)
                                            : (uint64_t)(63 - __builtin_clzll(source));
                interp_flags_nzcv(cpu, 0, 0, 0, 0);
                cpu->pf = result & 0xff;
                interp_reg_write(cpu, insn, insn->reg, width, result);
            }
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xB8: {
        if (!insn->rep) return interp_undefined(cpu, insn, pc, "0F B8 without the F3 prefix (JMPE)");
        int width = insn->opsize;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, width) & interp_mask(width);
        uint64_t result = (uint64_t)__builtin_popcountll(source);
        // ZF comes from the SOURCE; every other flag clears.
        interp_flags_nzcv(cpu, 0, source == 0, 0, 0);
        // pf holds a byte whose EVEN parity IS PF, so clearing PF means storing an ODD-parity byte.
        cpu->pf = 1;
        cpu->af = 0;
        interp_reg_write(cpu, insn, insn->reg, width, result);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xB0:
    case 0xB1: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t accumulator = interp_reg_read(cpu, insn, RAX, width);
        uint64_t source = interp_reg_read(cpu, insn, insn->reg, width);
        uint64_t observed;
        if (operand.is_memory && insn->lock) {
            // interp_locked_rmw cannot express "swap only if equal".
            uint64_t host_address = hl_x86_guest_pointer(operand.address);
            void *pointer = (void *)(uintptr_t)host_address;
            int swapped = 0;
            interp_access_begin(operand.address, (uint64_t)width);
            if ((host_address & (uint64_t)(width - 1)) == 0) {
                switch (width) {
                case 1: {
                    unsigned char expected = (unsigned char)accumulator;
                    swapped = __atomic_compare_exchange_n((unsigned char *)pointer, &expected,
                                                          (unsigned char)source, 0, __ATOMIC_SEQ_CST,
                                                          __ATOMIC_SEQ_CST);
                    observed = expected;
                    break;
                }
                case 2: {
                    unsigned short expected = (unsigned short)accumulator;
                    swapped = __atomic_compare_exchange_n((unsigned short *)pointer, &expected,
                                                          (unsigned short)source, 0, __ATOMIC_SEQ_CST,
                                                          __ATOMIC_SEQ_CST);
                    observed = expected;
                    break;
                }
                case 4: {
                    uint32_t expected = (uint32_t)accumulator;
                    swapped = __atomic_compare_exchange_n((uint32_t *)pointer, &expected, (uint32_t)source, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    observed = expected;
                    break;
                }
                default: {
                    uint64_t expected = accumulator;
                    swapped = __atomic_compare_exchange_n((uint64_t *)pointer, &expected, source, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    observed = expected;
                    break;
                }
                }
            } else { // split lock: hashed spinlock
                unsigned hash = (unsigned)((host_address >> 3) & (INTERP_SPLIT_LOCKS - 1));
                _Atomic unsigned *lock = &g_interp_split_lock[hash];
                while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE))
                    ;
                observed = 0;
                memcpy(&observed, pointer, (size_t)width);
                if ((observed & interp_mask(width)) == (accumulator & interp_mask(width))) {
                    memcpy(pointer, &source, (size_t)width);
                    swapped = 1;
                }
                __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
            }
            interp_access_end();
            if (swapped && jit86_store_alias_observation_active())
                jit86_store_alias_changed(operand.address, (uint64_t)width);
        } else {
            observed = interp_rm_read(cpu, insn, &operand, width);
            if ((observed & interp_mask(width)) == (accumulator & interp_mask(width)))
                interp_rm_write(cpu, insn, &operand, width, source);
        }
        // Flags are the full CMP(accumulator, destination), not just ZF.
        (void)interp_alu_sub(cpu, accumulator, observed, 0, width);
        if ((observed & interp_mask(width)) != (accumulator & interp_mask(width)))
            interp_reg_write(cpu, insn, RAX, width, observed); // Intel: on mismatch the accumulator reloads
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xC0:
    case 0xC1: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_reg_read(cpu, insn, insn->reg, width);
        uint64_t old;
        if (operand.is_memory && insn->lock) {
            old = interp_locked_rmw(operand.address, width, RMW_ADD, source, 0);
            (void)interp_alu_add(cpu, old, source, 0, width);
            interp_reg_write(cpu, insn, insn->reg, width, old); // the register receives the PRE-image
        } else {
            uint64_t sum;
            old = interp_rm_read(cpu, insn, &operand, width);
            sum = interp_alu_add(cpu, old, source, 0, width);
            // WRITE ORDER: the SUM lands last, so `xadd %ax, %ax` (SRC == DEST) leaves the SUM, not the
            // pre-image.
            interp_reg_write(cpu, insn, insn->reg, width, old);
            interp_rm_write(cpu, insn, &operand, width, sum);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // MOVNTI (0F C3): architecturally an ordinary store
    case 0xC3: {
        if (!insn->is_mem) return interp_undefined(cpu, insn, pc, "MOVNTI with a register destination (#UD)");
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_store(operand.address, insn->opsize, interp_reg_read(cpu, insn, insn->reg, insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xC7: {
        if ((insn->reg & 7) != 1 || !insn->is_mem)
            return interp_undefined(cpu, insn, pc, "0F C7 group (RDRAND/RDSEED/VMPTRLD)");
        interp_operand operand = interp_rm(cpu, insn, next);
        if (insn->opsize == 8) {
            // cmpxchg16b: the REBASED address goes to the shared helper.
            cpu->x87_ea = hl_x86_guest_pointer(operand.address);
            return interp_exit(cpu, next, R_CMPXCHG16);
        }
        // cmpxchg8b: EDX:EAX vs m64, ECX:EBX on match; only ZF is affected.
        uint64_t expected = ((cpu->r[RDX] & UINT64_C(0xffffffff)) << 32) | (cpu->r[RAX] & UINT64_C(0xffffffff));
        uint64_t desired = ((cpu->r[RCX] & UINT64_C(0xffffffff)) << 32) | (cpu->r[RBX] & UINT64_C(0xffffffff));
        uint64_t observed;
        int equal;
        if (insn->lock) {
            uint64_t host_address = hl_x86_guest_pointer(operand.address);
            uint64_t probe = expected;
            interp_access_begin(operand.address, 8);
            equal = __atomic_compare_exchange_n((uint64_t *)(uintptr_t)host_address, &probe, desired, 0,
                                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            interp_access_end();
            observed = probe;
            if (equal && jit86_store_alias_observation_active()) jit86_store_alias_changed(operand.address, 8);
        } else {
            observed = interp_load(operand.address, 8);
            equal = observed == expected;
            if (equal) interp_store(operand.address, 8, desired);
        }
        if (!equal) {
            interp_reg_write(cpu, insn, RAX, 4, observed & UINT64_C(0xffffffff));
            interp_reg_write(cpu, insn, RDX, 4, observed >> 32);
        }
        if (equal)
            cpu->nzcv |= NZ_Z;
        else
            cpu->nzcv &= ~NZ_Z;
        cpu->rip = next;
        return STEP_NEXT;
    }

    // 0F AE: fences and the FXSAVE/MXCSR group
    case 0xAE: {
        int sub = insn->reg & 7;
        if (sub >= 5 && !insn->is_mem) {
            // LFENCE/MFENCE/SFENCE: no-op under x86-TSO; interp_tso_fence is where a weak host would put
            // a real barrier, hence the call.
            interp_tso_fence();
            cpu->rip = next;
            return STEP_NEXT;
        }
        if ((sub == 0 || sub == 1) && insn->is_mem) {
            // fxsave / fxrstor: the shared helper owns the layout.
            cpu->x87_ea = hl_x86_guest_pointer(interp_ea(cpu, insn, next));
            return interp_exit(cpu, next, sub == 0 ? R_FXSAVE : R_FXRSTOR);
        }
#if defined(HL_HOST_CPU_X86_64)
        if ((sub == 2 || sub == 3) && insn->is_mem) {
            // LDMXCSR (/2) / STMXCSR (/3). The guest MXCSR IS the host MXCSR here, so read memory BEFORE
            // writing it: a faulting operand must leave rounding mode and sticky flags as they were.
            if (sub == 3) {
                uint32_t live = _mm_getcsr();
                interp_store(interp_ea(cpu, insn, next), 4, live);
            } else {
                uint32_t loaded = (uint32_t)interp_load(interp_ea(cpu, insn, next), 4);
                // MASK it: LDMXCSR #GPs outside MXCSR_MASK, and this word is GUEST data -- unmasked it
                // kills the ENGINE where hardware faults the guest. 0xffff keeps every defined field.
                _mm_setcsr(loaded & 0xffffu);
            }
            cpu->rip = next;
            return STEP_NEXT;
        }
#endif
        // XSAVE/XRSTOR (/4,/5): no extended-state layout exists in this model.
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): XSAVE/XRSTOR (0F AE)");
    }

    default: break;
    }

    // Legacy SSE/SSE2 has no shared C emulator to route to (unlike VEX -> R_AVX, 0F38/0F3A -> R_SSE3B).
    if (interp_is_legacy_sse(op)) {
        int handled = interp_step_sse(cpu, insn, pc, next);
        if (handled != STEP_SSE_UNHANDLED) return handled;
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): legacy SSE/SSE2 (0F map)");
    }
    if (op == 0x00 || op == 0x02 || op == 0x03 || op == 0x06 || op == 0x08 || op == 0x09 || op == 0x20 || op == 0x21 ||
        op == 0x22 || op == 0x23)
        return interp_undefined(cpu, insn, pc, "privileged system instruction (LGDT/LAR/MOV CRn/MOV DRn)");
    return interp_undefined(cpu, insn, pc, "two-byte (0F) opcode");
}

// The translated-code cache: permanent MISS, no-op save -- it stores HOST CODE and this backend emits
// none. IDENTITY still matters: pcache_engine_id mixes HL_HOST_CPU_ISA, and checkpoint.c validates that
// same id on restore -- without the host-ISA term a JIT-written checkpoint would restore against nothing.

static int g_pcache_forked;     // set by linux_abi/fork.c in a fork child; unread here
static int g_force_base_failed; // latched by the ELF loader on fixed-VA map fallback

static uint64_t pcache_engine_id(void) {
    uint64_t hash = 1469598103934665603ull;
    uint64_t self = hl_identity_source(&g_jit_services, g_self_path);
    for (const char *p = __DATE__ " " __TIME__; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 1099511628211ull;
    }
    hash ^= self;
    hash *= 1099511628211ull;
    // No emitted-code mode bits to mix in; the host-ISA term is the load-bearing one.
    return hl_identity_configuration(hash, 2, HL_HOST_CPU_ISA, 0);
}

static uint64_t pcache_make_id(const char *program_host, const char *interpreter_host, const char *argv0) {
    uint64_t program = hl_identity_source(&g_jit_services, program_host);
    uint64_t interpreter = interpreter_host ? hl_identity_source(&g_jit_services, interpreter_host) : 0xABCDEFull;
    return hl_identity_mix(program, interpreter, pcache_engine_id(), hl_identity_name(argv0));
}

static int pcache_load(uint64_t entry_jump) {
    (void)entry_jump;
    if (g_coldprof) fprintf(stderr, "[pcache] MISS (interpreter backend: the cache stores host code)\n");
    g_pcache_loaded = 0;
    return 0; // MISS: the dispatcher translates fresh
}

static void pcache_save(void) {
    // Empty: nothing could load what this would write.
}

static void pcache_directory_close(void) {
}

static void pcache_note_fixed_img(uint64_t base, uint64_t span) {
    // Nothing is revivable here, so no spans to record.
    (void)base;
    (void)span;
}

// ARM64 emitter stubs: lower/*.c is in IR_SOURCES on every host and references emitters only the AArch64
// arm of core/target/x86_64.c defines, and engine_global_init drags lower/repstr.c into the link here.
// Dead by construction, so they abort. TODO(amd64-host): fix per docs/amd64-host-findings.md 3.3.

static void interp_no_emitter(const char *name) {
    fprintf(stderr,
            "[hl] interp: ARM64 emitter %s() called on a " HL_HOST_CPU_NAME " host. This backend decodes and\n"
            "     executes x86-64; it emits nothing, so a per-class lowering file under\n"
            "     src/translator/guest/x86_64/lower/ was entered. Those are unreachable here by\n"
            "     construction -- see the emitter-stub note in interp.c.\n",
            name);
    abort();
}

#define INTERP_EMITTER_STUB(name, signature, ...)                                                                      \
    void name signature {                                                                                             \
        __VA_ARGS__;                                                                                                  \
        interp_no_emitter(#name);                                                                                     \
    }

INTERP_EMITTER_STUB(emit_exit_const, (uint64_t rip, uint64_t reason), (void)rip, (void)reason)
INTERP_EMITTER_STUB(emit_memory_guard, (int address_register, uint64_t size, uint64_t rip, uint32_t required),
                    (void)address_register, (void)size, (void)rip, (void)required)
INTERP_EMITTER_STUB(emit_soft_store_observe, (uint64_t size), (void)size)
INTERP_EMITTER_STUB(emit_soft_store_drain, (void), (void)0)
INTERP_EMITTER_STUB(hl_x86_emit_block_return, (void), (void)0)
INTERP_EMITTER_STUB(hl_x86_emit_spill, (void), (void)0)
INTERP_EMITTER_STUB(hl_x86_emit_reload, (void), (void)0)
INTERP_EMITTER_STUB(hl_x86_emit_vector_reset, (void), (void)0)
INTERP_EMITTER_STUB(hl_x86_emit_host_pointer, (int destination, uint64_t pointer), (void)destination, (void)pointer)
INTERP_EMITTER_STUB(e_movconst, (int destination, uint64_t value), (void)destination, (void)value)
INTERP_EMITTER_STUB(e_load, (int width, int destination, int address), (void)width, (void)destination, (void)address)
INTERP_EMITTER_STUB(e_store, (int width, int source, int address), (void)width, (void)source, (void)address)
INTERP_EMITTER_STUB(e_mov_rr, (int destination, int source, int sixty_four_bit), (void)destination, (void)source,
                    (void)sixty_four_bit)
INTERP_EMITTER_STUB(e_addi, (int destination, int source, unsigned immediate, int sixty_four_bit), (void)destination,
                    (void)source, (void)immediate, (void)sixty_four_bit)
INTERP_EMITTER_STUB(e_subi, (int destination, int source, unsigned immediate, int sixty_four_bit), (void)destination,
                    (void)source, (void)immediate, (void)sixty_four_bit)
INTERP_EMITTER_STUB(e_str, (int source, int base, int offset), (void)source, (void)base, (void)offset)
INTERP_EMITTER_STUB(e_ldr, (int destination, int base, int offset), (void)destination, (void)base, (void)offset)
INTERP_EMITTER_STUB(e_rrr, (uint32_t instruction, int destination, int left, int right, int sixty_four_bit, int shift),
                    (void)instruction, (void)destination, (void)left, (void)right, (void)sixty_four_bit, (void)shift)
INTERP_EMITTER_STUB(e_lsl_i, (int destination, int source, int shift, int sixty_four_bit), (void)destination,
                    (void)source, (void)shift, (void)sixty_four_bit)

// These return a value, so no macro.
int emit_soft_memory_active(void) {
    // Nothing is emitted here, so no guards; answered rather than aborting for shared callers.
    return 0;
}

uint32_t *hl_x86_emit_cursor(void) {
    interp_no_emitter("hl_x86_emit_cursor");
    return NULL;
}
