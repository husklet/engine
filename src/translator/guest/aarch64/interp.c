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
// which is only meaningful when the host is AArch64). Held here so a read-modify-write round-trips, which is
// what glibc's fegetround/fesetround do, but NOT ACTED ON: no floating-point arithmetic is implemented in this
// backend yet, so there is no rounding mode or exception trap for them to control. Whoever implements scalar FP
// must consume these rather than adding a second copy.
static __thread uint64_t g_interp_fpcr;
static __thread uint64_t g_interp_fpsr;

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
        case 16:
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23:
            return interp_undefined(cpu, insn, "data-processing register -- CRC32/CRC32C");
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
        // TODO(amd64-host): BRK/HLT should raise a guest SIGTRAP through the same synchronous-fault path the
        // JIT reaches via the host debug trap; HVC/SMC are EL1/EL2 and must stay undefined at EL0.
        return interp_undefined(cpu, insn, "exception generation -- BRK/HLT/HVC/SMC");
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
        case 0xD51B4400u: // MSR FPCR
            // Stored so a read-modify-write round-trips (glibc's fesetround does exactly that), but it does not
            // yet CHANGE anything: no floating-point arithmetic is implemented, so there is no rounding mode or
            // exception trap for it to control. Whoever implements scalar FP must consume this.
            g_interp_fpcr = interp_gpr(cpu, rt);
            break;
        case 0xD53B4420u: // MRS FPSR
            interp_set_gpr(cpu, rt, g_interp_fpsr);
            break;
        case 0xD51B4420u: // MSR FPSR
            g_interp_fpsr = interp_gpr(cpu, rt);
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
    } else if (!low) { // 8-bit element replicated across all 8 bytes
        if (o2) return 0;
        imm64 = imm8 * UINT64_C(0x0101010101010101);
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

    // ---- AdvSIMD load/store multiple structures: LD1/ST1 (and the LD2/LD3/LD4 de-interleaving forms) ----
    // A different top-level encoding from the scalar loads, which is why it is tested here explicitly rather
    // than falling out of the size/opc decode below. Only the CONTIGUOUS forms (LD1/ST1, opcode 0b0111 for one
    // register and 0b1010/0b0110/0b0010 for two/three/four) are implemented: they are what memcpy and the
    // string routines use, and the de-interleaving LD2/LD3/LD4 forms are a separate shuffle problem.
    if ((insn & 0xBFBF0000u) == 0x0C000000u || (insn & 0xBFA00000u) == 0x0C800000u) {
        unsigned load = (insn >> 22) & 1u, opcode = (insn >> 12) & 0xFu, size = (insn >> 10) & 3u;
        int post_index = (insn & 0x00800000u) != 0;
        unsigned registers;
        switch (opcode) {
        case 0x7: registers = 1; break; // LD1/ST1, one register
        case 0xA: registers = 2; break; // LD1/ST1, two registers
        case 0x6: registers = 3; break; // LD1/ST1, three registers
        case 0x2: registers = 4; break; // LD1/ST1, four registers
        default:
            return interp_undefined(cpu, insn,
                                    "AdvSIMD load/store -- de-interleaving multi-structure form (LD2/LD3/LD4)");
        }
        unsigned bytes = q ? 16u : 8u;
        uint64_t base = interp_gpr_sp(cpu, rn);
        uint64_t address = base;
        for (unsigned index = 0; index < registers; index++) {
            int reg = (rt + (int)index) % 32; // the register list wraps at V31
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
        (void)size;
        if (post_index) {
            // Rm == 31 means the immediate form, whose increment is the whole transfer size; otherwise the
            // increment is the register Rm.
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

        // The genuine exclusive pair. o1 selects the single-register forms from the pair (LDXP/STXP) forms.
        uint64_t address = interp_gpr_sp(cpu, rn);
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
            default:
                // LDSMAX/LDSMIN/LDUMAX/LDUMIN (opc 4..7). No __atomic_fetch_max exists, so these need an
                // explicit compare-and-swap retry loop; they are rare enough (no libc fast path uses them)
                // that reporting is better than a hurried implementation.
                interp_access_end();
                return interp_undefined(cpu, insn, "loads and stores -- LSE LDSMAX/LDSMIN/LDUMAX/LDUMIN");
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
        unsigned lanes = interp_vec_lanes(size, q);
        uint64_t mask = interp_element_mask(size);

        if (opcode == 0x10 || opcode == 0x11) {
            // SHRN / RSHRN (and their "2" variants when Q == 1). Read elements of TWICE the destination width,
            // shift right, and truncate -- this is how a 16-byte vector compare mask gets packed into 8 bytes so
            // that a single FMOV can carry it into a general register, which is the core of glibc's strlen and
            // memchr. There is no 128-bit source element, so immh's top bit is unallocated here.
            if (size == 3) return interp_undefined(cpu, insn, "AdvSIMD shift -- SHRN with a 64-bit result element");
            unsigned shift = 2u * esize - combined;
            unsigned narrow_lanes = 64u / esize; // the result is always 64 bits wide; Q selects WHICH half
            uint64_t wide_mask = interp_element_mask(size + 1u);
            interp_vec packed;
            memset(packed.byte, 0, sizeof packed.byte);
            for (unsigned lane = 0; lane < narrow_lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size + 1u, lane) & wide_mask;
                // RSHRN rounds rather than truncates, by adding half of the discarded field first.
                if (opcode == 0x11 && shift > 0) element += UINT64_C(1) << (shift - 1u);
                interp_vec_set_element(&packed, size, lane, (element >> shift) & mask);
            }
            if (!q) {
                // SHRN writes the low 64 bits and zeroes the upper half.
                interp_vec_write(cpu, rd, packed, 0);
            } else {
                // SHRN2 writes the UPPER 64 bits and must leave the lower half untouched.
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

        if (size == 3 && !q) return interp_undefined(cpu, insn, "AdvSIMD shift -- 64-bit element requires Q");
        if (opcode == 0x0A) { // SHL: shift left by (combined - esize)
            unsigned shift = combined - esize;
            for (unsigned lane = 0; lane < lanes; lane++)
                interp_vec_set_element(&result, size, lane,
                                       (interp_vec_element(&source, size, lane) << shift) & mask);
        } else if (opcode == 0x00 || opcode == 0x02) { // SSHR/USHR and the accumulating SSRA/USRA
            unsigned shift = 2u * esize - combined;
            interp_vec accumulate = interp_vec_read(cpu, rd);
            for (unsigned lane = 0; lane < lanes; lane++) {
                uint64_t element = interp_vec_element(&source, size, lane);
                // A right shift of the FULL element width is defined for these instructions (the result is 0,
                // or the replicated sign bit) but is undefined behaviour in C, so it is handled explicitly.
                uint64_t shifted;
                if (u)
                    shifted = shift >= esize ? 0 : (element >> shift);
                else {
                    int64_t signed_element = (int64_t)interp_element_sext(element, size);
                    shifted = (uint64_t)(shift >= esize ? (signed_element >> (esize - 1)) : (signed_element >> shift));
                }
                shifted &= mask;
                if (opcode == 0x02) shifted = (shifted + interp_vec_element(&accumulate, size, lane)) & mask;
                interp_vec_set_element(&result, size, lane, shifted);
            }
        } else {
            return interp_undefined(cpu, insn, "AdvSIMD shift by immediate -- unimplemented opcode");
        }
        interp_vec_write(cpu, rd, result, q);
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

    // ---- AdvSIMD across lanes: ADDV / UADDLV / SADDLV / UMAXV / SMAXV / UMINV / SMINV ----
    // Tested before two-register-misc: both live at bits[21:17] == 10000/11000 and differ only in bit 20.
    if ((insn & 0x9F3E0C00u) == 0x0E300800u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 12) & 0x1Fu;
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
            if (size == 3 && !q)
                return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- 1D compare is reserved");
            uint64_t mask = interp_element_mask(size);
            for (unsigned lane = 0; lane < interp_vec_lanes(size, q); lane++) {
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
            if (size == 3 && !q) return interp_undefined(cpu, insn, "AdvSIMD two-reg misc -- 1D ABS/NEG reserved");
            uint64_t mask = interp_element_mask(size);
            for (unsigned lane = 0; lane < interp_vec_lanes(size, q); lane++) {
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

    // ---- AdvSIMD three same: the bitwise group, the compares, ADD/SUB, MIN/MAX, ADDP, shifts ----
    if ((insn & 0x9F200400u) == 0x0E200400u) {
        unsigned size = (insn >> 22) & 3u, opcode = (insn >> 11) & 0x1Fu;
        interp_vec left = interp_vec_read(cpu, rn), right = interp_vec_read(cpu, rm), result;
        memset(result.byte, 0, sizeof result.byte);
        unsigned bytes = q ? 16u : 8u;
        unsigned lanes = interp_vec_lanes(size, q);
        uint64_t mask = interp_element_mask(size);

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
                    switch (size) {
                    case 0: value = (uint8_t)(a ^ b); break;                        // EOR
                    case 1: value = (uint8_t)((b & d) | (a & (uint8_t)~d)); break;   // BSL
                    case 2: value = (uint8_t)(d ^ ((d ^ a) & b)); break;             // BIT
                    default: value = (uint8_t)(d ^ ((d ^ a) & (uint8_t)~b)); break;  // BIF
                    }
                }
                result.byte[index] = value;
            }
            interp_vec_write(cpu, rd, result, q);
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }

        if (size == 3 && !q && opcode != 0x10)
            return interp_undefined(cpu, insn, "AdvSIMD three same -- reserved 1D form");

        switch (opcode) {
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
        case 0x13: { // MUL (U=0). PMUL (U=1) is polynomial multiply, which is a different operation.
            if (u) return interp_undefined(cpu, insn, "AdvSIMD three same -- PMUL (polynomial multiply)");
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

    // ---- Scalar FP <-> general register moves: FMOV only ----
    // The rest of the scalar-FP box needs an actual floating-point model (rounding modes, FPCR/FPSR, the NaN
    // rules) and is deliberately left out until a guest asks for it. The register moves are here because they
    // are how an integer routine gets a value into and out of the vector world, and they involve no arithmetic
    // at all -- they are pure bit copies, so implementing them costs nothing and risks nothing.
    if ((insn & 0x5F200000u) == 0x1E200000u && ((insn >> 10) & 0x3Fu) == 0) {
        unsigned type = (insn >> 22) & 3u, rmode = (insn >> 19) & 3u, opcode = (insn >> 16) & 7u;
        unsigned sf = (insn >> 31) & 1;
        if (rmode == 0 && opcode == 6) { // FMOV to a general register (Vn's low element -> Rd)
            interp_vec source = interp_vec_read(cpu, rn);
            if (type == 0 && !sf) // FMOV Wd, Sn
                interp_set_gpr32(cpu, rd, (uint32_t)interp_vec_element(&source, 2, 0));
            else if (type == 1 && sf) // FMOV Xd, Dn
                interp_set_gpr(cpu, rd, interp_vec_element(&source, 3, 0));
            else
                return interp_undefined(cpu, insn, "scalar FP -- unallocated FMOV to general register");
            cpu->pc = gpc + 4;
            return INTERP_NEXT;
        }
        if (rmode == 0 && opcode == 7) { // FMOV from a general register (Rn -> Vd's low element)
            interp_vec result;
            memset(result.byte, 0, sizeof result.byte);
            if (type == 0 && !sf) { // FMOV Sd, Wn
                interp_vec_set_element(&result, 2, 0, interp_gpr(cpu, rn) & 0xFFFFFFFFu);
            } else if (type == 1 && sf) { // FMOV Dd, Xn
                interp_vec_set_element(&result, 3, 0, interp_gpr(cpu, rn));
            } else {
                return interp_undefined(cpu, insn, "scalar FP -- unallocated FMOV from general register");
            }
            interp_vec_write(cpu, rd, result, 0); // a scalar write zeroes the upper 64 bits
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
        return interp_undefined(cpu, insn, "scalar FP -- integer/FP conversion (FCVT/SCVTF/UCVTF)");
    }

    // ---- Scalar FP register-to-register move, which is also a pure bit copy ----
    // FMOV (register): 0 0 0 11110 type 1 000000 10000 Rn Rd
    if ((insn & 0x5FFFFC00u) == 0x1E204000u) {
        unsigned type = (insn >> 22) & 3u;
        unsigned size = type == 0 ? 2u : (type == 1 ? 3u : 4u);
        if (size > 3) return interp_undefined(cpu, insn, "scalar FP -- FMOV of a half or quad precision value");
        interp_vec source = interp_vec_read(cpu, rn), result;
        memset(result.byte, 0, sizeof result.byte);
        interp_vec_set_element(&result, size, 0, interp_vec_element(&source, size, 0));
        interp_vec_write(cpu, rd, result, 0);
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
// External linkage with hidden visibility mirrors what the AArch64 arms of core/dispatch.c and
// guest/x86_64/translate.c do, for the same reason: core/target/aarch64.c takes &block_return, and a symbol
// that is taken by address should be one symbol.
void run_block(struct cpu *cpu, void *code) __attribute__((visibility("hidden")));
void block_return(void) __attribute__((visibility("hidden")));

void run_block(struct cpu *cpu, void *code) {
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
void block_return(void) {
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
