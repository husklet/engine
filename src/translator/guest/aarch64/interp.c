// translator/guest/aarch64/interp.c -- the AArch64 guest frontend for every host CPU that is NOT AArch64:
// an AArch64 DECODER + EXECUTOR, in place of the same-ISA transliterating JIT in translate.c.
//
// WHY THIS FILE EXISTS AT ALL
// ---------------------------
// translate.c is a transliterator, not a compiler. It copies most guest instruction words verbatim into the
// host code arena and rewrites only the ones that name an engine-stolen register, because it assumes the
// guest register file IS the host register file (x28 = cpu pointer, x30 = host link, x18/x16/x17 = engine
// scratch, everything else holding the guest's own value). That assumption is not an optimization detail --
// it is the whole design -- and it is true only when the host CPU executes AArch64. On an x86-64 Linux host
// there is nothing to transliterate INTO: no AArch64 assembler output can run, and there is no AArch64-
// register file for guest values to live in.
//
// So this backend keeps the guest's architectural state where struct cpu already puts it and interprets. The
// substitution is possible because the shared dispatcher's ENTIRE contract with a backend (core/dispatch.c's
// run_guest loop) is three lines:
//
//     code = translate_block(G_PC(c));   // produce something callable for this guest PC
//     run_block(c, code);                // ... call it
//     // on return: c->reason says why it stopped, c->pc is the next guest PC, and every piece of guest
//     // architectural state is back in *c.
//
// Nothing there requires `code` to be machine code, and nothing there requires run_block to be a trampoline.
// `code` is a block descriptor allocated from the same arena, and run_block is the C loop below. Everything
// downstream -- the block cache and its SMC invalidation, the whole of linux_abi (syscalls, signals,
// container, ELF), the stop-the-world machinery, even the checkpoint format -- is reused UNCHANGED, because
// struct cpu is untouched and the reason codes describe GUEST events rather than host ones.
//
// struct cpu is deliberately shared with the JIT and must not be edited: sizeof(struct cpu) is written into
// the checkpoint image and validated on restore, so one layout is what lets the two backends read each
// other's guest state. Its host_save[12]/host_v[16] fields are AArch64 callee-saved spill slots for
// run_block/block_return; this backend simply never touches them (it has no host register file to spill).
//
// WHAT IS AND IS NOT IMPLEMENTED YET, AND HOW TO EXTEND IT
// -------------------------------------------------------
// This file is the framework plus the INTEGER CORE. Implemented: data-processing immediate (add/sub imm,
// logical imm, MOVZ/MOVN/MOVK, ADR/ADRP, bitfield, EXTR); data-processing register (add/sub shifted and
// extended, ADC/SBC, the logical group, variable shifts, mul/div, the 1-source bit ops, CCMP/CCMN, the
// conditional-select group); every branch form; the hint/barrier space; SVC; and the `ic ivau`/ISB
// self-modifying-code interception. NOT yet implemented, and each routed to interp_undefined() with a
// TODO(amd64-host) marker naming its class: loads and stores, FP/AdvSIMD, atomics and load/store-exclusive,
// and system-register access (MRS/MSR, including TPIDR_EL0).
//
// The extension points, in the order a follow-up will want them:
//   * interp_step() is the single top-level decoder. It switches on op0 = insn[28:25] exactly as the ARM ARM
//     top-level table does, and hands each group to one interp_exec_<group>() function. Adding a group means
//     filling in that one function; nothing else moves.
//   * interp_read_guest() / interp_write_guest() are the guest memory accessors the load/store group needs.
//     They are already written, already carry the non-PIE rebias, already use memcpy (never a cast-and-
//     deref, so an unaligned guest access cannot fault or be reordered by the host compiler), and are
//     already bracketed by the fault marker described below. A load/store implementation calls them and
//     needs no new fault plumbing.
//   * interp_block_ends() is the pre-scan's view of where a block stops. It must stay in agreement with the
//     set of instructions interp_step() answers INTERP_END for; see translate_block() for why a disagreement
//     is safe but wasteful.
//   * interp_undefined() is the single diagnostic exit. Every unimplemented class funnels through it, so
//     "how far does this guest get" is always answered by one line on stderr naming the exact encoding.
//
// PERFORMANCE IS EXPLICITLY NOT A GOAL HERE. The block descriptor only delimits the guest instruction range;
// run_block re-fetches and re-decodes every instruction on every execution. Caching the decoded form in the
// descriptor is the obvious later optimisation and is discussed at translate_block(). Correctness first.

#include <setjmp.h>
// The guest's floating point is computed by the HOST's floating-point unit with the guest's FPCR.RMode
// projected onto the host's rounding control; see the long note at "Floating point" below for why that is the
// design and where it is not enough. <fenv.h> is the portable spelling of that projection (glibc implements it
// over MXCSR on x86-64) and <math.h> supplies the two operations C has no operator for -- fma, which is the
// only correctly-rounded spelling of FMADD, and sqrt.
#include <fenv.h>
#include <math.h>

#include "../../guest_fetch.h"
#include "../../identity.h"
#include "../../digest.h"
#include "../../../host/host_cpu.h"
#include "../../../host/range.h"
#include "../../../linux_abi/logical_vma.h"

// ---------------------------------------------------------------------------
// Engine-wide debug/identity state that stubs.c owns for the JIT.
// ---------------------------------------------------------------------------
// These are not JIT concepts, they are engine concepts that happened to be declared in the JIT's stub file
// because that was the first unit in the TU to need them. Everything downstream of this include -- the
// dispatcher's trace hook, the syscall tracer, the container's /proc synthesis, the ELF loader, the
// checkpoint writer -- reads them, so the backend that replaces stubs.c has to own them too. Values are the
// production defaults (all tracing off), reset explicitly in engine_global_init().
static int g_trace;         // G_TRACE_DUMP: per-block guest PC + register dump
static int g_systrace;      // syscall tracer
static int g_dbg_nochain;   // suppress inter-block chaining so every block re-enters the dispatcher
static int g_dbg_gprdump;   // dump all guest GPRs per block, for a register-value differential
// Guest path of the image this process is running, i.e. what /proc/self/exe must report. Set by the loader
// before the guest starts; a string literal rather than NULL so an early reader cannot dereference nothing.
static const char *g_exe_path = "";

// ---------------------------------------------------------------------------
// Non-PIE image geometry, and the one place a guest address is not a host address.
// ---------------------------------------------------------------------------
// Really defined (and set by load_elf) in linux_abi/elf.c + linux_abi/container/vfs.c, both compiled LATER in
// this same unity TU. Re-listing them here as tentative definitions -- exactly as translate.c does -- merges
// with that single later definition and lets the code below un-bias a PC. All three are 0 for PIE and
// static-PIE images, which is every image the test matrix loads, so every use is inert there.
static uint64_t g_nonpie_lo, g_nonpie_hi, g_nonpie_bias;

// Guest-signal delivery, really defined in linux_abi/signal.c -- which is compiled LATER in this same unity TU,
// so it is declared (not defined) here. Only BRK/HLT need it: they are the one instruction class whose whole
// architectural meaning is "raise a synchronous exception", and every other guest signal this backend produces
// travels the dispatcher's reason codes instead. Declaring a static before its definition is ordinary C and is
// the same technique translate.c uses for the tentative definitions above.
static void raise_guest_signal(struct cpu *c, int sig);

// PC-relative base for ADR/ADRP and for the return address a BL/BLR writes to x30.
//
// A non-PIE ET_EXEC is mapped HIGH (its low link range is reserved), and the dispatcher biases the guest PC
// into that high mapping before it looks the block up. But the image's own baked absolute pointers are LOW
// (non-PIE means no dynamic relocations), and real code compares an ADR/ADRP-computed pointer against such a
// stored pointer for identity. Materialising the HIGH value therefore breaks pointer identity. So compute
// PC-relative VALUES against the LOW (un-biased) PC; a data access through the resulting low pointer is
// served from the real high mapping by the nonpie_fixup fault path, and the dispatcher re-biases a low PC
// back to high on entry. Control flow keeps the HIGH pc throughout -- only the produced address is low.
//
// This is character-for-character translate.c's pcrel_base, and it must stay that way: linux_abi/signal.c
// hands the guest an un-biased PC in its signal frame through this same function (via
// signal_canonicalize_pc in core/target/aarch64.c), so the two backends must agree on what "the guest's own
// view of this PC" is or a restored checkpoint would disagree with itself.
static uint64_t pcrel_base(uint64_t gpc) {
    if (g_nonpie_lo && gpc >= g_nonpie_lo + g_nonpie_bias && gpc < g_nonpie_hi + g_nonpie_bias)
        return gpc - g_nonpie_bias;
    return gpc;
}

// Guest VA -> host VA. They are equal, with exactly one exception: a non-PIE ET_EXEC's low link range is
// served at +g_nonpie_bias. This is the ENTIRE address translation layer, and it must stay that way -- the
// engine's whole memory model is that the guest runs on real host mappings, which is what lets mmap, fork,
// shared memory, file mappings and the checkpoint work at all. Mirrors hl_x86_guest_pointer in
// core/target/x86_64.c so both frontends fold the same way.
static uint64_t interp_guest_pointer(uint64_t address) {
    return g_nonpie_lo && address >= g_nonpie_lo && address < g_nonpie_hi ? address + g_nonpie_bias : address;
}

// ---------------------------------------------------------------------------
// The fault model.
// ---------------------------------------------------------------------------
// With the JIT, a guest fault happens inside emitted code and hl_aarch64_signal_capture has to RECONSTRUCT
// guest state by copying the interrupted host register file back into struct cpu. Here that reconstruction
// problem does not exist: struct cpu is already authoritative at every instruction boundary and cpu->pc is
// exact. What the host handler needs instead is a way to ABANDON the C access that is in flight and get back
// to the dispatcher, because the faulting instruction is a memcpy inside interp_read_guest/
// interp_write_guest, and simply returning from the handler would re-execute it and fault forever.
//
// So: a thread-local marker is armed around every guest memory access, run_block does a sigsetjmp at its
// top, and the handler siglongjmps back through it.
//
// WHY siglongjmp OUT OF A SIGNAL HANDLER IS SAFE HERE. It is the standard interpreter technique, and the
// specific reasons it is sound in this engine are:
//   * The jump target is in the SAME thread. sigsetjmp/siglongjmp are async-signal-safe and are defined for
//     exactly this use; the saved-mask form (savemask = 1) restores the signal mask the handler entered
//     with, so the thread does not return with SIGSEGV still blocked.
//   * Nothing between the sigsetjmp and the marked access owns a resource that needs unwinding. The
//     dispatcher releases g_jit_lock BEFORE calling run_block, run_block itself takes no lock and allocates
//     nothing, and everything it touches is either automatic storage (discarded by the jump) or *cpu.
//   * Guest architectural state is already correct. A load commits its destination register only AFTER the
//     marked memcpy has returned (memcpy into a local, marker cleared, then the register write), and a
//     store reads its source register into a local BEFORE the marked memcpy. So the abandoned instruction
//     has made no partial architectural change, and cpu->pc still names it -- which is precisely what the
//     guest's own signal handler is entitled to see.
//   * The reason code is chosen by the handler path (linux_abi/signal.c sets c->reason = R_BRANCH and queues
//     the guest signal in c->tpending before asking us to resume), so run_block returning normally after the
//     jump lands the dispatcher in the ordinary "deliver a pending signal" path.
static __thread struct cpu *g_interp_marker_cpu;   // the cpu whose run_block armed the marker (NULL = none)
static __thread sigjmp_buf g_interp_marker_jmp;    // where interp_signal_resume goes
static __thread int g_interp_marker_armed;         // 1 while g_interp_marker_jmp is valid
static __thread int g_interp_access_active;        // 1 while a guest memory access is in flight
static __thread uint64_t g_interp_access_address;  // its effective guest address
static __thread uint64_t g_interp_access_bytes;    // its size
static __thread int g_interp_access_write;         // 1 for a store, 0 for a load

static void interp_access_begin(uint64_t address, uint64_t bytes, int write) {
    g_interp_access_address = address;
    g_interp_access_bytes = bytes;
    g_interp_access_write = write;
    // Publish LAST: the handler tests this flag to decide whether the fault is a guest memory fault, so the
    // description must already be readable when it becomes true. Ordinary stores suffice -- the reader is a
    // signal handler on this same thread, not another CPU -- but the compiler must not sink them past the
    // flag, hence the barrier.
    __atomic_store_n(&g_interp_access_active, 1, __ATOMIC_RELEASE);
}

static void interp_access_end(void) {
    __atomic_store_n(&g_interp_access_active, 0, __ATOMIC_RELEASE);
}

// Called (via sigframe_capture_fault in core/target/aarch64.c) from the host SIGSEGV/SIGBUS guard on the
// faulting thread. Returns 1 when the fault happened inside a marked guest access -- in which case *c is
// ALREADY the correct guest state and there is nothing to reconstruct -- and 0 otherwise, so that a genuine
// engine bug in our own C still reaches the crash report instead of being laundered into a guest signal.
//
// Note what is deliberately NOT tested: the host PC. The JIT's capture predicate is "is the host PC inside a
// retained code cache", because that is the only way to tell emitted guest code from engine code when the
// two share a register file. Here the arena holds no executable code at all, and the faulting host PC is
// inside memcpy or inside this file -- indistinguishable from an engine bug by address. The marker is the
// discriminator, and it is a strictly more precise one: it is set only around an access the GUEST asked for.
static int interp_signal_capture(struct cpu *c, void *ucontext) {
    (void)ucontext;
    if (c == NULL) return 0;
    if (!__atomic_load_n(&g_interp_access_active, __ATOMIC_ACQUIRE)) return 0;
    if (g_interp_marker_cpu != c) return 0; // a fault on another thread's cpu is not this thread's to own
    // The only state the caller cannot derive: which guest address the abandoned access was aiming at. The
    // host siginfo's si_addr is the faulting HOST address, which differs from the guest address for a folded
    // non-PIE access, and on some hosts is imprecise for a straddling access -- so record the exact
    // architectural effective address the decoder computed. linux_abi/signal.c consumes fault_addr for the
    // R_BUS path and fills sync_address from siginfo for the SIGSEGV path.
    c->fault_addr = g_interp_access_address;
    return 1;
}

// Called (via sigframe_resume_dispatch) once the handler has decided the guest owns this fault and has set
// up c->sync_signal / c->sync_code / c->tpending / c->reason. Abandons the in-flight access and returns
// control to run_block, which returns to the dispatcher with the reason the handler chose.
static void interp_signal_resume(struct cpu *c, void *ucontext) {
    (void)ucontext;
    if (!g_interp_marker_armed || g_interp_marker_cpu != c) {
        // Cannot happen through the capture-then-resume protocol (resume is only ever reached after
        // interp_signal_capture returned 1, which already proved the marker belongs to this cpu). Returning
        // is the only safe action left: the handler returns, the faulting access re-executes and re-faults,
        // and the loop is at least visible rather than a jump through a stale buffer.
        return;
    }
    siglongjmp(g_interp_marker_jmp, 1);
}

// ---------------------------------------------------------------------------
// Guest memory. Not yet used by any implemented instruction -- the integer core touches no memory -- but
// written now because it is the framework the load/store group plugs into, and because getting the fault
// bracketing right once is the point of this file.
// ---------------------------------------------------------------------------
// UNALIGNED ACCESS. AArch64 permits unaligned normal-memory loads and stores, and real guests rely on it.
// memcpy is used rather than a cast-and-deref for two independent reasons: on a host that traps unaligned
// access the deref would fault where the guest expects success, and even on x86-64 (where it would not)
// dereferencing a misaligned pointer is undefined behaviour that the compiler is free to assume away.
// The guest is little-endian AArch64 (the engine models no big-endian guest: see g_aarch64_cpu_model), and
// every host CPU this file can be compiled for is little-endian too. That equality is what lets a guest value
// be moved with a plain memcpy of its low-order bytes instead of an explicit per-byte assembly. It is an
// assumption rather than a fact about C, so state it where it would break.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the aarch64 interpreter backend assumes a little-endian host"
#endif

static int interp_read_guest(uint64_t address, void *destination, unsigned bytes) {
    uint64_t host = interp_guest_pointer(address);
    interp_access_begin(address, bytes, 0);
    memcpy(destination, (const void *)(uintptr_t)host, bytes);
    interp_access_end();
    return 1;
}

static int interp_write_guest(uint64_t address, const void *source, unsigned bytes) {
    uint64_t host = interp_guest_pointer(address);
    interp_access_begin(address, bytes, 1);
    memcpy((void *)(uintptr_t)host, source, bytes);
    interp_access_end();
    return 1;
}

// Load `bytes` (1, 2, 4 or 8) from the guest, zero-extended into a 64-bit value. The destination register is
// written by the CALLER, after this has returned -- which is what makes the fault path clean: if the access
// faults, the siglongjmp unwinds before any architectural state changed, so cpu->pc still names an
// instruction that has not executed and the guest's own handler sees exactly that.
static uint64_t interp_load_bits(uint64_t address, unsigned bytes) {
    uint64_t value = 0;
    interp_read_guest(address, &value, bytes);
    return value;
}

// Store the low `bytes` bytes of `value`. The caller has already read every source register into locals, so
// an abandoned store likewise leaves no half-committed state.
static void interp_store_bits(uint64_t address, uint64_t value, unsigned bytes) {
    interp_write_guest(address, &value, bytes);
}

// The host pointer an atomic read-modify-write operates on directly. The LSE and exclusive paths below need
// the address as a pointer rather than as bytes, because their whole point is that the access is indivisible
// and so cannot be expressed as a memcpy in and a memcpy out.
//
// Alignment is the guest's responsibility here and the architecture agrees: an atomic or exclusive access to
// an unaligned address is a guest fault (SP alignment and atomicity both require natural alignment), so
// refusing to proceed is correct rather than conservative. Returning NULL lets the caller raise it as such
// instead of invoking a misaligned host atomic, which on some hosts silently loses atomicity.
// A vector load/store of `bytes` (1, 2, 4, 8 or 16). Split into 64-bit chunks so it reuses the same
// unaligned-safe, fault-marked path as an integer access. A load of fewer than 16 bytes zeroes the rest of the
// destination register, which interp_vec_write handles via the q argument.
static void interp_vec_load(struct cpu *cpu, int reg, uint64_t address, unsigned bytes);
static void interp_vec_store(struct cpu *cpu, int reg, uint64_t address, unsigned bytes);

static void *interp_atomic_pointer(uint64_t address, unsigned bytes) {
    uint64_t host = interp_guest_pointer(address);
    if (host & (bytes - 1u)) return NULL;
    return (void *)(uintptr_t)host;
}

// ---------------------------------------------------------------------------
// Guest register file and condition flags.
// ---------------------------------------------------------------------------
// Register 31 means XZR in most encodings and SP in a few (the add/sub immediate and add/sub extended-
// register forms, and the destination of a non-flag-setting logical immediate). Getting that wrong is a
// silent wrong-answer bug, so the two meanings get two differently-named accessors at every use site rather
// than one accessor and a per-site flag.
static uint64_t interp_gpr(const struct cpu *cpu, int reg) {
    return reg == 31 ? UINT64_C(0) : cpu->x[reg];
}

static uint64_t interp_gpr_sp(const struct cpu *cpu, int reg) {
    return reg == 31 ? cpu->sp : cpu->x[reg];
}

static void interp_set_gpr(struct cpu *cpu, int reg, uint64_t value) {
    if (reg != 31) cpu->x[reg] = value;
}

static void interp_set_gpr_sp(struct cpu *cpu, int reg, uint64_t value) {
    if (reg == 31)
        cpu->sp = value;
    else
        cpu->x[reg] = value;
}

// A 32-bit result always zero-extends into the 64-bit register; there is no partial-register write on
// AArch64. Funnelling every 32-bit destination through here is what keeps that invariant in one place.
static void interp_set_gpr32(struct cpu *cpu, int reg, uint32_t value) {
    interp_set_gpr(cpu, reg, (uint64_t)value);
}

static void interp_set_gpr32_sp(struct cpu *cpu, int reg, uint32_t value) {
    interp_set_gpr_sp(cpu, reg, (uint64_t)value);
}

// cpu->nzcv holds the flags EXACTLY as `mrs Xt, nzcv` reads them and `msr nzcv, Xt` writes them: N at bit
// 31, Z at 30, C at 29, V at 28, everything else zero. That is the JIT's representation (stubs.c spills with
// `mrs x0,nzcv` and reloads with `msr nzcv,x9`), and it is also what crosses a signal frame: guest/aarch64/
// signal.c stores cpu->nzcv into the sigcontext's pstate word at mc+272 and reads it straight back on
// sigreturn. So a guest handler that inspects uc_mcontext.pstate, and a checkpoint written by either
// backend, both see the same bits. Do not switch to a packed 4-bit form.
#define INTERP_NZCV_N (UINT64_C(1) << 31)
#define INTERP_NZCV_Z (UINT64_C(1) << 30)
#define INTERP_NZCV_C (UINT64_C(1) << 29)
#define INTERP_NZCV_V (UINT64_C(1) << 28)

static void interp_set_flags(struct cpu *cpu, unsigned n, unsigned z, unsigned c, unsigned v) {
    cpu->nzcv = (n ? INTERP_NZCV_N : 0) | (z ? INTERP_NZCV_Z : 0) | (c ? INTERP_NZCV_C : 0) |
                (v ? INTERP_NZCV_V : 0);
}

// FPCR/FPSR. Per-thread, and deliberately NOT added to struct cpu -- that layout is the checkpoint format and
// is shared with the JIT, which does not model these fields either (emitted code uses the host's real FPCR,
// which is only meaningful when the host is AArch64). These two words ARE this backend's floating-point
// control and status: FPCR.RMode selects the rounding of every FP result, FPCR.FZ/FZ16/DN change what the
// arithmetic does with denormals and NaNs, and FPSR accumulates the five IEEE exception bits plus the AdvSIMD
// saturation bit. See the "Floating point" section below for how they are consumed.
//
// They are guest-visible by two routes and must agree on both: MRS/MSR (the cases in interp_exec_branch_system
// above), and the FPSIMD record guest/aarch64/signal.c writes into a signal frame, which carries FPSR and FPCR
// as the two words following V31 -- so a guest handler that inspects uc_mcontext's fpsimd_context reads exactly
// these.
static __thread uint64_t g_interp_fpcr;
static __thread uint64_t g_interp_fpsr;

// FPCR fields. Only these are modelled; everything else a guest writes is dropped by INTERP_FPCR_WRITABLE,
// which is itself a deliberate, guest-visible answer (see the MSR FPCR case).
#define INTERP_FPCR_FZ16(f) (((f) >> 19) & 1u)  // flush-to-zero for HALF-precision operations
#define INTERP_FPCR_RMODE(f) (((f) >> 22) & 3u) // 00 nearest-even, 01 +inf, 10 -inf, 11 zero
#define INTERP_FPCR_FZ(f) (((f) >> 24) & 1u)    // flush-to-zero for single/double
#define INTERP_FPCR_DN(f) (((f) >> 25) & 1u)    // default-NaN mode
#define INTERP_FPCR_AHP(f) (((f) >> 26) & 1u)   // alternative (non-IEEE) half-precision format

// The bits MSR FPCR may set. Everything else -- notably the six exception TRAP-ENABLE bits IDE/IXE/UFE/OFE/
// DZE/IOE at [15:8] -- reads back as zero, which is how the architecture spells "this implementation does not
// support trapped floating-point exceptions", and is the truth here: nothing below ever traps.
#define INTERP_FPCR_WRITABLE                                                                                           \
    ((UINT64_C(1) << 19) | (UINT64_C(3) << 22) | (UINT64_C(1) << 24) | (UINT64_C(1) << 25) | (UINT64_C(1) << 26))

// FPSR cumulative-exception bits, at their architectural positions. QC is the AdvSIMD saturation flag; it is
// not an IEEE exception and is set by the saturating integer instructions rather than by the FP core.
#define INTERP_FPSR_IOC 0x01u      // invalid operation
#define INTERP_FPSR_DZC 0x02u      // divide by zero
#define INTERP_FPSR_OFC 0x04u      // overflow
#define INTERP_FPSR_UFC 0x08u      // underflow
#define INTERP_FPSR_IXC 0x10u      // inexact
#define INTERP_FPSR_IDC 0x80u      // input denormal (set only when FPCR.FZ actually flushed one)
#define INTERP_FPSR_QC 0x08000000u // AdvSIMD cumulative saturation
#define INTERP_FPSR_WRITABLE (0x9Fu | INTERP_FPSR_QC)

static void interp_fpsr_raise(unsigned bits) {
    g_interp_fpsr |= bits;
}

static unsigned interp_flag_n(const struct cpu *cpu) { return (cpu->nzcv & INTERP_NZCV_N) != 0; }

static unsigned interp_flag_z(const struct cpu *cpu) { return (cpu->nzcv & INTERP_NZCV_Z) != 0; }

static unsigned interp_flag_c(const struct cpu *cpu) { return (cpu->nzcv & INTERP_NZCV_C) != 0; }

static unsigned interp_flag_v(const struct cpu *cpu) { return (cpu->nzcv & INTERP_NZCV_V) != 0; }

// ConditionHolds(). The low bit of the condition field inverts the test, except for the 0b111x pair where
// 0b1111 (NV) also means "always" -- the architecture defines NV as AL, not as "never".
static int interp_cond_holds(const struct cpu *cpu, unsigned cond) {
    unsigned n = interp_flag_n(cpu), z = interp_flag_z(cpu), c = interp_flag_c(cpu), v = interp_flag_v(cpu);
    int result;
    switch ((cond >> 1) & 7) {
    case 0: result = z; break;                     // EQ / NE
    case 1: result = c; break;                     // CS(HS) / CC(LO)
    case 2: result = n; break;                     // MI / PL
    case 3: result = v; break;                     // VS / VC
    case 4: result = c && !z; break;               // HI / LS
    case 5: result = (n == v); break;              // GE / LT
    case 6: result = (n == v) && !z; break;        // GT / LE
    default: result = 1; break;                    // AL / NV
    }
    if ((cond & 1) && (cond & 0xE) != 0xE) result = !result;
    return result;
}

// AddWithCarry(). Returns the result and, when `flags` is non-NULL, the NZCV it would set. Subtraction is
// expressed as AddWithCarry(a, ~b, 1), which is how the architecture defines it and is why SUBS leaves C set
// on "no borrow" rather than clear.
static uint64_t interp_add_with_carry64(uint64_t a, uint64_t b, unsigned carry_in, struct cpu *cpu, int set) {
    uint64_t partial, result;
    int carry_a = __builtin_add_overflow(a, b, &partial);
    int carry_b = __builtin_add_overflow(partial, (uint64_t)carry_in, &result);
    if (set)
        interp_set_flags(cpu, (result >> 63) & 1, result == 0, (unsigned)(carry_a | carry_b),
                         (unsigned)(((a ^ result) & (b ^ result)) >> 63) & 1u);
    return result;
}

static uint32_t interp_add_with_carry32(uint32_t a, uint32_t b, unsigned carry_in, struct cpu *cpu, int set) {
    uint32_t partial, result;
    int carry_a = __builtin_add_overflow(a, b, &partial);
    int carry_b = __builtin_add_overflow(partial, (uint32_t)carry_in, &result);
    if (set)
        interp_set_flags(cpu, (result >> 31) & 1, result == 0, (unsigned)(carry_a | carry_b),
                         (unsigned)(((a ^ result) & (b ^ result)) >> 31) & 1u);
    return result;
}

// Logical operations set N and Z from the result and always CLEAR C and V (they do not compute a carry).
static void interp_set_logical_flags(struct cpu *cpu, uint64_t result, unsigned sf) {
    unsigned negative = sf ? (unsigned)((result >> 63) & 1) : (unsigned)((result >> 31) & 1);
    uint64_t masked = sf ? result : (uint32_t)result;
    interp_set_flags(cpu, negative, masked == 0, 0, 0);
}

static int64_t interp_sext(uint64_t value, unsigned bits) {
    return (int64_t)(value << (64 - bits)) >> (64 - bits);
}

static uint64_t interp_ror64(uint64_t value, unsigned amount) {
    amount &= 63;
    return amount ? ((value >> amount) | (value << (64 - amount))) : value;
}

// CRC32/CRC32C, in the REFLECTED form the architecture defines. ARM spells it as bit-reversing the operands,
// doing a polynomial division by the generator, and bit-reversing the result; that is algebraically identical to
// the ordinary table-free reflected CRC loop below, which is also what the zlib and ACLE `__crc32*` spellings
// compute -- so a guest that checksums with these and compares against its own software CRC agrees. The two
// constants are the reflections of 0x04C11DB7 (CRC-32) and 0x1EDC6F41 (CRC-32C, Castagnoli).
//
// The data operand is consumed LOW BYTE FIRST, which is what makes CRC32X equal to two CRC32W steps over the
// same little-endian bytes. An implementation that fed it high byte first would produce plausible-looking
// checksums that never match anything, with no other symptom.
static uint64_t interp_crc32(uint32_t accumulator, uint64_t data, unsigned bytes, int castagnoli) {
    uint32_t polynomial = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (unsigned index = 0; index < bytes; index++) {
        accumulator ^= (uint32_t)((data >> (8u * index)) & 0xFFu);
        for (unsigned bit = 0; bit < 8u; bit++)
            accumulator = (accumulator >> 1) ^ (polynomial & (uint32_t)(-(int32_t)(accumulator & 1u)));
    }
    return (uint64_t)accumulator;
}

static uint32_t interp_ror32(uint32_t value, unsigned amount) {
    amount &= 31;
    return amount ? ((value >> amount) | (value << (32 - amount))) : value;
}

// DecodeBitMasks(). Shared by the logical-immediate group (immediate = 1, which forbids the all-ones
// element) and by the bitfield group (immediate = 0, which permits it). Returns 0 for an encoding the
// architecture leaves UNDEFINED so the caller can route it to interp_undefined rather than invent a result.
static int interp_bit_masks(unsigned sf, unsigned immn, unsigned imms, unsigned immr, int immediate,
                           uint64_t *wmask_out, uint64_t *tmask_out) {
    uint32_t combined = (immn << 6) | ((~imms) & 0x3Fu); // N : NOT(imms)
    int length = -1;
    for (int bit = 6; bit >= 0; --bit)
        if (combined & (1u << bit)) {
            length = bit;
            break;
        }
    if (length < 1) return 0;
    if (!sf && immn) return 0; // a 64-bit-only element size in a 32-bit instruction
    unsigned levels = (1u << (unsigned)length) - 1u;
    if (immediate && (imms & levels) == levels) return 0;
    unsigned s = imms & levels, r = immr & levels;
    unsigned diff = (s - r) & levels;
    unsigned esize = 1u << (unsigned)length;
    uint64_t element_mask = esize == 64 ? UINT64_MAX : ((UINT64_C(1) << esize) - 1u);
    uint64_t welem = s + 1u >= 64u ? UINT64_MAX : ((UINT64_C(1) << (s + 1u)) - 1u);
    uint64_t telem = diff + 1u >= 64u ? UINT64_MAX : ((UINT64_C(1) << (diff + 1u)) - 1u);
    uint64_t rotated = welem & element_mask;
    if (r) rotated = ((rotated >> r) | (rotated << (esize - r))) & element_mask;
    uint64_t wmask = 0, tmask = 0;
    for (unsigned offset = 0; offset < 64; offset += esize) {
        wmask |= rotated << offset;
        tmask |= (telem & element_mask) << offset;
    }
    if (wmask_out) *wmask_out = wmask;
    if (tmask_out) *tmask_out = tmask;
    return 1;
}

// ---------------------------------------------------------------------------
// The one diagnostic exit.
// ---------------------------------------------------------------------------
// Every encoding this backend cannot execute -- a class that is not written yet, and a genuinely unallocated
// encoding -- funnels through here. The two are not distinguished, deliberately: while coverage is partial
// there is no way to tell "the guest executed something illegal" from "this backend has a gap", and guessing
// wrong in either direction is worse than saying exactly what was seen. When coverage is complete the
// unallocated case becomes a guest SIGILL and this function keeps only the gap case.
//
// The report goes out two ways, because the two answer different questions. stderr is what a developer
// running the engine by hand needs and is the only route guaranteed to be visible (HL_ENABLE_LOGGING is 0 in
// the production build, which compiles hl_fatal_report's log emit out entirely). jit_fail() latches the
// status into g_jit_fatal, which the dispatcher tests at the top of every iteration -- that is what actually
// STOPS the guest, cleanly, with exit code 70, instead of letting it run on with a skipped instruction.
static int interp_undefined(struct cpu *cpu, uint32_t insn, const char *class_name) {
    char message[320];
    int written = snprintf(message, sizeof message,
                           "[interp] TODO(amd64-host) unimplemented aarch64 encoding 0x%08x at guest pc 0x%llx "
                           "class=\"%s\" op0=0x%x sf=%u Rd=%u Rn=%u Rm=%u Rt2=%u",
                           insn, (unsigned long long)cpu->pc, class_name, (unsigned)((insn >> 25) & 0xF),
                           (unsigned)((insn >> 31) & 1), (unsigned)(insn & 31), (unsigned)((insn >> 5) & 31),
                           (unsigned)((insn >> 16) & 31), (unsigned)((insn >> 10) & 31));
    if (written < 0) written = 0;
    if ((size_t)written >= sizeof message) written = (int)sizeof message - 1;
    fprintf(stderr, "%s\n", message);
    (void)jit_fail(HL_STATUS_NOT_SUPPORTED, message, (size_t)written);
    // Leave cpu->pc ON the offending instruction so the message and the architectural state agree, and exit
    // as an ordinary branch: the dispatcher's fatal check at the top of the next iteration is what ends the
    // run, and R_BRANCH is the only reason that does not additionally misinterpret the state on the way out.
    cpu->reason = R_BRANCH;
    return 1;
}

// ---------------------------------------------------------------------------
// Decode and execute.
// ---------------------------------------------------------------------------
#define INTERP_NEXT 0 // the instruction completed; cpu->pc has been advanced; keep going in this block
#define INTERP_END 1  // the block ends here; cpu->reason and cpu->pc are final

// Data processing -- immediate. Sub-class selected by insn[25:23], as in the ARM ARM table.
static int interp_exec_dp_immediate(struct cpu *cpu, uint32_t insn) {
    uint64_t gpc = cpu->pc;
    unsigned group = (insn >> 23) & 7;
    unsigned sf = (insn >> 31) & 1;
    int rd = (int)(insn & 31), rn = (int)((insn >> 5) & 31);

    switch (group) {
    case 0:
    case 1: { // PC-relative addressing: ADR / ADRP
        int64_t immediate = interp_sext((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u), 21);
        uint64_t value;
        if (insn & 0x80000000u) // ADRP: page-aligned base, immediate scaled by 4 KiB
            value = (pcrel_base(gpc) & ~UINT64_C(0xFFF)) + ((uint64_t)immediate << 12);
        else
            value = pcrel_base(gpc) + (uint64_t)immediate;
        interp_set_gpr(cpu, rd, value);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    case 2: { // Add/subtract (immediate)
        unsigned op = (insn >> 30) & 1, setflags = (insn >> 29) & 1, shift = (insn >> 22) & 1;
        uint64_t immediate = (insn >> 10) & 0xFFFu;
        if (shift) immediate <<= 12;
        // Rn is <Xn|SP> in every form. Rd is <Xd|SP> for ADD/SUB but <Xd> (XZR) for ADDS/SUBS, which is what
        // makes `cmp` (SUBS xzr, ...) discard its result instead of writing SP.
        if (sf) {
            uint64_t a = interp_gpr_sp(cpu, rn);
            uint64_t result = op ? interp_add_with_carry64(a, ~immediate, 1, cpu, (int)setflags)
                                 : interp_add_with_carry64(a, immediate, 0, cpu, (int)setflags);
            if (setflags)
                interp_set_gpr(cpu, rd, result);
            else
                interp_set_gpr_sp(cpu, rd, result);
        } else {
            uint32_t a = (uint32_t)interp_gpr_sp(cpu, rn);
            uint32_t result = op ? interp_add_with_carry32(a, ~(uint32_t)immediate, 1, cpu, (int)setflags)
                                 : interp_add_with_carry32(a, (uint32_t)immediate, 0, cpu, (int)setflags);
            if (setflags)
                interp_set_gpr32(cpu, rd, result);
            else
                interp_set_gpr32_sp(cpu, rd, result);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    case 3: // Add/subtract (immediate, with tags): ADDG/SUBG, part of MTE.
        return interp_undefined(cpu, insn, "data-processing immediate -- ADDG/SUBG (memory tagging)");
    case 4: { // Logical (immediate)
        unsigned opc = (insn >> 29) & 3, immn = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3Fu, imms = (insn >> 10) & 0x3Fu;
        uint64_t wmask;
        if (!interp_bit_masks(sf, immn, imms, immr, 1, &wmask, NULL))
            return interp_undefined(cpu, insn, "data-processing immediate -- undefined logical-immediate mask");
        uint64_t operand = interp_gpr(cpu, rn), result;
        switch (opc) {
        case 0: result = operand & wmask; break; // AND
        case 1: result = operand | wmask; break; // ORR
        case 2: result = operand ^ wmask; break; // EOR
        default: result = operand & wmask; break; // ANDS
        }
        if (!sf) result = (uint32_t)result;
        if (opc == 3) { // ANDS: flag-setting, so Rd is XZR when 31
            interp_set_logical_flags(cpu, result, sf);
            if (sf)
                interp_set_gpr(cpu, rd, result);
            else
                interp_set_gpr32(cpu, rd, (uint32_t)result);
        } else { // AND/ORR/EOR: Rd is <Xd|SP>
            if (sf)
                interp_set_gpr_sp(cpu, rd, result);
            else
                interp_set_gpr32_sp(cpu, rd, (uint32_t)result);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    case 5: { // Move wide (immediate): MOVN / MOVZ / MOVK
        unsigned opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
        uint64_t imm16 = (insn >> 5) & 0xFFFFu;
        if (opc == 1) return interp_undefined(cpu, insn, "data-processing immediate -- unallocated move-wide opc");
        if (!sf && (hw & 2)) return interp_undefined(cpu, insn, "data-processing immediate -- 32-bit move-wide hw>1");
        unsigned shift = hw * 16u;
        uint64_t field = imm16 << shift;
        uint64_t result;
        if (opc == 0)
            result = ~field; // MOVN
        else if (opc == 2)
            result = field; // MOVZ
        else                // MOVK: keep the other halfwords of the current value
            result = (interp_gpr(cpu, rd) & ~(UINT64_C(0xFFFF) << shift)) | field;
        if (sf)
            interp_set_gpr(cpu, rd, result);
        else
            interp_set_gpr32(cpu, rd, (uint32_t)result);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    case 6: { // Bitfield: SBFM / BFM / UBFM (and every alias: SXTB..ASR, BFI/BFXIL, UBFX/LSL/LSR)
        unsigned opc = (insn >> 29) & 3, immn = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3Fu, imms = (insn >> 10) & 0x3Fu;
        uint64_t wmask, tmask;
        if (opc == 3 || immn != sf)
            return interp_undefined(cpu, insn, "data-processing immediate -- unallocated bitfield encoding");
        if (!interp_bit_masks(sf, immn, imms, immr, 0, &wmask, &tmask))
            return interp_undefined(cpu, insn, "data-processing immediate -- undefined bitfield mask");
        uint64_t source = interp_gpr(cpu, rn);
        uint64_t rotated = sf ? interp_ror64(source, immr) : (uint64_t)interp_ror32((uint32_t)source, immr);
        uint64_t result;
        if (opc == 1) { // BFM: keep the destination bits outside the inserted field
            uint64_t destination = interp_gpr(cpu, rd);
            uint64_t bottom = (destination & ~wmask) | (rotated & wmask);
            result = (destination & ~tmask) | (bottom & tmask);
        } else {
            uint64_t bottom = rotated & wmask;
            // SBFM replicates bit S of the source above the field; UBFM zeroes it.
            uint64_t top = (opc == 0 && ((source >> imms) & 1)) ? UINT64_MAX : UINT64_C(0);
            result = (top & ~tmask) | (bottom & tmask);
        }
        if (sf)
            interp_set_gpr(cpu, rd, result);
        else
            interp_set_gpr32(cpu, rd, (uint32_t)result);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    default: { // 7: Extract -- EXTR (and its ROR alias when Rn == Rm)
        unsigned immn = (insn >> 22) & 1, imms = (insn >> 10) & 0x3Fu;
        int rm = (int)((insn >> 16) & 31);
        if (((insn >> 29) & 3) != 0 || ((insn >> 21) & 1) != 0 || immn != sf)
            return interp_undefined(cpu, insn, "data-processing immediate -- unallocated extract encoding");
        if (!sf && (imms & 0x20u))
            return interp_undefined(cpu, insn, "data-processing immediate -- 32-bit EXTR lsb>31");
        uint64_t high = interp_gpr(cpu, rn), low = interp_gpr(cpu, rm);
        // Result is the low datasize bits of (Rn:Rm) >> lsb, so the low half comes from Rm.
        if (sf) {
            uint64_t result = imms ? ((low >> imms) | (high << (64 - imms))) : low;
            interp_set_gpr(cpu, rd, result);
        } else {
            uint32_t result = imms ? (((uint32_t)low >> imms) | ((uint32_t)high << (32 - imms))) : (uint32_t)low;
            interp_set_gpr32(cpu, rd, result);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    }
}

// ShiftReg(): the shift a data-processing-register operand applies to Rm. `amount` is already masked to the
// operand size by the encoding (imm6's top bit is reserved in 32-bit forms, which the caller rejects).
static uint64_t interp_shift_operand(uint64_t value, unsigned shift_type, unsigned amount, unsigned sf) {
    if (sf) {
        switch (shift_type) {
        case 0: return amount ? (value << amount) : value;                       // LSL
        case 1: return amount ? (value >> amount) : value;                       // LSR
        case 2: return (uint64_t)(amount ? ((int64_t)value >> amount) : (int64_t)value); // ASR
        default: return interp_ror64(value, amount);                             // ROR
        }
    }
    uint32_t narrow = (uint32_t)value;
    switch (shift_type) {
    case 0: return (uint64_t)(uint32_t)(amount ? (narrow << amount) : narrow);
    case 1: return (uint64_t)(uint32_t)(amount ? (narrow >> amount) : narrow);
    case 2: return (uint64_t)(uint32_t)(amount ? (uint32_t)((int32_t)narrow >> amount) : narrow);
    default: return (uint64_t)interp_ror32(narrow, amount);
    }
}

// ExtendReg(): the sign/zero-extended, then shifted, Rm of an add/sub extended-register form. option selects
// the source width and signedness; UXTX/SXTX (option 3/7) read the whole 64-bit register.
static uint64_t interp_extend_operand(const struct cpu *cpu, int rm, unsigned option, unsigned shift,
                                      unsigned sf) {
    // The register read is <R><m>, which is Wm for every option except UXTX/SXTX. Reading the full 64-bit
    // value and masking below is equivalent and avoids a second accessor.
    uint64_t value = interp_gpr(cpu, rm);
    uint64_t extended;
    switch (option) {
    case 0: extended = (uint8_t)value; break;                                    // UXTB
    case 1: extended = (uint16_t)value; break;                                   // UXTH
    case 2: extended = (uint32_t)value; break;                                   // UXTW
    case 3: extended = value; break;                                             // UXTX
    case 4: extended = (uint64_t)(int64_t)(int8_t)value; break;                   // SXTB
    case 5: extended = (uint64_t)(int64_t)(int16_t)value; break;                  // SXTH
    case 6: extended = (uint64_t)(int64_t)(int32_t)value; break;                  // SXTW
    default: extended = value; break;                                            // SXTX
    }
    extended <<= shift;
    return sf ? extended : (uint64_t)(uint32_t)extended;
}

// Data processing -- register. Sub-class selected by insn[28:21] together with insn[30] (op0/op1/op2/op3 in
// the ARM ARM table); the tests below are written in the order that table lists them.
static int interp_exec_dp_register(struct cpu *cpu, uint32_t insn) {
    uint64_t gpc = cpu->pc;
    unsigned sf = (insn >> 31) & 1;
    int rd = (int)(insn & 31), rn = (int)((insn >> 5) & 31), rm = (int)((insn >> 16) & 31);

    // Add/subtract (shifted register) and Add/subtract (extended register) share insn[28:24] == 01011 and are
    // separated by insn[21].
    if ((insn & 0x1F000000u) == 0x0B000000u) {
        unsigned op = (insn >> 30) & 1, setflags = (insn >> 29) & 1;
        uint64_t operand;
        int destination_is_sp;
        if (insn & 0x00200000u) { // extended register
            unsigned option = (insn >> 13) & 7, shift = (insn >> 10) & 7;
            if (shift > 4) return interp_undefined(cpu, insn, "data-processing register -- extend shift > 4");
            operand = interp_extend_operand(cpu, rm, option, shift, sf);
            destination_is_sp = 1; // Rn is <Xn|SP>, and Rd is <Xd|SP> unless flag-setting
        } else { // shifted register
            unsigned shift_type = (insn >> 22) & 3, amount = (insn >> 10) & 0x3Fu;
            if (shift_type == 3) return interp_undefined(cpu, insn, "data-processing register -- add/sub ROR");
            if (!sf && (amount & 0x20u))
                return interp_undefined(cpu, insn, "data-processing register -- 32-bit add/sub shift > 31");
            operand = interp_shift_operand(interp_gpr(cpu, rm), shift_type, amount, sf);
            destination_is_sp = 0; // this form names neither SP; encoding 31 is XZR throughout
        }
        if (sf) {
            uint64_t a = destination_is_sp ? interp_gpr_sp(cpu, rn) : interp_gpr(cpu, rn);
            uint64_t result = op ? interp_add_with_carry64(a, ~operand, 1, cpu, (int)setflags)
                                 : interp_add_with_carry64(a, operand, 0, cpu, (int)setflags);
            if (destination_is_sp && !setflags)
                interp_set_gpr_sp(cpu, rd, result);
            else
                interp_set_gpr(cpu, rd, result);
        } else {
            uint32_t a = (uint32_t)(destination_is_sp ? interp_gpr_sp(cpu, rn) : interp_gpr(cpu, rn));
            uint32_t result = op ? interp_add_with_carry32(a, ~(uint32_t)operand, 1, cpu, (int)setflags)
                                 : interp_add_with_carry32(a, (uint32_t)operand, 0, cpu, (int)setflags);
            if (destination_is_sp && !setflags)
                interp_set_gpr32_sp(cpu, rd, result);
            else
                interp_set_gpr32(cpu, rd, result);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // Logical (shifted register): insn[28:24] == 01010. opc selects AND/ORR/EOR/ANDS and N inverts Rm.
    if ((insn & 0x1F000000u) == 0x0A000000u) {
        unsigned opc = (insn >> 29) & 3, shift_type = (insn >> 22) & 3, negate = (insn >> 21) & 1;
        unsigned amount = (insn >> 10) & 0x3Fu;
        if (!sf && (amount & 0x20u))
            return interp_undefined(cpu, insn, "data-processing register -- 32-bit logical shift > 31");
        uint64_t operand = interp_shift_operand(interp_gpr(cpu, rm), shift_type, amount, sf);
        if (negate) operand = sf ? ~operand : (uint64_t)(uint32_t)~(uint32_t)operand;
        uint64_t a = interp_gpr(cpu, rn), result;
        switch (opc) {
        case 0: result = a & operand; break;  // AND / BIC
        case 1: result = a | operand; break;  // ORR / ORN  (MOV register is ORR with Rn == XZR)
        case 2: result = a ^ operand; break;  // EOR / EON
        default: result = a & operand; break; // ANDS / BICS
        }
        if (!sf) result = (uint32_t)result;
        if (opc == 3) interp_set_logical_flags(cpu, result, sf);
        if (sf)
            interp_set_gpr(cpu, rd, result);
        else
            interp_set_gpr32(cpu, rd, (uint32_t)result);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // Everything remaining in this group has insn[28:24] == 11010 or 11011.
    if ((insn & 0x1F000000u) == 0x1B000000u) { // Data-processing (3 source)
        unsigned op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
        int ra = (int)((insn >> 10) & 31);
        uint64_t addend = interp_gpr(cpu, ra);
        switch (op31) {
        case 0: { // MADD / MSUB (and MUL / MNEG, which are these with Ra == XZR)
            if (sf) {
                uint64_t product = interp_gpr(cpu, rn) * interp_gpr(cpu, rm);
                interp_set_gpr(cpu, rd, o0 ? addend - product : addend + product);
            } else {
                uint32_t product = (uint32_t)interp_gpr(cpu, rn) * (uint32_t)interp_gpr(cpu, rm);
                uint32_t base = (uint32_t)addend;
                interp_set_gpr32(cpu, rd, o0 ? base - product : base + product);
            }
            break;
        }
        case 1: { // SMADDL / SMSUBL (SMULL / SMNEGL with Ra == XZR); 64-bit form only
            if (!sf) return interp_undefined(cpu, insn, "data-processing register -- 32-bit widening multiply");
            int64_t product = (int64_t)(int32_t)interp_gpr(cpu, rn) * (int64_t)(int32_t)interp_gpr(cpu, rm);
            interp_set_gpr(cpu, rd, o0 ? addend - (uint64_t)product : addend + (uint64_t)product);
            break;
        }
        case 2: { // SMULH
            if (!sf || o0) return interp_undefined(cpu, insn, "data-processing register -- unallocated SMULH form");
            __int128 product = (__int128)(int64_t)interp_gpr(cpu, rn) * (__int128)(int64_t)interp_gpr(cpu, rm);
            interp_set_gpr(cpu, rd, (uint64_t)(product >> 64));
            break;
        }
        case 5: { // UMADDL / UMSUBL (UMULL / UMNEGL with Ra == XZR); 64-bit form only
            if (!sf) return interp_undefined(cpu, insn, "data-processing register -- 32-bit widening multiply");
            uint64_t product = (uint64_t)(uint32_t)interp_gpr(cpu, rn) * (uint64_t)(uint32_t)interp_gpr(cpu, rm);
            interp_set_gpr(cpu, rd, o0 ? addend - product : addend + product);
            break;
        }
        case 6: { // UMULH
            if (!sf || o0) return interp_undefined(cpu, insn, "data-processing register -- unallocated UMULH form");
            unsigned __int128 product =
                (unsigned __int128)interp_gpr(cpu, rn) * (unsigned __int128)interp_gpr(cpu, rm);
            interp_set_gpr(cpu, rd, (uint64_t)(product >> 64));
            break;
        }
        default: return interp_undefined(cpu, insn, "data-processing register -- unallocated 3-source op31");
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x1FE00000u) == 0x1A000000u) { // Add/subtract (with carry): ADC / ADCS / SBC / SBCS
        unsigned op = (insn >> 30) & 1, setflags = (insn >> 29) & 1;
        if ((insn & 0x0000FC00u) != 0) return interp_undefined(cpu, insn, "data-processing register -- rotate/flag ops");
        unsigned carry = interp_flag_c(cpu);
        if (sf) {
            uint64_t a = interp_gpr(cpu, rn), b = interp_gpr(cpu, rm);
            uint64_t result = op ? interp_add_with_carry64(a, ~b, carry, cpu, (int)setflags)
                                 : interp_add_with_carry64(a, b, carry, cpu, (int)setflags);
            interp_set_gpr(cpu, rd, result);
        } else {
            uint32_t a = (uint32_t)interp_gpr(cpu, rn), b = (uint32_t)interp_gpr(cpu, rm);
            uint32_t result = op ? interp_add_with_carry32(a, ~b, carry, cpu, (int)setflags)
                                 : interp_add_with_carry32(a, b, carry, cpu, (int)setflags);
            interp_set_gpr32(cpu, rd, result);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x1FE00000u) == 0x1A400000u) { // Conditional compare (register and immediate): CCMN / CCMP
        unsigned op = (insn >> 30) & 1, setflags = (insn >> 29) & 1;
        unsigned cond = (insn >> 12) & 0xFu, immediate_form = (insn >> 11) & 1;
        unsigned nzcv = insn & 0xFu;
        if (!setflags || ((insn >> 10) & 1) != 0 || ((insn >> 4) & 1) != 0)
            return interp_undefined(cpu, insn, "data-processing register -- unallocated conditional-compare form");
        if (!interp_cond_holds(cpu, cond)) {
            // Condition failed: NZCV is REPLACED by the encoded literal. Not "left alone" -- that literal is
            // how a chained compare passes a known-false verdict down to the next CCMP.
            interp_set_flags(cpu, (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        uint64_t operand = immediate_form ? (uint64_t)((insn >> 16) & 0x1Fu) : interp_gpr(cpu, rm);
        if (sf) {
            uint64_t a = interp_gpr(cpu, rn);
            if (op)
                (void)interp_add_with_carry64(a, ~operand, 1, cpu, 1); // CCMP
            else
                (void)interp_add_with_carry64(a, operand, 0, cpu, 1); // CCMN
        } else {
            uint32_t a = (uint32_t)interp_gpr(cpu, rn);
            if (op)
                (void)interp_add_with_carry32(a, ~(uint32_t)operand, 1, cpu, 1);
            else
                (void)interp_add_with_carry32(a, (uint32_t)operand, 0, cpu, 1);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x1FE00000u) == 0x1A800000u) { // Conditional select: CSEL / CSINC / CSINV / CSNEG
        unsigned op = (insn >> 30) & 1, setflags = (insn >> 29) & 1, op2 = (insn >> 10) & 3;
        if (setflags || (op2 & 2))
            return interp_undefined(cpu, insn, "data-processing register -- unallocated conditional-select form");
        unsigned cond = (insn >> 12) & 0xFu;
        uint64_t result;
        if (interp_cond_holds(cpu, cond)) {
            result = interp_gpr(cpu, rn);
        } else {
            uint64_t other = interp_gpr(cpu, rm);
            if (!op)
                result = op2 ? other + 1 : other; // CSINC : CSEL
            else
                result = op2 ? (uint64_t)0 - other : ~other; // CSNEG : CSINV
        }
        if (sf)
            interp_set_gpr(cpu, rd, result);
        else
            interp_set_gpr32(cpu, rd, (uint32_t)result);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x1FE00000u) == 0x1AC00000u) { // Data-processing (1 source) and (2 source)
        if (insn & 0x40000000u) { // insn[30] == 1: 1 source
            unsigned opcode2 = (insn >> 16) & 0x1Fu, opcode = (insn >> 10) & 0x3Fu;
            if (((insn >> 29) & 1) || opcode2 != 0)
                return interp_undefined(cpu, insn, "data-processing register -- PAC/1-source extension");
            uint64_t value = interp_gpr(cpu, rn), result;
            switch (opcode) {
            case 0: { // RBIT
                uint64_t wide = value;
                wide = ((wide & UINT64_C(0x5555555555555555)) << 1) | ((wide >> 1) & UINT64_C(0x5555555555555555));
                wide = ((wide & UINT64_C(0x3333333333333333)) << 2) | ((wide >> 2) & UINT64_C(0x3333333333333333));
                wide = ((wide & UINT64_C(0x0F0F0F0F0F0F0F0F)) << 4) | ((wide >> 4) & UINT64_C(0x0F0F0F0F0F0F0F0F));
                wide = __builtin_bswap64(wide);
                result = sf ? wide : (uint64_t)(uint32_t)(wide >> 32);
                break;
            }
            case 1: { // REV16: byte-swap within each halfword
                uint64_t wide = value;
                result = ((wide & UINT64_C(0x00FF00FF00FF00FF)) << 8) | ((wide >> 8) & UINT64_C(0x00FF00FF00FF00FF));
                break;
            }
            case 2: // REV (32-bit form) / REV32 (64-bit form): byte-swap within each word
                if (sf) {
                    uint64_t wide = value;
                    result = ((wide & UINT64_C(0x000000FF000000FF)) << 24) |
                             ((wide & UINT64_C(0x0000FF000000FF00)) << 8) |
                             ((wide >> 8) & UINT64_C(0x0000FF000000FF00)) |
                             ((wide >> 24) & UINT64_C(0x000000FF000000FF));
                } else {
                    result = (uint64_t)__builtin_bswap32((uint32_t)value);
                }
                break;
            case 3: // REV (64-bit form)
                if (!sf) return interp_undefined(cpu, insn, "data-processing register -- 32-bit REV64");
                result = __builtin_bswap64(value);
                break;
            case 4: // CLZ
                if (sf)
                    result = value ? (uint64_t)__builtin_clzll(value) : 64u;
                else
                    result = (uint32_t)value ? (uint64_t)__builtin_clz((uint32_t)value) : 32u;
                break;
            case 5: { // CLS
                // CountLeadingSignBits(x) is CLZ over the (datasize-1)-bit value x[N-1:1] EOR x[N-2:0]. That
                // narrower value is bits [N-1:1] of (x ^ (x << 1)), so it has to be SHIFTED DOWN before the
                // count, and the count is then one less than a full-width CLZ because the value is one bit
                // short. An all-ones operand is the case that catches getting this wrong: the fold is 1, the
                // narrowed value is 0, and the answer is the full 63 (every bit matches the sign bit) rather
                // than anything a CLZ of the unshifted fold could produce.
                if (sf) {
                    uint64_t narrowed = (value ^ (value << 1)) >> 1;
                    result = narrowed ? (uint64_t)__builtin_clzll(narrowed) - 1u : 63u;
                } else {
                    uint32_t narrowed = (uint32_t)((uint32_t)value ^ ((uint32_t)value << 1)) >> 1;
                    result = narrowed ? (uint64_t)__builtin_clz(narrowed) - 1u : 31u;
                }
                break;
            }
            default: return interp_undefined(cpu, insn, "data-processing register -- unallocated 1-source opcode");
            }
            if (sf)
                interp_set_gpr(cpu, rd, result);
            else
                interp_set_gpr32(cpu, rd, (uint32_t)result);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        // insn[30] == 0: 2 source
        unsigned opcode = (insn >> 10) & 0x3Fu;
        if ((insn >> 29) & 1) return interp_undefined(cpu, insn, "data-processing register -- flag-setting 2-source");
        uint64_t a = interp_gpr(cpu, rn), b = interp_gpr(cpu, rm), result;
        switch (opcode) {
        case 2: // UDIV: division by zero yields 0, it does not trap
            if (sf)
                result = b ? a / b : 0;
            else
                result = (uint32_t)b ? (uint64_t)((uint32_t)a / (uint32_t)b) : 0;
            break;
        case 3: // SDIV: division by zero yields 0; INT_MIN / -1 saturates to INT_MIN (no overflow trap)
            if (sf) {
                int64_t x = (int64_t)a, y = (int64_t)b;
                result = y == 0 ? 0 : (y == -1 && x == INT64_MIN ? (uint64_t)x : (uint64_t)(x / y));
            } else {
                int32_t x = (int32_t)a, y = (int32_t)b;
                result = y == 0 ? 0 : (uint64_t)(uint32_t)(y == -1 && x == INT32_MIN ? x : x / y);
            }
            break;
        // The variable shifts mask their amount by the operand size, so LSLV by 64 is a no-op, not zero.
        case 8: result = interp_shift_operand(a, 0, (unsigned)(b & (sf ? 63u : 31u)), sf); break; // LSLV
        case 9: result = interp_shift_operand(a, 1, (unsigned)(b & (sf ? 63u : 31u)), sf); break; // LSRV
        case 10: result = interp_shift_operand(a, 2, (unsigned)(b & (sf ? 63u : 31u)), sf); break; // ASRV
        case 11: result = interp_shift_operand(a, 3, (unsigned)(b & (sf ? 63u : 31u)), sf); break; // RORV
        // CRC32B/H/W/X (opcode 10000..10011) and CRC32CB/H/W/X (10100..10111). The accumulator and result are
        // always 32-bit; sf selects only whether the DATA operand is a W or an X register, which is why sf must
        // be 1 for exactly the ..X forms and 0 for the others.
        case 16:
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23: {
            unsigned data_bytes = 1u << (opcode & 3u);
            if ((data_bytes == 8) != (sf != 0))
                return interp_undefined(cpu, insn, "data-processing register -- CRC32 size/sf mismatch");
            result = interp_crc32((uint32_t)a, b, data_bytes, (opcode & 4u) != 0);
            // The result is a 32-bit value in a W register whatever sf said, so it cannot go through the
            // sf-selected write below.
            interp_set_gpr32(cpu, rd, (uint32_t)result);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        default: return interp_undefined(cpu, insn, "data-processing register -- unallocated 2-source opcode");
        }
        if (sf)
            interp_set_gpr(cpu, rd, result);
        else
            interp_set_gpr32(cpu, rd, (uint32_t)result);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    return interp_undefined(cpu, insn, "data-processing register -- unallocated encoding");
}

// Branches, exception generating and system instructions.
//
// Every form here ends the block. Returning to the dispatcher at every branch is what makes this backend
// simple AND is what makes it at least as prompt as the JIT at noticing an async signal: the JIT's inline
// branch-target cache exists precisely so an indirect branch does NOT have to come back here, and dropping
// it costs throughput and nothing else. c->irq is polled by run_block, so a signal is never worse off.
static int interp_exec_branch_system(struct cpu *cpu, uint32_t insn) {
    uint64_t gpc = cpu->pc;

    // Unconditional branch (immediate): B / BL
    if ((insn & 0x7C000000u) == 0x14000000u) {
        int64_t offset = interp_sext(insn & 0x3FFFFFFu, 26) << 2;
        if (insn & 0x80000000u) {
            // BL writes the return address as the guest's OWN view of it: for a non-PIE image that is the
            // low, un-biased address, so the value matches the image's baked pointers and a later RET
            // through it is re-biased by the dispatcher. Identical to the JIT's emit_set_x30(pcrel_base + 4).
            cpu->x[30] = pcrel_base(gpc) + 4;
        }
        cpu->pc = gpc + (uint64_t)offset;
        cpu->reason = R_BRANCH;
        return INTERP_END;
    }

    // Conditional branch (immediate): B.cond, and BC.cond (the v8.8 consistent-branch hint, which differs
    // only in bit 4 and has identical architectural effect).
    if ((insn & 0xFF000010u) == 0x54000000u || (insn & 0xFF000010u) == 0x54000010u) {
        int64_t offset = interp_sext((insn >> 5) & 0x7FFFFu, 19) << 2;
        cpu->pc = interp_cond_holds(cpu, insn & 0xFu) ? gpc + (uint64_t)offset : gpc + 4;
        cpu->reason = R_BRANCH;
        return INTERP_END;
    }

    // Compare and branch (immediate): CBZ / CBNZ
    if ((insn & 0x7E000000u) == 0x34000000u) {
        unsigned sf = (insn >> 31) & 1, nonzero = (insn >> 24) & 1;
        int64_t offset = interp_sext((insn >> 5) & 0x7FFFFu, 19) << 2;
        uint64_t value = interp_gpr(cpu, (int)(insn & 31));
        int is_zero = sf ? value == 0 : (uint32_t)value == 0;
        cpu->pc = (nonzero ? !is_zero : is_zero) ? gpc + (uint64_t)offset : gpc + 4;
        cpu->reason = R_BRANCH;
        return INTERP_END;
    }

    // Test and branch (immediate): TBZ / TBNZ. The bit position is b5:b40, so a 64-bit-register bit index
    // above 31 is expressed by insn[31] rather than by an sf field.
    if ((insn & 0x7E000000u) == 0x36000000u) {
        unsigned nonzero = (insn >> 24) & 1;
        unsigned bit = (unsigned)(((insn >> 31) & 1) << 5) | ((insn >> 19) & 0x1Fu);
        int64_t offset = interp_sext((insn >> 5) & 0x3FFFu, 14) << 2;
        uint64_t value = interp_gpr(cpu, (int)(insn & 31));
        int set = (int)((value >> bit) & 1);
        cpu->pc = (nonzero ? set : !set) ? gpc + (uint64_t)offset : gpc + 4;
        cpu->reason = R_BRANCH;
        return INTERP_END;
    }

    // Unconditional branch (register): BR / BLR / RET (and the PAC/ERET forms, which are not modelled).
    if ((insn & 0xFE000000u) == 0xD6000000u) {
        unsigned opc = (insn >> 21) & 0xFu, op2 = (insn >> 16) & 0x1Fu, op3 = (insn >> 10) & 0x3Fu;
        int rn = (int)((insn >> 5) & 31);
        unsigned op4 = insn & 0x1Fu;
        if (op2 != 0x1F || op3 != 0 || op4 != 0)
            return interp_undefined(cpu, insn, "branch register -- pointer-authenticated branch (BRAA/BLRAA/RETAA)");
        switch (opc) {
        case 0: // BR
            cpu->pc = interp_gpr(cpu, rn);
            break;
        case 1: // BLR
        {
            uint64_t target = interp_gpr(cpu, rn);
            cpu->x[30] = pcrel_base(gpc) + 4;
            cpu->pc = target; // read the target BEFORE writing x30: `blr x30` must use the old value
            break;
        }
        case 2: // RET
            cpu->pc = interp_gpr(cpu, rn);
            break;
        default: return interp_undefined(cpu, insn, "branch register -- ERET/DRPS or unallocated opc");
        }
        cpu->reason = R_BRANCH;
        return INTERP_END;
    }

    // Exception generation: SVC / HVC / SMC / BRK / HLT / DCPS.
    if ((insn & 0xFF000000u) == 0xD4000000u) {
        unsigned opc = (insn >> 21) & 7, ll = insn & 3u;
        if (opc == 0 && ll == 1) {
            // SVC: the guest PC stays ON the svc while the kernel runs, which is an AArch64 ABI property and
            // therefore the same on every host. G_DISPATCH_REASON advances it by 4 after service() unless the
            // syscall (execve, sigreturn) set pc itself.
            cpu->pc = gpc;
            cpu->reason = R_SYSCALL;
            return INTERP_END;
        }
        // Everything else in this box is a GUEST event, not an engine limitation, so none of it belongs in
        // interp_undefined -- which latches a fatal engine status and stops the run with exit code 70. What
        // Linux/AArch64 does with each:
        //
        //   BRK #imm (opc == 001)  -- brk_handler() -> SIGTRAP/TRAP_BRKPT with si_addr and the PC left ON the
        //     BRK. The kernel does not advance past it, so a handler that returns re-executes it and traps
        //     again; that is real Linux behaviour and is reproduced here by leaving cpu->pc == gpc. This is the
        //     common case by far: __builtin_trap(), UBSan's trap-on-undefined, and a compiled `assert` that
        //     the compiler decided to fold into a trap all reach it.
        //   HLT #imm (opc == 010)  -- an EXTERNAL-debug halting instruction. With halting debug disabled, which
        //     it always is for an emulated EL0 process, the architecture leaves HLT UNDEFINED, so Linux
        //     delivers SIGILL. That is also what the JIT produces on an AArch64 host, where the guest's HLT is
        //     copied verbatim and executed: matching it keeps the two backends' guest-visible behaviour equal.
        //   HVC / SMC (opc == 000, LL 10/11) and DCPS1/2/3 (opc == 101) -- EL2/EL3 and debug-state
        //     instructions, all UNDEFINED at EL0, so SIGILL as well.
        //
        // raise_guest_signal() is the right delivery route rather than a hand-built frame: it already applies
        // the guest's disposition (a handler runs, SIG_IGN drops it, and SIG_DFL for these two fatal-default
        // signals goes through guest_group_fatal so the container reports 128+signo with the core-dump flag).
        // It is declared just above -- it lives in linux_abi/signal.c, later in this same unity translation
        // unit. The one fidelity gap is si_code: it comes out as SI_USER rather than TRAP_BRKPT/ILL_ILLOPC and
        // si_addr is 0, because the queued route carries no fault address. Closing that needs a
        // raise_guest_debug_trap() beside raise_guest_bus() in linux_abi/signal.c, which is not this file's to
        // add; see the findings doc.
        int signo = opc == 1 ? 5 /* SIGTRAP */ : 4 /* SIGILL */;
        cpu->pc = gpc; // the faulting instruction, which is what the guest's frame must name
        raise_guest_signal(cpu, signo);
        cpu->reason = R_BRANCH;
        return INTERP_END;
    }

    // Hints (the NOP space): 1101 0101 0000 0011 0010 CRm op2 11111. Every member is architecturally a no-op
    // for a user-space emulator -- NOP, YIELD, WFE/WFI (we are not a scheduler), SEV/SEVL, the pointer-auth
    // hints, BTI, CSDB/SSBB and the trace barriers. Treating the whole space as NOP rather than enumerating
    // it is deliberate: a hint the architecture adds later is still a hint.
    if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) {
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // Barriers: 1101 0101 0000 0011 0011 CRm op2 11111.
    // Note the trailing 0x1F: like the hint space, the barrier encodings pin Rt to 11111, so the constant is
    // 0xD503301F and not 0xD5033000. Getting that wrong sends every barrier -- including the ISB that commits
    // the guest's icache maintenance -- into the MRS/MSR catch-all below instead.
    if ((insn & 0xFFFFF01Fu) == 0xD503301Fu) {
        unsigned op2 = (insn >> 5) & 7;
        if (op2 == 6) {
            // ISB is the architecturally visible commit point of the guest's icache-maintenance dance
            // (dc cvau; dsb; ic ivau; dsb; isb). Exit R_ICCOMMIT so the dispatcher runs smc_commit() and any
            // block whose SOURCE bytes were rewritten is dropped before the guest can execute them. The
            // interpreter needs this exactly as much as the JIT does, for a subtler reason: it re-decodes on
            // every execution, so the guest's new BYTES are picked up for free -- but the cached block
            // EXTENT was computed from the old bytes, and a rewrite that moves the block-ending branch would
            // otherwise leave the region boundary stale.
            cpu->pc = gpc + 4;
            cpu->reason = R_ICCOMMIT;
            return INTERP_END;
        }
        // DSB / DMB / SB / CLREX. A guest thread is a host thread here, and this interpreter's guest loads
        // and stores are ordinary C accesses that the host compiler and CPU may reorder, so a guest data
        // barrier must become a real host barrier. Sequentially consistent is stronger than any of these
        // orderings and is the only one that is correct for all of them.
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // Cache maintenance, of which exactly two members matter to a translator.
    if ((insn & 0xFFFFFFE0u) == 0xD50B7B20u) { // dc cvau, Xt -- clean data cache to point of unification
        // A pure no-op for a dynamic translator: the host never instruction-fetches guest pages, so the
        // guest's data writes need no clean before we re-read them. (Real guests reach here because
        // __clear_cache issues the whole dance unconditionally.)
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }
    if ((insn & 0xFFFFFFE0u) == 0xD50B7520u) { // ic ivau, Xt -- invalidate instruction cache by VA
        // The guest is telling us it has rewritten code. Record the line and exit R_ICFLUSH; the dispatcher
        // refreshes any emulated file mapping over that page and calls smc_icflush() to QUEUE the dirty line
        // (the drop itself happens at the ISB, see above). pc resumes past the ic ivau.
        cpu->smc_va = interp_gpr(cpu, (int)(insn & 31));
        cpu->pc = gpc + 4;
        cpu->reason = R_ICFLUSH;
        return INTERP_END;
    }

    // ---- DC ZVA: zero the data cache block containing Xt ----
    // The block size is the one the guest was TOLD about via DCZID_EL0, not the host's. The JIT lowers this to
    // four explicit 16-byte stores for exactly that reason: copying the opcode verbatim would clear whatever
    // the host CPU's block size happens to be, and a runtime that places live metadata immediately after a
    // zeroed block then sees it corrupted. g_aarch64_cpu_model.dczid_el0 == 4 advertises 2^4 words = 64 bytes,
    // so 64 bytes is what gets zeroed.
    if ((insn & 0xFFFFFFE0u) == 0xD50B7420u) {
        uint64_t address = interp_gpr(cpu, (int)(insn & 31)) & ~UINT64_C(63);
        for (unsigned offset = 0; offset < 64u; offset += 8)
            interp_store_bits(address + offset, 0, 8);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- MSR (immediate): DAIFSet / DAIFClr and the other PSTATE fields ----
    // Interrupt masking is an EL1 concept with no meaning for an emulated EL0 process, so these are no-ops --
    // which is also what they effectively are for a Linux user-space program that executes them at all.
    if ((insn & 0xFFF8F01Fu) == 0xD500401Fu) {
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- MRS / MSR: system register access ----
    // Every value handed to the guest here comes from g_aarch64_cpu_model (cpu.h) or from emulated state, never
    // from the host CPU. That is a security property, not a convenience: this is an x86-64 host, so a host
    // register read is not merely wrong but meaningless, and even on an AArch64 host forwarding an ID register
    // leaks the real CPU and can make a guest JIT emit extensions the engine never advertised.
    if ((insn & 0xFFD00000u) == 0xD5100000u) {
        int rt = (int)(insn & 31);
        uint32_t reg = insn & 0xFFFFFFE0u;
        int is_read = (insn & 0x00200000u) != 0; // bit 21 is L: 1 = MRS, 0 = MSR

        // The EL1 ID-register space (op0 == 3, op1 == 0). HWCAP_CPUID is deliberately absent from the model, so
        // these are architecturally inaccessible at EL0; the JIT answers 0 rather than trapping, because a
        // trap here is far more disruptive to a guest that probes optimistically than a zero is.
        // Mask spelled exactly as translate.c's (0xFFFF0000 == 0xD5380000, i.e. op0 == 3 && op1 == 0 && CRn == 0)
        // so the two backends deny precisely the same register set. A guest's ifunc resolver branches on these,
        // so a broader or narrower gate here would make the same binary take a different path per backend.
        if (is_read && (insn & 0xFFFF0000u) == 0xD5380000u && !g_aarch64_cpu_model.user_id_registers) {
            interp_set_gpr(cpu, rt, 0);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        switch (reg) {
        case 0xD53B0020u: // MRS CTR_EL0 -- cache type. IDC=1/DIC=0 describe THIS translator's coherence model:
                          // no data-cache clean is needed before we re-read a guest's written bytes, but the
                          // guest must keep issuing `ic ivau` because that is the engine's SMC interception
                          // point. 64-byte I/D lines.
            interp_set_gpr(cpu, rt, g_aarch64_cpu_model.ctr_el0);
            break;
        case 0xD53B00E0u: // MRS DCZID_EL0 -- the DC ZVA block size the guest may rely on (see DC ZVA above)
            interp_set_gpr(cpu, rt, g_aarch64_cpu_model.dczid_el0);
            break;
        case 0xD53BD040u: // MRS TPIDR_EL0 -- the thread pointer, which the engine emulates in cpu->tls
            interp_set_gpr(cpu, rt, cpu->tls);
            break;
        case 0xD51BD040u: // MSR TPIDR_EL0
            cpu->tls = interp_gpr(cpu, rt);
            break;
        case 0xD53BD060u: // MRS TPIDRRO_EL0 -- read-only alias; the engine keeps one thread pointer
            interp_set_gpr(cpu, rt, cpu->tls);
            break;
        case 0xD53B4200u: // MRS NZCV
            interp_set_gpr(cpu, rt, cpu->nzcv & (INTERP_NZCV_N | INTERP_NZCV_Z | INTERP_NZCV_C | INTERP_NZCV_V));
            break;
        case 0xD51B4200u: // MSR NZCV -- only the four condition flags are writable
            cpu->nzcv = interp_gpr(cpu, rt) & (INTERP_NZCV_N | INTERP_NZCV_Z | INTERP_NZCV_C | INTERP_NZCV_V);
            break;
        case 0xD53B4220u: // MRS DAIF -- no interrupts are masked from an emulated EL0's point of view
            interp_set_gpr(cpu, rt, 0);
            break;
        case 0xD51B4220u: // MSR DAIF -- see the MSR-immediate note above
            break;
        case 0xD53B4400u: // MRS FPCR
            interp_set_gpr(cpu, rt, g_interp_fpcr);
            break;
        case 0xD51B4400u: // MSR FPCR -- authoritative: RMode selects the rounding of every FP result below,
                          // and FZ/FZ16/DN change what the arithmetic does with denormals and NaNs.
            // Masked to the fields this backend models, which is a GUEST-VISIBLE answer and the correct one:
            // the six exception trap-enable bits read back as zero, which is how the architecture says
            // "trapped floating-point exceptions are not implemented", and it is exactly what glibc's
            // feenableexcept() tests for (it writes the bit, reads FPCR back, and reports failure when it did
            // not stick). Every common AArch64 implementation answers the same way, so a guest sees here what
            // it would see on the hardware the JIT runs on.
            g_interp_fpcr = interp_gpr(cpu, rt) & INTERP_FPCR_WRITABLE;
            break;
        case 0xD53B4420u: // MRS FPSR
            interp_set_gpr(cpu, rt, g_interp_fpsr);
            break;
        case 0xD51B4420u: // MSR FPSR -- the guest clearing or presetting the cumulative exception bits, which
                          // is what feclearexcept/fesetexceptflag compile to. Only the six IEEE sticky bits
                          // and the AdvSIMD saturation bit QC are writable; the rest are RES0.
            g_interp_fpsr = interp_gpr(cpu, rt) & INTERP_FPSR_WRITABLE;
            break;
        case 0xD53BE000u: // MRS CNTFRQ_EL0 -- the counter frequency the guest should assume
            // 1 GHz, chosen so that the counter below IS a nanosecond count. Any other frequency would force a
            // scaling step whose rounding the guest could observe drifting against clock_gettime, which is
            // served from the same host monotonic clock.
            interp_set_gpr(cpu, rt, UINT64_C(1000000000));
            break;
        case 0xD53BE020u: // MRS CNTPCT_EL0 (physical counter)
        case 0xD53BE040u: // MRS CNTVCT_EL0 (virtual counter)
        case 0xD53BE0C0u: // MRS CNTVCTSS_EL0 (self-synchronising virtual counter)
            // The same host monotonic clock the engine answers clock_gettime from, in nanoseconds, matching the
            // 1 GHz frequency reported above. Sharing one source is what keeps a guest that cross-checks the
            // counter against clock_gettime (every runtime with its own fast clock does) from seeing time move
            // backwards between the two.
            interp_set_gpr(cpu, rt, now_ns());
            break;
        default:
            // An unmodelled register. Reporting rather than guessing is the right default here: silently
            // answering 0 for a register the guest actually depends on produces a failure arbitrarily far from
            // its cause, and the report names the exact encoding so the model can be extended deliberately.
            return interp_undefined(cpu, insn, "system -- unmodelled system register (MRS/MSR)");
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- The remaining SYS/SYSL cache and TLB maintenance space ----
    if ((insn & 0xFFC00000u) == 0xD5000000u)
        return interp_undefined(cpu, insn, "system -- SYS/SYSL maintenance operation");

    return interp_undefined(cpu, insn, "branches, exception generating and system -- unallocated encoding");
}

// ---------------------------------------------------------------------------
// The vector register file.
// ---------------------------------------------------------------------------
// cpu->v[] is 64 uint64_t holding V0..V31 as {low 64, high 64} pairs, in that order -- the layout
// guest/aarch64/signal.c memcpy's straight into the sigframe's fpsimd_context and back, so it is part of the
// guest-visible ABI and not a private choice.
//
// A vector is manipulated here as 16 raw bytes rather than as a union of typed arrays, because element size
// is a runtime value decoded from the instruction (size + Q) and not a compile-time type. That keeps one
// element accessor pair instead of one per width.
typedef struct {
    uint8_t byte[16];
} interp_vec;

static interp_vec interp_vec_read(const struct cpu *cpu, int reg) {
    interp_vec value;
    memcpy(value.byte, &cpu->v[2 * reg], 16);
    return value;
}

// THE RULE THAT IS EASY TO GET WRONG: writing a D-form (Q == 0, 64-bit) result must ZERO the upper 64 bits of
// the destination register. The architecture requires it for every AdvSIMD and scalar-FP write, and silently
// preserving the old upper half is the classic vector-interpreter bug -- it stays invisible until some later
// routine reads the top half of a register it believes it fully defined. Funnelling every vector write through
// this one function is what makes the rule impossible to forget at a call site.
static void interp_vec_write(struct cpu *cpu, int reg, interp_vec value, unsigned q) {
    if (!q) memset(value.byte + 8, 0, 8);
    memcpy(&cpu->v[2 * reg], value.byte, 16);
    // The JIT sets this to the (nonzero) cpu pointer at the first vector write of a region so a syscall exit
    // knows it must spill V state. Nothing needs spilling here -- cpu->v[] IS the register file and is always
    // current -- but the field is part of the checkpoint image, so keep it truthful rather than stale.
    cpu->vdirty = (uint64_t)(uintptr_t)cpu;
}

// Element read/write. `size` is the architecture's log2 of the element width: 0 = B, 1 = H, 2 = S, 3 = D.
static uint64_t interp_vec_element(const interp_vec *value, unsigned size, unsigned index) {
    uint64_t element = 0;
    memcpy(&element, value->byte + (index << size), (size_t)1u << size);
    return element;
}

static void interp_vec_set_element(interp_vec *value, unsigned size, unsigned index, uint64_t element) {
    memcpy(value->byte + (index << size), &element, (size_t)1u << size);
}

static unsigned interp_vec_lanes(unsigned size, unsigned q) {
    return (q ? 16u : 8u) >> size;
}

static uint64_t interp_element_mask(unsigned size) {
    return size >= 3 ? UINT64_MAX : ((UINT64_C(1) << (8u << size)) - 1u);
}

static uint64_t interp_element_sext(uint64_t element, unsigned size) {
    return (uint64_t)interp_sext(element, 8u << size);
}

// AdvSIMDExpandImm(): the shared immediate decoder for MOVI/MVNI and the immediate forms of ORR/BIC. Produces
// the 64-bit pattern that is then replicated across the destination. Returns 0 for a reserved cmode/op
// combination so the caller can report it rather than invent a value.
static int interp_advsimd_expand_imm(unsigned op, unsigned cmode, unsigned o2, unsigned q, uint64_t imm8,
                                     uint64_t *out) {
    unsigned selector = (cmode >> 1) & 7, low = cmode & 1;
    uint64_t imm64;
    if (selector <= 3 && !low) { // 32-bit element, imm8 shifted by 0/8/16/24
        uint32_t narrow = (uint32_t)(imm8 << (8u * selector));
        imm64 = ((uint64_t)narrow << 32) | narrow;
    } else if (selector <= 3) { // the same shifts, but the ORR/BIC-immediate spelling
        uint32_t narrow = (uint32_t)(imm8 << (8u * selector));
        imm64 = ((uint64_t)narrow << 32) | narrow;
    } else if (selector == 4 || selector == 5) { // 16-bit element, imm8 shifted by 0 or 8
        uint16_t narrow = (uint16_t)(imm8 << (8u * (selector & 1u)));
        uint32_t doubled = ((uint32_t)narrow << 16) | narrow;
        imm64 = ((uint64_t)doubled << 32) | doubled;
    } else if (selector == 6) { // 32-bit element with a "moving ones" low field (MSL)
        uint32_t narrow = low ? (uint32_t)((imm8 << 16) | 0xFFFFu) : (uint32_t)((imm8 << 8) | 0xFFu);
        imm64 = ((uint64_t)narrow << 32) | narrow;
    } else if (!low && !op) { // 8-bit element replicated across all 8 bytes: MOVI Vd.8B/16B, #imm8
        if (o2) return 0;
        imm64 = imm8 * UINT64_C(0x0101010101010101);
    } else if (!low) {
        // cmode == 1110 with op == 1: MOVI Dd, #imm / MOVI Vd.2D, #imm, where each BIT of imm8 expands to a
        // whole BYTE of the 64-bit element -- imm8<0> becomes the LOWEST byte and imm8<7> the highest. This is
        // how a lane mask is materialised in one instruction, so `movi v0.2d, #0xffffffff` (imm8 == 0x0f) means
        // 0x00000000ffffffff and NOT the byte-replicated 0x0f0f0f0f0f0f0f0f the op == 0 spelling above gives.
        // Sharing that arm was a silent wrong-answer bug: -O2 emits this form for the clamp mask in any
        // vectorised saturating add, so the mask compared as ~1.08e18 instead of 2^32-1 and never fired.
        if (o2) return 0;
        imm64 = 0;
        for (unsigned byte = 0; byte < 8u; byte++)
            if ((imm8 >> byte) & 1u) imm64 |= UINT64_C(0xFF) << (8u * byte);
    } else if (!op) { // single-precision float expansion, replicated
        uint32_t sign = (uint32_t)((imm8 >> 7) & 1), exponent = (uint32_t)((imm8 >> 4) & 7);
        uint32_t fraction = (uint32_t)(imm8 & 0xFu);
        uint32_t narrow = (sign << 31) | ((exponent & 4u) ? 0x3E000000u : 0x40000000u) |
                          ((exponent & 3u) << 23) | (fraction << 19);
        imm64 = ((uint64_t)narrow << 32) | narrow;
    } else { // double-precision float expansion (64-bit element, Q must be 1)
        if (!q) return 0;
        uint64_t sign = (imm8 >> 7) & 1, exponent = (imm8 >> 4) & 7, fraction = imm8 & 0xFu;
        imm64 = (sign << 63) | ((exponent & 4u) ? UINT64_C(0x3FC0000000000000) : UINT64_C(0x4000000000000000)) |
                (exponent & 3u) << 52 | (fraction << 48);
    }
    *out = imm64;
    return 1;
}

// The imm5 field of the AdvSIMD "copy" group encodes the element size in the position of its lowest set bit
// and the lane index in the bits above it. Returns 0 when no bit is set, which is a reserved encoding.
static int interp_imm5_element(unsigned imm5, unsigned *size_out, unsigned *index_out) {
    if (imm5 & 1u) {
        *size_out = 0;
        *index_out = (imm5 >> 1) & 0xFu;
    } else if (imm5 & 2u) {
        *size_out = 1;
        *index_out = (imm5 >> 2) & 7u;
    } else if (imm5 & 4u) {
        *size_out = 2;
        *index_out = (imm5 >> 3) & 3u;
    } else if (imm5 & 8u) {
        *size_out = 3;
        *index_out = (imm5 >> 4) & 1u;
    } else {
        return 0;
    }
    return 1;
}

static void interp_vec_load(struct cpu *cpu, int reg, uint64_t address, unsigned bytes) {
    interp_vec value;
    memset(value.byte, 0, sizeof value.byte);
    if (bytes <= 8) {
        uint64_t chunk = interp_load_bits(address, bytes);
        memcpy(value.byte, &chunk, bytes);
    } else {
        uint64_t low = interp_load_bits(address, 8), high = interp_load_bits(address + 8, 8);
        memcpy(value.byte, &low, 8);
        memcpy(value.byte + 8, &high, 8);
    }
    interp_vec_write(cpu, reg, value, bytes > 8);
}

static void interp_vec_store(struct cpu *cpu, int reg, uint64_t address, unsigned bytes) {
    interp_vec value = interp_vec_read(cpu, reg);
    uint64_t low, high;
    memcpy(&low, value.byte, 8);
    memcpy(&high, value.byte + 8, 8);
    if (bytes <= 8) {
        interp_store_bits(address, low, bytes);
    } else {
        interp_store_bits(address, low, 8);
        interp_store_bits(address + 8, high, 8);
    }
}

// The access width of a SIMD&FP load/store, which is spelled opc<1>:size rather than plain size: 0b0_00..0b0_11
// select B/H/S/D and 0b1_00 selects the 16-byte Q form. Returns 0 for an unallocated combination.
static unsigned interp_simd_access_bytes(unsigned size, unsigned opc) {
    if (opc & 2u) return size == 0 ? 16u : 0u;
    return 1u << size;
}

// ---------------------------------------------------------------------------
// Loads and stores.
// ---------------------------------------------------------------------------
// Two things in this group are where an interpreter silently corrupts a guest, so both are handled in exactly
// one place each:
//
//   * Rn == 31 IS SP, NOT XZR, for every addressing mode here. There is no load/store form that uses XZR as
//     a base -- `ldr x1, [sp]` is spelled with Rn == 31 and is the first instruction a static-pie image
//     executes. Every base read below goes through interp_gpr_sp, and every base WRITEBACK goes through
//     interp_set_gpr_sp. Rt/Rt2/Rm remain ordinary registers where 31 is XZR.
//   * The access itself is a memcpy (interp_load_bits / interp_store_bits), never a cast-and-deref, so an
//     unaligned guest access -- which AArch64 permits for normal memory and real guests rely on -- works
//     instead of faulting or being miscompiled. The one exception is the atomic/exclusive family, where the
//     architecture REQUIRES natural alignment and a misaligned access is a guest fault.
//
// Ordering of side effects is deliberate throughout: an access happens only after every source register has
// been read into a local, and a base writeback happens only after the access has completed. That is what lets
// the fault path just siglongjmp away without having to undo anything (see the fault-model comment at the top
// of this file), and it is also the architecturally required order when the base and the transfer register
// are the same register.

// The local exclusive monitor for LDXR/STXR. AArch64's load/store-exclusive pair is a compare-and-swap
// spelled as two instructions, and what a guest actually depends on is that the pair either commits as a
// whole or fails and is retried.
//
// So the monitor records the address AND the value LDXR observed, and STXR performs a real host
// compare-and-swap of that observed value against the new one. Guest threads are real host threads here, so a
// plain store would not be atomic against a peer; the CAS makes the pair genuinely atomic for the idiom the
// pair exists to express. What it does NOT reproduce is ABA: a peer that writes a different value and then
// writes the original one back leaves the CAS succeeding where real hardware would have cleared the monitor
// and failed. That is a strictly more permissive outcome than hardware, it is the same trade every
// interpreter without hardware monitor emulation makes, and it is invisible to lock and refcount code (which
// is what uses ll/sc) because those only care that the read-modify-write was not interleaved.
static __thread int g_interp_monitor_valid;
static __thread uint64_t g_interp_monitor_address;
static __thread unsigned g_interp_monitor_bytes;
static __thread uint64_t g_interp_monitor_value;
static __thread uint64_t g_interp_monitor_value2; // second register of an LDXP

static void interp_monitor_clear(void) {
    g_interp_monitor_valid = 0;
}


// A guest data fault this backend detects itself, rather than by taking a host signal: a misaligned atomic,
// which the architecture defines as an alignment fault. Routed through the same reason the JIT's inline
// soft-TLB probe uses for an address it cannot serve, so linux_abi/signal.c raises it on the guest as an
// ordinary synchronous SIGBUS/SIGSEGV at this exact PC.
static int interp_alignment_fault(struct cpu *cpu, uint64_t address) {
    cpu->fault_addr = address;
    cpu->bus_ea = address;
    cpu->reason = R_BUS;
    return INTERP_END;
}

static int interp_exec_load_store(struct cpu *cpu, uint32_t insn) {
    uint64_t gpc = cpu->pc;
    int rt = (int)(insn & 31), rn = (int)((insn >> 5) & 31);
    int rt2 = (int)((insn >> 10) & 31), rm = (int)((insn >> 16) & 31);
    unsigned vector = (insn >> 26) & 1;
    // Q here is the AdvSIMD structure forms' vector-length bit; the scalar addressing modes below do not use it.
    unsigned q = (insn >> 30) & 1;

    // ---- AdvSIMD load/store multiple structures: LD1/ST1 and the de-interleaving LD2/LD3/LD4, ST2/ST3/ST4 ----
    // A different top-level encoding from the scalar loads, which is why it is tested here explicitly rather
    // than falling out of the size/opc decode below.
    //   0 Q 0011 000 L 000000 opcode size Rn Rt   (no offset)
    //   0 Q 0011 001 L 0 Rm   opcode size Rn Rt   (post-index; Rm == 11111 selects the implicit immediate)
    // `opcode` names both the number of registers and whether they INTERLEAVE. The distinction is the whole
    // point of the group: LD1 with four registers is four consecutive full-register loads, while LD4 walks
    // memory one element at a time round-robin across the four registers -- the de-interleaving array-of-structs
    // load that a vectorising compiler emits and that cannot be expressed as "copy N bytes into a register".
    if ((insn & 0xBF200000u) == 0x0C000000u) {
        unsigned load = (insn >> 22) & 1u, opcode = (insn >> 12) & 0xFu, esize_code = (insn >> 10) & 3u;
        int post_index = (insn & 0x00800000u) != 0;
        unsigned registers, interleaved = 1;
        switch (opcode) {
        case 0x0: registers = 4; break;                    // LD4/ST4
        case 0x2: registers = 4; interleaved = 0; break;    // LD1/ST1, four registers
        case 0x4: registers = 3; break;                    // LD3/ST3
        case 0x6: registers = 3; interleaved = 0; break;    // LD1/ST1, three registers
        case 0x7: registers = 1; interleaved = 0; break;    // LD1/ST1, one register
        case 0x8: registers = 2; break;                    // LD2/ST2
        case 0xA: registers = 2; interleaved = 0; break;    // LD1/ST1, two registers
        default: return interp_undefined(cpu, insn, "AdvSIMD load/store -- unallocated multi-structure opcode");
        }
        unsigned bytes = q ? 16u : 8u;
        uint64_t base = interp_gpr_sp(cpu, rn);
        uint64_t address = base;
        // The 1D arrangement (size == 11, Q == 0) exists only for the one-register LD1/ST1 form; a
        // multi-register form needs a whole 128-bit vector per register when the element is 64 bits wide.
        if (esize_code == 3 && !q && registers > 1)
            return interp_undefined(cpu, insn, "AdvSIMD load/store -- 1D arrangement with several registers");
        if (interleaved) {
            unsigned lanes = interp_vec_lanes(esize_code, q), element_bytes = 1u << esize_code;
            for (unsigned lane = 0; lane < lanes; lane++)
                for (unsigned index = 0; index < registers; index++) {
                    int reg = (rt + (int)index) % 32; // the register list wraps at V31
                    if (load) {
                        uint64_t element = interp_load_bits(address, element_bytes);
                        // Read-modify-write one lane at a time, but the FIRST lane of each register starts from
                        // zero rather than from the register's old contents: the lanes this instruction does not
                        // write (all of bits [127:64] in a Q == 0 form) must end up zero, not stale.
                        interp_vec value;
                        if (lane == 0)
                            memset(value.byte, 0, sizeof value.byte);
                        else
                            value = interp_vec_read(cpu, reg);
                        interp_vec_set_element(&value, esize_code, lane, element);
                        interp_vec_write(cpu, reg, value, 1);
                    } else {
                        interp_vec value = interp_vec_read(cpu, reg);
                        interp_store_bits(address, interp_vec_element(&value, esize_code, lane), element_bytes);
                    }
                    address += element_bytes;
                }
        } else {
            for (unsigned index = 0; index < registers; index++) {
                int reg = (rt + (int)index) % 32;
                if (load) {
                    interp_vec value;
                    memset(value.byte, 0, sizeof value.byte);
                    for (unsigned offset = 0; offset < bytes; offset += 8) {
                        uint64_t chunk = interp_load_bits(address + offset, 8);
                        memcpy(value.byte + offset, &chunk, 8);
                    }
                    interp_vec_write(cpu, reg, value, q);
                } else {
                    interp_vec value = interp_vec_read(cpu, reg);
                    for (unsigned offset = 0; offset < bytes; offset += 8) {
                        uint64_t chunk;
                        memcpy(&chunk, value.byte + offset, 8);
                        interp_store_bits(address + offset, chunk, 8);
                    }
                }
                address += bytes;
            }
        }
        if (post_index) {
            // Rm == 31 means the immediate form, whose increment is the whole transfer size; otherwise the
            // increment is the register Rm. Committed AFTER every access, so an abandoned access leaves the
            // base register naming exactly what the guest's own fault handler expects.
            uint64_t increment = rm == 31 ? (uint64_t)registers * bytes : interp_gpr(cpu, rm);
            interp_set_gpr_sp(cpu, rn, base + increment);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- Load register (literal): LDR/LDRSW/PRFM, PC-relative ----
    if ((insn & 0x3B000000u) == 0x18000000u) {
        unsigned opc = (insn >> 30) & 3;
        int64_t offset = interp_sext((insn >> 5) & 0x7FFFFu, 19) << 2;
        // pcrel_base, not the raw PC, for the same reason ADR uses it: a non-PIE image's architectural PC is
        // its low link address, and interp_guest_pointer re-biases the resulting low address to the real high
        // mapping. Using it here also means a faulting literal load reports the address the guest expects.
        uint64_t address = pcrel_base(gpc) + (uint64_t)offset;
        if (vector) { // LDR St/Dt/Qt, literal: opc selects 4, 8 or 16 bytes
            if (opc == 3) return interp_undefined(cpu, insn, "loads and stores -- unallocated SIMD literal size");
            interp_vec_load(cpu, rt, address, 4u << opc);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (opc == 3) { // PRFM (literal): a hint, and a translator has no cache to prefetch into
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (opc == 2) // LDRSW: 4 bytes, sign-extended into a 64-bit register
            interp_set_gpr(cpu, rt, (uint64_t)interp_sext(interp_load_bits(address, 4), 32));
        else if (opc == 1) // LDR Xt
            interp_set_gpr(cpu, rt, interp_load_bits(address, 8));
        else // LDR Wt
            interp_set_gpr32(cpu, rt, (uint32_t)interp_load_bits(address, 4));
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- Load/store pair: STP/LDP/LDPSW/STNP/LDNP, in all four addressing modes ----
    if ((insn & 0x3A000000u) == 0x28000000u) {
        unsigned opc = (insn >> 30) & 3, load = (insn >> 22) & 1, mode = (insn >> 23) & 3;
        if (vector) { // STP/LDP of two S, D or Q registers -- glibc's memcpy moves 32 bytes at a time this way
            if (opc == 3) return interp_undefined(cpu, insn, "loads and stores -- unallocated SIMD pair opc");
            unsigned element = 4u << opc; // opc 0/1/2 -> 4, 8, 16 bytes per register
            int64_t vector_offset = interp_sext((insn >> 15) & 0x7Fu, 7) * (int64_t)element;
            uint64_t vector_base = interp_gpr_sp(cpu, rn);
            int vector_writeback = mode == 1 || mode == 3;
            uint64_t vector_address = mode == 1 ? vector_base : vector_base + (uint64_t)vector_offset;
            if (load) {
                interp_vec_load(cpu, rt, vector_address, element);
                interp_vec_load(cpu, rt2, vector_address + element, element);
            } else {
                interp_vec_store(cpu, rt, vector_address, element);
                interp_vec_store(cpu, rt2, vector_address + element, element);
            }
            if (vector_writeback) interp_set_gpr_sp(cpu, rn, vector_base + (uint64_t)vector_offset);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (opc == 3) return interp_undefined(cpu, insn, "loads and stores -- unallocated pair opc");
        if (opc == 1 && !load) return interp_undefined(cpu, insn, "loads and stores -- STGP (memory tagging)");
        unsigned bytes = opc == 2 ? 8u : 4u;
        unsigned scale = opc == 2 ? 3u : 2u;
        int64_t offset = interp_sext((insn >> 15) & 0x7Fu, 7) << scale;
        uint64_t base = interp_gpr_sp(cpu, rn);
        // mode 0 = LDNP/STNP (plain signed offset, no writeback), 1 = post-index, 2 = signed offset,
        // 3 = pre-index. Post-index is the one mode that accesses the UN-updated base.
        int writeback = mode == 1 || mode == 3;
        uint64_t address = mode == 1 ? base : base + (uint64_t)offset;
        if (load) {
            uint64_t first = interp_load_bits(address, bytes);
            uint64_t second = interp_load_bits(address + bytes, bytes);
            if (opc == 1) { // LDPSW: two 32-bit loads, each sign-extended into a 64-bit register
                interp_set_gpr(cpu, rt, (uint64_t)interp_sext(first, 32));
                interp_set_gpr(cpu, rt2, (uint64_t)interp_sext(second, 32));
            } else if (bytes == 8) {
                interp_set_gpr(cpu, rt, first);
                interp_set_gpr(cpu, rt2, second);
            } else {
                interp_set_gpr32(cpu, rt, (uint32_t)first);
                interp_set_gpr32(cpu, rt2, (uint32_t)second);
            }
        } else {
            // Read both source registers BEFORE either store, so that a store which overlaps the base or
            // transfer registers still writes the values the instruction was given.
            uint64_t first = interp_gpr(cpu, rt), second = interp_gpr(cpu, rt2);
            interp_store_bits(address, first, bytes);
            interp_store_bits(address + bytes, second, bytes);
        }
        // Writeback LAST. If Rn is also a transfer register the architecture calls the result CONSTRAINED
        // UNPREDICTABLE, and doing the writeback last is the behaviour real cores pick.
        if (writeback) interp_set_gpr_sp(cpu, rn, base + (uint64_t)offset);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- Load/store exclusive, and the ordered (LDAR/STLR) and CAS members of the same box ----
    if ((insn & 0x3F000000u) == 0x08000000u) {
        unsigned size = (insn >> 30) & 3, o2 = (insn >> 23) & 1, load = (insn >> 22) & 1;
        unsigned o1 = (insn >> 21) & 1, o0 = (insn >> 15) & 1;
        int rs = rm;
        unsigned bytes = 1u << size;

        if (o2 && o1) { // Compare and swap: CAS / CASA / CASL / CASAL (Rt2 is 11111)
            if (rt2 != 31) return interp_undefined(cpu, insn, "loads and stores -- unallocated CAS encoding");
            uint64_t address = interp_gpr_sp(cpu, rn);
            void *pointer = interp_atomic_pointer(address, bytes);
            if (pointer == NULL) return interp_alignment_fault(cpu, address);
            uint64_t compare = interp_gpr(cpu, rs), swap = interp_gpr(cpu, rt);
            // The comparand and the returned old value are the ACCESS width, not the register width, so mask
            // both: `casb w0, w1, [x2]` compares only the low byte.
            uint64_t mask = bytes == 8 ? UINT64_MAX : ((UINT64_C(1) << (bytes * 8)) - 1u);
            uint64_t expected = compare & mask, observed;
            interp_access_begin(address, bytes, 1);
            switch (bytes) {
            case 1: {
                uint8_t narrow = (uint8_t)expected;
                __atomic_compare_exchange_n((uint8_t *)pointer, &narrow, (uint8_t)swap, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
                observed = narrow;
                break;
            }
            case 2: {
                uint16_t narrow = (uint16_t)expected;
                __atomic_compare_exchange_n((uint16_t *)pointer, &narrow, (uint16_t)swap, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
                observed = narrow;
                break;
            }
            case 4: {
                uint32_t narrow = (uint32_t)expected;
                __atomic_compare_exchange_n((uint32_t *)pointer, &narrow, (uint32_t)swap, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
                observed = narrow;
                break;
            }
            default: {
                uint64_t wide = expected;
                __atomic_compare_exchange_n((uint64_t *)pointer, &wide, swap, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
                observed = wide;
                break;
            }
            }
            interp_access_end();
            // CAS returns the PRE-EXISTING value in Rs, whether or not the swap happened.
            if (bytes == 8)
                interp_set_gpr(cpu, rs, observed);
            else
                interp_set_gpr32(cpu, rs, (uint32_t)observed);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        if (o2) { // Load-acquire / store-release WITHOUT a monitor: LDAR / LDLAR / STLR / STLLR
            if (o1 || rs != 31 || rt2 != 31)
                return interp_undefined(cpu, insn, "loads and stores -- unallocated ordered-access encoding");
            uint64_t address = interp_gpr_sp(cpu, rn);
            // The host is x86-TSO, where loads already have acquire semantics and stores already have release
            // semantics, so these fences are compiled away into nothing on the host that matters. They are
            // written anyway rather than assumed: this file is the non-AArch64 backend generally, not the
            // x86-64 backend specifically, and on a weaker host they are load-bearing. The COMPILER also has
            // to be stopped from reordering the surrounding accesses, which a TSO argument does not cover.
            if (load) {
                uint64_t value = interp_load_bits(address, bytes);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (bytes == 8)
                    interp_set_gpr(cpu, rt, value);
                else
                    interp_set_gpr32(cpu, rt, (uint32_t)value);
            } else {
                uint64_t value = interp_gpr(cpu, rt);
                __atomic_thread_fence(__ATOMIC_RELEASE);
                interp_store_bits(address, value, bytes);
            }
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        uint64_t address = interp_gpr_sp(cpu, rn);

        // CASP / CASPA / CASPL / CASPAL. These share o1 == 1 with LDXP/STXP and are separated from them by
        // BIT 31, not by the size field: LDXP/STXP always have bit31 == 1 (their size is 0b10 or 0b11), while
        // CASP encodes bit31 == 0 with bit30 as its lone size bit. Testing only `size < 2` therefore rejects
        // every CASP as an unallocated pair size, which is what the ISA regression guest caught.
        if (o1 && !(insn & 0x80000000u)) {
            if (rt2 != 31) return interp_undefined(cpu, insn, "loads and stores -- unallocated CASP encoding");
            // Rs and Rt must both be even: each names the first of a register PAIR.
            if ((rs & 1) || (rt & 1))
                return interp_undefined(cpu, insn, "loads and stores -- CASP with an odd register pair");
            unsigned element = ((insn >> 30) & 1u) ? 8u : 4u; // bit30 selects a 32-bit or 64-bit pair
            unsigned total = element * 2u;
            void *pointer = interp_atomic_pointer(address, total);
            if (pointer == NULL) return interp_alignment_fault(cpu, address);
            uint64_t compare_low = interp_gpr(cpu, rs), compare_high = interp_gpr(cpu, rs + 1);
            uint64_t swap_low = interp_gpr(cpu, rt), swap_high = interp_gpr(cpu, rt + 1);
            uint64_t observed_low, observed_high;
            interp_access_begin(address, total, 1);
            if (element == 4) {
                // A 32-bit pair is one naturally-aligned 64-bit location; low-order register first, because the
                // guest is little-endian.
                uint64_t expected = (compare_low & 0xFFFFFFFFu) | ((compare_high & 0xFFFFFFFFu) << 32);
                uint64_t replacement = (swap_low & 0xFFFFFFFFu) | ((swap_high & 0xFFFFFFFFu) << 32);
                __atomic_compare_exchange_n((uint64_t *)pointer, &expected, replacement, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
                observed_low = expected & 0xFFFFFFFFu;
                observed_high = expected >> 32;
            } else {
                unsigned __int128 expected =
                    (unsigned __int128)compare_low | ((unsigned __int128)compare_high << 64);
                unsigned __int128 replacement = (unsigned __int128)swap_low | ((unsigned __int128)swap_high << 64);
                __atomic_compare_exchange_n((unsigned __int128 *)pointer, &expected, replacement, 0,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                observed_low = (uint64_t)expected;
                observed_high = (uint64_t)(expected >> 64);
            }
            interp_access_end();
            // Like CAS, CASP returns the PRE-EXISTING pair in the comparand registers whether or not it swapped.
            if (element == 8) {
                interp_set_gpr(cpu, rs, observed_low);
                interp_set_gpr(cpu, rs + 1, observed_high);
            } else {
                interp_set_gpr32(cpu, rs, (uint32_t)observed_low);
                interp_set_gpr32(cpu, rs + 1, (uint32_t)observed_high);
            }
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        // The genuine exclusive pair (LDXP/STXP), whose only allocated sizes are the 32-bit and 64-bit pairs.
        if (o1 && size < 2)
            return interp_undefined(cpu, insn, "loads and stores -- unallocated exclusive-pair size");
        unsigned access_bytes = o1 ? bytes * 2u : bytes;
        if (load) { // LDXR / LDAXR / LDXP / LDAXP
            if (rs != 31 || (!o1 && rt2 != 31))
                return interp_undefined(cpu, insn, "loads and stores -- unallocated load-exclusive encoding");
            if (interp_atomic_pointer(address, bytes) == NULL) return interp_alignment_fault(cpu, address);
            uint64_t first = interp_load_bits(address, bytes);
            uint64_t second = o1 ? interp_load_bits(address + bytes, bytes) : 0;
            if (o0) __atomic_thread_fence(__ATOMIC_ACQUIRE); // LDAXR/LDAXP
            // Arm the monitor with what we observed, which is what the store-exclusive will compare against.
            g_interp_monitor_address = address;
            g_interp_monitor_bytes = access_bytes;
            g_interp_monitor_value = first;
            g_interp_monitor_value2 = second;
            g_interp_monitor_valid = 1;
            if (bytes == 8) {
                interp_set_gpr(cpu, rt, first);
                if (o1) interp_set_gpr(cpu, rt2, second);
            } else {
                interp_set_gpr32(cpu, rt, (uint32_t)first);
                if (o1) interp_set_gpr32(cpu, rt2, (uint32_t)second);
            }
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        // STXR / STLXR / STXP / STLXP. Rs receives 0 on success, 1 on failure.
        if (!o1 && rt2 != 31)
            return interp_undefined(cpu, insn, "loads and stores -- unallocated store-exclusive encoding");
        void *pointer = interp_atomic_pointer(address, bytes);
        if (pointer == NULL) return interp_alignment_fault(cpu, address);
        unsigned failed = 1;
        if (g_interp_monitor_valid && g_interp_monitor_address == address &&
            g_interp_monitor_bytes == access_bytes) {
            uint64_t desired = interp_gpr(cpu, rt);
            if (o0) __atomic_thread_fence(__ATOMIC_RELEASE); // STLXR/STLXP
            interp_access_begin(address, access_bytes, 1);
            if (!o1) {
                // Single register: one compare-and-swap against the value LDXR observed.
                switch (bytes) {
                case 1: {
                    uint8_t expected = (uint8_t)g_interp_monitor_value;
                    failed = !__atomic_compare_exchange_n((uint8_t *)pointer, &expected, (uint8_t)desired, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    break;
                }
                case 2: {
                    uint16_t expected = (uint16_t)g_interp_monitor_value;
                    failed = !__atomic_compare_exchange_n((uint16_t *)pointer, &expected, (uint16_t)desired, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    break;
                }
                case 4: {
                    uint32_t expected = (uint32_t)g_interp_monitor_value;
                    failed = !__atomic_compare_exchange_n((uint32_t *)pointer, &expected, (uint32_t)desired, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    break;
                }
                default: {
                    uint64_t expected = g_interp_monitor_value;
                    failed = !__atomic_compare_exchange_n((uint64_t *)pointer, &expected, desired, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    break;
                }
                }
            } else {
                // STXP: a 128-bit (or 64-bit) pair must commit indivisibly. A 64-bit pair fits one 128-bit
                // compare-and-swap; a 32-bit pair fits one 64-bit compare-and-swap. __atomic on a 16-byte
                // object can lower to a libatomic lock rather than a real instruction on some hosts, which
                // would not be atomic against a peer using the 8-byte path -- but every access to a given
                // location by a given guest goes through this same code, so the lock is consistent with
                // itself. Correctness over cleverness; a native cmpxchg16b lowering is a later change.
                uint64_t desired2 = interp_gpr(cpu, rt2);
                if (bytes == 4) {
                    uint64_t expected = (g_interp_monitor_value & 0xFFFFFFFFu) |
                                        ((g_interp_monitor_value2 & 0xFFFFFFFFu) << 32);
                    uint64_t replacement = (desired & 0xFFFFFFFFu) | ((desired2 & 0xFFFFFFFFu) << 32);
                    failed = !__atomic_compare_exchange_n((uint64_t *)pointer, &expected, replacement, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                } else {
                    unsigned __int128 expected = (unsigned __int128)g_interp_monitor_value |
                                                 ((unsigned __int128)g_interp_monitor_value2 << 64);
                    unsigned __int128 replacement =
                        (unsigned __int128)desired | ((unsigned __int128)desired2 << 64);
                    failed = !__atomic_compare_exchange_n((unsigned __int128 *)pointer, &expected, replacement, 0,
                                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                }
            }
            interp_access_end();
        }
        // The monitor is cleared by ANY store-exclusive, successful or not: the architecture requires a
        // failed STXR to leave no armed monitor, or a retry loop could succeed without re-reading.
        interp_monitor_clear();
        interp_set_gpr32(cpu, rs, failed);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- Atomic memory operations (LSE): LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP ----
    // Encoding shares the register-offset box but is distinguished by bits[11:10] == 00.
    if ((insn & 0x3B200C00u) == 0x38200000u) {
        unsigned size = (insn >> 30) & 3, opc = (insn >> 12) & 7, o3 = (insn >> 15) & 1;
        int rs = rm;
        unsigned bytes = 1u << size;
        if (vector) return interp_undefined(cpu, insn, "loads and stores -- SIMD/FP atomic");
        uint64_t address = interp_gpr_sp(cpu, rn);
        void *pointer = interp_atomic_pointer(address, bytes);
        if (pointer == NULL) return interp_alignment_fault(cpu, address);
        uint64_t operand = interp_gpr(cpu, rs), old = 0;
        // Every one of these is a real host read-modify-write, not a load followed by a store: guest threads
        // are host threads, so an interleaved peer would otherwise lose an update. SEQ_CST covers all four
        // acquire/release combinations the A and R bits encode; on x86-TSO the stronger ordering costs nothing
        // beyond what the locked instruction already implies.
        interp_access_begin(address, bytes, 1);
#define INTERP_LSE_RMW(type, expression)                                                                        \
    do {                                                                                                        \
        type *slot = (type *)pointer;                                                                            \
        type argument = (type)operand;                                                                           \
        (void)argument;                                                                                          \
        old = (uint64_t)(expression);                                                                            \
    } while (0)
#define INTERP_LSE_WIDTHS(expression8, expression16, expression32, expression64)                                 \
    do {                                                                                                        \
        switch (bytes) {                                                                                        \
        case 1: INTERP_LSE_RMW(uint8_t, expression8); break;                                                     \
        case 2: INTERP_LSE_RMW(uint16_t, expression16); break;                                                    \
        case 4: INTERP_LSE_RMW(uint32_t, expression32); break;                                                    \
        default: INTERP_LSE_RMW(uint64_t, expression64); break;                                                   \
        }                                                                                                       \
    } while (0)
        if (o3) { // SWP
            if (opc != 0) {
                interp_access_end();
                return interp_undefined(cpu, insn, "loads and stores -- unallocated LSE swap/op3 encoding");
            }
            INTERP_LSE_WIDTHS(__atomic_exchange_n(slot, argument, __ATOMIC_SEQ_CST),
                              __atomic_exchange_n(slot, argument, __ATOMIC_SEQ_CST),
                              __atomic_exchange_n(slot, argument, __ATOMIC_SEQ_CST),
                              __atomic_exchange_n(slot, argument, __ATOMIC_SEQ_CST));
        } else {
            switch (opc) {
            case 0: // LDADD
                INTERP_LSE_WIDTHS(__atomic_fetch_add(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_add(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_add(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_add(slot, argument, __ATOMIC_SEQ_CST));
                break;
            case 1: // LDCLR: bit CLEAR, so the operand is complemented into an AND
                INTERP_LSE_WIDTHS(__atomic_fetch_and(slot, (uint8_t)~argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_and(slot, (uint16_t)~argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_and(slot, (uint32_t)~argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_and(slot, (uint64_t)~argument, __ATOMIC_SEQ_CST));
                break;
            case 2: // LDEOR
                INTERP_LSE_WIDTHS(__atomic_fetch_xor(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_xor(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_xor(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_xor(slot, argument, __ATOMIC_SEQ_CST));
                break;
            case 3: // LDSET
                INTERP_LSE_WIDTHS(__atomic_fetch_or(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_or(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_or(slot, argument, __ATOMIC_SEQ_CST),
                                  __atomic_fetch_or(slot, argument, __ATOMIC_SEQ_CST));
                break;
            case 4: // LDSMAX
            case 5: // LDSMIN
            case 6: // LDUMAX
            case 7: { // LDUMIN
                // There is no __atomic_fetch_max, so these are a compare-and-swap retry loop. The loop is what
                // makes them atomic against a peer guest thread: a load-compare-store would let another thread's
                // update land in between and be lost, which is precisely the bug these instructions exist to
                // avoid. The comparison happens at the ACCESS width and in the right signedness, so the sign
                // extension for the signed forms is part of the type the macro instantiates.
                unsigned want_max = opc == 4 || opc == 6;
                unsigned is_signed = opc < 6;
#define INTERP_LSE_MINMAX(type, signed_type)                                                                     \
    do {                                                                                                        \
        type *slot = (type *)pointer;                                                                            \
        type argument = (type)operand;                                                                           \
        type current = __atomic_load_n(slot, __ATOMIC_SEQ_CST);                                                  \
        for (;;) {                                                                                              \
            int argument_greater = is_signed ? ((signed_type)argument > (signed_type)current)                     \
                                             : (argument > current);                                             \
            type chosen = (argument_greater == (int)(want_max != 0)) ? argument : current;                        \
            /* Already correct: nothing to store, and `current` is still the pre-existing value to return. */    \
            if (chosen == current) break;                                                                       \
            if (__atomic_compare_exchange_n(slot, &current, chosen, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))       \
                break;                                                                                          \
            /* A peer won the race; `current` now holds its value, so re-decide against that. */                 \
        }                                                                                                       \
        old = (uint64_t)current;                                                                                 \
    } while (0)
                switch (bytes) {
                case 1: INTERP_LSE_MINMAX(uint8_t, int8_t); break;
                case 2: INTERP_LSE_MINMAX(uint16_t, int16_t); break;
                case 4: INTERP_LSE_MINMAX(uint32_t, int32_t); break;
                default: INTERP_LSE_MINMAX(uint64_t, int64_t); break;
                }
#undef INTERP_LSE_MINMAX
                break;
            }
            default:
                interp_access_end();
                return interp_undefined(cpu, insn, "loads and stores -- unallocated LSE atomic opcode");
            }
        }
#undef INTERP_LSE_WIDTHS
#undef INTERP_LSE_RMW
        interp_access_end();
        // Rt receives the PRE-operation value. Rt == 31 makes it the ST<op> alias, which discards it.
        if (bytes == 8)
            interp_set_gpr(cpu, rt, old);
        else
            interp_set_gpr32(cpu, rt, (uint32_t)old);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- The three single-register integer addressing modes ----
    // All three share the size/opc field layout, so decode the operation once and the address per mode.
    //   opc == 0            store of (1 << size) bytes
    //   opc == 1            zero-extending load
    //   opc == 2            sign-extending load into a 64-bit register (or PRFM when size == 3)
    //   opc == 3            sign-extending load into a 32-bit register
    unsigned size = (insn >> 30) & 3;
    unsigned opc = (insn >> 22) & 3;
    int scaled = (insn & 0x3B000000u) == 0x39000000u;
    int register_offset = (insn & 0x3B200C00u) == 0x38200800u;
    int unscaled = (insn & 0x3B200000u) == 0x38000000u;
    if (!scaled && !register_offset && !unscaled)
        return interp_undefined(cpu, insn, "loads and stores -- AdvSIMD structure or unallocated encoding");
    if (!vector && ((insn & 0x3B200C00u) == 0x38200400u || (insn & 0x3B200C00u) == 0x38200C00u))
        return interp_undefined(cpu, insn, "loads and stores -- LDRAA/LDRAB (pointer authentication)");

    // A SIMD&FP access shares all three addressing modes with the integer one; only the transfer width and the
    // load/store test differ (opc<1>:size gives the width, opc<0> selects load).
    unsigned bytes = vector ? interp_simd_access_bytes(size, opc) : (1u << size);
    if (vector && bytes == 0)
        return interp_undefined(cpu, insn, "loads and stores -- unallocated SIMD/FP access size");
    unsigned scale = vector ? (opc & 2u ? 4u : size) : size;
    uint64_t base = interp_gpr_sp(cpu, rn);
    uint64_t address;
    int writeback = 0;
    uint64_t writeback_value = 0;
    if (scaled) {
        address = base + (((uint64_t)((insn >> 10) & 0xFFFu)) << scale);
    } else if (register_offset) {
        unsigned option = (insn >> 13) & 7, s = (insn >> 12) & 1;
        // S selects whether the index is scaled by the access size; option selects the index width and
        // signedness. Only 010/011/110/111 (UXTW/LSL/SXTW/SXTX) are allocated here.
        if ((option & 3u) < 2u)
            return interp_undefined(cpu, insn, "loads and stores -- unallocated register-offset extend option");
        address = base + interp_extend_operand(cpu, rm, option, s ? scale : 0u, 1);
    } else {
        unsigned mode = (insn >> 10) & 3;
        int64_t offset = interp_sext((insn >> 12) & 0x1FFu, 9);
        // mode 0 = LDUR/STUR (unscaled signed offset), 1 = post-index, 2 = LDTR/STTR (an "unprivileged"
        // access, which at EL0 is an ordinary one, so it behaves exactly like mode 0), 3 = pre-index.
        writeback = mode == 1 || mode == 3;
        writeback_value = base + (uint64_t)offset;
        address = mode == 1 ? base : base + (uint64_t)offset;
    }

    if (vector) {
        if (opc & 1u)
            interp_vec_load(cpu, rt, address, bytes);
        else
            interp_vec_store(cpu, rt, address, bytes);
    } else if (opc == 0) { // store
        uint64_t value = interp_gpr(cpu, rt); // read the source before the access; see the ordering note above
        interp_store_bits(address, value, bytes);
    } else if (opc == 2 && size == 3) { // PRFM / PRFUM: a hint with no architectural effect here
        (void)0;
    } else if (opc == 1) { // zero-extending load
        uint64_t value = interp_load_bits(address, bytes);
        if (size == 3)
            interp_set_gpr(cpu, rt, value);
        else
            interp_set_gpr32(cpu, rt, (uint32_t)value); // a 32-bit destination zero-extends to 64 anyway
    } else { // sign-extending load: LDRSB / LDRSH / LDRSW
        if (size == 3 || (size == 2 && opc == 3))
            return interp_undefined(cpu, insn, "loads and stores -- unallocated sign-extending load size");
        uint64_t value = (uint64_t)interp_sext(interp_load_bits(address, bytes), bytes * 8u);
        if (opc == 2) // 64-bit destination
            interp_set_gpr(cpu, rt, value);
        else // 32-bit destination: sign-extend within 32 bits, then zero-extend to 64
            interp_set_gpr32(cpu, rt, (uint32_t)value);
    }
    if (writeback) interp_set_gpr_sp(cpu, rn, writeback_value);
    cpu->pc = gpc + 4;
    return INTERP_NEXT;
}

// ---------------------------------------------------------------------------
// Floating point: FPCR/FPSR, and how a guest FP result is actually computed.
// ---------------------------------------------------------------------------
// THE DECISION, AND WHY: PROJECT FPCR ONTO THE HOST FP ENVIRONMENT AND LET THE HOST FP UNIT COMPUTE.
//
// Two designs were available, and the choice is not obvious enough to leave implicit.
//   (a) Model IEEE-754 in software: unpack, compute on integers, round, repack. Host-independent by
//       construction, and about a thousand lines whose every rounding decision is mine to get wrong.
//   (b) Observe that the host FP unit is ALSO IEEE-754, with the same four rounding modes and the same five
//       exceptions, install the guest's FPCR.RMode into the host's rounding control around each operation,
//       let the hardware compute, and harvest the raised exceptions back into FPSR.
//
// This file does (b), for three reasons:
//
//   * For FADD/FSUB/FMUL/FDIV/FSQRT/FMADD and the conversions between single and double, an x86-64 SSE
//     operation and an AArch64 FP operation ARE THE SAME FUNCTION: both are defined as the correctly-rounded
//     IEEE-754 result under the active rounding mode, so they agree bit for bit. Bit-exactness then comes
//     from silicon rather than from my arithmetic, which matters because a rounding bug in a hand-written
//     soft-float surfaces as a one-digit difference in printf output arbitrarily far from its cause -- the
//     single hardest class of defect to localise in this corpus.
//   * The engine already does this projection in the mirror direction and it is proven there.
//     guest/x86_64/x87state.c maps a guest MXCSR onto the host AArch64 FPCR/FPSR field by field, including
//     the rounding-mode permutation and the five exception bits. This is that same map read right to left.
//   * It is far less code, and the code it does still need (interp_fp_pack below) is code a soft-float would
//     have needed anyway.
//
// The projection is spelled with <fenv.h> rather than with STMXCSR/LDMXCSR. fenv is the portable name for
// exactly the register this needs -- glibc implements fesetround/fetestexcept on x86-64 over MXCSR and on
// AArch64 over FPCR/FPSR -- and this file is the AArch64 frontend for EVERY non-AArch64 host, so an
// x86-specific spelling would have to be paired with a fallback that no build compiles and nothing tests.
// guest/x86_64/avx.c already reaches for fegetround() in its own non-AArch64 arm for the same reason.
//
// WHERE THE TWO ISAs GENUINELY DISAGREE, AND SO WHERE THE HOST IS NOT ASKED.
// The projection is sound only for the parts that agree. These do not, and each is computed here instead:
//
//   * NaN propagation. AArch64's FPProcessNaNs prefers a SIGNALLING operand over a quiet one regardless of
//     position (op1 SNaN, then op2 SNaN, then op1 QNaN, then op2 QNaN); x86 SSE prefers the FIRST operand
//     regardless of whether it signals. So (QNaN_a op SNaN_b) differs. Every operation below therefore
//     screens its operands for NaN FIRST and produces the result itself; the host only ever sees non-NaNs.
//   * The default NaN. When an invalid operation has no NaN operand at all (0/0, inf-inf, 0*inf, sqrt of a
//     negative), AArch64 produces FPDefaultNaN, whose SIGN BIT IS CLEAR (0x7ff8000000000000 for a double).
//     x86 produces the "QNaN indefinite", whose sign bit is SET. interp_fp_postprocess replaces any NaN the
//     host manufactured out of non-NaN inputs rather than forwarding it.
//   * FPCR.DN (default-NaN mode) and FPCR.FZ (flush-to-zero) have no portable host spelling at all -- x86
//     has no DN, and splits FZ into separate output (FZ) and input (DAZ) bits -- so both are modelled here.
//     Linux leaves both clear, which is why nothing in the corpus notices either way.
//   * FMIN/FMAX and FMINNM/FMAXNM. AArch64's FMAX propagates NaNs and defines max(+0,-0) = +0 by ANDing the
//     operand signs; x86's MAXSD returns its SECOND operand for any NaN and for either zero. Different
//     functions, so computed here.
//   * The integer conversions. FCVTZS/FCVTZU SATURATE to the destination range and raise Invalid; x86's
//     CVTTSD2SI produces the integer indefinite instead. And x86-64 has no baseline instruction for
//     unsigned-64 <-> floating point in either direction (the compiler's open-coded sequence for it is only
//     correctly rounded in round-to-nearest). Both directions therefore go through interp_fp_pack /
//     interp_fp_to_int, which also yields the fixed-point forms (a scale is just an addend on the exponent)
//     and all five rounding variants FCVT{N,A,P,M,Z} for free.
//   * Half precision. F16C is not baseline on x86-64 and a `_Float16` cast is round-to-nearest whatever the
//     FP environment says -- the same conclusion guest/x86_64/avx.c reached from the other side, see
//     avx_f32_to_f16_software. Software here too, sharing interp_fp_pack.
//
// ONE KNOWN, BOUNDED DIVERGENCE, stated rather than hidden. Tininess detection: AArch64 decides "underflow"
// from the magnitude of the value BEFORE rounding, x86-64 decides it AFTER. IEEE-754 explicitly permits
// either, so the two disagree for exactly one class of value -- one whose exact magnitude lies below the
// smallest normal but which rounds up to exactly the smallest normal. The RESULT is identical either way;
// only FPSR.UFC differs, and only for that value, and only for a host-computed operation (interp_fp_pack,
// which does the conversions, implements AArch64's rule). Closing it would need a third format wider than
// double, which is not available portably.
//
// PERFORMANCE. Each guest FP instruction costs an fegetround/fesetround/feclearexcept/fetestexcept bracket
// on top of the decode. That is several times the cost of the arithmetic and it is the right trade for a
// backend whose stated goal is correctness; the bracket also RESTORES the host rounding mode on the way out,
// so the guest's FP environment can never leak into the engine's own C.

// The three formats, ordered so that (fmt + 1) is the AdvSIMD element-size code the vector accessors take.
#define INTERP_FP_H 0u
#define INTERP_FP_S 1u
#define INTERP_FP_D 2u

static unsigned interp_fp_width(unsigned fmt) {
    return fmt == INTERP_FP_H ? 16u : (fmt == INTERP_FP_S ? 32u : 64u);
}

static unsigned interp_fp_mant(unsigned fmt) {
    return fmt == INTERP_FP_H ? 10u : (fmt == INTERP_FP_S ? 23u : 52u);
}

static int interp_fp_bias(unsigned fmt) {
    return fmt == INTERP_FP_H ? 15 : (fmt == INTERP_FP_S ? 127 : 1023);
}

static unsigned interp_fp_inf_exp(unsigned fmt) {
    return fmt == INTERP_FP_H ? 0x1Fu : (fmt == INTERP_FP_S ? 0xFFu : 0x7FFu);
}

static uint64_t interp_fp_mant_mask(unsigned fmt) {
    return (UINT64_C(1) << interp_fp_mant(fmt)) - 1u;
}

static uint64_t interp_fp_sign_mask(unsigned fmt) {
    return UINT64_C(1) << (interp_fp_width(fmt) - 1u);
}

// The ptype field of the scalar-FP encodings. 10 is unallocated in every box that uses it as a format (the
// one encoding that spells 10 there is FMOV to/from Vn.D[1], which is not a format-carrying form).
static int interp_fp_type_fmt(unsigned type, unsigned *fmt) {
    switch (type) {
    case 0: *fmt = INTERP_FP_S; return 1;
    case 1: *fmt = INTERP_FP_D; return 1;
    case 3: *fmt = INTERP_FP_H; return 1;
    default: return 0;
    }
}

// FPUnpack's classification. QNAN/SNAN are last so a "is this a NaN" test is one comparison.
#define INTERP_FPC_ZERO 0u
#define INTERP_FPC_DENORM 1u
#define INTERP_FPC_NORM 2u
#define INTERP_FPC_INF 3u
#define INTERP_FPC_QNAN 4u
#define INTERP_FPC_SNAN 5u

static unsigned interp_fp_class(uint64_t bits, unsigned fmt) {
    unsigned mant = interp_fp_mant(fmt);
    uint64_t frac = bits & interp_fp_mant_mask(fmt);
    unsigned biased = (unsigned)((bits >> mant) & (uint64_t)interp_fp_inf_exp(fmt));
    if (biased == 0) return frac == 0 ? INTERP_FPC_ZERO : INTERP_FPC_DENORM;
    if (biased != interp_fp_inf_exp(fmt)) return INTERP_FPC_NORM;
    if (frac == 0) return INTERP_FPC_INF;
    // The quiet bit is the MOST significant fraction bit. Its being set is what makes a NaN quiet, and it is
    // also what a signalling NaN must have OR-ed in when the architecture "quiets" it.
    return ((frac >> (mant - 1u)) & 1u) ? INTERP_FPC_QNAN : INTERP_FPC_SNAN;
}

// Rounding modes. The first four are FPCR.RMode verbatim; RA (ties away from zero) is not an FPCR encoding at
// all -- it is the mode FCVTA*/FRINTA name directly -- so it lives beyond the 2-bit field.
#define INTERP_RM_RN 0u // to nearest, ties to even
#define INTERP_RM_RP 1u // toward +infinity
#define INTERP_RM_RM 2u // toward -infinity
#define INTERP_RM_RZ 3u // toward zero
#define INTERP_RM_RA 4u // to nearest, ties away from zero

static int interp_fp_host_round(unsigned rmode) {
    switch (rmode) {
    case INTERP_RM_RP: return FE_UPWARD;
    case INTERP_RM_RM: return FE_DOWNWARD;
    case INTERP_RM_RZ: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
    }
}

// Given the bits a rounding step is about to discard, does `rmode` move the magnitude away from zero?
// `lsb` is the least significant retained bit, which only round-to-nearest-even consults.
static int interp_fp_round_away(unsigned rmode, unsigned sign, int round_bit, int sticky, unsigned lsb) {
    switch (rmode) {
    case INTERP_RM_RN: return round_bit && (sticky || lsb);
    case INTERP_RM_RA: return round_bit != 0;
    case INTERP_RM_RP: return !sign && (round_bit || sticky);
    case INTERP_RM_RM: return sign && (round_bit || sticky);
    default: return 0; // RZ truncates
    }
}

// The host FP-environment bracket. Enter installs the guest's rounding mode and clears the host's cumulative
// exception flags; leave reads them back, maps them onto FPSR bit positions and RESTORES the host's mode.
//
// Restoring matters: the engine's own C runs between guest instructions (the dispatcher, syscall service, the
// whole of linux_abi) and must not silently inherit a guest's round-toward-zero. Clearing the host's sticky
// flags is safe because nothing in this tree reads fetestexcept outside this bracket.
//
// The empty asm memory barriers and the volatile operands in the callers below exist because GCC compiles
// with FENV_ACCESS effectively OFF: it does not know that fesetround changes the meaning of a subsequent FP
// operation and is free to hoist one out of the bracket. avx.c states the same hazard for the same reason.
typedef struct {
    int host_round;
} interp_fpenv;

static void interp_fp_env_enter(interp_fpenv *env) {
    int want = interp_fp_host_round(INTERP_FPCR_RMODE(g_interp_fpcr));
    env->host_round = fegetround();
    if (env->host_round != want) (void)fesetround(want);
    (void)feclearexcept(FE_ALL_EXCEPT);
    __asm__ __volatile__("" ::: "memory");
}

static unsigned interp_fp_env_leave(interp_fpenv *env) {
    __asm__ __volatile__("" ::: "memory");
    int raised = fetestexcept(FE_ALL_EXCEPT);
    if (env->host_round != interp_fp_host_round(INTERP_FPCR_RMODE(g_interp_fpcr)))
        (void)fesetround(env->host_round);
    unsigned bits = 0;
    if (raised & FE_INVALID) bits |= INTERP_FPSR_IOC;
    if (raised & FE_DIVBYZERO) bits |= INTERP_FPSR_DZC;
    if (raised & FE_OVERFLOW) bits |= INTERP_FPSR_OFC;
    if (raised & FE_UNDERFLOW) bits |= INTERP_FPSR_UFC;
    if (raised & FE_INEXACT) bits |= INTERP_FPSR_IXC;
    return bits;
}

// Raw bits <-> host types. memcpy rather than a union or a cast for the same reason the guest memory
// accessors use it: type-punning through a pointer is undefined behaviour the compiler may assume away.
static double interp_fp_to_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint64_t interp_fp_from_double(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float interp_fp_to_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint64_t interp_fp_from_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return (uint64_t)bits;
}

// Half -> double, exactly, by re-encoding rather than by arithmetic (so it raises nothing and needs no
// bracket). A half denormal becomes a double NORMAL, which is why the leading one has to be found and the
// exponent walked down -- the same renormalisation FPUnpack does.
static double interp_fp_half_to_double(uint64_t bits) {
    uint64_t sign = (bits & 0x8000u) ? (UINT64_C(1) << 63) : 0;
    unsigned biased = (unsigned)((bits >> 10) & 0x1Fu);
    uint64_t frac = bits & 0x3FFu;
    if (biased == 0x1Fu) return interp_fp_to_double(sign | (UINT64_C(0x7FF) << 52) | (frac << 42));
    if (biased == 0) {
        if (frac == 0) return interp_fp_to_double(sign);
        int exponent = 1 - 15;
        while (!(frac & 0x400u)) {
            frac <<= 1;
            exponent--;
        }
        frac &= 0x3FFu;
        return interp_fp_to_double(sign | ((uint64_t)(exponent + 1023) << 52) | (frac << 42));
    }
    return interp_fp_to_double(sign | ((uint64_t)((int)biased - 15 + 1023) << 52) | (frac << 42));
}

// Every value of every format handled here widens to a host double EXACTLY: binary16 and binary32 are
// subsets of binary64 in both precision and exponent range. That is what lets the comparisons -- and the
// half-precision arithmetic -- borrow the host's double unit without adding a rounding step of their own.
// Callers must have screened NaNs first: a float -> double conversion of a signalling NaN quiets it.
static double interp_fp_widen(uint64_t bits, unsigned fmt) {
    if (fmt == INTERP_FP_D) return interp_fp_to_double(bits);
    if (fmt == INTERP_FP_S) return (double)interp_fp_to_float((uint32_t)bits);
    return interp_fp_half_to_double(bits);
}

static uint64_t interp_fp_default_nan(unsigned fmt) {
    unsigned mant = interp_fp_mant(fmt);
    return ((uint64_t)interp_fp_inf_exp(fmt) << mant) | (UINT64_C(1) << (mant - 1u));
}

// FPProcessNaN: quiet the operand (or answer the default NaN under FPCR.DN), raising Invalid if it signalled.
static uint64_t interp_fp_process_nan(uint64_t bits, unsigned fmt) {
    if (interp_fp_class(bits, fmt) == INTERP_FPC_SNAN) interp_fpsr_raise(INTERP_FPSR_IOC);
    if (INTERP_FPCR_DN(g_interp_fpcr)) return interp_fp_default_nan(fmt);
    return bits | (UINT64_C(1) << (interp_fp_mant(fmt) - 1u));
}

// FPProcessNaNs / FPProcessNaNs3, as one loop. The architectural order is every SIGNALLING operand first, in
// operand order, and only then every quiet one -- which is where AArch64 parts company with x86, whose binary
// operations simply prefer the first source. Returns 1 (and *result) when an operand was a NaN.
static int interp_fp_process_nans(unsigned fmt, unsigned count, const uint64_t *operands, uint64_t *result) {
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned index = 0; index < count; index++) {
            unsigned cls = interp_fp_class(operands[index], fmt);
            if (cls == (pass == 0 ? INTERP_FPC_SNAN : INTERP_FPC_QNAN)) {
                *result = interp_fp_process_nan(operands[index], fmt);
                return 1;
            }
        }
    return 0;
}

// FPUnpack's flush-to-zero step, applied to an INPUT operand. FPCR.FZ governs single and double; half has its
// own FPCR.FZ16, because a half denormal is representable in every wider format and flushing it is a separate
// decision. A flush is what sets FPSR.IDC -- the bit means "an input was denormal AND was discarded", not
// merely "an input was denormal", which is why nothing sets it when FZ is clear.
static uint64_t interp_fp_flush_input(uint64_t bits, unsigned fmt) {
    unsigned flush = fmt == INTERP_FP_H ? INTERP_FPCR_FZ16(g_interp_fpcr) : INTERP_FPCR_FZ(g_interp_fpcr);
    if (!flush || interp_fp_class(bits, fmt) != INTERP_FPC_DENORM) return bits;
    interp_fpsr_raise(INTERP_FPSR_IDC);
    return bits & interp_fp_sign_mask(fmt);
}

// The common tail of every FP result, whether the host computed it or interp_fp_pack did. Two jobs:
// substitute the architectural default NaN for whatever NaN encoding the host invented, and apply FPCR.FZ to
// a denormal result. Then commit the accumulated exception bits to FPSR.
static uint64_t interp_fp_postprocess(unsigned fmt, uint64_t bits, unsigned raised) {
    unsigned cls = interp_fp_class(bits, fmt);
    if (cls >= INTERP_FPC_QNAN) {
        // Reachable only when the operands held no NaN (they are screened before the host is asked), i.e. this
        // is an invalid operation: 0/0, inf-inf, 0*inf, sqrt of a negative. AArch64 answers FPDefaultNaN;
        // x86-64's QNaN indefinite has the sign bit set and another host could pick a third encoding, so the
        // value is replaced rather than forwarded. The Invalid flag itself came back from the host in `raised`.
        bits = interp_fp_default_nan(fmt);
    } else if (cls == INTERP_FPC_DENORM) {
        unsigned flush = fmt == INTERP_FP_H ? INTERP_FPCR_FZ16(g_interp_fpcr) : INTERP_FPCR_FZ(g_interp_fpcr);
        if (flush) {
            bits &= interp_fp_sign_mask(fmt);
            raised |= INTERP_FPSR_UFC;
        }
    }
    interp_fpsr_raise(raised);
    return bits;
}

// Round the exact value (-1)^sign * significand * 2^exponent into `fmt` under `rmode`, and return its
// encoding. OFC/UFC/IXC are OR-ed into *raised, which is never cleared, so a caller can accumulate.
//
// This is the file's only soft-float rounder, and it exists because three things cannot be delegated to the
// host: an unsigned 64-bit integer -> floating point conversion (x86-64 has no baseline instruction and the
// compiler's open-coded sequence is only correct in round-to-nearest); a conversion to half precision (F16C
// is not baseline and a _Float16 cast ignores the rounding mode); and the fixed-point conversion forms, whose
// scale factor is an exponent addend that no host instruction can apply without a second rounding.
//
// Unlike the host-computed paths it implements AArch64's BEFORE-rounding tininess rule exactly: `lsb_exp` is
// clamped to the subnormal ulp using the value's own exponent, so a quantity that is tiny before rounding
// raises Underflow even when it rounds up to the smallest normal.
static uint64_t interp_fp_pack(unsigned sign, uint64_t significand, int exponent, unsigned fmt, unsigned rmode,
                               unsigned *raised) {
    unsigned mant = interp_fp_mant(fmt);
    uint64_t sign_bit = sign ? interp_fp_sign_mask(fmt) : 0;
    if (significand == 0) return sign_bit; // an exact zero keeps its sign and raises nothing

    // The exponent of the value's leading one, then the exponent of the least significant bit the result can
    // hold: one ulp of a normal result at this magnitude, or the fixed ulp of the subnormal range if coarser.
    int value_exp = exponent + (63 - __builtin_clzll(significand));
    int min_exp = 1 - interp_fp_bias(fmt) - (int)mant; // exponent of the smallest subnormal's last bit
    int lsb_exp = value_exp - (int)mant;
    int tiny = lsb_exp < min_exp;
    if (tiny) lsb_exp = min_exp;

    // Shift the significand down onto that ulp, keeping the round bit and a sticky bit. A shift of 64 or more
    // is reachable only in the clamped (tiny) case, where the whole significand is below the result's ulp.
    int shift = lsb_exp - exponent;
    uint64_t quotient;
    int round_bit = 0, sticky = 0;
    if (shift <= 0) {
        // A left shift here cannot overflow: shift is (leading-one position - mant) when unclamped, so the
        // leading one lands exactly at bit `mant`, and in the clamped case it lands strictly below it.
        quotient = significand << (unsigned)(-shift);
    } else if (shift >= 64) {
        quotient = 0;
        round_bit = shift == 64 ? (int)((significand >> 63) & 1u) : 0;
        sticky = (significand & (shift == 64 ? ~(UINT64_C(1) << 63) : UINT64_MAX)) != 0;
    } else {
        quotient = significand >> (unsigned)shift;
        round_bit = (int)((significand >> (unsigned)(shift - 1)) & 1u);
        sticky = shift > 1 && (significand & ((UINT64_C(1) << (unsigned)(shift - 1)) - 1u)) != 0;
    }
    int inexact = round_bit | sticky;
    if (interp_fp_round_away(rmode, sign, round_bit, sticky, (unsigned)(quotient & 1u))) quotient++;
    if (inexact) *raised |= INTERP_FPSR_IXC;

    if ((quotient >> mant) == 0) {
        // Subnormal (or zero): the biased exponent is 0 and the stored fraction IS the quotient, so nothing
        // has to be assembled. Underflow is raised on the architecture's before-rounding test.
        if (tiny && inexact) *raised |= INTERP_FPSR_UFC;
        return sign_bit | quotient;
    }
    // A round-up can carry out of the significand (1.111..1 -> 10.000..0), moving the result into the next
    // binade. Note that the subnormal branch above needs no equivalent: there, a carry into bit `mant` lands
    // exactly on the encoding of the smallest normal (biased exponent 1, zero fraction) all by itself.
    if (quotient >> (mant + 1u)) {
        quotient >>= 1;
        lsb_exp++;
    }
    if (tiny && inexact) *raised |= INTERP_FPSR_UFC; // tiny before rounding, normal after: still Underflow
    int biased = lsb_exp + (int)mant + interp_fp_bias(fmt);
    if (biased >= (int)interp_fp_inf_exp(fmt)) {
        // Overflow. IEEE-754 gives an infinity when the active mode rounds AWAY from zero at this sign and the
        // largest finite otherwise, and always raises Inexact alongside Overflow.
        *raised |= INTERP_FPSR_OFC | INTERP_FPSR_IXC;
        int away = rmode == INTERP_RM_RN || rmode == INTERP_RM_RA || (rmode == INTERP_RM_RP && !sign) ||
                   (rmode == INTERP_RM_RM && sign);
        if (away) return sign_bit | ((uint64_t)interp_fp_inf_exp(fmt) << mant);
        return sign_bit | ((uint64_t)(interp_fp_inf_exp(fmt) - 1u) << mant) | interp_fp_mant_mask(fmt);
    }
    return sign_bit | ((uint64_t)(unsigned)biased << mant) | (quotient & interp_fp_mant_mask(fmt));
}

// A host double narrowed to half. NaNs are handled by the callers (they never reach here through the
// arithmetic paths); an infinity converts exactly.
static uint64_t interp_fp_half_from_double(double value, unsigned rmode, unsigned *raised) {
    uint64_t bits = interp_fp_from_double(value);
    unsigned sign = (unsigned)(bits >> 63);
    unsigned biased = (unsigned)((bits >> 52) & 0x7FFu);
    uint64_t frac = bits & ((UINT64_C(1) << 52) - 1u);
    if (biased == 0x7FFu)
        return frac == 0 ? (sign ? 0xFC00u : 0x7C00u) : 0x7E00u; // postprocess substitutes the default NaN
    if (biased == 0 && frac == 0) return sign ? 0x8000u : 0u;
    uint64_t significand = biased == 0 ? frac : (frac | (UINT64_C(1) << 52));
    int exponent = (int)(biased == 0 ? 1u : biased) - 1023 - 52;
    return interp_fp_pack(sign, significand, exponent, INTERP_FP_H, rmode, raised);
}

// ---- the arithmetic ----
#define INTERP_FPOP_ADD 0u
#define INTERP_FPOP_SUB 1u
#define INTERP_FPOP_MUL 2u
#define INTERP_FPOP_DIV 3u

// FADD/FSUB/FMUL/FDIV. Half precision is computed in the host's DOUBLE unit and then rounded to half, which
// is exact rather than a double-rounding hazard: binary64 carries 53 bits of significand and the classical
// bound for a single arithmetic operation to survive a second rounding is 2p+2 == 24 bits, with binary16's
// whole exponent range sitting far inside binary64's.
static uint64_t interp_fp_arith(unsigned fmt, unsigned op, uint64_t a, uint64_t b) {
    a = interp_fp_flush_input(a, fmt);
    b = interp_fp_flush_input(b, fmt);
    uint64_t operands[2] = {a, b}, nan;
    if (interp_fp_process_nans(fmt, 2, operands, &nan)) return nan;
    interp_fpenv env;
    unsigned raised;
    uint64_t out;
    if (fmt == INTERP_FP_S) {
        float left = interp_fp_to_float((uint32_t)a), right = interp_fp_to_float((uint32_t)b);
        interp_fp_env_enter(&env);
        volatile float x = left, y = right, r = 0;
        switch (op) {
        case INTERP_FPOP_ADD: r = x + y; break;
        case INTERP_FPOP_SUB: r = x - y; break;
        case INTERP_FPOP_MUL: r = x * y; break;
        default: r = x / y; break;
        }
        raised = interp_fp_env_leave(&env);
        out = interp_fp_from_float(r);
    } else {
        double left = interp_fp_widen(a, fmt), right = interp_fp_widen(b, fmt);
        interp_fp_env_enter(&env);
        volatile double x = left, y = right, r = 0;
        switch (op) {
        case INTERP_FPOP_ADD: r = x + y; break;
        case INTERP_FPOP_SUB: r = x - y; break;
        case INTERP_FPOP_MUL: r = x * y; break;
        default: r = x / y; break;
        }
        raised = interp_fp_env_leave(&env);
        out = fmt == INTERP_FP_D ? interp_fp_from_double(r)
                                 : interp_fp_half_from_double(r, INTERP_FPCR_RMODE(g_interp_fpcr), &raised);
    }
    return interp_fp_postprocess(fmt, out, raised);
}

static uint64_t interp_fp_sqrt(unsigned fmt, uint64_t a) {
    a = interp_fp_flush_input(a, fmt);
    uint64_t nan;
    if (interp_fp_process_nans(fmt, 1, &a, &nan)) return nan;
    interp_fpenv env;
    unsigned raised;
    uint64_t out;
    if (fmt == INTERP_FP_S) {
        float operand = interp_fp_to_float((uint32_t)a);
        interp_fp_env_enter(&env);
        volatile float x = operand, r;
        r = sqrtf(x);
        raised = interp_fp_env_leave(&env);
        out = interp_fp_from_float(r);
    } else {
        double operand = interp_fp_widen(a, fmt);
        interp_fp_env_enter(&env);
        volatile double x = operand, r;
        r = sqrt(x);
        raised = interp_fp_env_leave(&env);
        out = fmt == INTERP_FP_D ? interp_fp_from_double(r)
                                 : interp_fp_half_from_double(r, INTERP_FPCR_RMODE(g_interp_fpcr), &raised);
    }
    return interp_fp_postprocess(fmt, out, raised);
}

// FPMulAdd: addend + a*b with a SINGLE rounding, which is what C's fma() is defined to be. On a host with
// FMA3 glibc's ifunc resolves to the hardware instruction and the projection is exact; on a host without it,
// glibc's software path is still correctly rounded under the active mode. Using `x*y + z` instead would round
// twice and is a different function.
static uint64_t interp_fp_muladd(unsigned fmt, uint64_t addend, uint64_t a, uint64_t b) {
    addend = interp_fp_flush_input(addend, fmt);
    a = interp_fp_flush_input(a, fmt);
    b = interp_fp_flush_input(b, fmt);
    unsigned class_a = interp_fp_class(a, fmt), class_b = interp_fp_class(b, fmt);
    int zero_times_inf = (class_a == INTERP_FPC_INF && class_b == INTERP_FPC_ZERO) ||
                         (class_a == INTERP_FPC_ZERO && class_b == INTERP_FPC_INF);
    uint64_t operands[3] = {addend, a, b}, nan;
    int have_nan = interp_fp_process_nans(fmt, 3, operands, &nan);
    // FPMulAdd's one asymmetry, and it is deliberate in the architecture: a QUIET NaN addend does NOT win over
    // an invalid multiply. inf*0 with a quiet-NaN addend is Invalid and answers the default NaN, where plain
    // NaN propagation would have forwarded the addend's payload. A SIGNALLING addend still wins, which is why
    // this is tested after the propagation rule rather than before it.
    if (zero_times_inf && interp_fp_class(addend, fmt) == INTERP_FPC_QNAN) {
        interp_fpsr_raise(INTERP_FPSR_IOC);
        return interp_fp_default_nan(fmt);
    }
    if (have_nan) return nan;
    interp_fpenv env;
    unsigned raised;
    uint64_t out;
    if (fmt == INTERP_FP_S) {
        float left = interp_fp_to_float((uint32_t)a), right = interp_fp_to_float((uint32_t)b),
              extra = interp_fp_to_float((uint32_t)addend);
        interp_fp_env_enter(&env);
        volatile float x = left, y = right, z = extra, r;
        r = fmaf(x, y, z);
        raised = interp_fp_env_leave(&env);
        out = interp_fp_from_float(r);
    } else {
        double left = interp_fp_widen(a, fmt), right = interp_fp_widen(b, fmt), extra = interp_fp_widen(addend, fmt);
        interp_fp_env_enter(&env);
        volatile double x = left, y = right, z = extra, r;
        r = fma(x, y, z);
        raised = interp_fp_env_leave(&env);
        out = fmt == INTERP_FP_D ? interp_fp_from_double(r)
                                 : interp_fp_half_from_double(r, INTERP_FPCR_RMODE(g_interp_fpcr), &raised);
    }
    return interp_fp_postprocess(fmt, out, raised);
}

// FPMax/FPMin and FPMaxNum/FPMinNum. The NM forms are the IEEE-754 minNum/maxNum: a QUIET NaN operand is
// replaced by the infinity that loses, so the other operand is returned -- and note that a SIGNALLING NaN is
// NOT substituted, so it still propagates (with Invalid) through the plain form underneath.
static uint64_t interp_fp_minmax(unsigned fmt, uint64_t a, uint64_t b, int want_max, int numeric) {
    a = interp_fp_flush_input(a, fmt);
    b = interp_fp_flush_input(b, fmt);
    if (numeric) {
        uint64_t losing = ((uint64_t)interp_fp_inf_exp(fmt) << interp_fp_mant(fmt)) |
                          (want_max ? interp_fp_sign_mask(fmt) : UINT64_C(0));
        int a_quiet = interp_fp_class(a, fmt) == INTERP_FPC_QNAN, b_quiet = interp_fp_class(b, fmt) == INTERP_FPC_QNAN;
        if (a_quiet && !b_quiet)
            a = losing;
        else if (b_quiet && !a_quiet)
            b = losing;
    }
    uint64_t operands[2] = {a, b}, nan;
    if (interp_fp_process_nans(fmt, 2, operands, &nan)) return nan;
    double x = interp_fp_widen(a, fmt), y = interp_fp_widen(b, fmt);
    uint64_t chosen = (want_max ? (x > y) : (x < y)) ? a : b;
    if (interp_fp_class(chosen, fmt) == INTERP_FPC_ZERO) {
        // +0 and -0 compare EQUAL, so the numeric comparison above cannot separate them. The architecture
        // combines the operand signs instead: FPMax ANDs them (so +0 wins) and FPMin ORs them (so -0 does).
        unsigned sign_a = (a & interp_fp_sign_mask(fmt)) != 0, sign_b = (b & interp_fp_sign_mask(fmt)) != 0;
        unsigned sign = want_max ? (sign_a & sign_b) : (sign_a | sign_b);
        return sign ? interp_fp_sign_mask(fmt) : UINT64_C(0);
    }
    return chosen;
}

// FPCompare, writing NZCV. `quiet_signals` selects the FCMPE/FCCMPE forms, which raise Invalid for a QUIET
// NaN as well as a signalling one. No host arithmetic: the operands widen to double exactly, and comparing
// two non-NaN doubles raises nothing, so there is nothing to bracket or harvest.
static void interp_fp_compare(struct cpu *cpu, unsigned fmt, uint64_t a, uint64_t b, int quiet_signals) {
    a = interp_fp_flush_input(a, fmt);
    b = interp_fp_flush_input(b, fmt);
    unsigned class_a = interp_fp_class(a, fmt), class_b = interp_fp_class(b, fmt);
    if (class_a >= INTERP_FPC_QNAN || class_b >= INTERP_FPC_QNAN) {
        if (quiet_signals || class_a == INTERP_FPC_SNAN || class_b == INTERP_FPC_SNAN)
            interp_fpsr_raise(INTERP_FPSR_IOC);
        interp_set_flags(cpu, 0, 0, 1, 1); // unordered
        return;
    }
    double x = interp_fp_widen(a, fmt), y = interp_fp_widen(b, fmt);
    if (x == y)
        interp_set_flags(cpu, 0, 1, 1, 0);
    else if (x < y)
        interp_set_flags(cpu, 1, 0, 0, 0);
    else
        interp_set_flags(cpu, 0, 0, 1, 0);
}

// FPRoundInt: round to an integral value in the SAME format. `exact` is what distinguishes FRINTX (which
// raises Inexact when it changes the value) from FRINTI and the five explicitly-moded FRINTs (which do not).
static uint64_t interp_fp_round_integral(unsigned fmt, uint64_t bits, unsigned rmode, int exact) {
    bits = interp_fp_flush_input(bits, fmt);
    uint64_t nan;
    if (interp_fp_process_nans(fmt, 1, &bits, &nan)) return nan;
    unsigned cls = interp_fp_class(bits, fmt);
    if (cls == INTERP_FPC_INF || cls == INTERP_FPC_ZERO) return bits;

    unsigned mant = interp_fp_mant(fmt);
    unsigned sign = (bits & interp_fp_sign_mask(fmt)) != 0;
    uint64_t frac = bits & interp_fp_mant_mask(fmt);
    unsigned biased = (unsigned)((bits >> mant) & (uint64_t)interp_fp_inf_exp(fmt));
    uint64_t significand = biased == 0 ? frac : (frac | (UINT64_C(1) << mant));
    int exponent = (int)(biased == 0 ? 1u : biased) - interp_fp_bias(fmt) - (int)mant;
    if (exponent >= 0) return bits; // already integral: every retained bit is at or above the units place

    unsigned shift = (unsigned)(-exponent);
    uint64_t magnitude;
    int round_bit, sticky;
    if (shift >= 64) {
        // |value| < 1 and smaller than the significand can express against the units place. The integer part
        // is 0 and the whole value is the discarded fraction, so only the rounding direction decides.
        magnitude = 0;
        round_bit = shift == 64 ? (int)((significand >> 63) & 1u) : 0;
        sticky = (significand & (shift == 64 ? ~(UINT64_C(1) << 63) : UINT64_MAX)) != 0;
    } else {
        magnitude = significand >> shift;
        round_bit = (int)((significand >> (shift - 1u)) & 1u);
        sticky = shift > 1 && (significand & ((UINT64_C(1) << (shift - 1u)) - 1u)) != 0;
    }
    if (interp_fp_round_away(rmode, sign, round_bit, sticky, (unsigned)(magnitude & 1u))) magnitude++;
    if (exact && (round_bit || sticky)) interp_fpsr_raise(INTERP_FPSR_IXC);
    // The result is an integer no larger than 2^mant (we only got here because the value had a fraction), so
    // repacking it is exact and the mode is irrelevant. A zero result keeps the operand's sign: FRINTM of
    // -0.4 is -1.0 but FRINTZ of -0.4 is -0.0, and interp_fp_pack preserves that by construction.
    unsigned raised = 0;
    return interp_fp_pack(sign, magnitude, 0, fmt, INTERP_RM_RZ, &raised);
}

// FPConvertNaN: a NaN crossing formats keeps its sign and the TOP bits of its payload, is always quiet on the
// way out, and raises Invalid if the source signalled.
static uint64_t interp_fp_convert_nan(uint64_t bits, unsigned from, unsigned to) {
    if (interp_fp_class(bits, from) == INTERP_FPC_SNAN) interp_fpsr_raise(INTERP_FPSR_IOC);
    if (INTERP_FPCR_DN(g_interp_fpcr)) return interp_fp_default_nan(to);
    unsigned from_mant = interp_fp_mant(from), to_mant = interp_fp_mant(to);
    uint64_t payload = bits & interp_fp_mant_mask(from);
    payload = to_mant >= from_mant ? payload << (to_mant - from_mant) : payload >> (from_mant - to_mant);
    uint64_t sign = (bits & interp_fp_sign_mask(from)) ? interp_fp_sign_mask(to) : 0;
    return sign | ((uint64_t)interp_fp_inf_exp(to) << to_mant) | payload | (UINT64_C(1) << (to_mant - 1u));
}

// FCVT between formats. Widening is exact; narrowing rounds, and double -> single is the one narrowing the
// host does natively and identically (cvtsd2ss is the correctly-rounded IEEE result under MXCSR, exactly as
// AArch64's FCVT is under FPCR).
static uint64_t interp_fp_convert(unsigned from, unsigned to, uint64_t bits) {
    if (interp_fp_class(bits, from) >= INTERP_FPC_QNAN) return interp_fp_convert_nan(bits, from, to);
    bits = interp_fp_flush_input(bits, from);
    if (interp_fp_width(to) > interp_fp_width(from)) {
        // Exact in every direction: a wider format holds every value of a narrower one, including its
        // denormals (which become normals) and its infinities.
        double wide = interp_fp_widen(bits, from);
        if (to == INTERP_FP_D) return interp_fp_postprocess(to, interp_fp_from_double(wide), 0);
        return interp_fp_postprocess(to, interp_fp_from_float((float)wide), 0);
    }
    unsigned raised = 0;
    uint64_t out;
    if (to == INTERP_FP_H) {
        out = interp_fp_half_from_double(interp_fp_widen(bits, from), INTERP_FPCR_RMODE(g_interp_fpcr), &raised);
    } else {
        double wide = interp_fp_to_double(bits); // from == INTERP_FP_D, to == INTERP_FP_S
        interp_fpenv env;
        interp_fp_env_enter(&env);
        volatile double x = wide;
        volatile float r = (float)x;
        raised = interp_fp_env_leave(&env);
        out = interp_fp_from_float(r);
    }
    return interp_fp_postprocess(to, out, raised);
}

// The value SatQ() produces for an out-of-range conversion, per destination width and signedness. A 32-bit
// destination is returned sign-extended into 64 bits; the caller narrows it when it writes a W register.
static uint64_t interp_fp_int_saturate(unsigned sign, unsigned dest_bits, int is_signed) {
    if (!is_signed) return sign ? UINT64_C(0) : (dest_bits == 64 ? UINT64_MAX : ((UINT64_C(1) << dest_bits) - 1u));
    uint64_t limit = UINT64_C(1) << (dest_bits - 1u);
    return sign ? (UINT64_C(0) - limit) : (limit - 1u);
}

// FPToFixed: floating point to a `dest_bits`-wide integer, rounding per `rmode`, keeping `fbits` fractional
// bits (0 for the plain FCVT* forms, 64-scale for the fixed-point ones -- a scale is nothing but an addend on
// the exponent, which is why one function serves both).
//
// The architectural ORDER of the two exceptions matters and is easy to get wrong: the value is rounded to an
// integer FIRST (raising Inexact if that changed it), and only then range-checked (raising Invalid and
// saturating). So FCVTZU of -0.5 is 0 with Inexact and NO Invalid -- it rounds to zero, which is in range --
// while FCVTZU of -1.5 saturates to 0 with both. And a NaN is replaced by the value 0 BEFORE the range check,
// so it yields 0 with Invalid alone, no Inexact and no saturation.
static uint64_t interp_fp_to_int(unsigned fmt, uint64_t bits, unsigned dest_bits, int is_signed, unsigned rmode,
                                unsigned fbits) {
    bits = interp_fp_flush_input(bits, fmt);
    unsigned cls = interp_fp_class(bits, fmt);
    unsigned sign = (bits & interp_fp_sign_mask(fmt)) != 0;
    if (cls >= INTERP_FPC_QNAN) {
        interp_fpsr_raise(INTERP_FPSR_IOC);
        return 0;
    }
    if (cls == INTERP_FPC_INF) {
        interp_fpsr_raise(INTERP_FPSR_IOC);
        return interp_fp_int_saturate(sign, dest_bits, is_signed);
    }
    if (cls == INTERP_FPC_ZERO) return 0;

    unsigned mant = interp_fp_mant(fmt);
    uint64_t frac = bits & interp_fp_mant_mask(fmt);
    unsigned biased = (unsigned)((bits >> mant) & (uint64_t)interp_fp_inf_exp(fmt));
    uint64_t significand = biased == 0 ? frac : (frac | (UINT64_C(1) << mant));
    int exponent = (int)(biased == 0 ? 1u : biased) - interp_fp_bias(fmt) - (int)mant + (int)fbits;

    uint64_t magnitude = 0;
    int inexact = 0, too_large = 0;
    if (exponent >= 0) {
        // Already an integer, so nothing is inexact -- but it may not fit in the 64-bit magnitude below, let
        // alone in the destination, and shifting it there first would lose the very bits that prove it.
        too_large = exponent >= 64 || (significand >> (64 - (unsigned)exponent)) != 0;
        if (!too_large) magnitude = significand << (unsigned)exponent;
    } else {
        unsigned shift = (unsigned)(-exponent);
        int round_bit, sticky;
        if (shift >= 64) {
            round_bit = shift == 64 ? (int)((significand >> 63) & 1u) : 0;
            sticky = (significand & (shift == 64 ? ~(UINT64_C(1) << 63) : UINT64_MAX)) != 0;
        } else {
            magnitude = significand >> shift;
            round_bit = (int)((significand >> (shift - 1u)) & 1u);
            sticky = shift > 1 && (significand & ((UINT64_C(1) << (shift - 1u)) - 1u)) != 0;
        }
        inexact = round_bit | sticky;
        if (interp_fp_round_away(rmode, sign, round_bit, sticky, (unsigned)(magnitude & 1u))) magnitude++;
    }
    if (inexact) interp_fpsr_raise(INTERP_FPSR_IXC);
    if (too_large) {
        interp_fpsr_raise(INTERP_FPSR_IOC);
        return interp_fp_int_saturate(sign, dest_bits, is_signed);
    }
    if (!is_signed) {
        if (sign && magnitude != 0) {
            interp_fpsr_raise(INTERP_FPSR_IOC);
            return 0;
        }
        if (!sign && dest_bits < 64 && magnitude > ((UINT64_C(1) << dest_bits) - 1u)) {
            interp_fpsr_raise(INTERP_FPSR_IOC);
            return interp_fp_int_saturate(0, dest_bits, 0);
        }
        return sign ? UINT64_C(0) : magnitude;
    }
    uint64_t limit = UINT64_C(1) << (dest_bits - 1u);
    if (sign) {
        if (magnitude > limit) {
            interp_fpsr_raise(INTERP_FPSR_IOC);
            return interp_fp_int_saturate(1, dest_bits, 1);
        }
        return UINT64_C(0) - magnitude;
    }
    if (magnitude >= limit) {
        interp_fpsr_raise(INTERP_FPSR_IOC);
        return interp_fp_int_saturate(0, dest_bits, 1);
    }
    return magnitude;
}

// FixedToFP: an integer (optionally scaled down by 2^fbits) to floating point.
static uint64_t interp_fp_from_int(unsigned fmt, uint64_t value, unsigned source_bits, int is_signed, unsigned rmode,
                                  unsigned fbits) {
    unsigned sign = 0;
    uint64_t magnitude;
    if (is_signed) {
        int64_t signed_value = source_bits == 32 ? (int64_t)(int32_t)(uint32_t)value : (int64_t)value;
        sign = signed_value < 0;
        magnitude = sign ? (UINT64_C(0) - (uint64_t)signed_value) : (uint64_t)signed_value;
    } else {
        magnitude = source_bits == 32 ? (uint64_t)(uint32_t)value : value;
    }
    // Two statements, deliberately: passing interp_fp_pack(...) and `raised` as two arguments of one call
    // leaves their evaluation order unspecified, and the compiler is free to read `raised` BEFORE the pack that
    // fills it in -- which silently drops every Inexact/Overflow/Underflow this conversion raises. (Found by
    // the FPSR probe: UCVTF of UINT64_MAX rounds up to 2^64 and must report Inexact, and reported nothing.)
    unsigned raised = 0;
    uint64_t packed = interp_fp_pack(sign, magnitude, -(int)fbits, fmt, rmode, &raised);
    return interp_fp_postprocess(fmt, packed, raised);
}

// VFPExpandImm: the 8-bit FMOV-immediate encoding. One sign bit, then an exponent built as NOT(b) followed by
// (E-3) copies of b and then imm8<5:4>, then imm8<3:0> as the top of the fraction. The point of the odd
// exponent construction is that it spans a small window around 1.0 in every format with the same 8 bits.
static uint64_t interp_fp_expand_imm(unsigned fmt, uint64_t imm8) {
    unsigned exp_bits = interp_fp_width(fmt) - interp_fp_mant(fmt) - 1u;
    unsigned mant = interp_fp_mant(fmt);
    uint64_t sign = (imm8 >> 7) & 1u;
    uint64_t b = (imm8 >> 6) & 1u;
    uint64_t exponent = (b ? 0u : 1u) << (exp_bits - 1u);
    if (b)
        exponent |= (((UINT64_C(1) << (exp_bits - 3u)) - 1u) << 2);
    exponent |= (imm8 >> 4) & 3u;
    return (sign << (interp_fp_width(fmt) - 1u)) | (exponent << mant) | ((imm8 & 0xFu) << (mant - 4u));
}

// ---------------------------------------------------------------------------
// Integer saturation, for the AdvSIMD saturating group.
// ---------------------------------------------------------------------------
// FPSR.QC is CUMULATIVE and sticky exactly like the IEEE exception bits: any saturating instruction whose
// result was clamped sets it, nothing clears it but MSR FPSR, so a guest checks it once after a whole run of
// them. It is not an IEEE exception and no FP operation touches it.

// SQADD / SQSUB on one element. The 64-bit element form is real, so the wide case cannot be computed in a
// wider signed type; it uses overflow detection instead, and the clamp direction follows from the WRAPPED
// sign (a sum that wrapped positive was really too negative, and vice versa).
static uint64_t interp_sqadd_element(uint64_t a, uint64_t b, unsigned size, int subtract) {
    unsigned esize = 8u << size;
    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
    if (esize < 64) {
        int64_t result = subtract ? x - y : x + y;
        int64_t max = (int64_t)((UINT64_C(1) << (esize - 1u)) - 1u), min = -max - 1;
        if (result > max) {
            interp_fpsr_raise(INTERP_FPSR_QC);
            result = max;
        } else if (result < min) {
            interp_fpsr_raise(INTERP_FPSR_QC);
            result = min;
        }
        return (uint64_t)result & interp_element_mask(size);
    }
    int64_t result;
    int overflow = subtract ? __builtin_sub_overflow(x, y, &result) : __builtin_add_overflow(x, y, &result);
    if (overflow) {
        interp_fpsr_raise(INTERP_FPSR_QC);
        result = result < 0 ? INT64_MAX : INT64_MIN;
    }
    return (uint64_t)result;
}

static uint64_t interp_uqadd_element(uint64_t a, uint64_t b, unsigned size, int subtract) {
    uint64_t mask = interp_element_mask(size);
    a &= mask;
    b &= mask;
    if (subtract) {
        if (a < b) {
            interp_fpsr_raise(INTERP_FPSR_QC);
            return 0;
        }
        return a - b;
    }
    uint64_t sum;
    // A 64-bit element can carry out of the type itself, so the carry is tested as well as the mask.
    if (__builtin_add_overflow(a, b, &sum) || sum > mask) {
        interp_fpsr_raise(INTERP_FPSR_QC);
        return mask;
    }
    return sum;
}

// The saturating NARROWING conversions. `size` is the DESTINATION element width and the source element is
// twice that, so the source is at most 64 bits and the destination at most 32 -- which is why this needs no
// 64-bit edge case even though SQADD above does. Three combinations exist and they are genuinely different:
// SQXTN is signed->signed, UQXTN unsigned->unsigned, and SQXTUN signed->UNSIGNED (a negative source saturates
// to zero rather than to the most negative value).
static uint64_t interp_sat_narrow(uint64_t element, unsigned size, int source_signed, int dest_signed) {
    unsigned esize = 8u << size;
    uint64_t mask = interp_element_mask(size);
    if (source_signed) {
        int64_t value = (int64_t)interp_element_sext(element, size + 1u);
        if (dest_signed) {
            int64_t max = (int64_t)((UINT64_C(1) << (esize - 1u)) - 1u), min = -max - 1;
            if (value > max) {
                interp_fpsr_raise(INTERP_FPSR_QC);
                value = max;
            } else if (value < min) {
                interp_fpsr_raise(INTERP_FPSR_QC);
                value = min;
            }
            return (uint64_t)value & mask;
        }
        if (value < 0) {
            interp_fpsr_raise(INTERP_FPSR_QC);
            return 0;
        }
        if ((uint64_t)value > mask) {
            interp_fpsr_raise(INTERP_FPSR_QC);
            return mask;
        }
        return (uint64_t)value;
    }
    uint64_t value = element & interp_element_mask(size + 1u);
    if (value > mask) {
        interp_fpsr_raise(INTERP_FPSR_QC);
        return mask;
    }
    return value;
}

// Polynomial (carry-less) multiply of two `bits`-wide operands, low 64 bits of the product in *low and the
// high 64 in *high. PMUL keeps only the low half of an 8x8 product; PMULL keeps the whole 16-bit or, in the
// 64x64 form the crypto and CRC code uses, the whole 128-bit one.
static void interp_poly_mul(uint64_t a, uint64_t b, unsigned bits, uint64_t *low, uint64_t *high) {
    uint64_t result_low = 0, result_high = 0;
    for (unsigned bit = 0; bit < bits; bit++) {
        if (!((b >> bit) & 1u)) continue;
        // A shift by 0 must not become a shift of the high word by 64, which is undefined in C.
        result_low ^= a << bit;
        if (bit) result_high ^= a >> (64u - bit);
    }
    *low = result_low;
    *high = result_high;
}

// Read/write the low element of a vector register as a scalar of `fmt`. A scalar FP write ZEROES bits
// [127:N] of the destination, which is what interp_vec_write's q == 0 does.
static uint64_t interp_fp_read(const struct cpu *cpu, int reg, unsigned fmt) {
    interp_vec value = interp_vec_read(cpu, reg);
    return interp_vec_element(&value, fmt + 1u, 0);
}

static void interp_fp_write(struct cpu *cpu, int reg, unsigned fmt, uint64_t bits) {
    interp_vec result;
    memset(result.byte, 0, sizeof result.byte);
    interp_vec_set_element(&result, fmt + 1u, 0, bits);
    interp_vec_write(cpu, reg, result, 0);
}

// ---------------------------------------------------------------------------
// The scalar floating-point encoding space.
// ---------------------------------------------------------------------------
// Every member has bits[30:29] == 00 and bits[28:24] == 11110, or 11111 for the three-source multiply-adds.
// bit30 is what separates this from the AdvSIMD SCALAR boxes, which share op0 == 1111 but pin bits[31:30] to
// 01; bit29 (S) must be 0 because the architecture allocates no S == 1 form here. bit31 is free only because
// the two conversion boxes use it as `sf` (the general-register width); every other form requires M == 0.
static int interp_exec_fp_scalar(struct cpu *cpu, uint32_t insn) {
    uint64_t gpc = cpu->pc;
    int rd = (int)(insn & 31), rn = (int)((insn >> 5) & 31), rm = (int)((insn >> 16) & 31);
    unsigned type = (insn >> 22) & 3u, sf = (insn >> 31) & 1u;
    unsigned fmt = INTERP_FP_S;

    // ---- Floating-point data-processing (3 source): FMADD / FMSUB / FNMADD / FNMSUB ----
    if ((insn & 0x7F000000u) == 0x1F000000u) {
        if (sf) return interp_undefined(cpu, insn, "scalar FP -- 3-source with M set");
        if (!interp_fp_type_fmt(type, &fmt))
            return interp_undefined(cpu, insn, "scalar FP -- 3-source unallocated ptype");
        unsigned o1 = (insn >> 21) & 1u, o0 = (insn >> 15) & 1u;
        int ra = (int)((insn >> 10) & 31);
        uint64_t addend = interp_fp_read(cpu, ra, fmt);
        uint64_t left = interp_fp_read(cpu, rn, fmt), right = interp_fp_read(cpu, rm, fmt);
        // The architecture spells all four as ONE FPMulAdd with sign-flipped operands:
        //   FMADD =  Ra + Rn*Rm    FMSUB  =  Ra - Rn*Rm    FNMADD = -Ra - Rn*Rm    FNMSUB = -Ra + Rn*Rm
        // and the flip is a literal sign-bit toggle (FPNeg), which matters for a NaN operand: the sign of its
        // payload flips too, and that sign is observable in the propagated result.
        uint64_t sign = interp_fp_sign_mask(fmt);
        if (o1) addend ^= sign;
        if (o1 != o0) left ^= sign;
        interp_fp_write(cpu, rd, fmt, interp_fp_muladd(fmt, addend, left, right));
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if (!((insn >> 21) & 1u)) {
        // ---- Conversion between floating-point and FIXED-point ----
        // fbits = 64 - scale, and a 32-bit general register cannot name more than 32 fractional bits.
        unsigned rmode = (insn >> 19) & 3u, opcode = (insn >> 16) & 7u, scale = (insn >> 10) & 0x3Fu;
        unsigned fbits = 64u - scale;
        if (!interp_fp_type_fmt(type, &fmt))
            return interp_undefined(cpu, insn, "scalar FP -- fixed-point conversion unallocated ptype");
        if (!sf && scale < 32u)
            return interp_undefined(cpu, insn, "scalar FP -- 32-bit fixed-point conversion with scale < 32");
        if (rmode == 0 && (opcode == 2 || opcode == 3)) { // SCVTF / UCVTF (fixed-point)
            uint64_t value = interp_gpr(cpu, rn);
            interp_fp_write(cpu, rd, fmt,
                            interp_fp_from_int(fmt, value, sf ? 64u : 32u, opcode == 2,
                                               INTERP_FPCR_RMODE(g_interp_fpcr), fbits));
        } else if (rmode == 3 && opcode <= 1) { // FCVTZS / FCVTZU (fixed-point)
            uint64_t out = interp_fp_to_int(fmt, interp_fp_read(cpu, rn, fmt), sf ? 64u : 32u, opcode == 0,
                                           INTERP_RM_RZ, fbits);
            if (sf)
                interp_set_gpr(cpu, rd, out);
            else
                interp_set_gpr32(cpu, rd, (uint32_t)out);
        } else {
            return interp_undefined(cpu, insn, "scalar FP -- unallocated fixed-point conversion");
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // bit21 == 1. The remaining boxes are selected by bits[11:10], with the four bits[11:10] == 00 boxes
    // separated further by how far up bits[15:10] the fixed field reaches -- most specific first.
    unsigned op_low = (insn >> 10) & 3u;

    if (op_low == 1) { // ---- Floating-point conditional compare: FCCMP / FCCMPE ----
        if (sf) return interp_undefined(cpu, insn, "scalar FP -- FCCMP with M set");
        if (!interp_fp_type_fmt(type, &fmt)) return interp_undefined(cpu, insn, "scalar FP -- FCCMP ptype");
        unsigned cond = (insn >> 12) & 0xFu, quiet_signals = (insn >> 4) & 1u;
        if (interp_cond_holds(cpu, cond))
            interp_fp_compare(cpu, fmt, interp_fp_read(cpu, rn, fmt), interp_fp_read(cpu, rm, fmt),
                              (int)quiet_signals);
        else
            // The condition failed, so NO comparison happens and no exception can be raised; NZCV is loaded
            // from the instruction's own nzcv field instead. This is how a short-circuiting `&&` over FP
            // comparisons is compiled, which is why it is common in libm.
            cpu->nzcv = ((uint64_t)(insn & 0xFu)) << 28;
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if (op_low == 2) { // ---- Floating-point data-processing (2 source) ----
        if (sf) return interp_undefined(cpu, insn, "scalar FP -- 2-source with M set");
        if (!interp_fp_type_fmt(type, &fmt))
            return interp_undefined(cpu, insn, "scalar FP -- 2-source unallocated ptype");
        unsigned opcode = (insn >> 12) & 0xFu;
        uint64_t a = interp_fp_read(cpu, rn, fmt), b = interp_fp_read(cpu, rm, fmt), out;
        switch (opcode) {
        case 0: out = interp_fp_arith(fmt, INTERP_FPOP_MUL, a, b); break; // FMUL
        case 1: out = interp_fp_arith(fmt, INTERP_FPOP_DIV, a, b); break; // FDIV
        case 2: out = interp_fp_arith(fmt, INTERP_FPOP_ADD, a, b); break; // FADD
        case 3: out = interp_fp_arith(fmt, INTERP_FPOP_SUB, a, b); break; // FSUB
        case 4: out = interp_fp_minmax(fmt, a, b, 1, 0); break;           // FMAX
        case 5: out = interp_fp_minmax(fmt, a, b, 0, 0); break;           // FMIN
        case 6: out = interp_fp_minmax(fmt, a, b, 1, 1); break;           // FMAXNM
        case 7: out = interp_fp_minmax(fmt, a, b, 0, 1); break;           // FMINNM
        case 8:
            // FNMUL is FPNeg(FPMul(...)) -- the sign flip is applied to the PRODUCT, after rounding and after
            // NaN propagation, so it flips the sign of a propagated NaN too.
            out = interp_fp_arith(fmt, INTERP_FPOP_MUL, a, b) ^ interp_fp_sign_mask(fmt);
            break;
        default: return interp_undefined(cpu, insn, "scalar FP -- unallocated 2-source opcode");
        }
        interp_fp_write(cpu, rd, fmt, out);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if (op_low == 3) { // ---- Floating-point conditional select: FCSEL ----
        if (sf) return interp_undefined(cpu, insn, "scalar FP -- FCSEL with M set");
        if (!interp_fp_type_fmt(type, &fmt)) return interp_undefined(cpu, insn, "scalar FP -- FCSEL ptype");
        unsigned cond = (insn >> 12) & 0xFu;
        // A pure register copy: no unpacking, no flushing, no exceptions -- a signalling NaN passes through
        // unchanged and unreported, exactly as FMOV does.
        interp_fp_write(cpu, rd, fmt,
                        interp_cond_holds(cpu, cond) ? interp_fp_read(cpu, rn, fmt) : interp_fp_read(cpu, rm, fmt));
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x0000FC00u) == 0) { // ---- Conversion between floating-point and INTEGER ----
        unsigned rmode = (insn >> 19) & 3u, opcode = (insn >> 16) & 7u;
        if (rmode == 0 && opcode == 6) { // FMOV to a general register (Vn's low element -> Rd)
            interp_vec source = interp_vec_read(cpu, rn);
            if (type == 0 && !sf) // FMOV Wd, Sn
                interp_set_gpr32(cpu, rd, (uint32_t)interp_vec_element(&source, 2, 0));
            else if (type == 1 && sf) // FMOV Xd, Dn
                interp_set_gpr(cpu, rd, interp_vec_element(&source, 3, 0));
            else if (type == 3 && !sf) // FMOV Wd, Hn
                interp_set_gpr32(cpu, rd, (uint32_t)interp_vec_element(&source, 1, 0));
            else
                return interp_undefined(cpu, insn, "scalar FP -- unallocated FMOV to general register");
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (rmode == 0 && opcode == 7) { // FMOV from a general register (Rn -> Vd's low element)
            if (type == 0 && !sf) // FMOV Sd, Wn
                interp_fp_write(cpu, rd, INTERP_FP_S, interp_gpr(cpu, rn) & 0xFFFFFFFFu);
            else if (type == 1 && sf) // FMOV Dd, Xn
                interp_fp_write(cpu, rd, INTERP_FP_D, interp_gpr(cpu, rn));
            else if (type == 3 && !sf) // FMOV Hd, Wn
                interp_fp_write(cpu, rd, INTERP_FP_H, interp_gpr(cpu, rn) & 0xFFFFu);
            else
                return interp_undefined(cpu, insn, "scalar FP -- unallocated FMOV from general register");
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (rmode == 1 && opcode == 6 && type == 2 && sf) { // FMOV Xd, Vn.D[1]
            interp_vec source = interp_vec_read(cpu, rn);
            interp_set_gpr(cpu, rd, interp_vec_element(&source, 3, 1));
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (rmode == 1 && opcode == 7 && type == 2 && sf) { // FMOV Vd.D[1], Xn
            interp_vec destination = interp_vec_read(cpu, rd);
            interp_vec_set_element(&destination, 3, 1, interp_gpr(cpu, rn));
            interp_vec_write(cpu, rd, destination, 1); // single-lane write: keep the low half
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (rmode == 3 && opcode == 6 && type == 1 && !sf) {
            // ---- FJCVTZS: double -> int32 with JavaScript's ToInt32 semantics ----
            // Two things distinguish it from FCVTZS. It WRAPS modulo 2^32 instead of saturating (that is the
            // ToInt32 rule), and it reports exactness in Z so a JIT can branch on "this double really was a
            // small integer" without a second compare. Z is 1 only when the conversion lost nothing at all.
            uint64_t bits = interp_fp_flush_input(interp_fp_read(cpu, rn, INTERP_FP_D), INTERP_FP_D);
            unsigned cls = interp_fp_class(bits, INTERP_FP_D);
            unsigned exact = 1;
            uint64_t result = 0;
            if (cls >= INTERP_FPC_INF) { // NaN or infinity: Invalid, result 0, not exact
                interp_fpsr_raise(INTERP_FPSR_IOC);
                exact = 0;
            } else if (cls != INTERP_FPC_ZERO) {
                // Reuse the exact-integer machinery, but ask for a 64-bit signed destination so nothing
                // saturates, then take the low 32 bits and decide exactness from the flags it raised.
                uint64_t before = g_interp_fpsr;
                uint64_t wide = interp_fp_to_int(INTERP_FP_D, bits, 64u, 1, INTERP_RM_RZ, 0);
                unsigned raised = (unsigned)((g_interp_fpsr & ~before) & (INTERP_FPSR_IXC | INTERP_FPSR_IOC));
                if (raised) exact = 0;
                if ((int64_t)wide != (int64_t)(int32_t)(uint32_t)wide) {
                    interp_fpsr_raise(INTERP_FPSR_IOC);
                    exact = 0;
                }
                result = (uint64_t)(uint32_t)wide;
            } else if (bits & interp_fp_sign_mask(INTERP_FP_D)) {
                exact = 0; // -0.0 converts to +0, which ToInt32 does not consider exact
            }
            interp_set_gpr32(cpu, rd, (uint32_t)result);
            interp_set_flags(cpu, 0, exact, 0, 0);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (!interp_fp_type_fmt(type, &fmt))
            return interp_undefined(cpu, insn, "scalar FP -- integer conversion unallocated ptype");
        if (opcode == 2 || opcode == 3) { // SCVTF / UCVTF
            if (rmode != 0) return interp_undefined(cpu, insn, "scalar FP -- unallocated SCVTF/UCVTF rmode");
            interp_fp_write(cpu, rd, fmt,
                            interp_fp_from_int(fmt, interp_gpr(cpu, rn), sf ? 64u : 32u, opcode == 2,
                                               INTERP_FPCR_RMODE(g_interp_fpcr), 0));
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (opcode <= 1 || opcode == 4 || opcode == 5) {
            // FCVT{N,A,P,M,Z}{S,U}: rmode picks the rounding for opcode 0/1, and opcode 4/5 is the FCVTA pair
            // whose ties-away mode has no FPCR encoding at all.
            unsigned convert_mode;
            if (opcode >= 4) {
                if (rmode != 0) return interp_undefined(cpu, insn, "scalar FP -- unallocated FCVTA rmode");
                convert_mode = INTERP_RM_RA;
            } else {
                static const unsigned by_rmode[4] = {INTERP_RM_RN, INTERP_RM_RP, INTERP_RM_RM, INTERP_RM_RZ};
                convert_mode = by_rmode[rmode];
            }
            uint64_t out = interp_fp_to_int(fmt, interp_fp_read(cpu, rn, fmt), sf ? 64u : 32u, (opcode & 1u) == 0,
                                           convert_mode, 0);
            if (sf)
                interp_set_gpr(cpu, rd, out);
            else
                interp_set_gpr32(cpu, rd, (uint32_t)out);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        return interp_undefined(cpu, insn, "scalar FP -- unallocated integer conversion");
    }

    if ((insn & 0x00007C00u) == 0x00004000u) { // ---- Floating-point data-processing (1 source) ----
        if (sf) return interp_undefined(cpu, insn, "scalar FP -- 1-source with M set");
        unsigned opcode = (insn >> 15) & 0x3Fu;
        if (!interp_fp_type_fmt(type, &fmt))
            return interp_undefined(cpu, insn, "scalar FP -- 1-source unallocated ptype");
        // FCVT names its DESTINATION in the low two bits of the opcode and its source in ptype, so it has to
        // be split out before the single-format cases below.
        if ((opcode & 0x3Cu) == 0x04u) {
            unsigned to;
            if (!interp_fp_type_fmt(opcode & 3u, &to) || to == fmt)
                return interp_undefined(cpu, insn, "scalar FP -- unallocated FCVT destination");
            if ((to == INTERP_FP_H || fmt == INTERP_FP_H) && INTERP_FPCR_AHP(g_interp_fpcr))
                return interp_undefined(cpu, insn, "scalar FP -- FCVT with FPCR.AHP (alternative half format)");
            interp_fp_write(cpu, rd, to, interp_fp_convert(fmt, to, interp_fp_read(cpu, rn, fmt)));
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        uint64_t a = interp_fp_read(cpu, rn, fmt), out;
        switch (opcode) {
        case 0x00: out = a; break;                              // FMOV (register): a pure bit copy
        case 0x01: out = a & ~interp_fp_sign_mask(fmt); break;   // FABS: FPAbs clears the sign bit and nothing else
        case 0x02: out = a ^ interp_fp_sign_mask(fmt); break;    // FNEG: likewise a sign-bit toggle
        case 0x03: out = interp_fp_sqrt(fmt, a); break;          // FSQRT
        // The FRINT family. All round to an integral value in the same format; they differ only in the mode
        // and in whether the result being different from the operand is reported as Inexact. FRINTX is the
        // only one that reports it (its whole purpose is IEEE roundToIntegralExact); FRINTI and FRINTX share
        // FPCR.RMode and differ by nothing else.
        case 0x08: out = interp_fp_round_integral(fmt, a, INTERP_RM_RN, 0); break; // FRINTN
        case 0x09: out = interp_fp_round_integral(fmt, a, INTERP_RM_RP, 0); break; // FRINTP
        case 0x0A: out = interp_fp_round_integral(fmt, a, INTERP_RM_RM, 0); break; // FRINTM
        case 0x0B: out = interp_fp_round_integral(fmt, a, INTERP_RM_RZ, 0); break; // FRINTZ
        case 0x0C: out = interp_fp_round_integral(fmt, a, INTERP_RM_RA, 0); break; // FRINTA
        case 0x0E: out = interp_fp_round_integral(fmt, a, INTERP_FPCR_RMODE(g_interp_fpcr), 1); break; // FRINTX
        case 0x0F: out = interp_fp_round_integral(fmt, a, INTERP_FPCR_RMODE(g_interp_fpcr), 0); break; // FRINTI
        default: return interp_undefined(cpu, insn, "scalar FP -- unimplemented 1-source opcode");
        }
        interp_fp_write(cpu, rd, fmt, out);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x00003C00u) == 0x00002000u) { // ---- Floating-point compare: FCMP / FCMPE ----
        if (sf || ((insn >> 14) & 3u) != 0) return interp_undefined(cpu, insn, "scalar FP -- unallocated compare");
        if (!interp_fp_type_fmt(type, &fmt)) return interp_undefined(cpu, insn, "scalar FP -- compare ptype");
        unsigned opcode2 = insn & 0x1Fu;
        if (opcode2 & 7u) return interp_undefined(cpu, insn, "scalar FP -- unallocated compare opcode2");
        // opcode2<4> is E (raise Invalid for a quiet NaN too) and opcode2<3> selects the compare-with-zero
        // form, in which Rm is not a register operand at all.
        int quiet_signals = (opcode2 >> 4) & 1;
        uint64_t b = (opcode2 & 8u) ? UINT64_C(0) : interp_fp_read(cpu, rm, fmt);
        interp_fp_compare(cpu, fmt, interp_fp_read(cpu, rn, fmt), b, quiet_signals);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    if ((insn & 0x00001C00u) == 0x00001000u) { // ---- FMOV (immediate) ----
        if (sf || ((insn >> 5) & 0x1Fu) != 0)
            return interp_undefined(cpu, insn, "scalar FP -- unallocated FMOV immediate");
        if (!interp_fp_type_fmt(type, &fmt)) return interp_undefined(cpu, insn, "scalar FP -- FMOV immediate ptype");
        interp_fp_write(cpu, rd, fmt, interp_fp_expand_imm(fmt, (insn >> 13) & 0xFFu));
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    return interp_undefined(cpu, insn, "scalar FP -- unallocated encoding");
}

// ---------------------------------------------------------------------------
// Scalar floating-point and Advanced SIMD.
// ---------------------------------------------------------------------------
// Scope note, because this group is enormous and most of it will never execute here. What a static glibc
// guest actually needs is the integer-vector subset that its string and memory routines are written in:
// memset broadcasts a byte with DUP and stores with vector ST1/STR; strlen compares 16 bytes at a time with
// CMEQ and finds the first hit with a horizontal reduction; memcpy is vector LDR/STR pairs. That subset is
// what is implemented, and it was chosen by running the guest and reading interp_undefined's report rather
// than by working down the ARM ARM.
//
// Deliberately still absent, and each still reported by name: floating-point ARITHMETIC (FADD/FMUL/FDIV/
// FCVT/FCMP and the whole scalar-FP box beyond the FMOV register moves below), the saturating and widening
// integer forms, the pairwise-widening and long multiplies, the crypto and CRC blocks, and SVE. A guest that
// needs those will say so precisely.
static int interp_exec_simd(struct cpu *cpu, uint32_t insn) {
    uint64_t gpc = cpu->pc;
    int rd = (int)(insn & 31), rn = (int)((insn >> 5) & 31), rm = (int)((insn >> 16) & 31);
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1;

    // ---- The scalar floating-point space ----
    // Tested first because it is exactly disjoint from every AdvSIMD box below and the test is one mask: the
    // AdvSIMD VECTOR boxes all have bit28 == 0 (op0 == 0111) and the AdvSIMD SCALAR boxes all have bit30 == 1,
    // while every scalar-FP encoding has bit28 == 1 and bit30 == 0. See interp_exec_fp_scalar's header.
    if ((insn & 0x7F000000u) == 0x1E000000u || (insn & 0x7F000000u) == 0x1F000000u)
        return interp_exec_fp_scalar(cpu, insn);

    // ---- AdvSIMD SCALAR forms, normalised into their vector spelling ----
    // bits[31:30] == 01 with bits[28:25] == 1111 is the AdvSIMD scalar space: ADD/SUB/CMxx/NEG/ABS/ADDP on D
    // registers, the saturating group on any width, the shift-by-immediate group, the FP compares and
    // conversions, and DUP (element). Every one of them puts U, size, opcode, Rm, Rn and Rd at EXACTLY the
    // positions its vector counterpart does -- that is why the architecture chose those positions -- so the
    // scalar encoding becomes the vector encoding with Q == 0 by clearing two bits, and the only remaining
    // difference is that it operates on ONE lane and always zeroes bits [127:esize].
    //
    // Normalising here rather than writing a second copy of each handler is what keeps the two spellings from
    // drifting apart, and it is why `scalar` is threaded through the lane counts below rather than being
    // handled by a parallel switch. Two things it must NOT do: use interp_vec_lanes (which would give 2 lanes
    // for, say, a scalar 32-bit form), and apply the vector group's "1D is reserved" checks (a scalar form is
    // 64-bit-only precisely where the vector one is reserved).
    unsigned scalar = 0;
    if ((insn & 0xDE000000u) == 0x5E000000u) {
        scalar = 1;
        insn &= ~UINT32_C(0x50000000); // clear bit30 (becomes Q == 0) and bit28 (becomes the vector op0)
        q = 0;
    }

    // ---- AdvSIMD copy: DUP (element/general), INS (element/general), SMOV, UMOV ----
    if ((insn & 0x9F208400u) == 0x0E000400u) {
        unsigned op = (insn >> 29) & 1, imm4 = (insn >> 11) & 0xFu, imm5 = (insn >> 16) & 0x1Fu;
        unsigned size, index;
        if (op) { // INS (element): copy one lane of Rn to one lane of Rd
            if (!interp_imm5_element(imm5, &size, &index))
                return interp_undefined(cpu, insn, "AdvSIMD copy -- reserved imm5");
            unsigned source_index = imm4 >> size; // imm4 is the source lane, scaled by the element size
            interp_vec source = interp_vec_read(cpu, rn), destination = interp_vec_read(cpu, rd);
            interp_vec_set_element(&destination, size, index, interp_vec_element(&source, size, source_index));
            // INS writes a single lane of an existing 128-bit register, so it must NOT zero the upper half.
            interp_vec_write(cpu, rd, destination, 1);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        switch (imm4) {
        case 0: { // DUP (element): broadcast one lane of Rn across Rd
            if (!interp_imm5_element(imm5, &size, &index))
                return interp_undefined(cpu, insn, "AdvSIMD copy -- reserved imm5");
            if (size == 3 && !q) return interp_undefined(cpu, insn, "AdvSIMD copy -- DUP 1D is reserved");
            interp_vec source = interp_vec_read(cpu, rn), result;
            uint64_t element = interp_vec_element(&source, size, index);
            memset(result.byte, 0, sizeof result.byte);
            for (unsigned lane = 0; lane < interp_vec_lanes(size, q); lane++)
                interp_vec_set_element(&result, size, lane, element);
            interp_vec_write(cpu, rd, result, q);
            break;
        }
        case 1: { // DUP (general): broadcast a general-purpose register across Rd
            if (!interp_imm5_element(imm5, &size, &index))
                return interp_undefined(cpu, insn, "AdvSIMD copy -- reserved imm5");
            if (size == 3 && !q) return interp_undefined(cpu, insn, "AdvSIMD copy -- DUP 1D is reserved");
            uint64_t element = interp_gpr(cpu, rn) & interp_element_mask(size);
            interp_vec result;
            memset(result.byte, 0, sizeof result.byte);
            for (unsigned lane = 0; lane < interp_vec_lanes(size, q); lane++)
                interp_vec_set_element(&result, size, lane, element);
            interp_vec_write(cpu, rd, result, q);
            break;
        }
        case 3: { // INS (general): one general-purpose register into one lane of Rd
            if (!interp_imm5_element(imm5, &size, &index))
                return interp_undefined(cpu, insn, "AdvSIMD copy -- reserved imm5");
            interp_vec destination = interp_vec_read(cpu, rd);
            interp_vec_set_element(&destination, size, index, interp_gpr(cpu, rn) & interp_element_mask(size));
            interp_vec_write(cpu, rd, destination, 1); // single-lane write: preserve the upper half
            break;
        }
        case 5:   // SMOV: one lane to a general register, sign-extended
        case 7: { // UMOV: one lane to a general register, zero-extended
            if (!interp_imm5_element(imm5, &size, &index))
                return interp_undefined(cpu, insn, "AdvSIMD copy -- reserved imm5");
            interp_vec source = interp_vec_read(cpu, rn);
            uint64_t element = interp_vec_element(&source, size, index);
            if (imm4 == 5) element = interp_element_sext(element, size);
            // Q selects the destination register width, not the vector length, in this one form.
            if (q)
                interp_set_gpr(cpu, rd, element);
            else
                interp_set_gpr32(cpu, rd, (uint32_t)element);
            break;
        }
        default: return interp_undefined(cpu, insn, "AdvSIMD copy -- unallocated imm4");
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD modified immediate: MOVI / MVNI / ORR (imm) / BIC (imm) ----
    // Must be tested BEFORE shift-by-immediate: the two share an encoding box and are separated only by immh
    // (bits 22:19) being zero here and non-zero there.
    if ((insn & 0x9FF80400u) == 0x0F000400u) {
        unsigned op = (insn >> 29) & 1, cmode = (insn >> 12) & 0xFu, o2 = (insn >> 11) & 1;
        uint64_t imm8 = (uint64_t)(((insn >> 16) & 7u) << 5) | ((insn >> 5) & 0x1Fu);
        uint64_t pattern;
        if (!interp_advsimd_expand_imm(op, cmode, o2, q, imm8, &pattern))
            return interp_undefined(cpu, insn, "AdvSIMD modified immediate -- reserved cmode");
        // cmode<0> == 1 with cmode<3:1> != 111 selects the read-modify-write forms ORR/BIC rather than a
        // plain move, and `op` then chooses between them.
        int read_modify = (cmode & 1u) && ((cmode >> 1) & 7u) != 7u;
        interp_vec result = interp_vec_read(cpu, rd);
        uint64_t low, high;
        memcpy(&low, result.byte, 8);
        memcpy(&high, result.byte + 8, 8);
        if (read_modify) {
            if (op) { // BIC (immediate)
                low &= ~pattern;
                high &= ~pattern;
            } else { // ORR (immediate)
                low |= pattern;
                high |= pattern;
            }
        } else if (op && ((cmode >> 1) & 7u) != 7u) { // MVNI
            low = high = ~pattern;
        } else { // MOVI
            low = high = pattern;
        }
        memcpy(result.byte, &low, 8);
        memcpy(result.byte + 8, &high, 8);
        interp_vec_write(cpu, rd, result, q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD shift by immediate: SSHR / USHR / SSRA / USRA / SHL ----
    if ((insn & 0x9F800400u) == 0x0F000400u) {
        unsigned immh = (insn >> 19) & 0xFu, immb = (insn >> 16) & 7u, opcode = (insn >> 11) & 0x1Fu;
        unsigned size;
        if (immh & 8u)
            size = 3;
        else if (immh & 4u)
            size = 2;
        else if (immh & 2u)
            size = 1;
        else
            size = 0;
        unsigned esize = 8u << size;
        unsigned combined = (immh << 3) | immb;
        interp_vec source = interp_vec_read(cpu, rn), result;
        memset(result.byte, 0, sizeof result.byte);
        // A scalar shift-by-immediate (SHL/SSHR/USHR/SSRA/USRA on a D register) is this same encoding with one
        // lane, and the 64-bit element it names is exactly the vector group's reserved 1D form -- so the lane
        // count and the reservation check below both have to know which spelling this is.
        unsigned lanes = scalar ? 1u : interp_vec_lanes(size, q);
        uint64_t mask = interp_element_mask(size);

        // ---- the fixed-point FP conversions, which are the only members whose immh means something else ----
        // SCVTF/UCVTF and FCVTZS/FCVTZU with a fractional-bit count. immh still selects the element width, but
        // (immh:immb) is a SCALE rather than a shift: fbits = 2*esize - (immh:immb), the same relation the
        // scalar fixed-point conversions use, which is why both go through interp_fp_to_int/from_int.
        if (opcode == 0x1C || opcode == 0x1F) {
            if (size < 1) return interp_undefined(cpu, insn, "AdvSIMD shift -- fixed-point conversion needs immh != 0");
            unsigned fmt = size == 3 ? INTERP_FP_D : (size == 2 ? INTERP_FP_S : INTERP_FP_H);
            unsigned fbits = 2u * esize - combined;
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane);
                uint64_t value;
                if (opcode == 0x1C) // SCVTF / UCVTF (fixed-point)
                    value = interp_fp_from_int(fmt, element, esize, !u, INTERP_FPCR_RMODE(g_interp_fpcr), fbits);
                else // FCVTZS / FCVTZU (fixed-point): always round toward zero
                    value = interp_fp_to_int(fmt, element, esize, !u, INTERP_RM_RZ, fbits);
                interp_vec_set_element(&result, size, lane, value & mask);
            }
            interp_vec_write(cpu, rd, result, scalar ? 0u : q);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        if (opcode == 0x10 || opcode == 0x11 || opcode == 0x12 || opcode == 0x13) {
            // The NARROWING right shifts, all sharing one shape: read elements of TWICE the destination width,
            // shift right, and write 64 bits into the half of the destination Q selects. SHRN/RSHRN truncate,
            // which is how a 16-byte vector compare mask gets packed into 8 bytes so a single FMOV can carry it
            // to a general register (the core of glibc's strlen and memchr); the rest SATURATE, in three senses:
            //   opcode 10000/10001, U == 1  SQSHRUN / SQRSHRUN   signed source, UNSIGNED saturated result
            //   opcode 10010/10011, U == 0  SQSHRN  / SQRSHRN    signed source, signed saturated result
            //   opcode 10010/10011, U == 1  UQSHRN  / UQRSHRN    unsigned source, unsigned saturated result
            // The odd opcode of each pair is the ROUNDING variant, which adds half of the discarded field first.
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD shift -- narrowing shift with a 64-bit result");
            unsigned shift = 2u * esize - combined;
            unsigned narrow_lanes = 64u / esize; // the result is always 64 bits wide; Q selects WHICH half
            uint64_t wide_mask = interp_element_mask(size + 1u);
            int saturating = opcode >= 0x12 || u;
            int source_signed = opcode >= 0x12 ? !u : 1;
            int dest_signed = opcode >= 0x12 ? !u : 0;
            int rounding = (opcode & 1u) != 0;
            interp_vec packed;
            memset(packed.byte, 0, sizeof packed.byte);
            for (unsigned lane = 0; lane < (scalar ? 1u : narrow_lanes); lane++) {
                uint64_t element = interp_vec_element(&source, size + 1u, lane) & wide_mask;
                uint64_t shifted;
                if (saturating && source_signed) {
                    // Shift the SIGN-EXTENDED value, so the sign is replicated rather than zeroes shifted in,
                    // and so the saturation test below sees the true magnitude.
                    int64_t wide = (int64_t)interp_element_sext(element, size + 1u);
                    if (rounding && shift > 0) wide += (int64_t)(UINT64_C(1) << (shift - 1u));
                    shifted = (uint64_t)(wide >> shift) & wide_mask;
                } else {
                    if (rounding && shift > 0) element = (element + (UINT64_C(1) << (shift - 1u))) & wide_mask;
                    shifted = (element >> shift) & wide_mask;
                }
                interp_vec_set_element(&packed, size, lane,
                                      saturating ? interp_sat_narrow(shifted, size, source_signed, dest_signed)
                                                 : (shifted & mask));
            }
            if (!q || scalar) {
                // The unsuffixed mnemonic writes the low 64 bits and ZEROES the upper half.
                interp_vec_write(cpu, rd, packed, 0);
            } else {
                // The "2" mnemonic writes the UPPER 64 bits and must leave the lower half untouched.
                interp_vec destination = interp_vec_read(cpu, rd);
                memcpy(destination.byte + 8, packed.byte, 8);
                interp_vec_write(cpu, rd, destination, 1);
            }
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        if (opcode == 0x14) {
            // SSHLL / USHLL (and SXTL/UXTL, which are these with a zero shift). The counterpart of SHRN: widen
            // each element to twice its size and shift left. Q selects which half of the source is consumed.
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD shift -- SSHLL/USHLL with a 64-bit source");
            unsigned shift = combined - esize;
            unsigned wide_lanes = 64u / esize;
            uint64_t wide_mask = interp_element_mask(size + 1u);
            for (unsigned lane = 0; lane < wide_lanes; lane++) {
                // Q == 1 takes the source elements from the upper half of Vn.
                uint64_t element = interp_vec_element(&source, size, q ? lane + wide_lanes : lane);
                if (!u) element = interp_element_sext(element, size);
                interp_vec_set_element(&result, size + 1u, lane, (element << shift) & wide_mask);
            }
            // The destination is a full 128-bit register in both variants.
            interp_vec_write(cpu, rd, result, 1);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        if (size == 3 && !q && !scalar)
            return interp_undefined(cpu, insn, "AdvSIMD shift -- 64-bit element requires Q");
        if (opcode == 0x0A && !u) { // SHL: shift left by (combined - esize)
            unsigned shift = combined - esize;
            for (unsigned lane = 0; lane < lanes; lane++)
                interp_vec_set_element(&result, size, lane,
                                       (interp_vec_element(&source, size, lane) << shift) & mask);
        } else if (opcode == 0x08 || (opcode == 0x0A && u)) {
            // SRI (opcode 01000) and SLI (01010 with U == 1): shift and INSERT. The shifted-in bits are taken
            // from the DESTINATION rather than being zeroes, which is what makes them bitfield inserts.
            interp_vec destination = interp_vec_read(cpu, rd);
            unsigned shift = opcode == 0x08 ? 2u * esize - combined : combined - esize;
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane) & mask;
                uint64_t base = interp_vec_element(&destination, size, lane) & mask;
                uint64_t moved, keep;
                if (opcode == 0x08) { // SRI: keep the destination's TOP `shift` bits
                    moved = shift >= esize ? 0 : (element >> shift);
                    keep = shift == 0 ? 0 : (mask << (esize - shift)) & mask;
                } else { // SLI: keep the destination's BOTTOM `shift` bits
                    moved = (element << shift) & mask;
                    keep = shift == 0 ? 0 : ((UINT64_C(1) << shift) - 1u);
                }
                interp_vec_set_element(&result, size, lane, (moved & ~keep) | (base & keep));
            }
        } else if (opcode == 0x0C || opcode == 0x0E) {
            // SQSHLU (01100, U == 1 only), SQSHL and UQSHL (01110): saturating shift LEFT by an immediate.
            // Expressed as a repeated saturating doubling so that the one-bit-at-a-time overflow test is the
            // same one SQADD uses and FPSR.QC is set identically; a shift amount is at most esize - 1, so the
            // cost is bounded and the alternative (a wider intermediate type) does not exist for esize == 64.
            if (opcode == 0x0C && !u) return interp_undefined(cpu, insn, "AdvSIMD shift -- unallocated SQSHLU");
            unsigned shift = combined - esize;
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane);
                uint64_t value;
                if (opcode == 0x0E && u) { // UQSHL: unsigned source, unsigned saturation
                    value = element & mask;
                    for (unsigned step = 0; step < shift; step++) value = interp_uqadd_element(value, value, size, 0);
                } else if (opcode == 0x0E) { // SQSHL: signed source, signed saturation
                    value = element & mask;
                    for (unsigned step = 0; step < shift; step++) value = interp_sqadd_element(value, value, size, 0);
                } else { // SQSHLU: signed source, UNSIGNED saturation -- a negative operand saturates to zero
                    int64_t signed_element = (int64_t)interp_element_sext(element, size);
                    if (signed_element < 0) {
                        interp_fpsr_raise(INTERP_FPSR_QC);
                        value = 0;
                    } else {
                        value = (uint64_t)signed_element & mask;
                        for (unsigned step = 0; step < shift; step++)
                            value = interp_uqadd_element(value, value, size, 0);
                    }
                }
                interp_vec_set_element(&result, size, lane, value & mask);
            }
        } else if (opcode == 0x00 || opcode == 0x02 || opcode == 0x04 || opcode == 0x06) {
            // SSHR/USHR (00000), SSRA/USRA (00010) and their ROUNDING counterparts SRSHR/URSHR (00100) and
            // SRSRA/URSRA (00110). The 0x02 pair accumulates into the destination; the 0x04/0x06 pair adds half
            // of the field about to be discarded before shifting.
            unsigned shift = 2u * esize - combined;
            int rounding = opcode == 0x04 || opcode == 0x06;
            int accumulating = opcode == 0x02 || opcode == 0x06;
            interp_vec accumulate = interp_vec_read(cpu, rd);
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane);
                // A right shift of the FULL element width is defined for these instructions (the result is 0,
                // or the replicated sign bit) but is undefined behaviour in C, so it is handled explicitly.
                // The rounding increment is likewise the bit at position shift-1, which for a full-width shift
                // is the element's top bit.
                uint64_t shifted;
                if (u) {
                    uint64_t value = element & mask;
                    uint64_t round = rounding && shift > 0 ? ((value >> (shift - 1u)) & 1u) : 0u;
                    shifted = (shift >= esize ? 0 : (value >> shift)) + round;
                } else {
                    int64_t signed_element = (int64_t)interp_element_sext(element, size);
                    uint64_t round =
                        rounding && shift > 0
                            ? (uint64_t)((signed_element >> (shift > esize ? esize - 1u : shift - 1u)) & 1)
                            : 0u;
                    shifted = (uint64_t)(shift >= esize ? (signed_element >> (esize - 1)) : (signed_element >> shift));
                    shifted += round;
                }
                shifted &= mask;
                if (accumulating) shifted = (shifted + interp_vec_element(&accumulate, size, lane)) & mask;
                interp_vec_set_element(&result, size, lane, shifted);
            }
        } else {
            return interp_undefined(cpu, insn, "AdvSIMD shift by immediate -- unimplemented opcode");
        }
        interp_vec_write(cpu, rd, result, scalar ? 0u : q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD EXT: concatenate Rn:Rm and extract a byte-aligned window ----
    if ((insn & 0xBFE08400u) == 0x2E000000u) {
        unsigned position = (insn >> 11) & 0xFu;
        unsigned bytes = q ? 16u : 8u;
        if (!q && (position & 8u)) return interp_undefined(cpu, insn, "AdvSIMD EXT -- imm4 out of range for 8B");
        interp_vec first = interp_vec_read(cpu, rn), second = interp_vec_read(cpu, rm), result;
        memset(result.byte, 0, sizeof result.byte);
        for (unsigned index = 0; index < bytes; index++) {
            unsigned source = position + index;
            result.byte[index] = source < bytes ? first.byte[source] : second.byte[source - bytes];
        }
        interp_vec_write(cpu, rd, result, q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD table lookup: TBL / TBX ----
    if ((insn & 0xBF208C00u) == 0x0E000000u) {
        unsigned length = (insn >> 13) & 3u, extend = (insn >> 12) & 1u;
        unsigned bytes = q ? 16u : 8u;
        interp_vec index_vector = interp_vec_read(cpu, rm), result = interp_vec_read(cpu, rd);
        interp_vec table[4];
        for (unsigned entry = 0; entry <= length; entry++)
            table[entry] = interp_vec_read(cpu, (rn + (int)entry) % 32); // the table registers wrap at V31
        for (unsigned index = 0; index < bytes; index++) {
            unsigned selector = index_vector.byte[index];
            if (selector < (length + 1u) * 16u)
                result.byte[index] = table[selector / 16u].byte[selector % 16u];
            else if (!extend)
                result.byte[index] = 0; // TBL zeroes an out-of-range index; TBX leaves the destination byte
        }
        interp_vec_write(cpu, rd, result, q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD permute: ZIP1 / ZIP2 / UZP1 / UZP2 / TRN1 / TRN2 ----
    // Same mask as TBL/TBX above and separated from it only by bits[11:10] (00 there, 10 here), which is why
    // the two sit together. All six are pure lane shuffles -- no arithmetic, no flags, no FP -- so the whole
    // group is one indexing rule per opcode. They matter far more than their size suggests: a compiler emits
    // UZP1/ZIP1 for every de-interleaving struct-of-arrays copy, and `uzp1 v31.2d, v30.2d, v31.2d` is what a
    // 128-bit-pair spill in another hl-engine build reduces to, i.e. it is on the path of running this engine
    // under itself.
    if ((insn & 0xBF208C00u) == 0x0E000800u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 12) & 7u;
        if (size == 3 && !q) return interp_undefined(cpu, insn, "AdvSIMD permute -- 1D form is reserved");
        interp_vec left = interp_vec_read(cpu, rn), right = interp_vec_read(cpu, rm), result;
        memset(result.byte, 0, sizeof result.byte);
        unsigned lanes = interp_vec_lanes(size, q), half = lanes / 2u;
        for (unsigned lane = 0; lane < lanes; lane++) {
            uint64_t element;
            switch (opcode) {
            case 1: // UZP1: the EVEN-indexed elements of the concatenation Vn:Vm, in order
            case 5: { // UZP2: the ODD-indexed ones
                unsigned offset = opcode == 5 ? 1u : 0u;
                const interp_vec *source = lane < half ? &left : &right;
                unsigned index = (lane < half ? lane : lane - half) * 2u + offset;
                element = interp_vec_element(source, size, index);
                break;
            }
            case 2:   // TRN1: even lanes of Vn interleaved with the even lanes of Vm
            case 6: { // TRN2: the odd lanes of each
                unsigned offset = opcode == 6 ? 1u : 0u;
                const interp_vec *source = (lane & 1u) ? &right : &left;
                element = interp_vec_element(source, size, (lane & ~1u) + offset);
                break;
            }
            case 3:   // ZIP1: interleave the LOWER halves of Vn and Vm
            case 7: { // ZIP2: interleave the UPPER halves
                unsigned base = opcode == 7 ? half : 0u;
                const interp_vec *source = (lane & 1u) ? &right : &left;
                element = interp_vec_element(source, size, base + lane / 2u);
                break;
            }
            default: return interp_undefined(cpu, insn, "AdvSIMD permute -- unallocated opcode");
            }
            interp_vec_set_element(&result, size, lane, element);
        }
        interp_vec_write(cpu, rd, result, q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD across lanes: ADDV / UADDLV / SADDLV / UMAXV / SMAXV / UMINV / SMINV ----
    // Tested before two-register-misc: both live at bits[21:17] == 10000/11000 and differ only in bit 20.
    if ((insn & 0x9F3E0C00u) == 0x0E300800u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 12) & 0x1Fu;
        // ---- the floating-point reductions: FMAXNMV / FMAXV / FMINNMV / FMINV ----
        // Same field reinterpretation as the other FP boxes (bit23 selects max vs min, bit22 is sz), and they
        // are U == 1 only. Folded left to right so that a NaN or a signed zero reaches the same answer the
        // pairwise tree in hardware would: FPMax/FPMin are associative for those cases because their NaN and
        // zero-sign rules are symmetric in the two operands.
        if (u && (opcode == 0x0Cu || opcode == 0x0Fu)) {
            unsigned fmt = (size & 1u) ? INTERP_FP_D : INTERP_FP_S, high = (size >> 1) & 1u;
            unsigned element = fmt + 1u, lanes = interp_vec_lanes(element, q);
            if (fmt == INTERP_FP_D || !q)
                return interp_undefined(cpu, insn, "AdvSIMD across lanes -- unallocated FP reduction size");
            interp_vec source = interp_vec_read(cpu, rn), result;
            memset(result.byte, 0, sizeof result.byte);
            uint64_t accumulator = interp_vec_element(&source, element, 0);
            for (unsigned lane = 1; lane < lanes; lane++)
                accumulator = interp_fp_minmax(fmt, accumulator, interp_vec_element(&source, element, lane), !high,
                                              opcode == 0x0Cu);
            interp_vec_set_element(&result, element, 0, accumulator);
            interp_vec_write(cpu, rd, result, 0);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        // ---- the AdvSIMD SCALAR pairwise forms, which share this encoding box ----
        // ADDP Dd, Vn.2D and the FP pairwise reductions FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP Sd/Dd, Vn.2S/2D.
        // They are not reductions across a whole vector: they combine exactly the TWO lanes of the source, so
        // they need their own arm rather than the fold below (which starts from lane 0 and would be equivalent
        // only by accident) and they must bypass the vector group's size/Q reservations.
        if (scalar) {
            interp_vec source = interp_vec_read(cpu, rn), result;
            memset(result.byte, 0, sizeof result.byte);
            if (!u && opcode == 0x1Bu) { // ADDP (scalar): 64-bit element only
                if (size != 3) return interp_undefined(cpu, insn, "AdvSIMD scalar pairwise -- ADDP needs 2D");
                interp_vec_set_element(&result, 3, 0,
                                      interp_vec_element(&source, 3, 0) + interp_vec_element(&source, 3, 1));
            } else if (u && (opcode == 0x0Cu || opcode == 0x0Du || opcode == 0x0Fu)) {
                unsigned fmt = (size & 1u) ? INTERP_FP_D : INTERP_FP_S, high = (size >> 1) & 1u;
                unsigned element = fmt + 1u;
                uint64_t a = interp_vec_element(&source, element, 0), b = interp_vec_element(&source, element, 1);
                uint64_t value;
                if (opcode == 0x0Du) { // FADDP (scalar); bit23 set is unallocated here
                    if (high) return interp_undefined(cpu, insn, "AdvSIMD scalar pairwise -- unallocated FADDP");
                    value = interp_fp_arith(fmt, INTERP_FPOP_ADD, a, b);
                } else { // FMAXNMP/FMINNMP (0x0C) and FMAXP/FMINP (0x0F)
                    value = interp_fp_minmax(fmt, a, b, !high, opcode == 0x0Cu);
                }
                interp_vec_set_element(&result, element, 0, value);
            } else {
                return interp_undefined(cpu, insn, "AdvSIMD scalar pairwise -- unimplemented opcode");
            }
            interp_vec_write(cpu, rd, result, 0);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (size == 3 || (size == 2 && !q))
            return interp_undefined(cpu, insn, "AdvSIMD across lanes -- reserved size/Q combination");
        interp_vec source = interp_vec_read(cpu, rn), result;
        memset(result.byte, 0, sizeof result.byte);
        unsigned lanes = interp_vec_lanes(size, q);
        uint64_t accumulator = interp_vec_element(&source, size, 0);
        switch (opcode) {
        case 0x03: { // SADDLV / UADDLV: sum every lane into an element of TWICE the width
            uint64_t total = 0;
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane);
                total += u ? element : interp_element_sext(element, size);
            }
            interp_vec_set_element(&result, size + 1u, 0, total & interp_element_mask(size + 1u));
            break;
        }
        case 0x0A: // SMAXV / UMAXV
        case 0x1A: // SMINV / UMINV
        case 0x1B: // ADDV
            for (unsigned lane = 1; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane);
                if (opcode == 0x1B) {
                    accumulator = (accumulator + element) & interp_element_mask(size);
                } else if (u) {
                    int greater = element > accumulator;
                    if (opcode == 0x0A ? greater : !greater && element != accumulator) accumulator = element;
                } else {
                    int64_t left = (int64_t)interp_element_sext(accumulator, size);
                    int64_t right = (int64_t)interp_element_sext(element, size);
                    if (opcode == 0x0A ? right > left : right < left) accumulator = element;
                }
            }
            interp_vec_set_element(&result, size, 0, accumulator);
            break;
        default: return interp_undefined(cpu, insn, "AdvSIMD across lanes -- unimplemented opcode");
        }
        // A reduction produces a SCALAR, so only the low element is defined and the rest of the register is
        // zero -- hence the zeroed `result` above and the Q=0 write here.
        interp_vec_write(cpu, rd, result, 0);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD two-register misc: NOT / RBIT / CNT / REV16 / REV32 / REV64 / CMEQ-zero / ABS / NEG ----
    if ((insn & 0x9F3E0C00u) == 0x0E200800u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 12) & 0x1Fu;
        interp_vec source = interp_vec_read(cpu, rn), result;
        memset(result.byte, 0, sizeof result.byte);
        unsigned bytes = q ? 16u : 8u;

        // ---- the floating-point members of this box ----
        // Opcodes 01100..01111 and 10110..11111 are FP, and there `size` splits the way it does in the FP
        // three-same group: bit23 becomes an operation selector and bit22 is `sz`. That is how FRINTN (bit23
        // clear) and FRINTP (bit23 set) share opcode 11000, and it is why reading `size` as an element width
        // here would pick a 16-bit lane for every single-precision form.
        if ((opcode >= 0x0Cu && opcode <= 0x0Fu) || opcode >= 0x16u) {
            unsigned fmt = (size & 1u) ? INTERP_FP_D : INTERP_FP_S, high = (size >> 1) & 1u;
            unsigned element = fmt + 1u;
            uint64_t saved_nzcv = cpu->nzcv; // see the note in the three-same FP block
            // FCVTL/FCVTN change the element width, so they are handled before the equal-width loop. FCVTL
            // widens the selected half to the next format up; FCVTN narrows into the selected half. Both name
            // the NARROW format in sz, so sz == 0 means half->single and sz == 1 means single->double.
            if (opcode == 0x16u || opcode == 0x17u) {
                if (u && opcode == 0x16u)
                    return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- FCVTXN");
                if (high) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated FCVTL/FCVTN size");
                unsigned narrow = (size & 1u) ? INTERP_FP_S : INTERP_FP_H, wide = narrow + 1u;
                // The narrow side is always exactly 64 bits' worth of elements; Q says which half of the
                // 128-bit register those 64 bits live in.
                unsigned narrow_lanes = narrow == INTERP_FP_S ? 2u : 4u;
                if (opcode == 0x17u) { // FCVTL / FCVTL2: widen
                    for (unsigned lane = 0; lane < narrow_lanes; lane++) {
                        uint64_t element_bits =
                            interp_vec_element(&source, narrow + 1u, q ? lane + narrow_lanes : lane);
                        interp_vec_set_element(&result, wide + 1u, lane, interp_fp_convert(narrow, wide, element_bits));
                    }
                    interp_vec_write(cpu, rd, result, 1);
                } else { // FCVTN / FCVTN2: narrow
                    interp_vec packed;
                    memset(packed.byte, 0, sizeof packed.byte);
                    for (unsigned lane = 0; lane < narrow_lanes; lane++) {
                        uint64_t element_bits = interp_vec_element(&source, wide + 1u, lane);
                        interp_vec_set_element(&packed, narrow + 1u, lane, interp_fp_convert(wide, narrow, element_bits));
                    }
                    if (!q) {
                        interp_vec_write(cpu, rd, packed, 0);
                    } else {
                        interp_vec destination = interp_vec_read(cpu, rd);
                        memcpy(destination.byte + 8, packed.byte, 8);
                        interp_vec_write(cpu, rd, destination, 1);
                    }
                }
                cpu->pc = gpc + 4;
                return INTERP_NEXT;
            }
            if (fmt == INTERP_FP_D && !q && !scalar)
                return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- 2D form requires Q");
            // Spelled as an explicit per-width choice rather than as interp_vec_lanes(element, q): the element
            // width here is DERIVED (from sz, not from a size field), and a shift by a derived width leaves the
            // optimiser unable to see that the lane count and the element stride come from the same number --
            // which it reports as a possible out-of-bounds element access. Writing the two together makes the
            // correlation visible and is also simply clearer about what the two arrangements are.
            unsigned fp_lanes = scalar ? 1u : (element == 3u ? (q ? 2u : 1u) : (q ? 4u : 2u));
            for (unsigned lane = 0; lane < fp_lanes; lane++) {
                uint64_t a = interp_vec_element(&source, element, lane), value;
                uint64_t all_ones = interp_element_mask(element);
                if (opcode >= 0x0Cu && opcode <= 0x0Fu) {
                    // The compare-against-zero forms, plus FABS/FNEG which share opcode 01111 across U.
                    if (opcode == 0x0Fu) {
                        value = u ? (a ^ interp_fp_sign_mask(fmt)) : (a & ~interp_fp_sign_mask(fmt));
                    } else {
                        interp_fp_compare(cpu, fmt, a, 0, 0);
                        int ordered = !(interp_flag_c(cpu) && interp_flag_v(cpu));
                        int zero = interp_flag_z(cpu) != 0, negative = interp_flag_n(cpu) != 0;
                        int holds;
                        if (opcode == 0x0Cu)
                            holds = ordered && (u ? (!negative) : (!negative && !zero)); // FCMGE / FCMGT
                        else if (opcode == 0x0Du)
                            holds = ordered && (u ? (negative || zero) : zero); // FCMLE / FCMEQ
                        else
                            holds = ordered && negative && !u; // FCMLT
                        if (opcode == 0x0Eu && u)
                            return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated FP compare");
                        value = holds ? all_ones : UINT64_C(0);
                    }
                } else {
                    switch (opcode) {
                    case 0x18: // FRINTN (bit23 clear) / FRINTP (set), and FRINTA / FRINTX under U
                        value = interp_fp_round_integral(fmt, a, u ? INTERP_RM_RA : (high ? INTERP_RM_RP : INTERP_RM_RN),
                                                        0);
                        if (u && high) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated FRINT");
                        break;
                    case 0x19:
                        // FRINTM / FRINTZ (U == 0), FRINTX / FRINTI (U == 1). FRINTX is the only one of the
                        // four that reports Inexact, which is its whole reason for existing.
                        if (u)
                            value = interp_fp_round_integral(fmt, a, INTERP_FPCR_RMODE(g_interp_fpcr), high ? 0 : 1);
                        else
                            value = interp_fp_round_integral(fmt, a, high ? INTERP_RM_RZ : INTERP_RM_RM, 0);
                        break;
                    case 0x1A: // FCVTNS/FCVTNU (bit23 clear) / FCVTPS/FCVTPU (set)
                        value = interp_fp_to_int(fmt, a, interp_fp_width(fmt), !u, high ? INTERP_RM_RP : INTERP_RM_RN, 0);
                        break;
                    case 0x1B: // FCVTMS/FCVTMU (bit23 clear) / FCVTZS/FCVTZU (set)
                        value = interp_fp_to_int(fmt, a, interp_fp_width(fmt), !u, high ? INTERP_RM_RZ : INTERP_RM_RM, 0);
                        break;
                    case 0x1C: // FCVTAS/FCVTAU (bit23 clear); URECPE/URSQRTE (set) are estimates, see below
                        if (high) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- URECPE/URSQRTE");
                        value = interp_fp_to_int(fmt, a, interp_fp_width(fmt), !u, INTERP_RM_RA, 0);
                        break;
                    case 0x1D:
                        // SCVTF/UCVTF (bit23 clear). FRECPE/FRSQRTE (set) are the reciprocal ESTIMATES, whose
                        // result is an architecturally specified but coarse approximation; reported rather than
                        // approximated for the same reason as FRECPS above.
                        if (high) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- FRECPE/FRSQRTE");
                        value = interp_fp_from_int(fmt, a, interp_fp_width(fmt), !u, INTERP_FPCR_RMODE(g_interp_fpcr),
                                                  0);
                        break;
                    case 0x1F: // FSQRT (U == 1 only)
                        if (!u) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated opcode 11111");
                        value = interp_fp_sqrt(fmt, a);
                        break;
                    default: return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unimplemented FP opcode");
                    }
                }
                interp_vec_set_element(&result, element, lane, value);
            }
            cpu->nzcv = saved_nzcv;
            interp_vec_write(cpu, rd, result, q);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        switch (opcode) {
        case 0x02:   // SADDLP / UADDLP: pairwise add of adjacent lanes into elements of TWICE the width
        case 0x06: { // SADALP / UADALP: the same, accumulated into the destination
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated ADDLP size");
            unsigned wide = size + 1u, wide_lanes = scalar ? 1u : interp_vec_lanes(wide, q);
            uint64_t wide_mask = interp_element_mask(wide);
            interp_vec accumulate = interp_vec_read(cpu, rd);
            for (unsigned lane = 0; lane < wide_lanes; lane++) {
                uint64_t a = interp_vec_element(&source, size, lane * 2u);
                uint64_t b = interp_vec_element(&source, size, lane * 2u + 1u);
                if (!u) {
                    a = interp_element_sext(a, size);
                    b = interp_element_sext(b, size);
                }
                uint64_t total = (a + b) & wide_mask;
                if (opcode == 0x06) total = (total + interp_vec_element(&accumulate, wide, lane)) & wide_mask;
                interp_vec_set_element(&result, wide, lane, total);
            }
            interp_vec_write(cpu, rd, result, q);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        case 0x03:
            // SUQADD ("signed accumulator += unsigned operand, saturating as signed") and USQADD (the mirror).
            // Reported rather than guessed at: mixed-signedness saturation is not the same rule as either
            // SQADD's or UQADD's, and getting it wrong would be silent.
            return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- SUQADD/USQADD");
        case 0x04: { // CLS (U=0) / CLZ (U=1)
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated CLS/CLZ size");
            unsigned esize = 8u << size, lanes = scalar ? 1u : interp_vec_lanes(size, q);
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&source, size, lane) & interp_element_mask(size);
                // CLS counts the leading bits that MATCH the sign bit, not counting the sign bit itself, so
                // it is CLZ of the value with its sign folded out and is 0..esize-1 rather than 0..esize.
                uint64_t folded = ((a >> 1) ^ a) & (interp_element_mask(size) >> 1);
                unsigned count;
                if (!u)
                    count = folded == 0 ? esize - 1u
                                        : (unsigned)(esize - 2u - (unsigned)(63 - __builtin_clzll(folded)));
                else
                    count = a == 0 ? esize : (unsigned)(esize - 1u - (unsigned)(63 - __builtin_clzll(a)));
                interp_vec_set_element(&result, size, lane, count);
            }
            break;
        }
        case 0x07: { // SQABS (U=0) / SQNEG (U=1): saturating, because negating the most negative value overflows
            unsigned lanes = scalar ? 1u : interp_vec_lanes(size, q);
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&source, size, lane);
                // Expressed as 0 - a (SQNEG) or |a| = 0 - a when negative (SQABS), so the one overflowing input
                // -- the most negative value, whose negation is not representable -- saturates through the same
                // helper the rest of the group uses and sets FPSR.QC identically.
                int64_t x = (int64_t)interp_element_sext(a, size);
                uint64_t value;
                if (!u && x >= 0)
                    value = a & interp_element_mask(size);
                else
                    value = interp_sqadd_element(0, a, size, 1);
                interp_vec_set_element(&result, size, lane, value);
            }
            break;
        }
        case 0x12:   // XTN (U=0) / SQXTUN (U=1)
        case 0x14: { // SQXTN (U=0) / UQXTN (U=1)
            // Narrowing: `size` names the RESULT element and the source elements are twice as wide, so the
            // result is 64 bits and Q selects which half of the destination register receives it -- exactly
            // SHRN's shape. XTN simply truncates; the other three saturate, in three different senses.
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated XTN size");
            unsigned narrow_lanes = 64u / (8u << size);
            interp_vec packed;
            memset(packed.byte, 0, sizeof packed.byte);
            for (unsigned lane = 0; lane < (scalar ? 1u : narrow_lanes); lane++) {
                uint64_t wide_element = interp_vec_element(&source, size + 1u, lane);
                uint64_t value;
                if (opcode == 0x12 && !u)
                    value = wide_element & interp_element_mask(size); // XTN: plain truncation
                else if (opcode == 0x12)
                    value = interp_sat_narrow(wide_element, size, 1, 0); // SQXTUN: signed -> unsigned
                else
                    value = interp_sat_narrow(wide_element, size, u ? 0 : 1, u ? 0 : 1); // UQXTN / SQXTN
                interp_vec_set_element(&packed, size, lane, value);
            }
            if (!q || scalar) {
                interp_vec_write(cpu, rd, packed, 0);
            } else {
                interp_vec destination = interp_vec_read(cpu, rd);
                memcpy(destination.byte + 8, packed.byte, 8);
                interp_vec_write(cpu, rd, destination, 1);
            }
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        case 0x13: { // SHLL / SHLL2 (U=1): widen each element and shift left by the FULL element width
            if (!u || size == 3) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated SHLL");
            unsigned wide = size + 1u, wide_lanes = 64u / (8u << size);
            for (unsigned lane = 0; lane < wide_lanes; lane++) {
                uint64_t a = interp_vec_element(&source, size, q ? lane + wide_lanes : lane);
                interp_vec_set_element(&result, wide, lane,
                                      (a << (8u << size)) & interp_element_mask(wide));
            }
            interp_vec_write(cpu, rd, result, 1);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        default: break;
        }

        switch (opcode) {
        case 0x00:   // REV64 (U=0) / REV32 (U=1)
        case 0x01: { // REV16 (U=0)
            // Reverse the byte order within each container. The container is 8, 4 or 2 bytes wide depending on
            // the opcode; `size` gives the element width being reversed, which must be smaller.
            unsigned container = opcode == 0x01 ? 2u : (u ? 4u : 8u);
            unsigned element = 1u << size;
            if (element >= container)
                return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- REV element wider than container");
            for (unsigned base = 0; base < bytes; base += container)
                for (unsigned offset = 0; offset < container; offset += element)
                    memcpy(result.byte + base + (container - element - offset), source.byte + base + offset,
                           element);
            break;
        }
        case 0x05: { // CNT (U=0) / NOT i.e. MVN (U=1, size=00) / RBIT (U=1, size=01)
            if (!u) { // CNT: per-byte population count
                if (size != 0) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- CNT requires 8B/16B");
                for (unsigned index = 0; index < bytes; index++)
                    result.byte[index] = (uint8_t)__builtin_popcount(source.byte[index]);
            } else if (size == 0) { // NOT / MVN
                for (unsigned index = 0; index < bytes; index++)
                    result.byte[index] = (uint8_t)~source.byte[index];
            } else if (size == 1) { // RBIT: reverse the bits within each byte
                for (unsigned index = 0; index < bytes; index++) {
                    uint8_t value = source.byte[index];
                    value = (uint8_t)(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
                    value = (uint8_t)(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
                    value = (uint8_t)(((value & 0x0Fu) << 4) | ((value >> 4) & 0x0Fu));
                    result.byte[index] = value;
                }
            } else {
                return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated CNT/NOT/RBIT size");
            }
            break;
        }
        case 0x08:   // CMGT (zero, U=0) / CMGE (zero, U=1)
        case 0x09:   // CMEQ (zero, U=0) / CMLE (zero, U=1)
        case 0x0A: { // CMLT (zero, U=0)
            if (size == 3 && !q && !scalar)
                return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- 1D compare is reserved");
            uint64_t mask = interp_element_mask(size);
            for (unsigned lane = 0; lane < (scalar ? 1u : interp_vec_lanes(size, q)); lane++) {
                int64_t element = (int64_t)interp_element_sext(interp_vec_element(&source, size, lane), size);
                int holds;
                if (opcode == 0x08)
                    holds = u ? element >= 0 : element > 0;
                else if (opcode == 0x09)
                    holds = u ? element <= 0 : element == 0;
                else {
                    if (u) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- unallocated compare-zero");
                    holds = element < 0;
                }
                // A vector compare produces an all-ones element for true and all-zeroes for false, which is
                // what makes the following horizontal reduction a "was there a hit" test.
                interp_vec_set_element(&result, size, lane, holds ? mask : UINT64_C(0));
            }
            break;
        }
        case 0x0B: { // ABS (U=0) / NEG (U=1)
            if (size == 3 && !q && !scalar)
                return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- 1D ABS/NEG reserved");
            uint64_t mask = interp_element_mask(size);
            for (unsigned lane = 0; lane < (scalar ? 1u : interp_vec_lanes(size, q)); lane++) {
                int64_t element = (int64_t)interp_element_sext(interp_vec_element(&source, size, lane), size);
                uint64_t value = u ? (uint64_t)(-element) : (uint64_t)(element < 0 ? -element : element);
                interp_vec_set_element(&result, size, lane, value & mask);
            }
            break;
        }
        default: return interp_undefined(cpu, insn, "AdvSIMD two-register misc -- unimplemented opcode");
        }
        interp_vec_write(cpu, rd, result, q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD three different: the widening and narrowing group ----
    //   0 Q U 01110 size 1 Rm opcode(15:12) 00 Rn Rd
    // Separated from three-same (bit10 == 1) and from across-lanes/two-reg-misc (bits[11:10] == 10) by
    // bits[11:10] == 00 alone. This is the one structural shape nothing else in this file has: the SOURCE and
    // DESTINATION element widths differ, and `size` always names the NARROWER of the two. So for the widening
    // forms the sources are `size` and the result is `size + 1`, while for the narrowing ADDHN/SUBHN it is the
    // other way round -- the same width trap SHRN sets, in a group where every member steps on it.
    //
    // Q does not select a lane count here (it is fixed at 64 bits of the narrow side, i.e. 64/esize lanes).
    // It selects WHICH HALF of the 128-bit narrow-side register is used: the "2" mnemonics (ADDHN2, SMULL2)
    // read the upper half of their narrow sources, or write the upper half of their narrow destination while
    // leaving the lower half untouched. Getting that backwards silently corrupts half a register.
    if ((insn & 0x9F200C00u) == 0x0E200000u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 12) & 0xFu;
        interp_vec left = interp_vec_read(cpu, rn), right = interp_vec_read(cpu, rm);
        int narrowing = opcode == 0x4 || opcode == 0x6; // ADDHN/RADDHN and SUBHN/RSUBHN
        // PMULL's 64x64 -> 128 form is the only member whose result element is 128 bits wide, so it cannot go
        // through the element accessors at all and is handled on its own below.
        if (opcode == 0xE && size == 3) {
            uint64_t a, b, low, high;
            memcpy(&a, left.byte + (q ? 8 : 0), 8);
            memcpy(&b, right.byte + (q ? 8 : 0), 8);
            interp_poly_mul(a, b, 64, &low, &high);
            interp_vec result;
            memcpy(result.byte, &low, 8);
            memcpy(result.byte + 8, &high, 8);
            interp_vec_write(cpu, rd, result, 1);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (size == 3)
            return interp_undefined(cpu, insn, "AdvSIMD three different -- 64-bit narrow element is reserved");
        unsigned wide = size + 1u, lanes = 64u / (8u << size); // always 64 bits' worth of narrow elements
        uint64_t narrow_mask = interp_element_mask(size), wide_mask = interp_element_mask(wide);
        interp_vec result;
        memset(result.byte, 0, sizeof result.byte);
        interp_vec destination = interp_vec_read(cpu, rd);

        for (unsigned lane = 0; lane < lanes; lane++) {
            // The widening forms take their narrow operand(s) from the upper half when Q is set; the narrowing
            // forms read full-width sources from lane `lane` and place the result in the selected half.
            unsigned narrow_lane = q && !narrowing ? lane + lanes : lane;
            uint64_t a, b;
            if (narrowing) {
                a = interp_vec_element(&left, wide, lane);
                b = interp_vec_element(&right, wide, lane);
                uint64_t sum = (opcode == 0x4 ? a + b : a - b) & wide_mask;
                // RADDHN/RSUBHN round rather than truncate, by adding half of the discarded low field first.
                if (u) sum = (sum + (UINT64_C(1) << ((8u << size) - 1u))) & wide_mask;
                interp_vec_set_element(&result, size, lane, (sum >> (8u << size)) & narrow_mask);
                continue;
            }
            a = interp_vec_element(&left, opcode == 0x1 || opcode == 0x3 ? wide : size,
                                   opcode == 0x1 || opcode == 0x3 ? lane : narrow_lane);
            b = interp_vec_element(&right, size, narrow_lane);
            // Every widening form sign-extends when U == 0 and zero-extends when U == 1, except PMULL, whose
            // operands are polynomials with no sign at all.
            uint64_t extended_a = opcode == 0x1 || opcode == 0x3 ? a
                                  : (u ? a & narrow_mask : (interp_element_sext(a, size) & wide_mask));
            uint64_t extended_b = u ? (b & narrow_mask) : (interp_element_sext(b, size) & wide_mask);
            uint64_t value;
            switch (opcode) {
            case 0x0: value = extended_a + extended_b; break; // SADDL / UADDL
            case 0x1: value = extended_a + extended_b; break; // SADDW / UADDW (Rn is already wide)
            case 0x2: value = extended_a - extended_b; break; // SSUBL / USUBL
            case 0x3: value = extended_a - extended_b; break; // SSUBW / USUBW
            case 0x5:   // SABAL / UABAL: accumulate the absolute difference
            case 0x7: { // SABDL / UABDL: the absolute difference alone
                uint64_t difference;
                if (u) {
                    uint64_t x = a & narrow_mask, y = b & narrow_mask;
                    difference = x > y ? x - y : y - x;
                } else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    difference = (uint64_t)(x > y ? x - y : y - x);
                }
                value = difference;
                if (opcode == 0x5) value += interp_vec_element(&destination, wide, lane);
                break;
            }
            case 0x8:   // SMLAL / UMLAL
            case 0xA:   // SMLSL / UMLSL
            case 0xC: { // SMULL / UMULL
                uint64_t product;
                if (u)
                    product = (a & narrow_mask) * (b & narrow_mask);
                else
                    product = (uint64_t)((int64_t)interp_element_sext(a, size) * (int64_t)interp_element_sext(b, size));
                uint64_t base = interp_vec_element(&destination, wide, lane);
                value = opcode == 0x8 ? base + product : (opcode == 0xA ? base - product : product);
                break;
            }
            case 0xE: { // PMULL: polynomial multiply, 8x8 -> 16 (the 64x64 form is handled above)
                if (u || size != 0)
                    return interp_undefined(cpu, insn, "AdvSIMD three different -- unallocated PMULL form");
                uint64_t low, high;
                interp_poly_mul(a & narrow_mask, b & narrow_mask, 8, &low, &high);
                value = low;
                break;
            }
            default:
                return interp_undefined(cpu, insn,
                                        "AdvSIMD three different -- saturating doubling form (SQDMULL/SQDMLAL/"
                                        "SQDMLSL)");
            }
            interp_vec_set_element(&result, wide, lane, value & wide_mask);
        }
        if (!narrowing) {
            interp_vec_write(cpu, rd, result, 1); // a widening result is always a full 128-bit register
        } else if (!q) {
            interp_vec_write(cpu, rd, result, 0); // ADDHN writes the low 64 bits and ZEROES the upper half
        } else {
            memcpy(destination.byte + 8, result.byte, 8); // ADDHN2 writes the upper half, preserving the lower
            interp_vec_write(cpu, rd, destination, 1);
        }
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    // ---- AdvSIMD three same: the bitwise group, the compares, ADD/SUB, MIN/MAX, ADDP, shifts ----
    if ((insn & 0x9F200400u) == 0x0E200400u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 11) & 0x1Fu;
        interp_vec left = interp_vec_read(cpu, rn), right = interp_vec_read(cpu, rm), result;
        memset(result.byte, 0, sizeof result.byte);
        unsigned bytes = q ? 16u : 8u;
        unsigned lanes = scalar ? 1u : interp_vec_lanes(size, q);
        uint64_t mask = interp_element_mask(size);

        // ---- the floating-point members of this box ----
        // From opcode 11000 upward the fields are reinterpreted: bit23 (size<1>) stops being part of the
        // element width and becomes an operation selector, and bit22 (size<0>) is `sz` -- 0 for single, 1 for
        // double. That is why FADD and FSUB share one opcode and differ only in bit23, and it is the reason a
        // naive `size` read here would silently pick a 16-bit element for every single-precision form.
        if (opcode >= 0x18) {
            unsigned fmt = (size & 1u) ? INTERP_FP_D : INTERP_FP_S, high = (size >> 1) & 1u;
            if (fmt == INTERP_FP_D && !q && !scalar)
                return interp_undefined(cpu, insn, "AdvSIMD three same -- 2D form requires Q");
            unsigned fp_lanes = scalar ? 1u : interp_vec_lanes(fmt + 1u, q);
            unsigned element = fmt + 1u;
            interp_vec accumulate = interp_vec_read(cpu, rd);
            uint64_t sign = interp_fp_sign_mask(fmt);
            // interp_fp_compare writes NZCV because that is what the SCALAR FCMP family is defined to do. A
            // VECTOR compare must not touch the flags at all, so the incoming value is kept and restored below.
            uint64_t saved_nzcv = cpu->nzcv;
            for (unsigned lane = 0; lane < fp_lanes; lane++) {
                // The pairwise forms (FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP) draw both operands from the
                // CONCATENATION Vn:Vm rather than from matching lanes, exactly as the integer ADDP does.
                int pairwise = u && (opcode == 0x18 || opcode == 0x1A || opcode == 0x1E) &&
                               !(opcode == 0x1A && high);
                uint64_t a, b;
                if (pairwise) {
                    const interp_vec *source = lane < fp_lanes / 2u ? &left : &right;
                    unsigned base = (lane < fp_lanes / 2u ? lane : lane - fp_lanes / 2u) * 2u;
                    a = interp_vec_element(source, element, base);
                    b = interp_vec_element(source, element, base + 1u);
                } else {
                    a = interp_vec_element(&left, element, lane);
                    b = interp_vec_element(&right, element, lane);
                }
                uint64_t value;
                if (!u) {
                    switch (opcode) {
                    case 0x18: value = interp_fp_minmax(fmt, a, b, !high, 1); break; // FMAXNM / FMINNM
                    case 0x19: { // FMLA / FMLS: fused multiply-accumulate INTO the destination lane
                        uint64_t addend = interp_vec_element(&accumulate, element, lane);
                        value = interp_fp_muladd(fmt, addend, high ? (a ^ sign) : a, b);
                        break;
                    }
                    case 0x1A:
                        value = interp_fp_arith(fmt, high ? INTERP_FPOP_SUB : INTERP_FPOP_ADD, a, b);
                        break; // FADD / FSUB
                    case 0x1B:
                        // FMULX. Deliberately reported rather than approximated: it is FMUL with inf * 0
                        // redefined to +-2.0, and it exists only as the first step of a reciprocal Newton
                        // iteration whose remaining steps (FRECPE/FRECPS below) this backend also declines. A
                        // partially-correct reciprocal sequence is worse than an honest report, because it
                        // produces answers that are close but not equal to the JIT's.
                        return interp_undefined(cpu, insn, "AdvSIMD three same -- FMULX");
                    case 0x1C: { // FCMEQ (register)
                        if (high) return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated FP opcode");
                        interp_fp_compare(cpu, fmt, a, b, 0);
                        value = interp_flag_z(cpu) ? interp_element_mask(element) : UINT64_C(0);
                        break;
                    }
                    case 0x1E: value = interp_fp_minmax(fmt, a, b, !high, 0); break; // FMAX / FMIN
                    case 0x1F:
                        // FRECPS / FRSQRTS, the Newton-Raphson refinement steps. Reported for the same reason
                        // as FMULX: the architecture defines each as a SINGLE rounding of (2 - a*b) or
                        // (3 - a*b)/2 with its own special cases for zero times infinity, and an
                        // almost-right version of a reciprocal iteration is the worst possible outcome.
                        return interp_undefined(cpu, insn, "AdvSIMD three same -- FRECPS/FRSQRTS");
                    default: return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated FP opcode");
                    }
                } else {
                    switch (opcode) {
                    case 0x18: value = interp_fp_minmax(fmt, a, b, !high, 1); break; // FMAXNMP / FMINNMP
                    case 0x1A:
                        // FADDP when bit23 is clear, FABD (the absolute difference) when it is set -- and FABD
                        // is not pairwise, which is why the `pairwise` predicate above excludes it.
                        value = high ? (interp_fp_arith(fmt, INTERP_FPOP_SUB, a, b) & ~sign)
                                     : interp_fp_arith(fmt, INTERP_FPOP_ADD, a, b);
                        break;
                    case 0x1B:
                        if (high) return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated FP opcode");
                        value = interp_fp_arith(fmt, INTERP_FPOP_MUL, a, b);
                        break; // FMUL
                    case 0x1C: // FCMGE / FCMGT
                    case 0x1D: { // FACGE / FACGT: the same comparison on the ABSOLUTE values
                        uint64_t x = a, y = b;
                        if (opcode == 0x1D) {
                            x &= ~sign;
                            y &= ~sign;
                        }
                        interp_fp_compare(cpu, fmt, x, y, 0);
                        // "greater or equal" is C set with the unordered case excluded, and "greater" adds
                        // Z clear. A vector compare answers all-ones or all-zeroes, never a flag.
                        int ordered = !(interp_flag_c(cpu) && interp_flag_v(cpu));
                        int holds = ordered && interp_flag_c(cpu) && (!high || !interp_flag_z(cpu));
                        value = holds ? interp_element_mask(element) : UINT64_C(0);
                        break;
                    }
                    case 0x1E: value = interp_fp_minmax(fmt, a, b, !high, 0); break; // FMAXP / FMINP
                    case 0x1F:
                        if (high) return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated FP opcode");
                        value = interp_fp_arith(fmt, INTERP_FPOP_DIV, a, b);
                        break; // FDIV
                    default: return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated FP opcode");
                    }
                }
                interp_vec_set_element(&result, element, lane, value);
            }
            // A vector FP compare writes NZCV as a side effect of borrowing interp_fp_compare, which the
            // architecture does NOT do -- only the SCALAR FCMP family touches the flags. Restore them.
            if (opcode == 0x1C || opcode == 0x1D) cpu->nzcv = saved_nzcv;
            interp_vec_write(cpu, rd, result, q);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        if (opcode == 0x03) { // the purely bitwise group: size is a sub-opcode here, not an element width
            interp_vec destination = interp_vec_read(cpu, rd);
            for (unsigned index = 0; index < bytes; index++) {
                uint8_t a = left.byte[index], b = right.byte[index], d = destination.byte[index];
                uint8_t value;
                if (!u) {
                    switch (size) {
                    case 0: value = (uint8_t)(a & b); break;         // AND
                    case 1: value = (uint8_t)(a & ~b); break;        // BIC
                    case 2: value = (uint8_t)(a | b); break;         // ORR (and the MOV alias when Rn == Rm)
                    default: value = (uint8_t)(a | (uint8_t)~b); break; // ORN
                    }
                } else {
                    // The three insert/select forms all take the DESTINATION as a third operand, and which
                    // register supplies the mask differs -- which is exactly why they are easy to get backwards
                    // and why a mistake is invisible until a vectorised `?:` gives the opposite answer.
                    //   BSL  the DESTINATION is the mask:  Vd = Vd ? Vn : Vm
                    //   BIT  Vm is the mask, insert if true:  Vd = Vm ? Vn : Vd
                    //   BIF  Vm is the mask, insert if false: Vd = Vm ? Vd : Vn
                    // BSL had Vn and Vm the wrong way round here, so every `cmgt`/`fcmgt` + `bsl` pair -- which
                    // is what -O2 emits for a vectorised min/max or clamp -- selected the operand the guest did
                    // NOT ask for and silently computed a minimum where the source said maximum.
                    switch (size) {
                    case 0: value = (uint8_t)(a ^ b); break;                          // EOR
                    case 1: value = (uint8_t)((a & d) | (b & (uint8_t)~d)); break;     // BSL
                    case 2: value = (uint8_t)(d ^ ((d ^ a) & b)); break;               // BIT
                    default: value = (uint8_t)(d ^ ((d ^ a) & (uint8_t)~b)); break;    // BIF
                    }
                }
                result.byte[index] = value;
            }
            interp_vec_write(cpu, rd, result, q);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        // The vector group reserves a 64-bit element with Q == 0 (there is no 1D arrangement); the SCALAR
        // spelling of the same encoding is precisely the D-register form, so it must not be rejected here.
        if (size == 3 && !q && !scalar && opcode != 0x10)
            return interp_undefined(cpu, insn, "AdvSIMD three same -- reserved 1D form");

        switch (opcode) {
        case 0x00:   // SHADD / UHADD: halving add, which cannot overflow because it halves first
        case 0x02:   // SRHADD / URHADD: rounding halving add
        case 0x04: { // SHSUB / UHSUB
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane), b = interp_vec_element(&right, size, lane);
                uint64_t value;
                if (u) {
                    a &= mask;
                    b &= mask;
                    if (opcode == 0x04)
                        value = (a - b) >> 1; // the borrow bit is exactly what the shift discards
                    else
                        // (a + b) can carry out of a 64-bit element, so the sum is formed as the average
                        // directly: (a & b) + ((a ^ b) >> 1) is (a + b) >> 1 without the intermediate carry.
                        value = (a & b) + (((a ^ b) >> 1) & (mask >> 1)) + (opcode == 0x02 ? ((a ^ b) & 1u) : 0u);
                } else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    if (opcode == 0x04)
                        value = (uint64_t)((x - y) >> 1);
                    else
                        value = (uint64_t)((x & y) + ((x ^ y) >> 1) + (opcode == 0x02 ? ((x ^ y) & 1) : 0));
                }
                interp_vec_set_element(&result, size, lane, value & mask);
            }
            break;
        }
        case 0x01:   // SQADD / UQADD
        case 0x05: { // SQSUB / UQSUB
            int subtract = opcode == 0x05;
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane), b = interp_vec_element(&right, size, lane);
                interp_vec_set_element(&result, size, lane,
                                      u ? interp_uqadd_element(a, b, size, subtract)
                                        : interp_sqadd_element(a, b, size, subtract));
            }
            break;
        }
        case 0x09:   // SQSHL / UQSHL: a saturating variable left shift (a right shift when Rm's lane is negative)
        case 0x0A:   // SRSHL / URSHL: rounding variable shift
        case 0x0B: { // SQRSHL / UQRSHL
            unsigned esize = 8u << size;
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane);
                int8_t amount = (int8_t)(interp_vec_element(&right, size, lane) & 0xFFu);
                uint64_t value;
                if (amount >= 0) {
                    unsigned shift = (unsigned)amount;
                    if (opcode == 0x0A) { // SRSHL/URSHL left: an exact shift, wrapping like SSHL/USHL
                        value = shift >= esize ? 0 : (a << shift) & mask;
                    } else if (u) {
                        uint64_t saturated = (a & mask);
                        // Saturate when any bit would leave the element, which is the same test as "the value
                        // no longer fits after the shift".
                        if (shift >= esize ? saturated != 0 : (saturated >> (esize - shift)) != 0) {
                            interp_fpsr_raise(INTERP_FPSR_QC);
                            value = mask;
                        } else {
                            value = (saturated << shift) & mask;
                        }
                    } else {
                        int64_t x = (int64_t)interp_element_sext(a, size);
                        int64_t max = esize == 64 ? INT64_MAX : (int64_t)((UINT64_C(1) << (esize - 1u)) - 1u);
                        int64_t min = esize == 64 ? INT64_MIN : -max - 1;
                        int64_t shifted = x;
                        int overflowed = 0;
                        for (unsigned step = 0; step < shift && !overflowed; step++) {
                            if (shifted > (max >> 1) || shifted < (min >> 1) || (shifted << 1) >> 1 != shifted)
                                overflowed = 1;
                            else
                                shifted <<= 1;
                        }
                        if (overflowed || shifted > max || shifted < min) {
                            interp_fpsr_raise(INTERP_FPSR_QC);
                            shifted = x < 0 ? min : max;
                        }
                        value = (uint64_t)shifted & mask;
                    }
                } else {
                    // A negative amount is a right shift, which never saturates. The rounding variants add half
                    // of the discarded field first, which is what distinguishes SRSHL from SSHL.
                    unsigned shift = (unsigned)(-amount);
                    int rounding = opcode == 0x0A || opcode == 0x0B;
                    if (u) {
                        uint64_t x = a & mask;
                        uint64_t round = rounding && shift <= 64u && shift > 0 ? (x >> (shift - 1u)) & 1u : 0u;
                        value = shift >= esize ? round : ((x >> shift) + round);
                    } else {
                        int64_t x = (int64_t)interp_element_sext(a, size);
                        uint64_t round = rounding && shift > 0 ? (uint64_t)((x >> (shift >= 64u ? 63u : shift - 1u)) & 1)
                                                              : 0u;
                        int64_t shifted = shift >= esize ? (x >> (esize - 1u)) : (x >> shift);
                        value = (uint64_t)shifted + round;
                    }
                    value &= mask;
                }
                interp_vec_set_element(&result, size, lane, value);
            }
            break;
        }
        case 0x0E:   // SABD / UABD: absolute difference
        case 0x0F: { // SABA / UABA: absolute difference accumulated into the destination
            interp_vec accumulate = interp_vec_read(cpu, rd);
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane), b = interp_vec_element(&right, size, lane);
                uint64_t difference;
                if (u) {
                    a &= mask;
                    b &= mask;
                    difference = a > b ? a - b : b - a;
                } else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    difference = (uint64_t)(x > y ? x - y : y - x);
                }
                if (opcode == 0x0F) difference += interp_vec_element(&accumulate, size, lane);
                interp_vec_set_element(&result, size, lane, difference & mask);
            }
            break;
        }
        case 0x16: { // SQDMULH / SQRDMULH: doubled high half of the product, saturating
            if (size == 0 || size == 3)
                return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated SQDMULH element size");
            unsigned esize = 8u << size;
            for (unsigned lane = 0; lane < lanes; lane++) {
                int64_t x = (int64_t)interp_element_sext(interp_vec_element(&left, size, lane), size);
                int64_t y = (int64_t)interp_element_sext(interp_vec_element(&right, size, lane), size);
                // esize is 16 or 32 here, so 2*x*y fits in int64 with room and no wide type is needed.
                int64_t product = 2 * x * y;
                if (u) product += (int64_t)(UINT64_C(1) << (esize - 1u)); // SQRDMULH rounds
                int64_t narrowed = product >> esize;
                int64_t max = (int64_t)((UINT64_C(1) << (esize - 1u)) - 1u), min = -max - 1;
                if (narrowed > max) {
                    interp_fpsr_raise(INTERP_FPSR_QC);
                    narrowed = max;
                } else if (narrowed < min) {
                    interp_fpsr_raise(INTERP_FPSR_QC);
                    narrowed = min;
                }
                interp_vec_set_element(&result, size, lane, (uint64_t)narrowed & mask);
            }
            break;
        }
        case 0x06:   // CMGT (U=0) / CMHI (U=1)
        case 0x07:   // CMGE (U=0) / CMHS (U=1)
        case 0x11: { // CMTST (U=0) / CMEQ (U=1)
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane), b = interp_vec_element(&right, size, lane);
                int holds;
                if (opcode == 0x11)
                    holds = u ? a == b : (a & b) != 0;
                else if (u)
                    holds = opcode == 0x06 ? a > b : a >= b;
                else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    holds = opcode == 0x06 ? x > y : x >= y;
                }
                interp_vec_set_element(&result, size, lane, holds ? mask : UINT64_C(0));
            }
            break;
        }
        case 0x08: { // SSHL / USHL: element-wise shift by the LOW BYTE of the corresponding lane of Rm
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane);
                int8_t amount = (int8_t)(interp_vec_element(&right, size, lane) & 0xFFu);
                unsigned esize = 8u << size;
                uint64_t value;
                if (amount >= 0) {
                    value = (unsigned)amount >= esize ? 0 : (a << amount);
                } else {
                    unsigned shift = (unsigned)(-amount);
                    if (u)
                        value = shift >= esize ? 0 : (a >> shift);
                    else {
                        int64_t signed_a = (int64_t)interp_element_sext(a, size);
                        value = (uint64_t)(shift >= esize ? (signed_a >> (esize - 1)) : (signed_a >> shift));
                    }
                }
                interp_vec_set_element(&result, size, lane, value & mask);
            }
            break;
        }
        case 0x0C:   // SMAX / UMAX
        case 0x0D: { // SMIN / UMIN
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane), b = interp_vec_element(&right, size, lane);
                uint64_t chosen;
                if (u)
                    chosen = opcode == 0x0C ? (a > b ? a : b) : (a < b ? a : b);
                else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    chosen = (opcode == 0x0C ? (x > y) : (x < y)) ? a : b;
                }
                interp_vec_set_element(&result, size, lane, chosen);
            }
            break;
        }
        case 0x10: { // ADD (U=0) / SUB (U=1)
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t a = interp_vec_element(&left, size, lane), b = interp_vec_element(&right, size, lane);
                interp_vec_set_element(&result, size, lane, (u ? a - b : a + b) & mask);
            }
            break;
        }
        case 0x17: { // ADDP: pairwise add across the CONCATENATION of Rn and Rm
            if (u) return interp_undefined(cpu, insn, "AdvSIMD three same -- unallocated ADDP U bit");
            for (unsigned lane = 0; lane < lanes; lane++) {
                const interp_vec *source = lane < lanes / 2 ? &left : &right;
                unsigned base = (lane < lanes / 2 ? lane : lane - lanes / 2) * 2u;
                uint64_t a = interp_vec_element(source, size, base);
                uint64_t b = interp_vec_element(source, size, base + 1u);
                interp_vec_set_element(&result, size, lane, (a + b) & mask);
            }
            break;
        }
        case 0x12: { // MLA (U=0) / MLS (U=1): multiply-accumulate into the destination
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD three same -- 64-bit element MLA/MLS");
            interp_vec accumulate = interp_vec_read(cpu, rd);
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t product = interp_vec_element(&left, size, lane) * interp_vec_element(&right, size, lane);
                uint64_t base = interp_vec_element(&accumulate, size, lane);
                interp_vec_set_element(&result, size, lane, (u ? base - product : base + product) & mask);
            }
            break;
        }
        case 0x13: { // MUL (U=0) / PMUL (U=1), which is carry-less polynomial multiply, not a scaled product
            if (u) {
                if (size != 0) return interp_undefined(cpu, insn, "AdvSIMD three same -- PMUL requires 8B/16B");
                for (unsigned lane = 0; lane < lanes; lane++) {
                    uint64_t low, high;
                    interp_poly_mul(interp_vec_element(&left, 0, lane), interp_vec_element(&right, 0, lane), 8,
                                    &low, &high);
                    interp_vec_set_element(&result, 0, lane, low & 0xFFu);
                }
                break;
            }
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD three same -- 64-bit element MUL");
            for (unsigned lane = 0; lane < lanes; lane++)
                interp_vec_set_element(&result, size, lane,
                                       (interp_vec_element(&left, size, lane) *
                                        interp_vec_element(&right, size, lane)) & mask);
            break;
        }
        case 0x14: { // SMAXP/UMAXP (U selects signedness) -- pairwise, the same shape as ADDP
            for (unsigned lane = 0; lane < lanes; lane++) {
                const interp_vec *source = lane < lanes / 2 ? &left : &right;
                unsigned base = (lane < lanes / 2 ? lane : lane - lanes / 2) * 2u;
                uint64_t a = interp_vec_element(source, size, base);
                uint64_t b = interp_vec_element(source, size, base + 1u);
                uint64_t chosen;
                if (u)
                    chosen = a > b ? a : b;
                else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    chosen = x > y ? a : b;
                }
                interp_vec_set_element(&result, size, lane, chosen);
            }
            break;
        }
        case 0x15: { // SMINP / UMINP
            for (unsigned lane = 0; lane < lanes; lane++) {
                const interp_vec *source = lane < lanes / 2 ? &left : &right;
                unsigned base = (lane < lanes / 2 ? lane : lane - lanes / 2) * 2u;
                uint64_t a = interp_vec_element(source, size, base);
                uint64_t b = interp_vec_element(source, size, base + 1u);
                uint64_t chosen;
                if (u)
                    chosen = a < b ? a : b;
                else {
                    int64_t x = (int64_t)interp_element_sext(a, size), y = (int64_t)interp_element_sext(b, size);
                    chosen = x < y ? a : b;
                }
                interp_vec_set_element(&result, size, lane, chosen);
            }
            break;
        }
        default: return interp_undefined(cpu, insn, "AdvSIMD three same -- unimplemented opcode");
        }
        interp_vec_write(cpu, rd, result, q);
        cpu->pc = gpc + 4;
        return INTERP_NEXT;
    }

    return interp_undefined(cpu, insn, "scalar floating-point and Advanced SIMD");
}

// One guest instruction. Fetches, decodes, executes, and leaves cpu->pc naming the NEXT instruction (or the
// branch target). Returns INTERP_NEXT to stay in the block or INTERP_END with cpu->reason set.
//
// The switch is the ARM ARM's top-level encoding table on op0 = insn[28:25], in its order. Keeping that
// shape -- rather than a flat list of masks -- is what makes the unimplemented groups obvious and lets each
// be filled in independently.
static int interp_step(struct cpu *cpu) {
    uint32_t insn = 0;
    if (hl_guest_fetch_u32(cpu->pc, &insn) != 0) {
        // The instruction itself could not be read: a logical executable mapping refused the fetch, or the
        // page is gone. This is the interpreter's equivalent of the JIT's R_FETCHFAULT exit stub, and the
        // dispatcher turns it into a guest SIGSEGV at exactly this PC.
        cpu->fault_addr = cpu->pc;
        cpu->reason = R_FETCHFAULT;
        return INTERP_END;
    }
    switch ((insn >> 25) & 0xF) {
    case 0x0:
        // op0 == 0000 is reserved; the all-zero word (UDF #0) is what execution off the end of a .text
        // section runs into, so naming it specifically makes that failure legible.
        return interp_undefined(cpu, insn, insn == 0 ? "reserved -- UDF #0 (executed zero bytes)" : "reserved");
    case 0x1:
        return interp_undefined(cpu, insn, "unallocated (SME)");
    case 0x2:
        return interp_undefined(cpu, insn, "SVE");
    case 0x3:
        return interp_undefined(cpu, insn, "unallocated");
    case 0x8:
    case 0x9:
        return interp_exec_dp_immediate(cpu, insn);
    case 0xA:
    case 0xB:
        return interp_exec_branch_system(cpu, insn);
    case 0x4:
    case 0x6:
    case 0xC:
    case 0xE:
        return interp_exec_load_store(cpu, insn);
    case 0x5:
    case 0xD:
        return interp_exec_dp_register(cpu, insn);
    default:
        return interp_exec_simd(cpu, insn);
    }
}

// The pre-scan's view of "this instruction ends a block". It must answer 1 for every encoding interp_step
// answers INTERP_END for, and it is allowed to answer 1 for more.
//
// A disagreement is safe in both directions but wasteful, which is worth being explicit about. If the scan
// ends a block EARLY, the descriptor covers fewer instructions than the guest will run straight through, and
// run_block simply returns R_BRANCH at the boundary -- indistinguishable from a chain exit. If the scan ends
// it LATE (because the bytes changed between translation and execution), run_block stops at the real branch
// and never reads past it. What is NOT allowed is for the recorded source range to be a SUBSET of the bytes
// actually executed, because map_put's range is what SMC invalidation tests -- so the scan must never skip
// an instruction it decoded.
static int interp_block_ends(uint32_t insn) {
    if ((insn & 0x7C000000u) == 0x14000000u) return 1;  // B / BL
    if ((insn & 0xFF000010u) == 0x54000000u) return 1;  // B.cond
    if ((insn & 0xFF000010u) == 0x54000010u) return 1;  // BC.cond
    if ((insn & 0x7E000000u) == 0x34000000u) return 1;  // CBZ / CBNZ
    if ((insn & 0x7E000000u) == 0x36000000u) return 1;  // TBZ / TBNZ
    if ((insn & 0xFE000000u) == 0xD6000000u) return 1;  // BR / BLR / RET / ERET
    if ((insn & 0xFF000000u) == 0xD4000000u) return 1;  // SVC / BRK / HLT / ...
    if ((insn & 0xFFFFFFE0u) == 0xD50B7520u) return 1;  // ic ivau -> R_ICFLUSH
    if ((insn & 0xFFFFF01Fu) == 0xD503301Fu && ((insn >> 5) & 7) == 6) return 1; // ISB -> R_ICCOMMIT
    return 0;
}

// ---------------------------------------------------------------------------
// The block descriptor, and translate_block.
// ---------------------------------------------------------------------------
// Bound on the guest instructions one descriptor may cover. A block is capped rather than unbounded for the
// same reason the JIT caps its emitted size: the descriptor must be admitted within the arena headroom the
// dispatcher guarantees, and an unbounded straight-line run (generated code can contain tens of thousands of
// instructions before its first branch) would otherwise make one block's interpretation unpreemptible by the
// c->irq poll for an arbitrarily long time. Splitting at the cap is exactly an ordinary chain exit.
#define INTERP_BLOCK_MAX_INSNS 4096u

// WHY THE DESCRIPTOR ONLY DELIMITS, AND DOES NOT CACHE DECODED INSTRUCTIONS.
//
// The dispatcher's only requirement is that translate_block return a DISTINCT NON-NULL pointer per guest PC:
// map_host() returning non-NULL is what suppresses re-translation, so a NULL or a shared pointer would either
// re-translate forever or alias two guest PCs onto one cache entry. It is allocated from the arena's existing
// bump pointer (g_cp inside g_cache/CACHE_SZ) rather than from malloc so that ALL the existing accounting
// keeps working untouched: the dispatcher's flush-on-full test, jit_resolve_rw_code's RW->RX resolution,
// generation tagging, and the stop-the-world retire/reclaim discipline all reason about "is this address in
// the current arena", and a heap pointer would silently fall out of every one of them.
//
// The contents are just the guest range. run_block re-fetches and re-decodes each instruction on every
// execution. That is the simple and correct choice, and it has one real advantage worth keeping in mind when
// it is optimised: because the guest's own bytes are the only source of truth at execution time,
// self-modifying code is coherent almost by construction -- only the block EXTENT can go stale, which is
// what the ic-ivau/ISB path above handles.
//
// Caching the decoded form here (an array of {handler, pre-extracted operands} appended after the header) is
// the obvious later optimisation and would remove the per-execution fetch, mask and branch tree. It would
// also make SMC coherence load-bearing rather than incidental, so it must land together with a test that a
// rewritten line really does invalidate the decoded array.
#define INTERP_BLOCK_MAGIC UINT64_C(0x484C494E54455250) // "HLINTERP"

struct interp_block {
    uint64_t magic;       // guards against executing a foreign/stale descriptor (see run_block)
    uint64_t guest_start; // entry guest PC == the map key
    uint64_t guest_end;   // one past the last instruction the pre-scan decoded
    uint64_t insn_count;  // guest instructions in [guest_start, guest_end); diagnostics only
};

// Called by the dispatcher only under G_BLOCK_ALIGN, which interp_dispatch.h defines as the literal 0 -- so
// this is unreachable. It must nonetheless exist, because the call is compiled (inside `if (0)`, not inside
// `#if 0`) and an omission would be a link error a long way from its cause. The body is the honest one: a
// 4-byte arena write, so that if anyone ever turns entry alignment back on the bump pointer stays consistent
// even though the padding can never execute on this host.
static void emit32(uint32_t instruction) {
    memcpy(g_cp, &instruction, sizeof instruction);
    g_cp += sizeof instruction;
}

// There is no second tier. The JIT's tier-2 promotion exists to fold a hot self-loop's back-edge into a
// single host conditional branch, which presupposes emitted host branches; the interpreter's back-edge is an
// assignment to cpu->pc and there is nothing to fold. It is never reached either -- R_TIER2 is only ever
// raised by an emitted in-cache counter -- but core/dispatch.c calls it unconditionally after every block,
// so the symbol must exist.
static void tier2_promote(uint64_t gpc) {
    (void)gpc;
}

static void *translate_block(uint64_t gpc) {
    HL_LOGF(&g_jit_log, HL_LOG_TAG_TRANSLATE, "isa=aarch64 backend=interp guest_pc=%#llx", (unsigned long long)gpc);
    // Observe writes made through another MAP_SHARED alias before reading an executable view that is backed
    // by an emulated host-page snapshot -- the same reason translate.c does this before decoding.
    uint64_t source_page = gpc & ~UINT64_C(0xFFF);
    filemap_refresh_emulated(source_page, source_page + UINT64_C(0x1000));

    // Delimit the block. The scan stops after a block-ending instruction, at the instruction cap, or at the
    // first byte it cannot fetch (in which case the range still covers that instruction, so run_block reaches
    // it and raises R_FETCHFAULT at the right PC rather than silently ending the block one early).
    uint64_t cursor = gpc;
    uint64_t count = 0;
    while (count < INTERP_BLOCK_MAX_INSNS) {
        uint32_t insn = 0;
        count++;
        cursor += 4;
        if (hl_guest_fetch_u32(cursor - 4, &insn) != 0) break;
        if (interp_block_ends(insn)) break;
    }

    // Allocate the descriptor from the arena bump pointer. The dispatcher has already guaranteed
    // CACHE_EMIT_HEADROOM bytes of room (it flushes wholesale, or stops the world and rotates to a fresh
    // arena, before calling us), so this cannot overflow; the check is here because "cannot" is a property of
    // a caller, and scribbling past the arena would corrupt whatever the kernel mapped after it.
    while ((uintptr_t)g_cp & 15u)
        *g_cp++ = 0;
    if (g_cp + sizeof(struct interp_block) > g_cache + CACHE_SZ) {
        static const char message[] = "interpreter block descriptor does not fit the code arena";
        (void)jit_fail(HL_STATUS_OUT_OF_MEMORY, message, sizeof message - 1u);
        return NULL;
    }
    struct interp_block *block = (struct interp_block *)g_cp;
    g_cp += sizeof *block;
    block->magic = INTERP_BLOCK_MAGIC;
    block->guest_start = gpc;
    block->guest_end = cursor;
    block->insn_count = count;

    // Register the translation exactly as the JIT does, with the same argument meanings: the key is the entry
    // PC, `host` is what map_host()/the dispatcher will hand to run_block, and [guest_start, guest_end) is
    // the decoded SOURCE interval that map_invalidate_source_ranges() intersects against a rewritten line.
    // `body` is the JIT's chained/indirect entry point, which is the same address as the descriptor here --
    // there is no prologue to skip. Passing the descriptor for both keeps map_body() non-NULL, which
    // G_DISPATCH_CHAIN's patch_links_to() treats as "this PC has a live translation" (it then finds no
    // pending links to patch, because nothing here ever records one).
    map_put(gpc, gpc, cursor, block, block);
    // SMC precise gate. txpg_mark records the guest pages this block sourced, and the 64-byte line set is the
    // finer gate txln_flush_class() classifies an `ic ivau` against. Both must be populated for the same
    // reason as in the JIT: a flush of a line we never translated has nothing stale to drop, and a flush of a
    // line we DID translate must reach map_invalidate_source_ranges. Without this the interpreter's cached
    // block EXTENT would survive a rewrite of the very branch that determined it.
    txpg_mark(gpc, cursor);
    if (g_txln_active)
        for (uint64_t line = gpc >> 6; line <= (cursor - 1) >> 6; line++)
            txln_put(line);
    return block;
}

// ---------------------------------------------------------------------------
// run_block / block_return -- the boundary the dispatcher calls through.
// ---------------------------------------------------------------------------
// interp_dispatch.h defines G_OWN_TRAMPOLINES so core/dispatch.c does not emit its AArch64 assembly pair,
// and these are what it calls instead. They are plain C: there is no host register file holding guest state,
// so there is nothing to spill on entry and nothing to restore on exit, and cpu->host_sp / host_save[] /
// host_v[] stay untouched (they remain part of the checkpoint image, which is why they are still in
// struct cpu at all).
//
// LOCAL linkage, and that is load-bearing rather than tidiness. __attribute__((visibility("hidden"))) is not
// local linkage: it controls export from a shared object, but the symbol stays STB_GLOBAL inside a static link.
// The dual activation archive links BOTH per-guest-ISA target objects into one binary, and core/target/
// namespace.h -- which renames the per-ISA symbols precisely so the two can coexist -- does not cover these
// two names, so a global run_block in each interpreter is a multiple definition. `static` also matches what the
// JIT already does: its trampolines come from a file-scope __asm__ block containing `.hidden`, which the
// assembler emits as a genuinely local symbol (nm reports a lowercase `t`, not `T`).
//
// Nothing is lost. interp.c is #included into the target translation unit, so every caller is in this same TU:
// core/dispatch.c's run_block(c, code) call and core/target/aarch64.c taking &block_return. Taking the address
// of a static function within its TU is ordinary C.
static void run_block(struct cpu *cpu, void *code);
static void block_return(void);

static void run_block(struct cpu *cpu, void *code) {
    const struct interp_block *block = (const struct interp_block *)code;
    if (block == NULL || block->magic != INTERP_BLOCK_MAGIC) {
        // Not a descriptor this backend wrote. The only way to get here is a restored persistent cache or
        // checkpoint whose arena holds JIT-emitted host code -- both of which are supposed to be rejected on
        // host-ISA identity (see pcache_engine_id below), so reaching this is an identity bug and must not be
        // papered over by executing whatever the bytes happen to say.
        static const char message[] = "interpreter entered a block that it did not translate";
        (void)jit_fail(HL_STATUS_CORRUPT, message, sizeof message - 1u);
        cpu->reason = R_BRANCH;
        return;
    }

    // The fault marker. sigsetjmp with savemask=1 so a siglongjmp out of the SIGSEGV/SIGBUS handler restores
    // the mask the handler was entered with; see the fault-model comment at the top of this file for why
    // jumping out of a handler is safe here.
    if (sigsetjmp(g_interp_marker_jmp, 1) != 0) {
        // Arrived from interp_signal_resume. The abandoned access left no partial architectural state, and
        // the handler has already set cpu->reason (plus sync_signal/sync_code and the pending-signal bit) --
        // so returning is all that is left to do. The dispatcher delivers the guest signal.
        g_interp_access_active = 0;
        g_interp_marker_armed = 0;
        g_interp_marker_cpu = NULL;
        return;
    }
    g_interp_marker_cpu = cpu;
    g_interp_marker_armed = 1;

    uint64_t executed = 0;
    for (;;) {
        // Async-interrupt poll, the interpreter's form of the JIT's 2-instruction ldr+cbz block header: a
        // caught guest signal that became pending while this thread spins in a loop with no syscalls must
        // still reach the dispatcher at a safe boundary. Polled AFTER at least one instruction has retired,
        // which is what guarantees forward progress -- an exit with cpu->pc unchanged would have the
        // dispatcher hand us the same block again forever. cpu->irq is cleared by the dispatcher each
        // iteration, so a masked-but-undeliverable signal cannot bounce us every time round.
        if (executed && cpu->irq) {
            cpu->reason = R_BRANCH;
            break;
        }
        // Leaving the descriptor's range is an ordinary chain exit: the guest ran straight through to the
        // next block (or past the instruction cap). cpu->pc already names where to resume.
        if (cpu->pc < block->guest_start || cpu->pc >= block->guest_end) {
            cpu->reason = R_BRANCH;
            break;
        }
        if (interp_step(cpu) == INTERP_END) break;
        executed++;
    }

    g_interp_marker_armed = 0;
    g_interp_marker_cpu = NULL;
}

// The far end of a bridge that does not exist here. Under the JIT, emitted blocks branch to block_return to
// unwind the host callee-saved state that run_block spilled; nothing in this backend's arena is executable,
// so nothing can branch here. It must still be a real, address-taken symbol because core/target/aarch64.c's
// sigframe_resume_dispatch bakes its address as the PC to resume a captured fault at -- and once that call
// site is routed to interp_signal_resume (which siglongjmps instead), even that use disappears.
//
// Aborting rather than returning is the point: a silent return would leave cpu->reason holding the previous
// block's value and send the dispatcher round the loop against stale state, turning a specific bug into an
// unbounded spin somewhere else.
static void block_return(void) {
    fprintf(stderr, "hl: block_return() entered under the aarch64 interpreter backend on a " HL_HOST_CPU_NAME
                    " host.\n"
                    "    Nothing in the code arena is executable here, so no translated block can have branched\n"
                    "    to this address -- it was baked into something that then ran, which means a stale\n"
                    "    persistent-cache image or a checkpoint written by the JIT was accepted.\n");
    abort();
}

// ---------------------------------------------------------------------------
// Self-modifying guest code.
// ---------------------------------------------------------------------------
// Same model as the JIT, and it must be, because the reason codes and the queue in struct cpu are part of the
// checkpoint image: the guest is architecturally required to run the icache-maintenance dance before
// executing freshly written bytes, the frontend intercepts `ic ivau` to record the dirty line, and the ISB
// that ends the dance is where the recorded lines are acted on.
static void smc_queue_line(struct cpu *c, uint64_t address) {
    // An ET_EXEC image's code is mapped at a high collision-avoidance bias while its architectural pointers
    // stay link-time low. Translation-map source intervals use the real executable address, so normalize an
    // ic-ivau operand the same way instruction dispatch does before classifying it.
    if (g_nonpie_lo && address >= g_nonpie_lo && address < g_nonpie_hi) address += g_nonpie_bias;
    uint64_t start = address & ~UINT64_C(63), end = start + 64;
    for (uint32_t i = 0; i < c->smc_range_count; i++) {
        if (end < c->smc_ranges[i][0] || start > c->smc_ranges[i][1]) continue;
        if (start < c->smc_ranges[i][0]) c->smc_ranges[i][0] = start;
        if (end > c->smc_ranges[i][1]) c->smc_ranges[i][1] = end;
        return;
    }
    if (c->smc_range_count == SMC_RANGE_CAP) {
        c->smc_range_overflow = 1;
        return;
    }
    c->smc_ranges[c->smc_range_count][0] = start;
    c->smc_ranges[c->smc_range_count][1] = end;
    c->smc_range_count++;
}

static void aarch64_smc_queue_range(uint64_t first, uint64_t last, void *opaque) {
    struct cpu *c = opaque;
    for (uint64_t line = first & ~UINT64_C(63); line < last;) {
        smc_queue_line(c, line);
        if (line > UINT64_MAX - 64) break;
        line += 64;
    }
}

// Declared in abi.h and reached through G_SMC_COPYOUT: a syscall that copies to user memory writes guest
// bytes without going through any instruction this backend decoded, so a write that lands on code the
// interpreter has a cached EXTENT for must be queued here. This is NOT inert on the interpreter -- it is the
// same real work the JIT does -- because the block descriptor caches where a block ends, and a copyout can
// move the branch that determined it.
static void aarch64_smc_copyout(uint64_t first, uint64_t last) {
    if (last <= first) return;
    struct cpu *c = pthread_getspecific(g_cpu_key);
    if (c == NULL) return;
    aarch64_smc_queue_range(first, last, c);
    hl_logical_vma_visit_exec_aliases(first, last, aarch64_smc_queue_range, c);
}

// A guest `ic ivau` reached the dispatcher (R_ICFLUSH). Queue only; smc_commit() below owns activation,
// membership and content classification under g_jit_lock, so that a line whose bytes changed is classified
// once rather than immediately looking unchanged on a second observation.
static void smc_icflush(struct cpu *c, uint64_t va) {
    // Latch unconditionally, even when the line turns out never to have been translated: g_smc_seen is what
    // the rest of the engine reads to mean "this guest generates code", and it must not depend on the
    // outcome of the precise gate.
    __atomic_store_n(&g_smc_seen, 1, __ATOMIC_RELEASE);
    smc_queue_line(c, va);
}

// The guest's ISB (R_ICCOMMIT): act on every line queued since the last commit. This is the JIT's
// implementation, and the reuse is deliberate -- map_invalidate_source_ranges()/map_clear() are exactly the
// right primitives here, because what has to be dropped is not host code (there is none) but the gpc->
// descriptor lookup, whose recorded guest interval may no longer describe the bytes at that PC.
static int smc_commit(struct cpu *c) {
    pthread_mutex_lock(&g_jit_lock);
    txln_activate();                // arm eager line recording; may request a priming wholesale drop
    int force_whole = g_txln_prime; // first SMC after lazy activation: no lines recorded yet -> cannot classify
    g_txln_prime = 0;
    if (!force_whole && !c->smc_range_count && !c->smc_range_overflow) {
        pthread_mutex_unlock(&g_jit_lock);
        return 1;
    }
    __atomic_store_n(&g_smc_seen, 1, __ATOMIC_RELEASE);
    if (!c->smc_range_overflow && !force_whole) {
        uint32_t retained = 0;
        for (uint32_t i = 0; i < c->smc_range_count; i++) {
            uint64_t dirty_start = UINT64_MAX, dirty_end = 0;
            for (uint64_t line = c->smc_ranges[i][0]; line < c->smc_ranges[i][1]; line += 64) {
                // class 0 = never translated (nothing stale), 1 = first flush or bytes changed (drop),
                // 2 = translated but unchanged, i.e. benign icache maintenance (skip).
                if (txln_flush_class(line) == 1) {
                    if (dirty_start == UINT64_MAX) dirty_start = line;
                    dirty_end = line + 64;
                }
            }
            if (dirty_start != UINT64_MAX) {
                c->smc_ranges[retained][0] = dirty_start;
                c->smc_ranges[retained][1] = dirty_end;
                retained++;
            }
        }
        c->smc_range_count = retained;
        if (!retained) {
            pthread_mutex_unlock(&g_jit_lock);
            c->smc_range_overflow = 0;
            return 1;
        }
    }
    pthread_mutex_unlock(&g_jit_lock);
    // Bring every peer to a dispatcher boundary before mutating the lookup: map readers are lock-free, and a
    // peer that resolved a descriptor an instant ago must not still be inside run_block against a range this
    // window is about to declare stale.
    stw_mapping_begin();
    uint32_t removed;
    if (force_whole || c->smc_range_overflow) {
        removed = g_live_map_count;
        map_clear();
        // The inline branch-target cache is never populated by this backend (G_IBTC_FILL is a no-op), so this
        // clear is inert. It is kept so both backends leave identical state behind a wholesale drop -- the
        // table is process-global and a checkpoint written here can be restored by the JIT.
        memset(g_ibtc, 0, sizeof g_ibtc);
        txpg_clear();
    } else {
        removed = map_invalidate_source_ranges((const uint64_t (*)[2])c->smc_ranges, c->smc_range_count);
    }
    pend_reset();
    HL_LOGF(&g_jit_log, HL_LOG_TAG_JIT, "smc invalidate backend=interp mode=%s ranges=%u removed=%u retained=%u",
            (force_whole || c->smc_range_overflow) ? "whole" : "targeted", c->smc_range_count, removed,
            g_live_map_count);
    stw_mapping_end();
    c->smc_range_count = 0;
    c->smc_range_overflow = 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Contract stubs. Each one is inert for a specific, stated reason.
// ---------------------------------------------------------------------------
// Declared in abi.h and called through G_SOFT_TLB_REFRESH from the stop-the-world registry. This one is NOT
// inert -- it publishes the conservative hull of the sparse logical VMAs into the cpu, and it is plain C with
// no host-code dependency -- so it is the JIT's implementation unchanged. It is cheap and keeps the field
// meaningful for anything that reads it (including a checkpoint), even though this backend resolves a
// logical-VMA access inline rather than by consulting the hull.
static void aarch64_soft_filter_refresh(struct cpu *c) {
    uint64_t first = UINT64_MAX, last = 0;
    hl_logical_vma_snapshot *snapshot =
        atomic_load_explicit(hl_logical_vma_global_snapshot_source(), memory_order_acquire);
    if (snapshot != NULL && snapshot->count != 0) {
        first = snapshot->views[0].guest_first;
        last = snapshot->views[snapshot->count - 1].guest_last;
    }
    c->soft_filter_first = first;
    c->soft_filter_last = last;
}

// The soft-TLB reason handlers. The JIT emits an inline software-TLB probe into every memory instruction and
// exits R_SOFTMISS/R_SOFTSPAN/R_SOFTCOMMIT when it cannot resolve an address, because re-entering C from
// emitted code is expensive enough to be worth an inline fast path. This backend is ALREADY in C: when the
// load/store group lands it will resolve a logical-VMA access inline and never need to leave the block to
// ask, so it cannot raise these reasons at all.
//
// They exist because the reason codes are part of the shared vocabulary: interp_dispatch.h's
// G_DISPATCH_REASON handles them (by turning them into a guest fetch fault rather than falling through to
// R_BRANCH and resuming at a bogus PC) so that a checkpoint restored from a JIT-written image, or a future
// inline-probe variant of this backend, cannot silently mis-resume. Returning "not resolved" is the only
// answer that is true for a reason this backend never produces.
static int aarch64_soft_tlb_miss(struct cpu *c) {
    (void)c;
    return 0;
}

static int aarch64_soft_tlb_span(struct cpu *c) {
    (void)c;
    return 0;
}

static int aarch64_soft_bounce_commit(struct cpu *c) {
    // No bounce buffer can be pending: only aarch64_soft_prepare_bounce (a JIT path) ever arms one. Answer 1
    // = "committed", which is what the JIT answers for an unarmed bounce, so the dispatcher continues rather
    // than manufacturing a fault out of nothing.
    (void)c;
    return 1;
}

// §B shadow-return prediction caches HOST return addresses, so that a guest `ret` can become a host `ret`
// into the code arena. There are no host return addresses here and cpu->ssp is never non-zero (the shared
// dispatcher still resets it, deliberately, so a checkpoint written by this backend cannot hand the JIT a
// shadow stack full of another process's addresses -- see interp_dispatch.h).
//
// The value is not consulted: the JIT reads shadowgate() through G_BLOCK_ALIGN to decide entry padding, and
// interp_dispatch.h defines G_BLOCK_ALIGN as the literal 0. 0 spells "§B on" in the JIT's encoding, which is
// the value that makes every gate this function feeds take its ordinary, non-tuning path.
static int shadowgate(void) {
    return 0;
}

// ---------------------------------------------------------------------------
// The persistent translated-code cache: permanently absent on this backend.
// ---------------------------------------------------------------------------
// The pcache stores HOST CODE plus a table of relocations describing baked host pointers inside it. There is
// no host code here to store, and a file written by the JIT contains AArch64 instructions that this backend
// would have to execute to use -- so this backend must never load one and must never save one. Rather than
// spread `#ifdef` over the call sites in core/target/aarch64.c and linux_abi/, load is a clean MISS and save
// is a no-op, which is exactly the behaviour those call sites already handle (a cold miss is the normal
// first-run path).
//
// PCACHE_FLUSH_HOOK / PCACHE_FORK_HOOK / PCACHE_EXEC_HOOKS / PCACHE_SAVE_HOOK are deliberately NOT defined:
// every one of their call sites is inside an #ifdef, so leaving them undefined removes the bookkeeping
// entirely instead of stubbing it.
//
// These globals are still needed as storage because code outside this file reads them: linux_abi/elf.c
// honours g_force_base to map an image at a fixed VA, and core/target/aarch64.c reads g_pcache/g_coldprof and
// writes g_pc_binid/g_pc_entry around the load/save calls below.
#define PC_IMG_BASE 0x0000040000000000ull    // fixed guest image base, when a cache could be keyed to it
#define PC_INTERP_BASE 0x0000048000000000ull // fixed interpreter (ld.so) base

static int g_pcache;             // HL_PCACHE=1 was requested (it will simply never hit)
static int g_coldprof;           // cache timing diagnostics; production keeps this disabled
static uint64_t g_force_base;    // one-shot fixed-VA request consumed by load_elf
static int g_force_base_failed;  // a fixed-VA map fell back to a kernel base
static uint64_t g_pc_binid;      // identity of guest binary + interp + argv0 + engine build + host ISA
static uint64_t g_pc_entry;      // initial guest pc (sanity key)
static int g_pcache_loaded;      // never set: this backend cannot restore an arena
static int g_pcache_forked;      // never set: read by linux_abi/fork.c's save guard, which never fires here
static int g_nreloc;             // recorded baked-host-pointer slots; always zero here

// The engine-identity mix-in for the cache key. The one thing this MUST get right even though the cache is
// never used: host_isa is HL_HOST_CPU_ISA, not a hardcoded 1. The AArch64 JIT passes host_isa = 1 because it
// only ever runs on an AArch64 host; passing 1 from here would make an identity computed on an x86-64 host
// collide with a JIT-written cache for the same guest binary, and the collision would be resolved by
// executing AArch64 instructions on x86-64. The same value is mixed into the CHECKPOINT image identity
// (linux_abi/checkpoint.c calls this function), where the consequence of a collision is identical.
static uint64_t pcache_engine_id(void) {
    static const char tag[] = __DATE__ " " __TIME__;
    uint64_t build = hl_digest_bytes(HL_DIGEST_SEED, tag, sizeof tag - 1);
    uint64_t self = hl_identity_source(&g_jit_services, g_self_path);
    build = hl_digest_bytes(build, &self, sizeof self);
    // The JIT folds its codegen-mode switches in here so a mode change invalidates the cache. This backend
    // has no codegen modes; bit 0 marks "interpreter", so an identity from this backend can never equal one
    // from the JIT even if a future host somehow shared an ISA number.
    uint64_t modes = 1u;
    return hl_identity_configuration(build, HL_HOST_CPU_ISA_AARCH64, HL_HOST_CPU_ISA, modes);
}

static uint64_t pcache_make_id(const char *prog_host, const char *interp_host, const char *argv0) {
    uint64_t program = hl_identity_source(&g_jit_services, prog_host);
    uint64_t interpreter = interp_host ? hl_identity_source(&g_jit_services, interp_host) : 0xABCDEFull;
    return hl_identity_mix(program, interpreter, pcache_engine_id(), hl_identity_name(argv0));
}

// A clean MISS, always. The caller treats 0 as "translate fresh", which is the correct and only outcome here.
static int pcache_load(uint64_t entry_jump) {
    (void)entry_jump;
    if (g_pcache && g_coldprof)
        fprintf(stderr, "[pcache] MISS (interpreter backend stores no host code)\n");
    return 0;
}

// A no-op, always. Saving would persist block descriptors that describe nothing reusable (they are keyed to
// this process's guest mapping and hold no translated bytes), and any consumer of the file would be a JIT.
static void pcache_save(void) {
}

// The JIT refuses to persist an arena that a non-default codegen mode baked unrecorded host pointers into.
// Nothing is ever persisted here, so there is nothing to poison.
static void pcache_poison_check(void) {
}

// No cache directory is ever opened, so there is no descriptor to close.
static void pcache_directory_close(void) {
}
