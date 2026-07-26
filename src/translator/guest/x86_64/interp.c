// translator/guest/x86_64/interp.c -- the x86-64 guest backend for every host CPU that is NOT AArch64:
// DECODE AND EXECUTE x86-64 instead of emitting ARM64 for it.
//
// Why this file exists at all. Everything else in this directory is one half of an x86-64 -> ARM64
// translator: emit.c's header says "arm64 host emitters + NEON/SSE encoders (xmm->v0..15)", and
// core/target/x86_64.c states the register model it presupposes -- guest rax..r15 pinned in host x0..x15,
// the cpu pointer in host x28. None of that can run on a host that does not execute ARM64, so
// core/target/x86_64.c forks on HL_HOST_CPU_AARCH64 (src/host/host_cpu.h) and includes this file on the
// other arm, in place of emit.c + translate.c + cache.c. It is textually included into that unity
// translation unit, so everything above the include is already in scope here (cpu.h, abi.h, glue.h,
// interp_dispatch.h, translator/cache.c and its arena/map/STW machinery, hl_x86_guest_pointer, the
// jit86_store_alias_* forward declarations) and everything BELOW the include -- linux_abi/thread.c,
// linux_abi/signal.c, the syscall layer, linux_abi/x86.c, checkpoint.c, core/dispatch.c -- depends on the
// names defined here. That is the real specification of this file: it must define exactly the set of names
// those three JIT files defined, and no more.
//
// Why substituting an interpreter for a code generator is legitimate rather than a hack. The shared
// dispatcher's ENTIRE contract with a backend (core/dispatch.c run_guest) is three lines:
//
//     code = translate_block(G_PC(c));   // produce something callable for this guest PC
//     run_block(c, code);                // ... call it
//     // on return: c->reason says why it stopped, c->rip is the next guest PC, and ALL guest
//     // architectural state is back in *c.
//
// Nothing in it requires `code` to be machine code. Here `code` is a BLOCK DESCRIPTOR (struct interp_block
// below) and run_block is a C loop that decodes guest bytes with the host-neutral decoder in decode.c and
// applies each instruction to *cpu. Every consumer above and below the seam therefore stays byte-identical
// between the two backends, including the checkpoint image: struct cpu is untouched (its size is written
// into the image and validated on restore), and x86 EFLAGS keeps living on the ARM-NZCV substrate that
// hl_x86_signal_nzcv_to_eflags / ..._eflags_to_nzcv translate at every guest-visible boundary. Keeping that
// representation is not aesthetic: linux_abi/syscall/ptrace.c's GETREGS and the rt_sigframe builder both
// read it, so changing it would break the checkpoint format and the guest ABI at once.
//
// ---------------------------------------------------------------------------------------------------
// EXTENSION POINTS -- read this before adding an instruction class.
//
// The dispatch is a two-level opcode decision tree over the already-decoded `struct insn`:
//   interp_step_one_byte()   the one-byte map (and, for group opcodes, the ModRM /reg subfield)
//   interp_step_two_byte()   the 0F map
// Both return STEP_NEXT ("instruction retired, rip advanced, keep going in this block") or STEP_END
// ("cpu->rip and cpu->reason are final, return to the dispatcher").
//
// To add a class, add its arm to the matching function and delete the interp_undefined() route that
// currently catches it. Three routes are DELIBERATELY not local decisions:
//
//   * VEX/EVEX (AVX/AVX2/AVX-512) already works: interp_step() exits with R_AVX and the dispatcher calls
//     hl_x86_avx_run(), which owns the whole v[]/vhi[]/vz[]/vx[]/kreg[] file and advances rip itself.
//     Legacy 0F38/0F3A (SSSE3/SSE4/AES/SHA/PCLMUL/CRC32/MOVBE) likewise exits with R_SSE3B to
//     hl_x86_sse_run(). Both helpers are host-neutral C. Do not reimplement either.
//   * x87 m80 loads/stores, the transcendental subset, and fxsave/fxrstor have host-neutral helpers too
//     (x87state.c, x87math.c) reached through R_X87FLD / R_X87FSTP / R_X87FUNC / R_FXSAVE / R_FXRSTOR.
//     Adding x87 means decoding D8..DF, routing the m80 and transcendental forms at those reasons, and
//     computing the ordinary arithmetic on the c->st[] double stack in place.
//   * LEGACY SSE (the 0F map proper: 10/11/28/29/6E/6F/7E/7F/D6/EF/54/57, pshufd, pcmpeqb, pmovmskb, ...)
//     has NO existing C emulator -- avx.c's do_sse3b covers only the 0F38/0F3A escape maps. That is the
//     largest remaining gap and the one glibc's string and memory routines reach first. See the
//     TODO(amd64-host) marker in interp_step_two_byte().
//
// Guest memory is reached ONLY through interp_load/interp_store/interp_locked_rmw (and the shared rep
// helpers). Do not add a raw dereference: those carry the rebias, the unaligned-safe memcpy, the
// shared-mapping store notification, and the fault marker the signal path keys on.
// ---------------------------------------------------------------------------------------------------

#include <math.h> // x87: floor/ceil/trunc/sqrt/fmod/remainder on the double-precision ST stack
#include <setjmp.h>

#if defined(HL_HOST_CPU_X86_64)
// SSE/SSE2 intrinsics. Both are baseline on x86-64, so no -m flag is involved. They are what makes the
// guest's floating point execute as the host's -- see "SSE / SSE2 FLOATING POINT" below for why that, and
// not a software model, is the honest implementation on this host. xmmintrin.h also supplies
// _mm_getcsr/_mm_setcsr (STMXCSR/LDMXCSR), which is where the guest's MXCSR actually lives.
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#include "../../identity.h"
#include "decoder.h"

// Included for the DECLARATIONS core/target/x86_64.c's engine_global_init needs
// (hl_x86_rep_set_store_commit / hl_x86_rep_set_access_validators), which on the AArch64 arm arrive through
// translate.c's include chain.
//
// Its hl_x86_rep_movs / hl_x86_rep_stos are genuinely host-neutral C and were the obvious reuse for
// `rep movs`/`rep stos`, but they are deliberately NOT used: they live in the same OBJECT as
// hl_x86_lower_repstr, the ARM64 emitter for that idiom, so pulling repstr.c.o into the link brings
// undefined references to every ARM64 emitter it calls. Those references are unavoidable anyway (the two
// setter calls above already pull the object in) and are satisfied by the emitter stubs at the bottom of
// this file, but the string ops are still executed element-at-a-time below -- which is not merely a
// fallback: per-element pointer/counter updates are what make a fault mid-string architecturally correct.
#include "lower/repstr.h"

// ---------------------------------------------------------------------------------------------------
// Names the JIT files own that the rest of the unity TU needs. Grouped here because they are not part of
// the interpreter proper -- they are the seam. Each says why an interpreter's answer differs from the
// JIT's, where it does.
// ---------------------------------------------------------------------------------------------------

// W6A item 1 (non-PIE): the Go type section and the V8 embedded-blob code base of a biased ET_EXEC image,
// in LOW link coordinates. linux_abi/x86.c's load_elf / go_rebase_nonpie set them and
// linux_abi/syscall/proc.c resets g_nonpie_blob_code across execve. The JIT reads them to decide whether a
// materialized constant must be rebased to the high mapping; the interpreter materializes no constants at
// translate time, so it reads them for exactly one decision -- see interp_call_return_pc().
// g_nonpie_lo/hi/bias themselves are declared above this include, in core/target/x86_64.c.
static uint64_t g_nonpie_types_lo, g_nonpie_types_hi;
static uint64_t g_nonpie_blob_code;

// Fixed guest VA bases the persistent cache pins images at. Named here because core/target/x86_64.c and
// linux_abi/{fork,x86}.c reference them unconditionally. Values match guest/x86_64/cache.c so switching
// host CPU does not silently relocate the guest image.
#define PC_IMG_BASE 0x0000040000000000ull    // 4 TB
#define PC_INTERP_BASE 0x0000048000000000ull // 4.5 TB

// The vDSO fast-syscall lever. On AArch64, emit.c inlines clock_gettime/gettimeofday into the block and
// s1_calibrate() decides whether the host counter is trustworthy enough to do so. There is no emitted block
// to inline into here, so the lever is permanently off and calibration has nothing to measure.
// g_fastsys/g_fast_count still have to EXIST because core/target/x86_64.c reports them at exit; a
// consequence is that cpu->fastclk_ptr/fastclk_resume stay permanently disarmed, which is exactly the state
// signal.c's hl_x86_signal_fast_clock_fault documents for this host.
static int g_fastsys;
static uint64_t g_fast_count;

static void s1_calibrate(void) {
    // Deliberately empty, and deliberately not a warning: "no inline fast clock on this host" is a property
    // of having no code generator, not a misconfiguration. Every guest clock syscall takes the ordinary
    // R_SYSCALL exit through service(), which is the same path a calibration FAILURE selects on AArch64 --
    // a supported configuration, just never the fast one.
    g_fastsys = 0;
}

// A guest unmapped or remapped an executable VA range (abi.h's G_SMC_UNMAP, expanded in the shared munmap /
// MAP_FIXED / mremap paths). The JIT must drop host code translated from those bytes. This backend caches
// no instruction bytes -- run_block re-decodes from guest memory every time -- so a stale DECODE is
// impossible, and the only thing worth dropping is the block-descriptor map entry, which would otherwise
// keep a descriptor alive for a guest PC that no longer has code at it. Dropping it costs one range scan
// and means the next execution of that VA takes a fresh fetch (and a truthful fetch fault if the range is
// now unmapped) rather than running a descriptor whose gpc has become meaningless.
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

// abi.h's G_THREAD_START_FLUSH / G_SHARED_MAP_BARRIERS. On AArch64 these exist for ONE reason: emit.c
// elides the x86-TSO DMB on every guest access while the guest is single-threaded, so the moment a peer
// thread (or a peer PROCESS via MAP_SHARED) can observe guest memory, every barrier-free block must be
// discarded and re-translated with barriers. There are no barrier-elided blocks here -- see the memory
// ordering note above interp_load() -- so the transition needs no flush. Nonzero means success; the clone
// path treats 0 as a clone failure, so this must not report failure for work it simply does not have.
static int hl_x86_flush_for_thread_start(void) {
    return 1;
}

static int hl_x86_force_barriers_for_shared(void) {
    return 1;
}

// The shared dispatcher's block-entry alignment pad (`while ((uintptr_t)g_cp & 15) emit32(nop)`) is gated
// on G_BLOCK_ALIGN, which interp_dispatch.h defines as a compile-time 0 -- so this is never called. It must
// still be a real function for that dead branch to type-check. Aborting rather than quietly appending a
// word is the honest choice: reaching it would mean something started treating the descriptor arena as an
// instruction stream, and appending an ARM64 nop encoding would corrupt the arena silently instead of
// pointing at the mistake. Non-static because lower/primitives.h declares it with external linkage (the
// lowering files are compiled independently and call it), and that declaration is in scope here.
void emit32(uint32_t instruction) {
    (void)instruction;
    fprintf(stderr, "[hl] emit32() called on a " HL_HOST_CPU_NAME " host: the interpreter backend emits no\n"
                    "     instructions, so a caller believes the code arena holds machine code. G_BLOCK_ALIGN\n"
                    "     is 0 precisely so the shared dispatcher's alignment pad never reaches here.\n");
    abort();
}

// ---------------------------------------------------------------------------------------------------
// The fault model.
//
// With a JIT, a guest memory fault lands in emitted code and hl_x86_signal_capture() reconstructs guest
// state out of the host register file (AArch64 x0..x15 == rax..r15) before hl_x86_signal_resume() rewrites
// the host PC to block_return so the block unwinds. Neither step applies here: *cpu is ALREADY the
// authoritative guest state and cpu->rip is exact (interp_execute publishes it at every instruction
// boundary and no instruction advances it before it can fault), so capture is trivial.
//
// What is NOT trivial is getting out. The faulting access is a memcpy inside a C function several frames
// deep; the host handler must ABANDON it, not resume past it the way the JIT's host-PC rewrite does. So
// run_block establishes a sigsetjmp landing pad and every guest access is bracketed by a thread-local
// marker; interp_signal_capture() claims the fault only when the marker says a guest access was in flight,
// and interp_signal_resume() siglongjmps back to the pad.
//
// The marker is what keeps a genuine ENGINE bug reportable. A hook that claimed every fault on the thread
// would swallow a null dereference in the syscall layer and resume the guest as though the guest had
// faulted. With the marker, such a fault leaves interp_signal_capture() returning 0, the shared delivery
// path declines it, and jit86_lazyguard re-raises into the crash report -- which is exactly the contract
// hl_x86_signal_capture() already has on this host (it returns 0 unconditionally, by design).
//
// sigsetjmp saves the signal mask (savemask=1) because the pad is entered FROM a signal handler, where the
// delivered signal is blocked; siglongjmp must unblock it again or the next guest fault would kill the
// process outright.
// ---------------------------------------------------------------------------------------------------

static __thread sigjmp_buf g_interp_fault_pad;
static __thread int g_interp_pad_armed;             // a run_block landing pad is live on this thread
static __thread volatile int g_interp_guest_access; // a guest memory access is in flight right now

static inline void interp_access_begin(void) {
    g_interp_guest_access = 1;
    __atomic_signal_fence(__ATOMIC_SEQ_CST); // the marker must be visible to a handler BEFORE the access
}

static inline void interp_access_end(void) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    g_interp_guest_access = 0;
}

int interp_signal_capture(struct cpu *cpu, void *native_context);
void interp_signal_resume(struct cpu *cpu, void *native_context);

// Called by core/target/x86_64.c's sigframe_capture_fault on this host (see the report accompanying this
// file: that call site must try interp_signal_capture() where hl_x86_signal_capture() declines). Returns 1
// only for a fault taken inside a marked guest access, i.e. only for a fault the GUEST caused.
int interp_signal_capture(struct cpu *cpu, void *native_context) {
    (void)native_context; // no host register file to read: *cpu is already authoritative
    if (cpu == NULL || !g_interp_pad_armed || !g_interp_guest_access) return 0;
    return 1;
}

// The counterpart of hl_x86_signal_resume: hand control back to the dispatcher. The guest signal has
// already been queued (cpu->tpending) and cpu->reason set to R_BRANCH by the shared delivery path, so the
// pad simply returns from run_block and run_guest's maybe_deliver_signal builds the rt_sigframe.
void interp_signal_resume(struct cpu *cpu, void *native_context) {
    (void)cpu;
    (void)native_context;
    if (!g_interp_pad_armed) return; // not ours; the caller re-raises
    siglongjmp(g_interp_fault_pad, 1);
}

// ---------------------------------------------------------------------------------------------------
// Guest memory.
//
// Guest VA == host VA, with one exception: a non-PIE ET_EXEC's low link range is served at +g_nonpie_bias
// (hl_x86_guest_pointer, defined above this include). So a guest access is a host dereference after that
// rebias and nothing more -- there is deliberately no address-translation layer, no software TLB and no
// page table. Two properties of that dereference are load-bearing:
//
//   * UNALIGNED ACCESS MUST WORK. x86 permits every unaligned normal load and store and real guest code
//     relies on it constantly (glibc's word-at-a-time string loops, packed structures). A
//     cast-and-dereference through a uint64_t* is undefined behaviour on an unaligned address and a real
//     fault on some hosts, so every access goes through memcpy, which the compiler lowers to the host's
//     native unaligned move.
//   * NO BYTE SWAPPING. x86 is little-endian and so is every host this backend currently builds for, so
//     the memcpy reproduces guest byte order exactly. A big-endian host would need a swap here and in the
//     decoder, which is why this is stated rather than assumed.
//
// MEMORY ORDERING. The JIT emits an explicit DMB around guest accesses because ARM is weakly ordered while
// x86 guests assume x86-TSO. This backend emits nothing, so the ordering the guest observes is exactly the
// ordering the HOST gives the C accesses below. On an x86-64 host that is host TSO, which IS guest TSO, so
// plain loads and stores are already correct and no fence is needed -- that equivalence is the whole reason
// the helper below is empty today. It would NOT hold on a future weakly-ordered host (a POWER or RISC-V
// build): there, guest TSO would have to be restored explicitly. The dependency is named in code rather
// than left as a property nobody wrote down. Locked read-modify-write is a separate matter and is always
// made atomic explicitly (interp_locked_rmw), on every host.
// ---------------------------------------------------------------------------------------------------

static inline void interp_tso_fence(void) {
#if !defined(HL_HOST_CPU_X86_64)
    // Not an x86-64 host: host ordering is weaker than x86-TSO, so restore the guest's ordering
    // explicitly. Around every guest access rather than only the synchronising ones, because an
    // interpreter has no way to know which guest access is the synchronising one.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static uint64_t interp_load(uint64_t guest_address, int width) {
    uint64_t value = 0;
    const void *host = (const void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_access_begin();
    memcpy(&value, host, (size_t)width); // unaligned-safe; little-endian host == little-endian guest
    interp_access_end();
    interp_tso_fence();
    return value;
}

static void interp_store(uint64_t guest_address, int width, uint64_t value) {
    void *host = (void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_tso_fence();
    interp_access_begin();
    memcpy(host, &value, (size_t)width);
    interp_access_end();
    // A store into an emulated MAP_SHARED mapping, or into an executable alias of one, must be queued so
    // jit86_smc_commit publishes it before the next syscall can let a peer observe it. Gated on the same
    // predicate the shared rep helpers use, so the ordinary case pays one predicate call.
    if (jit86_store_alias_observation_active()) jit86_store_alias_changed(guest_address, (uint64_t)width);
}

// The same two accesses for an operand WIDER than a general register, i.e. the 8/16-byte halves and the full
// 16 bytes of an xmm operand. Kept separate from interp_load/interp_store rather than generalising them,
// because those return and take a uint64_t by value and the SSE paths work on a byte image: a 128-bit
// operand has no scalar type here, and forcing one would mean two 64-bit halves and two fault windows where
// the architecture has one access.
//
// The fault bracket is the ONE thing that must match interp_load/interp_store exactly. A single
// interp_access_begin/end pair spans the whole transfer, so a fault anywhere inside a straddling 16-byte
// access is still claimed by interp_signal_capture and still abandons the instruction with no architectural
// change -- which is why every caller below reads its operands into locals FIRST and commits to cpu->v[] or
// to guest memory only after the last access has returned.
static void interp_load_bytes(uint64_t guest_address, void *destination, unsigned length) {
    const void *host = (const void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_access_begin();
    memcpy(destination, host, length); // unaligned-safe, and one window for the whole operand
    interp_access_end();
    interp_tso_fence();
}

static void interp_store_bytes(uint64_t guest_address, const void *source, unsigned length) {
    void *host = (void *)(uintptr_t)hl_x86_guest_pointer(guest_address);
    interp_tso_fence();
    interp_access_begin();
    memcpy(host, source, length);
    interp_access_end();
    if (jit86_store_alias_observation_active()) jit86_store_alias_changed(guest_address, (uint64_t)length);
}

// A biased ET_EXEC executes at link_pc+bias, but the address x86 CALL pushes is guest-visible architectural
// state: DWARF FDE lookup, dladdr, backtrace and forced unwinding must see the LINK address, and the
// dispatcher rebiases it again on the RET. Go and V8 are the deliberate exceptions -- linux_abi/x86.c
// rebases their runtime code metadata into the high execution domain, so their stack walkers require high
// return PCs. Byte-for-byte the JIT's call_return_pc (translate.c), because the two backends must push the
// same value into guest memory.
static uint64_t interp_call_return_pc(uint64_t pc) {
    if (g_nonpie_lo && !g_nonpie_types_lo && !g_nonpie_blob_code && pc >= g_nonpie_lo + g_nonpie_bias &&
        pc < g_nonpie_hi + g_nonpie_bias)
        return pc - g_nonpie_bias;
    return pc;
}

// ---------------------------------------------------------------------------------------------------
// The flag substrate.
//
// This frontend models x86 EFLAGS on ARM NZCV in cpu->nzcv plus two side lanes for the flags NZCV has no
// bit for. The encoding is fixed by the checkpoint format and by signal.c's converters, so it is restated
// here rather than rediscovered:
//
//   bit 31  N  = x86 SF
//   bit 30  Z  = x86 ZF
//   bit 29  C  = NOT x86 CF       (ARM's borrow convention: SUBS sets C on "no borrow")
//   bit 28  V  = x86 OF
//   cpu->pf      a BYTE whose EVEN PARITY is x86 PF. Flag-setting integer ops store the low byte of their
//                result, so the parity consumers (setp/jp/cmovp/lahf/pushfq) just popcount it.
//   cpu->af      (a ^ b ^ result) from the last add/sub-shaped op; x86 AF is bit 4 of it. Logical ops,
//                where x86 leaves AF undefined, store 0 (matching qemu's CC_OP_LOGIC and the JIT).
//   cpu->df      x86 DF (direction), 0 = forward. Runtime state, so a cross-block `std` is honoured.
//   cpu->idflag  x86 RFLAGS.ID (bit 21), the software-toggleable bit 32-bit CPUID probes flip.
//
// Getting the C inversion wrong is the classic way an x86 interpreter passes its own tests and fails
// against hardware, so every producer below goes through these helpers and never writes bit 29 directly.
// ---------------------------------------------------------------------------------------------------

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

// Write N/Z/C/V from x86-flag values (x86_cf, not the stored borrow form).
static void interp_flags_nzcv(struct cpu *cpu, unsigned sf, unsigned zf, unsigned x86_cf, unsigned of) {
    uint64_t nzcv = 0;
    if (sf) nzcv |= NZ_N;
    if (zf) nzcv |= NZ_Z;
    if (!x86_cf) nzcv |= NZ_C; // stored C is the INVERSE of x86 CF
    if (of) nzcv |= NZ_V;
    cpu->nzcv = nzcv;
}

static unsigned interp_cf(const struct cpu *cpu) {
    return (unsigned)(((cpu->nzcv >> 29) & 1) ^ 1); // x86 CF = NOT stored borrow-C
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

// ADD-shaped result + flags (also serves ADC via carry_in).
static uint64_t interp_alu_add(struct cpu *cpu, uint64_t a, uint64_t b, unsigned carry_in, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned __int128 wide = (unsigned __int128)(a & m) + (b & m) + carry_in;
    uint64_t result = (uint64_t)wide & m;
    unsigned cf = (unsigned)((wide >> bits) & 1);
    // x86 OF for addition: both inputs agree in sign and the result disagrees with them.
    unsigned of = (unsigned)((((a ^ result) & (b ^ result)) >> (bits - 1)) & 1);
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, cf, of);
    cpu->pf = result & 0xff;
    cpu->af = a ^ b ^ result; // x86 AF is bit 4 (the carry out of bit 3)
    return result;
}

// SUB-shaped result + flags (also serves SBB via borrow_in and CMP by discarding the result).
static uint64_t interp_alu_sub(struct cpu *cpu, uint64_t a, uint64_t b, unsigned borrow_in, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned __int128 wide = (unsigned __int128)(a & m) - (b & m) - borrow_in;
    uint64_t result = (uint64_t)wide & m;
    unsigned cf = (unsigned)((wide >> bits) & 1); // the subtraction went negative -> borrow
    // x86 OF for subtraction: the inputs disagree in sign and the result disagrees with the minuend.
    unsigned of = (unsigned)((((a ^ b) & (a ^ result)) >> (bits - 1)) & 1);
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, cf, of);
    cpu->pf = result & 0xff;
    cpu->af = a ^ b ^ result;
    return result;
}

// AND/OR/XOR/TEST: x86 clears CF and OF and leaves AF undefined (stored as 0, matching the JIT).
static void interp_flags_logic(struct cpu *cpu, uint64_t result, int width) {
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, 0, 0);
    cpu->pf = result & 0xff;
    cpu->af = 0;
}

// INC/DEC: identical to ADD/SUB of 1 except that x86 leaves CF UNTOUCHED.
static uint64_t interp_alu_incdec(struct cpu *cpu, uint64_t a, int decrement, int width) {
    unsigned cf = interp_cf(cpu);
    uint64_t result = decrement ? interp_alu_sub(cpu, a, 1, 0, width) : interp_alu_add(cpu, a, 1, 0, width);
    interp_set_cf(cpu, cf);
    return result;
}

// x86 condition codes, indexed by the low nibble of a Jcc/SETcc/CMOVcc opcode.
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

// x86 RFLAGS as pushfq/lahf see it, assembled from the substrate. Bit 1 reads as 1 (the converter supplies
// it) and IF (bit 9) reads as 1 because a guest always observes interrupts enabled. Mirrors the JIT's
// pushfq lowering bit for bit -- a guest that pushfq/popfq round-trips must see no difference.
static uint64_t interp_read_rflags(const struct cpu *cpu) {
    uint64_t flags = hl_x86_signal_nzcv_to_eflags(cpu->nzcv);
    flags |= UINT64_C(1) << 9;                     // IF
    flags |= (cpu->df & 1) << 10;                  // DF
    if (interp_pf(cpu)) flags |= UINT64_C(1) << 2; // PF
    flags |= ((cpu->af >> 4) & 1) << 4;            // AF
    flags |= (cpu->idflag & 1) << 21;              // ID (software-toggleable CPUID probe bit)
    return flags;
}

// popfq/sahf: distribute an x86 flag word back across the substrate.
static void interp_write_rflags(struct cpu *cpu, uint64_t flags) {
    cpu->nzcv = hl_x86_signal_eflags_to_nzcv(flags);
    cpu->df = (flags >> 10) & 1;
    cpu->idflag = (flags >> 21) & 1;
    cpu->af = ((flags >> 4) & 1) << 4;
    // The PF lane stores a byte whose EVEN parity is PF, so write the byte that encodes the popped bit: 0
    // has even parity (PF=1), 1 has odd parity (PF=0). The same trick the JIT's sahf lowering uses.
    cpu->pf = ((flags >> 2) & 1) ^ 1u;
}

// ---------------------------------------------------------------------------------------------------
// Register and r/m operand access.
// ---------------------------------------------------------------------------------------------------

// AH/CH/DH/BH: WITHOUT a REX prefix, byte register numbers 4..7 name the HIGH byte of rAX/rCX/rDX/rBX
// rather than the low byte of rSP/rBP/rSI/rDI. Every byte-width register access has to ask.
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
    // A byte or word write MERGES: x86 preserves the surrounding bits of the 64-bit register.
    case 1: cpu->r[number] = (cpu->r[number] & ~UINT64_C(0xff)) | (value & 0xff); break;
    case 2: cpu->r[number] = (cpu->r[number] & ~UINT64_C(0xffff)) | (value & 0xffff); break;
    // A 32-bit write ZERO-EXTENDS. This is the x86-64 rule that makes `xor eax,eax` clear all of rax and
    // `mov eax,N` a legitimate way to load a small constant; getting it wrong leaves stale high bits that
    // surface much later as a wild pointer.
    case 4: cpu->r[number] = value & UINT64_C(0xffffffff); break;
    default: cpu->r[number] = value; break;
    }
}

// The effective address of a memory r/m operand, in GUEST coordinates (no rebias: LEA must yield the
// address the guest would compute, and interp_load/interp_store rebias at the dereference).
static uint64_t interp_ea(const struct cpu *cpu, const struct insn *insn, uint64_t next) {
    uint64_t address;
    if (insn->rip_rel) {
        // RIP-relative is measured from the END of the instruction, which is why `next` is threaded down
        // to here rather than recomputed from insn->len.
        address = next + (uint64_t)insn->disp;
    } else {
        address = 0;
        if (insn->m_hasbase) address += cpu->r[insn->m_base];
        if (insn->m_hasindex) address += cpu->r[insn->m_index] << insn->m_scale;
        address += (uint64_t)insn->disp;
    }
    // 0x67 (address-size override): the whole computation happens modulo 2^32 and zero-extends, so one
    // mask at the end is equivalent to masking every term.
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

// ---------------------------------------------------------------------------------------------------
// Atomic read-modify-write for LOCK-prefixed instructions (and for XCHG with a memory operand, which x86
// makes implicitly locked).
//
// The naturally-aligned case -- essentially all of it, because compilers align their atomics -- uses a host
// compare-exchange, which on an x86-64 host is a real `lock cmpxchg` and therefore has exactly the guest's
// atomicity and ordering. The unaligned (split-lock) case, which x86 permits and ARM's LSE atomics refuse,
// falls back to a hashed spinlock: the same shape cmpxchg.c uses for cmpxchg16b, and livelock-free where a
// hardware wide CAS is not.
// ---------------------------------------------------------------------------------------------------

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
    RMW_CMP, // LOCK CMP is legal and touches nothing: read-only
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

// Perform *address = apply(old, operand) atomically and return the PRE-image, so the caller can compute
// flags from exactly the values a non-locked path would have seen.
static uint64_t interp_locked_rmw(uint64_t guest_address, int width, enum interp_rmw_kind kind, uint64_t operand,
                                  unsigned carry_in) {
    uint64_t host_address = hl_x86_guest_pointer(guest_address);
    void *pointer = (void *)(uintptr_t)host_address;
    uint64_t old = 0;
    if ((host_address & (uint64_t)(width - 1)) == 0) {
        interp_access_begin();
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
        // Split lock. Hash on the address so two guest threads hitting the same bytes serialise while
        // unrelated split-lock sites do not contend.
        unsigned hash = (unsigned)((host_address >> 3) & (INTERP_SPLIT_LOCKS - 1));
        _Atomic unsigned *lock = &g_interp_split_lock[hash];
        uint64_t next_value;
        while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE))
            ; // spin: an exchange always makes forward progress
        interp_access_begin();
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

// ---------------------------------------------------------------------------------------------------
// The block descriptor and the seam functions.
// ---------------------------------------------------------------------------------------------------

// One descriptor per translated guest PC. It carries no decoded instructions ON PURPOSE: run_block
// re-decodes from guest memory on every execution, which makes self-modifying guest code coherent by
// construction (see interp_dispatch.h's SMC note) at the cost of a decode per instruction -- an acceptable
// trade in a backend whose premise is correctness over speed.
//
// It is allocated from the shared CODE ARENA through the ordinary bump pointer (g_cp) rather than from
// malloc, and that is the whole reason it is a struct at all: the dispatcher's accounting is written in
// terms of that arena. Bumping g_cp is what eventually makes `g_cp + CACHE_EMIT_HEADROOM > g_cache +
// CACHE_SZ` true, so the wholesale flush, the stop-the-world rotation to a fresh generation,
// jit_publish_code, retired-arena reclamation, and jit_resolve_rw_code's generation stamp all keep working
// with no interpreter-specific special case. The magic word and gpc make a stale pointer -- a descriptor
// from a reclaimed arena, or a host-code pointer restored from a JIT-written persistent cache -- fail
// loudly at the first execution instead of being run as though it meant something.
#define INTERP_BLOCK_MAGIC UINT64_C(0x496e74657270426b) // "InterpBk"

struct interp_block {
    uint64_t magic;
    uint64_t gpc;        // the guest PC this descriptor stands for
    uint64_t generation; // arena generation it was allocated in (diagnostic)
};

// Produce something callable for this guest PC. Returns a distinct non-NULL pointer per guest PC, because
// map_host() returning non-NULL is exactly what suppresses re-translation.
static void *translate_block(uint64_t gpc) {
    // Observe writes made through another MAP_SHARED alias before reading an executable view backed by an
    // emulated host-page snapshot -- the same reason the JIT's translate_block opens with this.
    uint64_t source_page = gpc & ~UINT64_C(0xfff);
    filemap_refresh_emulated(source_page, source_page + UINT64_C(0x1000));
    HL_LOGF(&g_jit_log, HL_LOG_TAG_TRANSLATE, "isa=x86_64 interp guest_pc=%#llx", (unsigned long long)gpc);
    // 16-byte align the record so a descriptor never straddles a cache line and an arena dump reads
    // cleanly. Nothing here requires it; it costs at most 15 bytes per block.
    while ((uintptr_t)g_cp & 15)
        g_cp++;
    struct interp_block *block = (struct interp_block *)g_cp;
    g_cp += sizeof *block;
    block->magic = INTERP_BLOCK_MAGIC;
    block->gpc = gpc;
    block->generation = g_cache_gen;
    // Register it exactly as the JIT registers a block: host == body (there is no separate chain entry
    // point past a prologue to skip), and the SOURCE range [gpc, gpc+1) so SMC range invalidation and
    // jit86_drop_range_translations can find it by guest address. The JIT records the whole decoded extent
    // because its host code embeds those bytes; a descriptor embeds none, so one byte -- "there is a
    // translation keyed at this PC" -- is the honest extent.
    map_put(gpc, gpc, gpc + 1, block, block);
    return block;
}

// W5B tier-2 promotion recompiles a hot self-loop with a folded back-edge. There is nothing to fold: this
// backend has no emitted back-edge and no in-cache counter to trip, so R_TIER2 is unreachable here
// (interp_dispatch.h normalizes it to R_BRANCH should a restored image ever carry it). The function
// survives because core/dispatch.c calls it unconditionally after the reason hook.
static void tier2_promote(uint64_t gpc) {
    (void)gpc;
}

// ---------------------------------------------------------------------------------------------------
// Unimplemented-instruction reporting.
//
// Every class this backend does not execute yet funnels here, so exactly one place decides what a gap looks
// like from the outside. It reports through the engine's existing route for this event -- stderr plus the
// translate log, then reason 99, which interp_dispatch.h's dispatcher arm turns into a clean guest
// termination with exit code 70 -- and unlike the JIT's report_unimpl it leaves cpu->rip EXACT rather than
// overwriting it with a 0xDEAD marker, so the reported PC is directly usable in a disassembly.
//
// The report names the instruction CLASS as well as the bytes, because "which class do I implement next" is
// the actual question its reader has. Bytes are read through the guest fetch path, not a raw dereference,
// so reporting an instruction next to an unmapped page cannot itself fault.
// ---------------------------------------------------------------------------------------------------
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
    cpu->rip = pc; // exact, unlike the JIT's 0xDEAD marker
    cpu->reason = 99;
    return 1; // STEP_END
}

// ---------------------------------------------------------------------------------------------------
// The interpreter.
// ---------------------------------------------------------------------------------------------------

enum { STEP_NEXT = 0, STEP_END = 1 };

// Deliver a guest trap signal by ending the block with R_TRAP, exactly as the JIT's emit_guest_signal does:
// cpu->divop carries (linux_signo | si_code<<8) and cpu->rip is the architectural PC the handler observes.
// Routing through the dispatcher rather than raising a host signal keeps delivery independent of what the
// host would do with the equivalent illegal instruction.
static int interp_guest_trap(struct cpu *cpu, uint64_t rip, int signo, int si_code) {
    cpu->divop = (uint64_t)((signo & 0xff) | ((si_code & 0xff) << 8));
    cpu->rip = rip;
    cpu->reason = R_TRAP;
    return STEP_END;
}

// End the block at `rip` with `reason`, for the classes whose emulation lives in a shared C helper the
// dispatcher calls (CPUID, AVX, the 0F38/0F3A map, rep cmps/scas, the x87 helpers, 128/64 division).
static int interp_exit(struct cpu *cpu, uint64_t rip, uint64_t reason) {
    cpu->rip = rip;
    cpu->reason = reason;
    return STEP_END;
}

// In 64-bit mode push/pop have a DEFAULT operand size of 64 bits: REX.W is redundant and only 0x66 changes
// it. The decoder reports opsize from REX.W alone, so every stack instruction asks for this separately
// rather than reading insn->opsize.
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

// ---- shifts and rotates ---------------------------------------------------------------------------
// The exact x86 flag rules, which are where interpreters usually break:
//   * A zero effective count changes NO flags at all (and for a 32-bit register destination still zeroes
//     the upper half, because every 32-bit write does -- handled at the call site).
//   * The count is masked to 5 bits for 8/16/32-bit operands and 6 bits for 64-bit, so `shlb $8` really
//     shifts by 8 and produces 0; it does not become a no-op.
//   * CF is the last bit shifted out. OF is architecturally defined only for a 1-bit shift; hardware
//     computes it as below and the JIT reproduces exactly these formulas, so this does too.
//   * ROL/ROR write ONLY CF (plus OF for a 1-bit rotate): SF/ZF/PF/AF are left alone. And CF is written
//     whenever the MASKED count is nonzero even if the rotate amount (count % width) is zero -- `rolb $8`
//     leaves the value unchanged but still sets CF.
// RCL/RCR by CL is handed to the shared C helper via R_RCL, whose descriptor format rotate.c fixes.
static uint64_t interp_shift(struct cpu *cpu, int kind, uint64_t value, unsigned count_raw, int width) {
    uint64_t m = interp_mask(width);
    unsigned bits = (unsigned)(8 * width);
    unsigned count = count_raw & (width == 8 ? 63u : 31u);
    uint64_t original = value & m;
    uint64_t result = original;
    if (kind == 0 || kind == 1) { // ROL / ROR -- rotate WITHIN the operand width
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
    if (count == 0) return original; // SHL/SHR/SAR by 0: value and flags both untouched
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
    // SF/ZF/PF come from the result. AF is x86-undefined here and the JIT's shift path writes none, so it
    // is left untouched to keep the two backends bit-identical.
    interp_flags_nzcv(cpu, interp_msb(result, width), result == 0, cf, of);
    cpu->pf = result & 0xff;
    return result;
}

// SHLD/SHRD: a double-precision shift of `value` with `fill` supplying the bits shifted in. Flags follow
// the single shift of the same direction; a zero count changes nothing.
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

// ---- DIV / IDIV -----------------------------------------------------------------------------------
// x86 raises #DE both for a zero divisor and for a quotient that does not fit the result width (DIV
// 0x1FF/1 at byte width, IDIV INT_MIN/-1). Delivering that signal needs raise_guest_de, which lives past
// this include and cannot `continue` the dispatcher loop from in here anyway, so both cases exit with
// divop == 0 and reason R_DIV/R_IDIV at the FAULTING instruction's PC -- byte-for-byte the convention the
// JIT's emit_div_zero_check / emit_div_ovf_check establish, so the dispatcher arm serves both backends.
//
// The 64-bit form is not computed here at all: it needs a true 128/64 division, which the dispatcher
// already performs (with the same overflow check) when handed divop = the divisor and rip = next. Reusing
// that is both less code and guaranteed-identical behaviour.
static int interp_divide(struct cpu *cpu, uint64_t divisor, int width, int is_signed, uint64_t pc, uint64_t next) {
    uint64_t reason = is_signed ? R_IDIV : R_DIV;
    if (divisor == 0) {
        cpu->divop = 0;
        return interp_exit(cpu, pc, reason); // #DE at the faulting instruction
    }
    if (width == 8) {
        cpu->divop = divisor;
        return interp_exit(cpu, next, reason); // 128/64 in the dispatcher, including quotient overflow
    }
    unsigned bits = (unsigned)(8 * width);
    uint64_t m = interp_mask(width);
    if (!is_signed) {
        // The dividend is the low `bits` of AX/DX:AX/EDX:EAX -- for byte width it is AX alone, which is why
        // the byte case reads the low 16 bits of rax rather than assembling from rdx.
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
            // A 32-bit result zero-extends into the full 64-bit register; a 16-bit result merges.
            uint64_t q = quotient & m, r = remainder & m;
            cpu->r[RAX] = width == 4 ? q : ((cpu->r[RAX] & ~m) | q);
            cpu->r[RDX] = width == 4 ? r : ((cpu->r[RDX] & ~m) | r);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }
    // Signed: sign-extend both the dividend halves and the divisor out of their operand width.
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
    if (low != quotient) { // the quotient does not fit the result width -> #DE
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

// ---- MUL / IMUL -----------------------------------------------------------------------------------
// The widening forms (F6/F7 /4 and /5) set CF and OF when the high half is significant and leave
// SF/ZF/PF/AF x86-undefined. The JIT writes N=Z=0 with C/V from that predicate (e_mul_set_oc), so this
// does the same rather than inventing SF/ZF: two backends disagreeing about an undefined flag is exactly
// the kind of difference that shows up as a mysterious compat-corpus diff much later.
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
        // CF=OF when the high half is not the sign extension of the low half.
        int64_t sign_extended_low = (int64_t)(low << (64 - 8 * width)) >> (64 - 8 * width);
        overflow = (unsigned)((__int128)sign_extended_low != product);
    } else {
        unsigned __int128 product = (unsigned __int128)(cpu->r[RAX] & m) * (source & m);
        low = (uint64_t)product & m;
        high = (uint64_t)(product >> (8 * width)) & m;
        overflow = high != 0;
    }
    if (width == 1) {
        // MUL r/m8 writes AX, not AH:AL as two operands: the whole 16-bit product lands in AX.
        cpu->r[RAX] = (cpu->r[RAX] & ~UINT64_C(0xffff)) | (low & 0xff) | ((high & 0xff) << 8);
    } else {
        interp_reg_write(cpu, insn, RAX, width, low);
        interp_reg_write(cpu, insn, RDX, width, high);
    }
    interp_flags_nzcv(cpu, 0, 0, overflow, overflow);
}

// Two/three-operand IMUL (0F AF, 69, 6B): the result is truncated to the operand width and CF=OF report
// that the untruncated product did not fit. SF/ZF are x86-undefined; hardware sets them from the truncated
// result, and so does the JIT's e_imul2, so do that.
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
// x87 (the D8..DF ESC space) is decoded in the one-byte map but lives with the other floating point, below
// interp_step_sse, so that the whole FP story -- and the one MXCSR decision behind it -- reads in one place.
static int interp_step_x87(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next);

// One instruction. On STEP_NEXT cpu->rip has been advanced; on STEP_END cpu->rip and cpu->reason are final.
static int interp_step(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    // VEX/EVEX -- AVX/AVX2/AVX-512. Emulated in C by avx.c, which owns the full v[]/vhi[]/vz[]/vx[]/kreg[]
    // register file and advances rip itself, so rip must name THIS instruction on the way out.
    if (insn->vex) {
        // An EVEX prefix (0x62) with map field mm==0 is a reserved encoding, and 0x62's legacy meaning
        // (BOUND) is invalid in long mode: both are #UD. Deliver SIGILL to the guest rather than reporting
        // an engine gap.
        if (insn->evex && insn->vex_map == 0) return interp_guest_trap(cpu, pc, 4, 2);
        return interp_exit(cpu, pc, R_AVX);
    }
    // Legacy (non-VEX) 0F38/0F3A: SSSE3/SSE4.1/SSE4.2/AES/SHA/PCLMUL/CRC32/MOVBE, emulated by avx.c's
    // do_sse3b, which likewise advances rip.
    if (insn->map3) return interp_exit(cpu, pc, R_SSE3B);
    if (insn->two) return interp_step_two_byte(cpu, insn, pc, next);
    return interp_step_one_byte(cpu, insn, pc, next);
}

// The decode-and-execute loop. Runs until a block-ending event, which for this backend means every guest
// control transfer: a branch, a call, a return, a syscall, an exit to a C helper, or a fault. Ending the
// block at every branch is deliberate -- it is what makes the dispatcher's per-iteration work happen at
// guest basic-block granularity without the interpreter reimplementing any of it: the async-signal poll
// that lets a caught signal reach a guest loop making no syscalls, the checkpoint safepoint, and the
// stop-the-world safepoint are all at the top of run_guest's loop.
static void interp_execute(struct cpu *cpu) {
    for (;;) {
        uint64_t pc = cpu->rip; // authoritative and exact: a fault below reports precisely this PC
        struct insn insn;
        if (hl_x86_decode(pc, &insn) < 0) {
            // Instruction fetch failed a logical executable-mapping check. That is a guest fetch fault,
            // not an engine dereference crash: deliver SIGSEGV/SEGV_ACCERR at the faulting PC, exactly as
            // the JIT's translate_block does when its decode fails.
            (void)interp_guest_trap(cpu, pc, 11, 2);
            return;
        }
        if (interp_step(cpu, &insn, pc, pc + (uint64_t)insn.len) == STEP_END) return;
    }
}

// ---------------------------------------------------------------------------------------------------
// run_block / block_return -- the two symbols core/dispatch.c and the fault path name.
//
// On AArch64 these are hand-written trampolines: run_block spills the host callee-saved registers into
// struct cpu and branches into emitted code, and block_return is the address emitted code jumps back
// through. Here run_block IS the interpreter, so there is no host register file to spill and no far end to
// the bridge. block_return keeps existing as a real, address-taken function because
// core/target/x86_64.c's sigframe_resume_dispatch passes its address to hl_x86_signal_resume; reaching it
// would mean something branched to an address only emitted code ever branches to, so it says so and aborts
// rather than returning into a stack it does not own.
// ---------------------------------------------------------------------------------------------------

// STATIC, and that is load-bearing rather than tidiness. The dual activation archive links BOTH target
// objects into one binary (cmake/Phase2Production.cmake builds hl-engine-activation from dual_aarch64_target
// and dual_x86_64_target), and src/core/target/namespace.h -- which renames the per-ISA symbols so the two
// can coexist -- does not cover run_block/block_return. The JIT gets away with that because its trampolines
// come from a file-scope __asm__ block with `.hidden`, which GCC emits as a LOCAL symbol: `nm` on either
// aarch64 object shows a lowercase `t`. A non-static C definition here is a GLOBAL, so both interpreters
// defining one collides at link time and takes dual-backend-link-test with it -- 300-odd cascading undefined
// references, because the linker then rejects the whole object. Local linkage matches what the JIT already
// does, and it costs nothing: interp.c is #included into the target translation unit, so every caller --
// core/dispatch.c's run_block(c, rxcode), and the code that takes &block_return -- is in this same TU.
static void run_block(struct cpu *cpu, void *code);
static void block_return(void);

static void run_block(struct cpu *cpu, void *code) {
    struct interp_block *block = (struct interp_block *)code;
    // Validate the descriptor. A mismatch means the cache handed us something that is not one of ours -- a
    // host-code pointer restored from a JIT-written persistent cache, or a descriptor from an arena that
    // was reclaimed under us. Both are conditions where continuing executes nonsense, and both are engine
    // bugs rather than guest input, so name the likely cause and stop.
    if (block == NULL || block->magic != INTERP_BLOCK_MAGIC) {
        fprintf(stderr,
                "[hl] interp: block descriptor at %p is not one of ours (magic=%llx, expected %llx) for rip=%llx.\n"
                "     A persistent cache written by the ARM64 backend, or a reclaimed arena generation, is the\n"
                "     only way to reach this.\n",
                code, (unsigned long long)(block ? block->magic : 0), (unsigned long long)INTERP_BLOCK_MAGIC,
                (unsigned long long)cpu->rip);
        abort();
    }
    // The landing pad for a guest memory fault. savemask=1 because the jump arrives from a signal handler
    // with the fault signal blocked; without it the next guest fault would kill the process.
    int previous = g_interp_pad_armed;
    if (sigsetjmp(g_interp_fault_pad, 1) != 0) {
        // A guest access faulted; the shared delivery path queued the guest signal, set cpu->reason and
        // longjmped here. cpu->rip already names the faulting instruction.
        g_interp_guest_access = 0;
        g_interp_pad_armed = previous;
        return;
    }
    g_interp_pad_armed = 1;
    interp_execute(cpu);
    g_interp_pad_armed = previous;
}

static void block_return(void) {
    fprintf(stderr, "[hl] interp: block_return() entered on a " HL_HOST_CPU_NAME " host. Only translated ARM64\n"
                    "     blocks branch here and this backend emits none -- so its address was baked into\n"
                    "     something that then ran (a stale persistent-cache image or a mis-relocated exit).\n");
    abort();
}

// ---------------------------------------------------------------------------------------------------
// The one-byte opcode map.
// ---------------------------------------------------------------------------------------------------

// The eight ALU kinds, in x86 opcode order (op >> 3 for the 00..3F block, ModRM /reg for group 1).
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

static const enum interp_rmw_kind g_alu_rmw[8] = {RMW_ADD, RMW_OR, RMW_ADC, RMW_SBB,
                                                  RMW_AND, RMW_SUB, RMW_XOR, RMW_CMP};

// Apply one ALU kind and set flags. *store is cleared for CMP, which discards its result.
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

// One ALU instruction whose destination is the r/m operand. Factored out because five encodings reach it
// (00/01, 08/09, ..., plus group 1's 80/81/83) and because the LOCK path must be shared by all of them.
static void interp_alu_to_rm(struct cpu *cpu, struct insn *insn, const interp_operand *operand, int kind, int width,
                             uint64_t source) {
    int store;
    if (insn->lock && operand->is_memory && kind != ALU_CMP) {
        // Read-modify-write atomically, then recompute the flags from the pre-image. The double computation
        // is deliberate: it guarantees a locked op and its unlocked twin set flags identically.
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

    // ---- 00..3F: the ALU block (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP) --------------------------------------
    // Forms, by op & 7: 0 = r/m8,r8   1 = r/m,r   2 = r8,r/m8   3 = r,r/m   4 = AL,imm8   5 = eAX,imm.
    // Forms 6 and 7 in this range are the segment push/pop and BCD opcodes, all invalid in long mode, so
    // they fall through to the report at the bottom.
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
    // ---- PUSH / POP register (50..5F) ------------------------------------------------------------
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57: {
        int width = interp_stack_width(insn);
        // The register number is in the low three opcode bits, extended by REX.B -- there is no ModRM.
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
        // `pop rsp` must take the POPPED value, not the incremented one -- interp_pop already advanced
        // rsp, so writing the register afterwards is the correct order.
        if (width == 2)
            cpu->r[number] = (cpu->r[number] & ~UINT64_C(0xffff)) | (value & 0xffff);
        else
            cpu->r[number] = value;
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOVSXD (63): sign-extend a 32-bit r/m into a 64-bit register -----------------------------
    case 0x63: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, 4);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         insn->opsize == 8 ? (uint64_t)(int64_t)(int32_t)(uint32_t)source : source);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- PUSH immediate (68 imm32 / 6A imm8), sign-extended to the stack width --------------------
    case 0x68:
    case 0x6A:
        interp_push(cpu, (uint64_t)insn->imm, interp_stack_width(insn));
        cpu->rip = next;
        return STEP_NEXT;

    // ---- IMUL r, r/m, imm (69 imm32 / 6B imm8) ---------------------------------------------------
    case 0x69:
    case 0x6B: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, insn->opsize);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         interp_imul_truncating(cpu, source, (uint64_t)insn->imm, insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- Jcc rel8 (70..7F) -----------------------------------------------------------------------
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
        // A conditional branch ends the block on BOTH edges. The taken edge obviously must; the fall
        // through edge does too, because returning to the dispatcher is what gives the async-signal poll
        // and the safepoints a chance to run inside a guest loop.
        cpu->rip = interp_cond(cpu, op & 0xf) ? next + (uint64_t)insn->imm : next;
        cpu->reason = R_BRANCH;
        return STEP_END;

    // ---- Group 1: ALU r/m, imm (80 ib / 81 iz / 83 ib-sign-extended) -----------------------------
    case 0x80:
    case 0x81:
    case 0x83: {
        int width = (op == 0x80) ? 1 : insn->opsize;
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_alu_to_rm(cpu, insn, &operand, insn->reg & 7, width, (uint64_t)insn->imm);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- TEST r/m, r (84/85) ---------------------------------------------------------------------
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

    // ---- XCHG r/m, r (86/87) ---------------------------------------------------------------------
    // With a memory operand this is IMPLICITLY locked on x86, whether or not a LOCK prefix is present --
    // which is why it goes through interp_locked_rmw unconditionally rather than only under insn->lock.
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

    // ---- MOV (88/89 r/m<-r, 8A/8B r<-r/m) --------------------------------------------------------
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

    // ---- LEA (8D): the address, never a dereference ----------------------------------------------
    case 0x8D: {
        if (!insn->is_mem) return interp_undefined(cpu, insn, pc, "LEA with a register operand (#UD encoding)");
        // Deliberately interp_ea, not interp_load: LEA computes an address in the GUEST domain and must not
        // rebias, or a non-PIE guest would see a pointer it never formed.
        interp_reg_write(cpu, insn, insn->reg, insn->opsize, interp_ea(cpu, insn, next));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- POP r/m (8F /0) -------------------------------------------------------------------------
    case 0x8F: {
        int width = interp_stack_width(insn);
        if ((insn->reg & 7) != 0) return interp_undefined(cpu, insn, pc, "group 1A opcode other than POP r/m");
        uint64_t value = interp_pop(cpu, width);
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, width, value);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- NOP (90) and XCHG rAX, r (91..97) -------------------------------------------------------
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {
        int number = (op & 7) | (insn->rexB << 3);
        // 0x90 without REX.B is XCHG rAX,rAX, i.e. NOP -- including `xchg ax,ax` under 0x66 and the F3 90
        // PAUSE hint, which has no architectural effect beyond a scheduling hint.
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

    // ---- CBW/CWDE/CDQE (98) and CWD/CDQ/CQO (99) -------------------------------------------------
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
        // The sign bit of the accumulator, replicated across the whole of rDX at the operand width.
        uint64_t sign = interp_msb(cpu->r[RAX] & interp_mask(width), width) ? UINT64_MAX : 0;
        interp_reg_write(cpu, insn, RDX, width, sign);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- FWAIT (9B): no x87 exceptions are ever pending in this model ----------------------------
    case 0x9B:
        cpu->rip = next;
        return STEP_NEXT;

    // ---- PUSHFQ / POPFQ / SAHF / LAHF (9C..9F) ---------------------------------------------------
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
        // Preserve OF: SAHF writes only the low byte of the flag word, and OF lives at bit 11.
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

    // ---- MOV to/from a direct moffs address (A0..A3) ---------------------------------------------
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

    // ---- MOVS / STOS / LODS (A4/A5, AA/AB, AC/AD) ------------------------------------------------
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
        // Element at a time, repeated in place under REP, updating RCX/RSI/RDI after EACH element. That
        // per-element update is what makes a fault mid-string architecturally correct: x86 reports the
        // fault with the pointers and counter partially advanced, so a guest handler that maps the page and
        // returns resumes exactly where it stopped rather than replaying bytes it already copied. (The
        // shared bulk helper cannot offer that, and cannot be linked here at all -- see the note at the top
        // of this file about lower/repstr.c. TODO(amd64-host): once those runtime helpers are split out of
        // the lowering object, the forward non-overlapping case can take the one-memcpy path.)
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

    // ---- CMPS / SCAS (A6/A7, AE/AF) --------------------------------------------------------------
    // Handed whole to hl_x86_rep_compare through R_REPSTR: it writes the exact end state of RCX/RSI/RDI
    // and the compare's flags, including the REPE-vs-REPNE stop conditions and the RCX==0 "nothing
    // happened, flags unchanged" rule. The descriptor layout is rep.c's, not ours.
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

    // ---- TEST AL/eAX, imm (A8/A9) ----------------------------------------------------------------
    case 0xA8:
    case 0xA9: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_flags_logic(cpu, interp_reg_read(cpu, insn, RAX, width) & (uint64_t)insn->imm & interp_mask(width),
                           width);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOV r, imm (B0..B7 byte, B8..BF word/dword/movabs) --------------------------------------
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

    // ---- Group 2: shifts and rotates -------------------------------------------------------------
    // C0/C1 by imm8, D0/D1 by 1, D2/D3 by CL. /2 and /3 are RCL/RCR (rotate through carry); the by-CL
    // form of those needs a count reduction modulo width+1 and is handed to the shared C helper.
    case 0xC0:
    case 0xC1:
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {
        int kind = insn->reg & 7;
        if (kind == 6) kind = 4; // the /6 encoding of SHL is SAL, the same operation
        int width = (op & 1) ? insn->opsize : 1;
        int by_cl = (op == 0xD2 || op == 0xD3);
        int by_one = (op == 0xD0 || op == 0xD1);
        interp_operand operand = interp_rm(cpu, insn, next);
        if (kind == 2 || kind == 3) { // RCL / RCR
            uint64_t descriptor = (uint64_t)width | ((uint64_t)(kind == 3) << 8);
            if (operand.is_memory) {
                // rotate.c dereferences this address directly, so hand it the REBASED host address: it has
                // no access to the non-PIE fold.
                cpu->x87_ea = hl_x86_guest_pointer(operand.address);
                descriptor |= UINT64_C(1) << 9;
            } else {
                int high_byte = interp_hi8(insn, operand.number, width);
                descriptor |= ((uint64_t)(high_byte ? 1 : 0) << 10) |
                              ((uint64_t)((high_byte ? operand.number - 4 : operand.number) & 0x1f) << 16);
            }
            if (!by_cl) {
                // A constant count: pre-load CL's slot in the descriptor by staging the count where the
                // helper reads it. The helper reads cpu->r[RCX], so a constant-count RCL/RCR would have to
                // clobber RCX -- which it must not. Do the constant case here instead.
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
            // The by-CL form goes to the shared helper, which reads its whole operand description out of
            // cpu->divop (rotate.c's hl_x86_rotate_carry). Publishing it is not optional: without this
            // store the helper decoded a STALE divop -- whatever the last R_DIV/R_TRAP/R_REPSTR left there
            // -- and a width of 0 makes its `masked % (bits + 1)` reduce to 0, so `rclb %cl, %bl` retired
            // without rotating anything and without touching CF.
            cpu->divop = descriptor;
            return interp_exit(cpu, next, R_RCL);
        }
        unsigned count = (unsigned)(by_cl ? (cpu->r[RCX] & 0xff) : (by_one ? 1u : (unsigned)(insn->imm & 0xff)));
        uint64_t value = interp_rm_read(cpu, insn, &operand, width);
        uint64_t result = interp_shift(cpu, kind, value, count, width);
        // A zero effective count still WRITES a 32-bit register destination (every 32-bit write
        // zero-extends), and still rewrites a memory destination unchanged. Both are what the JIT does.
        interp_rm_write(cpu, insn, &operand, width, result);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- RET (C3) and RET imm16 (C2) -------------------------------------------------------------
    case 0xC2:
    case 0xC3: {
        int width = interp_stack_width(insn);
        uint64_t target = interp_pop(cpu, width);
        if (op == 0xC2) cpu->r[RSP] += (uint64_t)(uint16_t)insn->imm;
        cpu->dbg_ibsrc = pc; // debug: the guest PC of the last indirect branch
        cpu->rip = target;
        cpu->reason = R_BRANCH;
        return STEP_END;
    }

    // ---- MOV r/m, imm (C6 ib / C7 iz) ------------------------------------------------------------
    case 0xC6:
    case 0xC7: {
        if ((insn->reg & 7) != 0) return interp_undefined(cpu, insn, pc, "group 11 opcode other than MOV r/m,imm");
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, width, (uint64_t)insn->imm);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- LEAVE (C9): rsp = rbp; pop rbp ----------------------------------------------------------
    case 0xC9: {
        int width = interp_stack_width(insn);
        cpu->r[RSP] = cpu->r[RBP];
        cpu->r[RBP] = interp_pop(cpu, width);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- INT3 (CC): #BP -> SIGTRAP at the instruction AFTER the trap -----------------------------
    case 0xCC:
        return interp_guest_trap(cpu, next, 5 /*SIGTRAP*/, 1 /*TRAP_BRKPT*/);

    // ---- LOOP / LOOPE / LOOPNE (E0/E1/E2) and JRCXZ (E3) -----------------------------------------
    case 0xE0:
    case 0xE1:
    case 0xE2: {
        // The counter is RCX, or ECX under a 0x67 address-size override (which then also zero-extends).
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

    // ---- CALL rel32 (E8) -------------------------------------------------------------------------
    case 0xE8:
        // interp_call_return_pc, not `next`: a biased non-PIE image must push the LINK address so guest
        // unwinders see the PCs they would see on Linux.
        interp_push(cpu, interp_call_return_pc(next), interp_stack_width(insn));
        cpu->rip = next + (uint64_t)insn->imm;
        cpu->reason = R_BRANCH;
        return STEP_END;

    // ---- JMP rel32 (E9) / rel8 (EB) --------------------------------------------------------------
    case 0xE9:
    case 0xEB:
        cpu->rip = next + (uint64_t)insn->imm;
        cpu->reason = R_BRANCH;
        return STEP_END;

    // ---- CMC / CLC / STC / CLD / STD -------------------------------------------------------------
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

    // ---- Group 3 (F6/F7): TEST imm, NOT, NEG, MUL, IMUL, DIV, IDIV -------------------------------
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

    // ---- Group 4/5 (FE/FF): INC, DEC, CALL r/m, JMP r/m, PUSH r/m --------------------------------
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
        // /3 CALLF and /5 JMPF are far transfers through a memory descriptor: there are no segments to
        // transfer to in this model, and Linux user space never issues them.
        return interp_undefined(cpu, insn, pc, "far CALL/JMP through a segment descriptor");
    }

    // ---- HLT (F4): privileged, so #GP in user space -> SIGSEGV -----------------------------------
    case 0xF4:
        return interp_guest_trap(cpu, pc, 11 /*SIGSEGV*/, 2 /*SEGV_ACCERR*/);

    default: break;
    }

    // x87: the D8..DF ESC space, decoded here but implemented next to the SSE floating point below.
    if (op >= 0xD8 && op <= 0xDF) return interp_step_x87(cpu, insn, pc, next);
    if (op == 0xD7) return interp_undefined(cpu, insn, pc, "TODO(amd64-host): XLATB");
    if (op == 0xCD || op == 0xCE || op == 0xCF || op == 0xCA || op == 0xCB)
        return interp_undefined(cpu, insn, pc, "software interrupt / far return (INT/INTO/IRET/RETF)");
    if (op == 0x6C || op == 0x6D || op == 0x6E || op == 0x6F || op == 0xE4 || op == 0xE5 || op == 0xE6 || op == 0xE7 ||
        op == 0xEC || op == 0xED || op == 0xEE || op == 0xEF)
        return interp_undefined(cpu, insn, pc, "port I/O (IN/OUT/INS/OUTS), privileged");
    return interp_undefined(cpu, insn, pc, "one-byte opcode");
}

// ---------------------------------------------------------------------------------------------------
// LEGACY SSE / SSE2 (the 0F map).
//
// WHY THIS IS HERE AND NOT IN avx.c. The dispatcher already routes VEX/EVEX to hl_x86_avx_run and the
// 0F38/0F3A escape maps to hl_x86_sse_run (R_AVX / R_SSE3B), so SSSE3/SSE4/AES/SHA/CRC32 come for free.
// The base 0F map has no such emulator: the JIT lowered it instruction-by-instruction to NEON, which is
// exactly the code that cannot exist on this host. So it is implemented here, in the interpreter, where the
// register file is already plain memory.
//
// WHAT IS IMPLEMENTED, AND THE LINE THAT WAS DRAWN. Everything below is DATA MOVEMENT, BITWISE LOGIC, or
// INTEGER SIMD -- operations whose result is a pure function of the input bits. Packed and scalar
// FLOATING-POINT ARITHMETIC (ADDPS/MULPD/DIVSS/SQRTPD, the CVT* conversions, UCOMISS/COMISS/CMPPS) is
// deliberately NOT implemented and routes to interp_undefined naming itself. That is not laziness: those
// need MXCSR to be authoritative -- rounding mode, the denormals-are-zero and flush-to-zero controls, and
// the six sticky exception flags -- and x87state.c already owns a real x86-64 MXCSR arm via
// _mm_getcsr/_mm_setcsr. Emulating them with host C arithmetic would silently ignore the guest's rounding
// mode and never accumulate an exception flag, which is the kind of wrong that passes every smoke test and
// then produces a subtly different number a year later. The bit-movement and integer classes have no such
// coupling, which is precisely why the boundary is where it is.
//
// THE UPPER-BITS RULE, which is the opposite of the AArch64 one and the easiest thing to get wrong here.
// A legacy (non-VEX) SSE instruction writes the low 128 bits of the register and LEAVES BITS 128 AND ABOVE
// UNTOUCHED. VEX-encoded instructions zero them. cpu->v[2r..2r+1] is xmm r; cpu->vhi[2r..]/vz[] carry the
// AVX upper state, and avx.c honours the same split (see its "legacy SSE leaves the upper YMM bits intact"
// comment). So interp_xmm_put writes ONLY v[] and never clears vhi -- a `pxor %xmm0,%xmm0` must not
// silently truncate a ymm0 that a preceding VEX instruction filled, or a subsequent vpaddb would read
// zeros the guest never wrote.
//
// cpu->vdirty is deliberately not touched. It exists so the JIT's slim R_SYSCALL exit knows whether guest
// xmm is live in HOST v registers and needs spilling; here cpu->v[] IS the register file at every
// instruction boundary, so there is nothing to spill and nothing to mark.
// ---------------------------------------------------------------------------------------------------

// xmm register file. The byte image is the natural representation: every operation below is defined on
// lanes, the guest is little-endian, so is the host, and a byte array sidesteps both strict aliasing and
// any question about lane order.
static void interp_xmm_get(const struct cpu *cpu, int number, uint8_t out[16]) {
    memcpy(out, &cpu->v[2 * number], 16);
}

static void interp_xmm_put(struct cpu *cpu, int number, const uint8_t in[16]) {
    memcpy(&cpu->v[2 * number], in, 16); // low 128 bits only -- see the upper-bits rule above
}

// Lane accessors. memcpy rather than a cast so an odd lane offset (PINSRW into word 3, an unaligned m128)
// stays defined behaviour.
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

// The mandatory prefix that selects an SSE opcode's variant. F2/F3 outrank 0x66 when a guest emits both,
// matching the hardware precedence, so the tests are ordered rather than combined.
enum { SSE_NP = 0, SSE_66 = 1, SSE_F3 = 2, SSE_F2 = 3 };

static int interp_sse_prefix(const struct insn *insn) {
    if (insn->rep) return SSE_F3;
    if (insn->repne) return SSE_F2;
    if (insn->p66) return SSE_66;
    return SSE_NP;
}

// Read the r/m operand of an SSE instruction as `bytes` bytes of image: an xmm register (mod == 3) or
// memory. `bytes` is 16 for the packed forms and 4/8 for the scalar and half-register ones; the untouched
// tail of `out` is zeroed so a caller that merges only the low lanes still sees defined bytes.
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

// MOVDQA / MOVAPS / MOVAPD / MOVNTDQ / MOVNTPS require a 16-byte-aligned memory operand and raise #GP(0)
// when it is not. That fault is honoured rather than quietly accepted: a guest can legitimately DEPEND on
// it (a runtime probing alignment, or a bug it expects to crash on), and silently serving the access would
// make this backend disagree with real hardware in the one direction nobody would ever notice until it
// mattered. Linux reports #GP as SIGSEGV with si_code SI_KERNEL and si_addr 0, which is what the R_TRAP
// path below produces for signal 11.
static int interp_sse_unaligned(const struct cpu *cpu, const struct insn *insn, uint64_t next) {
    return insn->is_mem && (interp_ea(cpu, insn, next) & 15u) != 0;
}

// ---- packed integer primitives -------------------------------------------------------------------
// Each takes the destination image (operand 1, which x86 SSE always both reads and writes) and the source
// image, and is defined on a lane width in bytes. Writing them as loops over a byte image rather than as
// per-width duplicates keeps the arithmetic in one place, which is where the saturation rules can be
// checked once instead of four times.

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

// Saturating add/subtract. `subtract` picks the direction, `signed_form` the saturation domain.
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

// PCMPGT is SIGNED on every lane width -- the unsigned comparison has no SSE2 encoding, which is why
// string code reaches for PMINUB/PCMPEQB instead.
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

// Per-lane shifts. A count at or beyond the lane width produces zero (or, for the arithmetic right shift,
// a full sign fill) rather than being reduced modulo the width -- the x86 rule, and the opposite of what a
// bare C shift by an over-wide amount would do (which is undefined behaviour).
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

// PSLLDQ / PSRLDQ: a shift of the WHOLE 128-bit register by a BYTE count, not a per-lane bit shift. The
// byte-image representation makes this a memmove, which is the clearest statement of what it does.
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

// PUNPCK*: interleave lanes from the low (or high) half of destination and source. Destination lanes come
// first in each pair, which is what makes PUNPCKLBW of a value with itself a byte-doubling broadcast.
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

// PACKSSWB / PACKUSWB / PACKSSDW: narrow with saturation, destination lanes then source lanes.
//
// `per` is 16/source_lane, NOT 8/source_lane: each 16-byte operand holds EIGHT words (or four dwords) and
// contributes all of them, filling the whole 16-byte result between the two operands. With 8/source_lane it
// was half that -- so PACKUSWB wrote only bytes 0..3 from the destination and 4..7 from the SOURCE (where
// the source's belong at 8..15), and left out[8..15] UNINITIALISED, i.e. it stored stack garbage into a
// guest register. gcc's vectoriser emits exactly this pack at the end of every byte-store loop it builds
// out of wider arithmetic, so a plain `for (i) buf[i] = i * 7 + 3;` corrupted 12 of every 16 bytes.
static void interp_pack(uint8_t *d, const uint8_t *s, int source_lane, int signed_result) {
    uint8_t out[16];
    int per = 16 / source_lane; // narrowed lanes contributed by each operand
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

// The 0F (two-byte) opcode map's SSE space. Returns STEP_NEXT/STEP_END when it owned the opcode, or
// STEP_SSE_UNHANDLED so interp_step_two_byte can fall through to its own diagnostic.
enum { STEP_SSE_UNHANDLED = -1 };

// The floating-point arithmetic / conversion / compare space. Named as a set because the routing decision
// ("can this host run guest FP natively at all?") is one decision for the whole class -- see
// interp_step_sse_fp below -- and because the diagnostic on a host that cannot should say which CLASS is
// missing rather than quote an opcode byte.
static int interp_sse_is_float_arithmetic(uint8_t op) {
    if (op >= 0x58 && op <= 0x5F) return 1;                 // add/mul/cvt/sub/min/div/max ps/pd/ss/sd
    if (op == 0x51 || op == 0x52 || op == 0x53) return 1;   // sqrt / rsqrt / rcp
    if (op == 0x2A || (op >= 0x2C && op <= 0x2F)) return 1; // cvtsi2s* / cvt*2si / ucomis* / comis*
    if (op == 0x5A || op == 0x5B) return 1;                 // cvtps2pd / cvtdq2ps and friends
    if (op == 0xC2) return 1;                               // cmpps/cmppd/cmpss/cmpsd
    if (op == 0xE6) return 1;                               // cvtdq2pd / cvtpd2dq / cvttpd2dq
    if (op == 0x7C || op == 0x7D) return 1;                 // SSE3 haddps/haddpd / hsubps/hsubpd
    if (op == 0xD0) return 1;                               // SSE3 addsubps / addsubpd
    return 0;
}

// ---------------------------------------------------------------------------------------------------
// SSE / SSE2 FLOATING POINT.
//
// THE MXCSR DECISION, which is the whole reason this class was held back from the bit-movement one.
//
// Guest MXCSR is guest-visible architectural state in four ways at once: the rounding-control field
// (14:13) selects the rounding of every arithmetic result and of every non-truncating conversion; DAZ (6)
// and FZ (15) change whether denormals exist on the way in and on the way out; the six sticky exception
// flags (0..5 = IE,DE,ZE,OE,UE,PE) ACCUMULATE and are read back by STMXCSR and by FXSAVE
// (hl_x86_fxsave); and the exception masks (12:7) decide whether an unmasked exception is reported.
//
// There is no cpu->mxcsr, and that is deliberate rather than an omission: on EVERY host this frontend
// keeps the guest MXCSR in the HOST's floating-point control register, because the guest's SSE work is
// performed by the host FP unit and so the host register is the only place the answers actually come
// from. The AArch64 backend does it the hard way -- translate.c's emit_ldmxcsr/emit_stmxcsr re-map the
// rounding field (ARM RMode orders the two directed modes the other way round), fold FZ|DAZ onto the
// single FPCR.FZ, and scatter/gather the six exception flags across FPSR's non-contiguous bits. On an
// x86-64 host that projection collapses to the identity: the guest MXCSR IS the host MXCSR, field for
// field, which is exactly what x87state.c's x86-64 arm already states and relies on.
//
// So this file takes option (a): SET NOTHING PER OPERATION, execute the guest instruction as the
// corresponding HOST instruction, and let the architectural side effects land where the guest will read
// them. LDMXCSR writes the host MXCSR; STMXCSR reads it; every op in between rounds per the live RC,
// honours DAZ/FZ, and ORs its exceptions into the live sticky flags -- because the hardware does, not
// because code here arranged it. What that buys, concretely, is everything the fpdnan and fpedge
// fixtures exist to check, for no code at all:
//
//   * NaN GENERATION with x86's sign. An invalid operation with no NaN input yields the QNaN indefinite
//     with the sign bit SET (0xFFC00000 / 0xFFF8000000000000). ARM hardware yields the same payload with
//     the sign CLEAR, which is why the AArch64 backend carries emit_dnan_pre/emit_dnan_post to stamp the
//     sign onto generated-but-not-propagated NaNs. Here the host generates x86's NaN because it IS an x86.
//   * NaN PROPAGATION and payload/operand selection. `mulss %xmm1,%xmm0` with a NaN in src2 must return
//     THAT NaN, quietened, sign intact; a two-NaN operation returns src1's. Instruction-for-instruction
//     execution reproduces the selection rule without naming it -- which is why the arithmetic below uses
//     the _mm_*_ss/_sd/_ps/_pd intrinsics (fixed operand order, one instruction) rather than C `a + b`,
//     whose operands GCC is entitled to commute.
//   * MIN/MAX's non-IEEE tie rules (`MINSS` is `(a<b)?a:b`, so a NaN in EITHER operand or a +0/-0 pair
//     returns src2 verbatim), the signed zeros, denormal inputs and outputs, and the out-of-range
//     float->int result 0x80000000/0x8000000000000000.
//
// A software model would have to re-derive every one of those, and the failure mode of getting one wrong
// is a number that is subtly different rather than a test that fails.
//
// WHAT IT COSTS, stated rather than hidden: the host MXCSR is now shared between the guest and the
// engine's own C code, so any FP the engine itself executes between guest instructions (libm inside
// hl_x86_x87_math, a printf of a diagnostic) can OR a sticky flag the guest did not raise. The AArch64
// arm has the identical leak by the identical mechanism -- it reads the live host FPSR at STMXCSR/FXSAVE
// time -- so this is a property of the "host FP register is the guest's" model, not of this host. Closing
// it means saving and restoring MXCSR around every engine excursion, which is a change to the dispatcher
// and to core/target/x86_64.c, not to this file.
//
// UPPER LANES. Every scalar form below MERGES: the low 32 (SS) or 64 (SD) bits of the destination are
// written and the rest of the 128 are the destination's own previous bits, not the source's and not zero.
// The _mm_*_ss/_mm_*_sd intrinsics have exactly that shape ("a" is the destination), which is why the
// asymmetric-looking `_mm_move_ss(a, _mm_sqrt_ss(b))` appears for the unary single-precision ops: the
// one-argument intrinsic would carry the SOURCE's upper lanes through. On top of that, interp_xmm_put
// leaves bits 128 and above alone (the legacy-SSE rule stated at the top of this section).
// ---------------------------------------------------------------------------------------------------

#if defined(HL_HOST_CPU_X86_64)

// The mandatory prefix selects the element type and whether the op is scalar. F2/F3 outrank 0x66 exactly
// as in interp_sse_prefix, so asking these two questions of `prefix` is the whole decode.
static int interp_fp_is_double(int prefix) {
    return prefix == SSE_66 || prefix == SSE_F2; // PD, SD
}

static int interp_fp_is_scalar(int prefix) {
    return prefix == SSE_F3 || prefix == SSE_F2; // SS, SD
}

// Bytes an SSE FP instruction reads from a MEMORY r/m operand. It matters twice over: a scalar op next to
// the end of a mapping must not fault on the 12 bytes it never reads, and reading too FEW bytes silently
// substitutes zeros for real source lanes.
//
// "F2/F3 means scalar" is the rule for the arithmetic block and it is NOT universal, which is the trap in
// this space. Several opcodes carry a mandatory F2/F3 on a fully PACKED operation -- CVTPD2DQ is F2 0F E6,
// CVTTPS2DQ is F3 0F 5B, CVTDQ2PD is F3 0F E6, and the whole SSE3 horizontal group spells PS with F2 (F2 0F
// 7C is HADDPS) -- and two more take an m64 while their packed sibling on the same opcode byte takes an
// m128. So the exceptions are listed by (opcode, prefix) pair before the general rule is applied, rather
// than being left to a predicate that is right for 0x58..0x5F and wrong here.
static unsigned interp_fp_source_bytes(uint8_t op, int prefix) {
    if (op == 0x5A && prefix == SSE_NP) return 8;  // CVTPS2PD: an m64 holding two floats
    if (op == 0xE6 && prefix == SSE_F3) return 8;  // CVTDQ2PD: an m64 holding two int32
    if (op == 0xE6 && prefix == SSE_F2) return 16; // CVTPD2DQ: PACKED despite the F2 prefix
    if (op == 0x5B) return 16;                     // CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ: all 4-lane
    if (op == 0x7C || op == 0x7D || op == 0xD0) return 16; // HADD/HSUB/ADDSUB: packed under 66 AND under F2
    if (!interp_fp_is_scalar(prefix)) return 16;
    return interp_fp_is_double(prefix) ? 8u : 4u;
}

// The vector types and the 16-byte operand images are the same bits; go between them with memcpy, for the
// same reason every lane accessor above does -- it is defined for any alignment and needs no claim about
// how a uint8_t[16] happens to be laid out.
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

// CMPPS/CMPSS predicate (imm8[2:0]). The eight legacy predicates are not symmetric in their NaN handling:
// EQ/NEQ/UNORD/ORD are QUIET (a QNaN operand raises nothing) while LT/LE/NLT/NLE are SIGNALLING (a QNaN
// operand raises #IE). Using the eight matching intrinsics keeps that difference, and the "N" forms'
// unordered-is-true result, in the hardware instead of in a truth table here.
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

// COMISS/COMISD (0F 2F) and UCOMISS/UCOMISD (0F 2E) are the only SSE instructions that write EFLAGS, and
// the ONLY difference between the two is which NaN signals: UCOMIS* raises #IE for a signalling NaN,
// COMIS* for any NaN including a quiet one. Executing the real instruction is what makes that distinction
// -- and therefore the resulting MXCSR.IE the guest can read back -- correct without classifying NaNs here.
//
// The result is ZF/PF/CF only (greater = 000, less = 001, equal = 100, unordered = 111 as ZF,PF,CF), with
// OF, SF and AF architecturally ZEROED, so this writes the whole flag substrate rather than merging.
//
// setcc rather than PUSHFQ deliberately: `pushfq` writes at [rsp-8], which under the SysV ABI is inside
// this function's own 128-byte red zone, and clobbering the red zone from inline asm is the kind of defect
// that only shows up at some optimisation levels.
static void interp_fp_comis_flags(struct cpu *cpu, unsigned char zf, unsigned char pf, unsigned char cf) {
    interp_flags_nzcv(cpu, 0 /*SF*/, zf, cf, 0 /*OF*/);
    // cpu->pf is a byte whose EVEN parity is x86 PF, so PF=1 (unordered) is the byte 0 and PF=0 is 1.
    cpu->pf = pf ? 0u : 1u;
    cpu->af = 0; // x86 leaves AF *defined* as 0 here, not undefined
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

    // Several opcodes in this space have an MMX sibling selected by the absent-or-0x66 prefix
    // (CVTPI2PS/CVTPS2PI/CVTTPS2PI and their PD forms at 0x2A/0x2C/0x2D). This model has no mm[] register
    // file at all, so those are reported rather than silently aliased onto xmm -- the same rule, and the
    // same reason, as the integer-SIMD MMX check in interp_step_sse.
    if ((op == 0x2A || op == 0x2C || op == 0x2D) && !scalar)
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX CVTPI2P*/CVTP*2PI (no mm[] register file)");
    // RSQRT and RCP are single-precision only; there is no RSQRTPD/RCPSD encoding to reach.
    if ((op == 0x52 || op == 0x53) && dbl) return interp_undefined(cpu, insn, pc, "reserved (no RSQRTPD/RCPPD)");

    switch (op) {
    // ---- CVTSI2SS / CVTSI2SD (F3/F2 0F 2A): INTEGER r/m32-or-r/m64 -> the low element ---------------
    // The r/m operand is an integer (a GPR or m32/m64), so it goes through the ordinary integer r/m path
    // and REX.W selects the 64-bit source. The destination merges.
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

    // ---- CVTTSS2SI/CVTTSD2SI (0F 2C, truncating) and CVTSS2SI/CVTSD2SI (0F 2D, per MXCSR.RC) --------
    // The destination is a GENERAL register (ModRM.reg names a GPR here, not an xmm) and REX.W selects the
    // 64-bit form. 0x2C ignores the rounding mode BY DEFINITION -- that is what the second T means -- while
    // 0x2D obeys it, which is the pair the fpedge fixture checks against a round-down MXCSR.
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
        interp_reg_write(cpu, insn, destination, width, (uint64_t)value); // width 4 zero-extends, as always
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- UCOMISS/UCOMISD (0F 2E) and COMISS/COMISD (0F 2F): the EFLAGS writers ----------------------
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

    // ---- SQRT (0F 51), RSQRT (0F 52), RCP (0F 53) --------------------------------------------------
    // Unary: the result comes from the SOURCE, but a scalar form's upper lanes come from the DESTINATION.
    // _mm_sqrt_sd already has that two-operand shape; the single-precision intrinsics do not, hence the
    // explicit _mm_move_ss of the computed low lane into the destination.
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

    // ---- ADD (58) MUL (59) SUB (5C) MIN (5D) DIV (5E) MAX (5F) --------------------------------------
    // Listed one opcode at a time rather than as 0x58..0x5F, because that range is NOT homogeneous: 0x5A
    // and 0x5B are conversions with entirely different operand sizes and sit right in the middle of it.
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

    // ---- 0F 5A: CVTPS2PD (np) / CVTPD2PS (66) / CVTSS2SD (F3) / CVTSD2SS (F2) ----------------------
    // Four different shapes on one opcode. The two PACKED forms write the whole destination (CVTPD2PS
    // zeroes the upper 64 bits, which is why it is a plain store rather than a merge); the two SCALAR
    // forms merge into the destination.
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

    // ---- 0F 5B: CVTDQ2PS (np) / CVTPS2DQ (66) / CVTTPS2DQ (F3) ------------------------------------
    // All three are 4-lane, 16-byte in and out, and write the whole destination. 0x66 rounds per MXCSR.RC;
    // F3 truncates.
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

    // ---- 0F E6: CVTTPD2DQ (66) / CVTDQ2PD (F3) / CVTPD2DQ (F2) ------------------------------------
    // CVTDQ2PD widens two int32 from an m64 or the source's low half; the two PD->DQ forms narrow to two
    // int32 in the low half and ZERO the upper half.
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

    // ---- SSE3 horizontal add/subtract (0F 7C / 0F 7D) and ADDSUBPS/PD (0F D0) ---------------------
    // Built out of SSE2 shuffles plus one ADD or SUB rather than out of _mm_hadd_ps, deliberately: the SSE3
    // intrinsics need -msse3, and the engine ships one binary per host OS/CPU pair rather than per
    // micro-architecture, so an unconditional HADDPS would SIGILL on a pre-Prescott x86-64. avx.c refuses
    // -mf16c for exactly this reason and says so. The composition is exact, not an approximation: each
    // result lane of HADDPS is a single ADDPS-shaped addition of two source lanes, and gathering the
    // even-indexed lanes into src1 and the odd ones into src2 preserves the operand ORDER, which is what
    // decides NaN selection. ADDSUBPS likewise selects whole lanes bitwise from an ADDPS and a SUBPS, so
    // every NaN payload and signed zero is the one the single instruction would have produced.
    //
    // Note the prefix mapping: 0x66 is the PD form and 0xF2 is the PS form -- F2 does NOT mean scalar here.
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
                __m128d even = _mm_unpacklo_pd(a, b); // {a0, b0}
                __m128d odd = _mm_unpackhi_pd(a, b);  // {a1, b1}
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

    // ---- CMPPS / CMPPD / CMPSS / CMPSD (0F C2 imm8) -----------------------------------------------
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

// A host that is neither AArch64 (translate.c's NEON lowering) nor x86-64 (the native execution above) has
// no floating-point unit this frontend knows how to make authoritative, and the missing piece is
// specifically MXCSR: rounding control, DAZ/FZ and the six sticky exception flags all have to come from
// somewhere, and on such a host that somewhere would be a software model plus a cpu->mxcsr field -- i.e. a
// change to the checkpoint format, not to this file. Say so instead of computing plausible numbers.
static int interp_step_sse_fp(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    (void)next;
    return interp_undefined(cpu, insn, pc,
                            "SSE floating point on a non-x86-64 host (needs a software MXCSR: rounding, "
                            "DAZ/FZ and the sticky exception flags)");
}

#endif

// ---------------------------------------------------------------------------------------------------
// x87 -- the D8..DF ESC space.
//
// THE STATE MODEL IS NOT A CHOICE MADE HERE. struct cpu already carries the register stack as
// `double st[8]` with the live top in `fptop`, the condition codes in `fpsw` and the control word in
// `fpcw`, and that is a THREE-WAY ABI (see cpu.h): the AArch64 emitters bake those offsets, the
// checkpoint image records sizeof(struct cpu), and signal.c projects fpcw into the guest's xsave area.
// x87state.c's fxsave/fxrstor and x87math.c's transcendentals read exactly this layout. So ST(i) is a
// double here because it is a double everywhere, and the precision that costs is documented at length in
// lower/x87.c (the H11 note): 53 significant bits instead of 64, and binary64's exponent range. This
// backend reproduces the existing model rather than introducing a second one -- a checkpoint written by
// the JIT must restore here and vice versa.
//
// WHAT AN x86-64 HOST GETS FOR FREE, and it is most of what makes x87 fiddly elsewhere: the arithmetic
// below is ordinary C double arithmetic, which the host performs with SSE2 scalar instructions, so the
// generated QNaN indefinite already has x86's SET sign bit. The AArch64 backend needs
// hl_x86_x87_dnan_pre/post around every op to stamp that sign on; there is deliberately no analogue here.
// Likewise the FCOM/FUCOM distinction (which NaN raises #IA) is obtained by executing the host's
// COMISD/UCOMISD, whose #IE behaviour differs in exactly the same way.
//
// THE ONE APPROXIMATION, stated because it is inherited rather than introduced: x87 has its own rounding
// domain (FCW bits 11:10) separate from SSE's MXCSR.RC, and the ordinary arithmetic ops below round per the
// live host MXCSR, i.e. per the SSE mode. That is precisely what the AArch64 backend does too (both x87 and
// SSE share ARM FPCR.RMode there), and like it, the two places where the x87 control word is genuinely
// load-bearing for real code -- FRNDINT and the FIST/FISTP integer stores, which glibc's floorl/ceill/
// truncl and llrint drive through FLDCW -- DO honour FCW.RC explicitly.
//
// The ESC space is a decode trap of the same family as PUNPCKLQDQ at 0x6C: the /digit meaning depends on
// the opcode byte AND on mod, the m32/m64/m16/m32-integer operand type is carried by the opcode rather
// than by a prefix, and the register forms of DC/DE SWAP the reverse-subtract and reverse-divide digits
// relative to D8. Every case below is therefore explicit, and the swap is called out where it happens.
// ---------------------------------------------------------------------------------------------------

// ST(i), relative to the live top. The stack grows DOWNWARD: a push predecrements fptop.
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

// The four condition-code bits, at their FSW positions: C0=8, C1=9, C2=10, C3=14. cpu->fpsw holds ONLY
// these (mask 0x4700) -- the exception flags live in the host FP status word and are projected at read
// time, exactly as x87state.c's fxsave does -- so writing them is a masked merge.
static void interp_x87_condition(struct cpu *cpu, unsigned c0, unsigned c1, unsigned c2, unsigned c3) {
    uint64_t status = cpu->fpsw & ~UINT64_C(0x4700);
    if (c0) status |= UINT64_C(1) << 8;
    if (c1) status |= UINT64_C(1) << 9;
    if (c2) status |= UINT64_C(1) << 10;
    if (c3) status |= UINT64_C(1) << 14;
    cpu->fpsw = status;
}

// C1 alone (FSW bit 9). Almost every x87 instruction WRITES C1 -- as the "result was rounded up"
// indicator for the arithmetic and the stores, as the sign for FXAM, as quotient bit 0 for FPREM/FPREM1 --
// and leaving a stale one behind is directly observable through FNSTSW/FNSTENV/FXSAVE, which is how the
// divergence this exists to fix was found (glibc's fmod runs an FPREM loop, so a C1 left set by it showed
// up in an unrelated FNSTENV hundreds of instructions later). So the ops that architecturally define C1
// write it, and the ones that leave the rest of the condition codes alone write ONLY it.
//
// The residual gap, named because it is real: for the ARITHMETIC ops "rounded up" would need the exact
// (unrounded) result to compare against, which double arithmetic cannot produce, so they write 0. That is
// right whenever the operation was exact and wrong only for an inexact result that rounded away from
// zero -- strictly better than a stale bit, and the AArch64 backend does not model C1 at all.
static void interp_x87_c1(struct cpu *cpu, unsigned value) {
    if (value)
        cpu->fpsw |= UINT64_C(1) << 9;
    else
        cpu->fpsw &= ~(UINT64_C(1) << 9);
}

// "The result was rounded up", for the ops where both the rounded result and the original value are in
// hand: the narrowing stores and the integral rounds. A NaN makes the comparison false, i.e. C1 = 0.
static unsigned interp_x87_rounded_up(double result, double original) {
    return (unsigned)(result > original);
}

// Round-half-to-EVEN, spelled out rather than delegated to rint/nearbyint because those follow the live
// HOST rounding mode and this must not: it is x87's RC=00 (the default) and it is also FPREM1's quotient
// rounding, which is round-to-nearest-even whatever RC says.
static double interp_round_half_even(double value) {
    double truncated;
    double fraction;
    double magnitude;
    if (!isfinite(value)) return value;
    truncated = trunc(value);
    fraction = value - truncated; // exact: |value| >= 2^52 is already integral, so fraction is 0
    magnitude = fabs(fraction);
    if (magnitude > 0.5 || (magnitude == 0.5 && fmod(truncated, 2.0) != 0.0)) truncated += fraction > 0.0 ? 1.0 : -1.0;
    return truncated;
}

// OR bits into the sticky exception flags the guest reads back through FNSTSW/FXSAVE. Only the paths that
// raise an exception the HOST FP unit cannot raise for us need this -- today just the FIST/FISTP
// out-of-range #IA, whose result is produced by a range test in C rather than by a host conversion.
static void interp_fp_raise(unsigned flags) {
#if defined(HL_HOST_CPU_X86_64)
    _mm_setcsr(_mm_getcsr() | (flags & 0x3fu));
#else
    (void)flags; // no host FP status word this backend has agreed to own -- see interp_step_sse_fp
#endif
}

// FNCLEX / FNINIT: drop the sticky exception flags (and with them the projected ES/B) while leaving the
// condition codes, TOP and the control word alone. The counterpart of hl_x86_x87_clear_exceptions, and note
// what it implies: because the x87 FSW exception bits and MXCSR's are the SAME host bits in this model, a
// guest FNCLEX also clears its SSE sticky flags. That is a consequence of the shared substrate the JIT
// already has (both project from the single host FPSR there) rather than something introduced here.
static void interp_fp_clear_exceptions(void) {
#if defined(HL_HOST_CPU_X86_64)
    _mm_setcsr(_mm_getcsr() & ~0x3fu);
#endif
}

// The live x87 status word: the condition codes, TOP in bits 13:11, and the sticky exception flags with
// ES(7)/B(15) set when any RAISED exception is UNMASKED per FCW. Byte-for-byte the word x87state.c's
// hl_x86_fxsave builds, deliberately: FNSTSW and FXSAVE must not disagree about the same register.
static uint16_t interp_x87_status_word(const struct cpu *cpu) {
    uint16_t status = (uint16_t)((cpu->fpsw & 0x4700) | ((cpu->fptop & 7) << 11));
#if defined(HL_HOST_CPU_X86_64)
    uint16_t raised = (uint16_t)(_mm_getcsr() & 0x3fu); // FSW exception bits sit at MXCSR's positions
    status |= raised;
    if (raised & (uint16_t)(~cpu->fpcw & 0x3f)) status |= (uint16_t)0x8080; // ES(7) + B(15)
#endif
    return status;
}

// FCOM/FUCOM/FCOMI/FUCOMI all reduce to one three-way answer plus "unordered", and on an x86-64 host the
// host instruction produces exactly the guest's: COMISD raises #IE for ANY NaN, UCOMISD only for a
// signalling one -- the very distinction between the FCOM and FUCOM families -- and the resulting IE is
// what the guest reads back through FNSTSW. Returned in EFLAGS terms (ZF/PF/CF) because both consumers
// want it that way: FCOMI writes those bits directly, and the FSW condition codes are (C0,C2,C3) =
// (CF,PF,ZF) -- greater 000, less 100, equal 001, unordered 111 -- which is not a coincidence but the
// reason the SSE compare instructions chose that flag encoding.
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
    // No host instruction to borrow: the three-way answer is still exact, only the signalling-vs-quiet
    // #IE distinction is lost (and on such a host no FP exception flag is modelled at all -- see
    // interp_step_sse_fp).
    (void)signalling;
    int unordered = isunordered(left, right);
    *pf = (unsigned char)(unordered ? 1 : 0);
    *zf = (unsigned char)((unordered || left == right) ? 1 : 0);
    *cf = (unsigned char)((unordered || left < right) ? 1 : 0);
#endif
}

// FCOM/FUCOM: the comparison result goes to the FSW condition codes. C1 is cleared (it reports a stack
// fault or the rounding direction, neither of which this model produces).
static void interp_x87_compare_fpsw(struct cpu *cpu, double left, double right, int signalling) {
    unsigned char zf, pf, cf;
    interp_x87_compare_flags(left, right, signalling, &zf, &pf, &cf);
    interp_x87_condition(cpu, cf, 0, pf, zf);
}

// FCOMI/FUCOMI/FCOMIP/FUCOMIP: the comparison result goes to the integer EFLAGS instead -- ZF/PF/CF, with
// OF/SF/AF zeroed -- which is the same shape as UCOMISS/COMISS and shares its helper.
static void interp_x87_compare_eflags(struct cpu *cpu, double left, double right, int signalling) {
    unsigned char zf, pf, cf;
    interp_x87_compare_flags(left, right, signalling, &zf, &pf, &cf);
    interp_flags_nzcv(cpu, 0, zf, cf, 0);
    cpu->pf = pf ? 0u : 1u; // a byte whose EVEN parity is x86 PF
    cpu->af = 0;
    interp_x87_c1(cpu, 0); // the FSW condition codes are untouched by these, but C1 is defined as 0
}

// Round to an integral value under the x87 control word's RC field (FCW bits 11:10), independently of the
// live host rounding mode -- x87 rounding and SSE rounding are separate domains and a guest that set one
// did not set the other. floor/ceil/trunc are exact and mode-independent; round-half-to-EVEN (RC=00, the
// x87 default) is spelled out rather than delegated to rint/nearbyint, because those read the host mode.
static double interp_x87_round_integral(const struct cpu *cpu, double value) {
    unsigned rc = (unsigned)((cpu->fpcw >> 10) & 3u);
    if (!isfinite(value)) return value;
    switch (rc) {
    case 1: return floor(value);                   // toward -inf
    case 2: return ceil(value);                    // toward +inf
    case 3: return trunc(value);                   // toward zero
    default: return interp_round_half_even(value); // RC=00, the x87 default
    }
}

// FIST/FISTP/FISTTP: store ST0 as a signed integer of `bytes` bytes. A value out of the destination's
// range -- including a NaN or an infinity, for which every comparison below is false -- stores the INTEGER
// INDEFINITE (the most negative value) and raises #IA, which is the one x87 exception this file has to
// raise itself: the result comes from a range test in C, not from a host conversion instruction.
static uint64_t interp_x87_to_integer(double value, int bytes, int *invalid) {
    double low = bytes == 2 ? -32768.0 : bytes == 4 ? -2147483648.0 : -9223372036854775808.0;
    double high = bytes == 2 ? 32767.0 : bytes == 4 ? 2147483647.0 : 9223372036854775808.0;
    // The 8-byte upper bound is EXCLUSIVE because 2^63 is representable as a double but not as an int64;
    // the 2- and 4-byte bounds are the exact representable extremes, so they are inclusive.
    int in_range = bytes == 8 ? (value >= low && value < high) : (value >= low && value <= high);
    if (!in_range) {
        *invalid = 1;
        return bytes == 2 ? UINT64_C(0x8000) : bytes == 4 ? UINT64_C(0x80000000) : (UINT64_C(1) << 63);
    }
    *invalid = 0;
    return (uint64_t)(int64_t)value;
}

// The six arithmetic digits, applied as `destination OP source`. FSUBR/FDIVR are expressed as the reversed
// operand order rather than as separate cases so the DC/DE digit swap below is a single decision.
static double interp_x87_arith(int kind, double destination, double source) {
    switch (kind) {
    case 0: return destination + source;  // FADD
    case 1: return destination * source;  // FMUL
    case 4: return destination - source;  // FSUB
    case 5: return source - destination;  // FSUBR
    case 6: return destination / source;  // FDIV
    default: return source / destination; // FDIVR (kind 7)
    }
}

// FXAM's class encoding, from the IEEE-754 fields of the double. {C3,C2,C0}: zero=100, NaN=001, Inf=011,
// denormal=110, normal=010; C1 is the sign. This model keeps no tag word, so "empty" (101) cannot be
// reported and every slot reads as its value -- and because the carrier is a double, the 80-bit
// unsupported and pseudo-denormal encodings cannot arise. Same derivation as hl_x86_x87_classify.
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

// FXTRACT: ST0 <- the unbiased exponent, then the significand (in [1,2), ST0's sign) is pushed, so on exit
// ST0 = significand and ST1 = exponent. Field surgery on the double, mirroring hl_x86_x87_extract: for a
// zero or a non-finite operand a real 80-bit FPU reports -inf / +inf / the operand's NaN, which this
// derivation does not, and that divergence is the JIT's too -- kept identical rather than improved on one
// backend only.
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

// FSCALE: ST0 *= 2^trunc(ST1). Deliberately the same construction as hl_x86_x87_scale -- identity for a
// non-finite or zero ST0 (scalbn's rule, which the exponent clamp below cannot express), otherwise a 2^n
// built straight into the double exponent field with the BIASED exponent clamped to [0,2047] so a huge
// negative ST1 yields +0.0 and a huge positive one +Inf.
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

// FPREM (quotient truncated) / FPREM1 (quotient round-half-even): ST0 = ST0 - ST1*Q. fmod/remainder are
// the IEEE-exact spellings of exactly those two, so they are used instead of reproducing the JIT's
// divide-round-multiply-subtract sequence, which loses bits for a large ratio. The reduction is completed
// in one step, so C2 <- 0 ("reduction complete"), which is what makes libc's `do { fprem } while (C2)`
// loop terminate.
//
// BOTH forms publish |Q|'s low three bits as C1/C3/C0 (bit 0/1/2). lower/x87.c and qemu's helper_fprem
// leave them clear for FPREM1; real hardware does not, and a differential run against native catches it
// immediately (FPREM1 of 17 by 5 gives Q=3, so C3 and C1 are set). The two differ only in how Q is
// rounded, which is the same distinction that picks fmod over remainder.
static void interp_x87_remainder(struct cpu *cpu, int ieee) {
    double st0 = interp_x87_get(cpu, 0);
    double st1 = interp_x87_get(cpu, 1);
    double result = ieee ? remainder(st0, st1) : fmod(st0, st1);
    unsigned c0 = 0, c1 = 0, c3 = 0;
    interp_x87_set(cpu, 0, result);
    if (isfinite(st0) && isfinite(st1) && st1 != 0.0) {
        double ratio = st0 / st1;
        double quotient = ieee ? interp_round_half_even(ratio) : trunc(ratio);
        if (fabs(quotient) < 9007199254740992.0) { // 2^53: beyond it the low bits are not represented
            uint64_t magnitude = (uint64_t)fabs(quotient);
            c1 = (unsigned)(magnitude & 1u);
            c3 = (unsigned)((magnitude >> 1) & 1u);
            c0 = (unsigned)((magnitude >> 2) & 1u);
        }
    }
    interp_x87_condition(cpu, c0, c1, 0 /*C2: reduction complete*/, c3);
}

// FNSTENV m28 / FLDENV m28: the 28-byte 32-bit protected-mode environment. FCW@0, FSW@4, FTW@8, then
// FIP/FCS/FOO/FOS @12/16/20/24. No per-register tags are modelled, so FTW is written as all-empty
// (0xffff) and ignored on the way in, and the instruction/data pointers are zeroed -- the same content the
// JIT's emit path writes, for the same reason (OpenBLAS's startup, the only real caller, saves and
// restores FCW/FSW/FTW and nothing else).
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
    cpu->fpsw = status & 0x4700; // condition codes only; the exception flags live in the host FP status
    cpu->fptop = (status >> 11) & 7;
}

// The eight FLD-constant values (D9 /5 register forms E8..EF), as bit patterns rather than C literals so
// the loaded double is exactly the one a real FPU's 80-bit constant narrows to. Same table as translate.c.
static const uint64_t g_x87_constants[8] = {
    UINT64_C(0x3FF0000000000000), // FLD1     1.0
    UINT64_C(0x400A934F0979A371), // FLDL2T   log2(10)
    UINT64_C(0x3FF71547652B82FE), // FLDL2E   log2(e)
    UINT64_C(0x400921FB54442D18), // FLDPI    pi
    UINT64_C(0x3FD34413509F79FF), // FLDLG2   log10(2)
    UINT64_C(0x3FE62E42FEFA39EF), // FLDLN2   ln(2)
    UINT64_C(0x0000000000000000), // FLDZ     +0.0
    UINT64_C(0x0000000000000000), // (no EF encoding)
};

// A float/double loaded from guest memory. Through interp_load so the access carries the rebias and the
// fault marker; memcpy for the reinterpretation, so no alignment or aliasing claim is made.
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

// Returns the C1 "rounded up" indicator: FST m32 is the one x87 store that can actually round, since the
// carrier is already a double.
static unsigned interp_x87_store_f32(uint64_t address, double value) {
    float narrowed = (float)value; // rounds per the live host mode -- see THE ONE APPROXIMATION above
    uint32_t bits;
    memcpy(&bits, &narrowed, sizeof bits);
    interp_store(address, 4, bits);
    return interp_x87_rounded_up((double)narrowed, value);
}

static void interp_x87_store_f64(uint64_t address, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    interp_store(address, 8, bits); // always exact: the ST carrier IS a double
}

// The memory forms of the D8/DC (m32/m64 float) and DA/DE (m32/m16 SIGNED integer) arithmetic group. All
// four share one /digit encoding, and the destination is always ST0.
static int interp_x87_memory_arith(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next, double source) {
    int kind = insn->reg & 7;
    double st0 = interp_x87_get(cpu, 0);
    if (kind == 2 || kind == 3) { // FCOM / FCOMP: signalling on any NaN
        interp_x87_compare_fpsw(cpu, st0, source, 1);
        if (kind == 3) interp_x87_pop(cpu);
        cpu->rip = next;
        return STEP_NEXT;
    }
    (void)pc;
    interp_x87_set(cpu, 0, interp_x87_arith(kind, st0, source));
    interp_x87_c1(cpu, 0); // arithmetic defines C1 as "rounded up", which this model cannot compute
    cpu->rip = next;
    return STEP_NEXT;
}

static int interp_step_x87(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;
    // The /digit and the ST(i) selector. NEITHER is extended by REX: x87 has eight stack slots and the
    // REX.R/REX.B bits the decoder folded into insn->reg / insn->rm_reg name xmm and GPR numbers that do
    // not exist here, so the raw 3-bit fields are the operand.
    int reg = insn->reg & 7;
    int rm = insn->rm & 7;

    if (insn->is_mem) {
        uint64_t address = interp_ea(cpu, insn, next);
        switch (op) {
        // ---- the arithmetic group: m32 float / m64 float / m32 int / m16 int ----------------------
        case 0xD8: return interp_x87_memory_arith(cpu, insn, pc, next, interp_x87_load_f32(address));
        case 0xDC: return interp_x87_memory_arith(cpu, insn, pc, next, interp_x87_load_f64(address));
        case 0xDA: return interp_x87_memory_arith(cpu, insn, pc, next, (double)(int32_t)interp_load(address, 4));
        case 0xDE: return interp_x87_memory_arith(cpu, insn, pc, next, (double)(int16_t)interp_load(address, 2));

        // ---- D9: m32 loads/stores and the control-word group -------------------------------------
        case 0xD9:
            switch (reg) {
            case 0: // FLD m32 -- exact widening, so C1 (which FLD also writes) is 0
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

        // ---- DB: m32 integer loads/stores, and the m80 pair ------------------------------------
        case 0xDB:
            switch (reg) {
            case 0: // FILD m32 -- exact for every int32, so C1 = 0
                interp_x87_push(cpu, (double)(int32_t)interp_load(address, 4));
                interp_x87_c1(cpu, 0);
                break;
            case 1:   // FISTTP m32
            case 2:   // FIST m32
            case 3: { // FISTP m32
                int invalid = 0;
                // FISTTP (SSE3) truncates unconditionally; FIST/FISTP round per the x87 control word.
                double value = interp_x87_get(cpu, 0);
                double rounded = reg == 1 ? trunc(value) : interp_x87_round_integral(cpu, value);
                uint64_t stored = interp_x87_to_integer(rounded, 4, &invalid);
                if (invalid) interp_fp_raise(1u /*IE*/);
                interp_store(address, 4, stored);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                if (reg != 2) interp_x87_pop(cpu);
                break;
            }
            case 5: { // FLD m80 -- the 80-bit converter, shared with the JIT (x87state.c)
                uint8_t image[10];
                double value;
                interp_load_bytes(address, image, sizeof image);
                value = hl_x86_ext80_load(image);
                interp_x87_push(cpu, value);
                interp_x87_c1(cpu, 0);
                break;
            }
            case 7: { // FSTP m80 -- widening, so exact, so C1 = 0
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

        // ---- DD: m64 loads/stores, and FNSTSW m16 ---------------------------------------------
        case 0xDD:
            switch (reg) {
            case 0: // FLD m64 -- the carrier IS a double, so exact
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
                // FRSTOR m108 (/4) and FNSAVE m108 (/6) save and restore the whole 28-byte environment
                // PLUS the eight 80-bit register slots. Nothing in the corpus reaches them (glibc uses
                // FXSAVE, which IS implemented, via 0F AE) and getting the 108-byte layout wrong silently
                // would be worse than reporting it.
                return interp_undefined(cpu, insn, pc, "x87 FNSAVE/FRSTOR m108 (DD /4,/6)");
            }
            cpu->rip = next;
            return STEP_NEXT;

        // ---- DF: m16/m64 integer loads/stores ------------------------------------------------
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
            // FILD m64. The only x87 LOAD that can round in this model: a magnitude beyond 2^53 does not
            // fit the double carrier, where a real 80-bit register would hold every int64 exactly. C1 is
            // written 0 rather than reporting the direction of that rounding -- the same gap, for the same
            // reason, as the arithmetic ops (see interp_x87_c1).
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
                // FBLD m80 (/4) and FBSTP m80 (/6) are packed BCD, an encoding no compiler emits.
                return interp_undefined(cpu, insn, pc, "x87 packed-BCD FBLD/FBSTP (DF /4,/6)");
            }
            cpu->rip = next;
            return STEP_NEXT;

        default: return interp_undefined(cpu, insn, pc, "x87 memory form");
        }
    }

    // ---- register (mod == 3) forms -------------------------------------------------------------
    switch (op) {
    // ---- D8 / DC / DE: arithmetic and compares against ST(i) --------------------------------
    case 0xD8:
    case 0xDC:
    case 0xDE:
        if (reg == 2 || reg == 3) { // FCOM / FCOMP, and DE D9 = FCOMPP (compare, then pop TWICE)
            interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), 1);
            if (op == 0xDE && rm == 1) interp_x87_pop(cpu);
            if (reg == 3) interp_x87_pop(cpu);
            cpu->rip = next;
            return STEP_NEXT;
        }
        {
            // D8 writes ST0 and reads ST(i); DC and DE write ST(i) and read ST0. And the REVERSE digits
            // are SWAPPED between the two directions: D8 /4 is FSUB (ST0 = ST0-ST(i)) while DC /4 is
            // FSUBR (ST(i) = ST0-ST(i)), and likewise /6 FDIV against /6 FDIVR. Encoding the swap as a
            // digit flip on the DC/DE side keeps one arithmetic function for all six operations; the
            // alternative -- separate cases per opcode -- is where an emulator silently computes b-a.
            int kind = reg;
            int target = op == 0xD8 ? 0 : rm;
            double destination = interp_x87_get(cpu, target);
            double source = op == 0xD8 ? interp_x87_get(cpu, rm) : interp_x87_get(cpu, 0);
            if (op != 0xD8 && (kind == 4 || kind == 5 || kind == 6 || kind == 7)) kind ^= 1;
            interp_x87_set(cpu, target, interp_x87_arith(kind, destination, source));
            interp_x87_c1(cpu, 0);               // "rounded up" -- not computable here, see interp_x87_c1
            if (op == 0xDE) interp_x87_pop(cpu); // the DE forms pop after writing
        }
        cpu->rip = next;
        return STEP_NEXT;

    // ---- D9: the no-operand group ---------------------------------------------------------
    case 0xD9:
        // Every arm below writes C1 (as 0), because every one of these instructions architecturally does;
        // FXAM, FPREM and FPREM1 then overwrite it with the value they define.
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
                interp_x87_set(cpu, 0, -interp_x87_get(cpu, 0)); // FCHS: a sign flip, NaNs included
            else if (rm == 1)
                interp_x87_set(cpu, 0, fabs(interp_x87_get(cpu, 0))); // FABS
            else if (rm == 4)
                interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), 0.0, 1); // FTST
            else if (rm == 5)
                interp_x87_classify(cpu); // FXAM
            else
                return interp_undefined(cpu, insn, pc, "x87 D9 /4 (reserved encoding)");
            break;
        case 5: { // FLD1 / FLDL2T / FLDL2E / FLDPI / FLDLG2 / FLDLN2 / FLDZ
            double value;
            if (rm == 7) return interp_undefined(cpu, insn, pc, "x87 D9 EF (no such constant)");
            memcpy(&value, &g_x87_constants[rm], sizeof value);
            interp_x87_push(cpu, value);
            break;
        }
        case 6:
            // F0..F3 are transcendentals with no host FP instruction: they exit to x87math.c, exactly as
            // the JIT does, rather than growing a second libm caller here. F4..F7 are field surgery and
            // stack bookkeeping and stay inline.
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
                cpu->fptop = (cpu->fptop - 1) & 7; // FDECSTP: rotate the top, touch no slot
            else
                cpu->fptop = (cpu->fptop + 1) & 7; // FINCSTP
            break;
        case 7:
            switch (rm) {
            case 0: interp_x87_remainder(cpu, 0); break;                         // FPREM
            case 2: interp_x87_set(cpu, 0, sqrt(interp_x87_get(cpu, 0))); break; // FSQRT
            case 4: { // FRNDINT: rounds per the x87 control word, and reports the direction in C1
                double value = interp_x87_get(cpu, 0);
                double rounded = interp_x87_round_integral(cpu, value);
                interp_x87_set(cpu, 0, rounded);
                interp_x87_c1(cpu, interp_x87_rounded_up(rounded, value));
                break;
            }
            case 5: // FSCALE
                interp_x87_set(cpu, 0, interp_x87_scale(interp_x87_get(cpu, 0), interp_x87_get(cpu, 1)));
                break;
            default: { // F9 FYL2XP1, FB FSINCOS, FE FSIN, FF FCOS -- host libm, via x87math.c
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

    // ---- DA: FCMOVcc, and DA E9 = FUCOMPP -----------------------------------------------
    case 0xDA:
        if (reg <= 3) {
            // The condition comes from the INTEGER EFLAGS, not from the FSW: FCMOVB/FCMOVE/FCMOVBE/FCMOVU
            // are jb/je/jbe/jp. DB /0../3 below are the same four negated.
            static const int condition[4] = {2 /*B*/, 4 /*E*/, 6 /*BE*/, 10 /*U (P)*/};
            if (interp_cond(cpu, condition[reg])) interp_x87_set(cpu, 0, interp_x87_get(cpu, rm));
        } else if (reg == 5 && rm == 1) { // FUCOMPP: compare ST0 with ST1, pop twice
            interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, 1), 0);
            interp_x87_pop(cpu);
            interp_x87_pop(cpu);
        } else {
            return interp_undefined(cpu, insn, pc, "x87 DA register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    // ---- DB: FCMOVNcc, FNCLEX/FNINIT, FUCOMI/FCOMI --------------------------------------
    case 0xDB:
        if (reg <= 3) {
            static const int condition[4] = {3 /*NB*/, 5 /*NE*/, 7 /*NBE*/, 11 /*NU (NP)*/};
            if (interp_cond(cpu, condition[reg])) interp_x87_set(cpu, 0, interp_x87_get(cpu, rm));
        } else if (reg == 4 && rm == 2) { // FNCLEX: clear the sticky exception flags, keep TOP and the CCs
            interp_fp_clear_exceptions();
        } else if (reg == 4 && rm == 3) { // FNINIT
            cpu->fptop = 0;
            cpu->fpsw = 0;
            cpu->fpcw = 0x037f; // round-to-nearest, 64-bit precision, all exceptions masked
            interp_fp_clear_exceptions();
        } else if (reg == 4) { // FNENI / FNDISI / FNSETPM: 8087/80287 compatibility no-ops
            /* nothing */
        } else if (reg == 5 || reg == 6) { // FUCOMI (/5, quiet) / FCOMI (/6, signalling) -> EFLAGS
            interp_x87_compare_eflags(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), reg == 6);
        } else {
            return interp_undefined(cpu, insn, pc, "x87 DB register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    // ---- DD: FFREE, FST/FSTP ST(i), FUCOM/FUCOMP ---------------------------------------
    case 0xDD:
        switch (reg) {
        case 0: break; // FFREE: no tag word is modelled
        case 2:        // FST ST(i) -- a register-to-register move, so exact, so C1 = 0
        case 3:        // FSTP ST(i)
            interp_x87_set(cpu, rm, interp_x87_get(cpu, 0));
            interp_x87_c1(cpu, 0);
            if (reg == 3) interp_x87_pop(cpu);
            break;
        case 4: // FUCOM ST(i): quiet, so only a signalling NaN raises #IA
        case 5: // FUCOMP ST(i)
            interp_x87_compare_fpsw(cpu, interp_x87_get(cpu, 0), interp_x87_get(cpu, rm), 0);
            if (reg == 5) interp_x87_pop(cpu);
            break;
        default: return interp_undefined(cpu, insn, pc, "x87 DD register form");
        }
        cpu->rip = next;
        return STEP_NEXT;

    // ---- DF: FNSTSW AX, FUCOMIP/FCOMIP ------------------------------------------------
    case 0xDF:
        if (reg == 4 && rm == 0) { // FNSTSW AX -- a 16-bit write, so bits 63:16 of RAX are PRESERVED
            interp_reg_write(cpu, insn, RAX, 2, interp_x87_status_word(cpu));
        } else if (reg == 5 || reg == 6) { // FUCOMIP (/5) / FCOMIP (/6): compare to EFLAGS, then pop
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
    int destination = insn->reg; // ModRM.reg names the xmm destination in almost every form here

    // Floating-point arithmetic, conversion and comparison is a separate decision (native host FP under the
    // guest's own MXCSR) and takes its own function; route it before anything else claims the opcode.
    if (interp_sse_is_float_arithmetic(op)) return interp_step_sse_fp(cpu, insn, pc, next);

    // The integer SIMD opcodes exist in both an MMX (no mandatory prefix, mm0..7) and an SSE2 (0x66, xmm)
    // encoding. This model has no MMX register file at all -- struct cpu carries no mm[] and the checkpoint
    // format has no room for one -- so the MMX forms are reported rather than silently aliased onto xmm,
    // which would corrupt whichever register the guest was really using.
    int integer_simd = (op >= 0x60 && op <= 0x6D) || op == 0x6E || op == 0x6F || (op >= 0x71 && op <= 0x76) ||
                       op == 0x7E || op == 0x7F || op == 0xD4 || op == 0xD5 || op == 0xD7 ||
                       (op >= 0xD1 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF) || (op >= 0xE0 && op <= 0xE5) ||
                       op == 0xE7 || (op >= 0xE8 && op <= 0xEF) || (op >= 0xF1 && op <= 0xFE) || op == 0x70 ||
                       op == 0xC4 || op == 0xC5;
    if (integer_simd && prefix == SSE_NP && op != 0x6F && op != 0x7E && op != 0x70)
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX form (no mm[] register file is modelled)");

    uint8_t d[16], s[16];

    switch (op) {
    // ---- MOVUPS/MOVUPD/MOVSS/MOVSD, load direction (0F 10) ---------------------------------------
    case 0x10:
        if (prefix == SSE_F3) { // MOVSS: from memory the upper 96 bits are ZEROED, from a register kept
            interp_sse_rm_get(cpu, insn, next, 4, s);
            if (insn->is_mem) {
                memset(d, 0, 16);
                memcpy(d, s, 4);
            } else {
                interp_xmm_get(cpu, destination, d);
                memcpy(d, s, 4);
            }
        } else if (prefix == SSE_F2) { // MOVSD: same rule at 64 bits
            interp_sse_rm_get(cpu, insn, next, 8, s);
            if (insn->is_mem) {
                memset(d, 0, 16);
                memcpy(d, s, 8);
            } else {
                interp_xmm_get(cpu, destination, d);
                memcpy(d, s, 8);
            }
        } else {
            interp_sse_rm_get(cpu, insn, next, 16, d); // MOVUPS/MOVUPD: unaligned is explicitly permitted
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- MOVUPS/MOVUPD/MOVSS/MOVSD, store direction (0F 11) --------------------------------------
    case 0x11: {
        unsigned bytes = prefix == SSE_F3 ? 4u : prefix == SSE_F2 ? 8u : 16u;
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, bytes, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOVLPS/MOVLPD/MOVHLPS/MOVDDUP/MOVSLDUP (0F 12) and MOVHPS/MOVHPD/MOVLHPS/MOVSHDUP (0F 16) --
    case 0x12:
    case 0x16: {
        int high = (op == 0x16);
        interp_xmm_get(cpu, destination, d);
        if (prefix == SSE_F3) {
            // SSE3 MOVSLDUP (F3 0F 12) / MOVSHDUP (F3 0F 16): duplicate the EVEN (resp. ODD) single-
            // precision lanes across the whole register from a full m128 source. Pure lane duplication, so
            // it belongs in this half rather than with the FP arithmetic. It is called out because the F3
            // form previously fell into the MOVLPS/MOVHPS arm below, which reads only 8 bytes and merges
            // them into one half -- silently wrong output rather than a reported gap, and the F3 prefix is
            // the only thing distinguishing the two encodings.
            interp_sse_rm_get(cpu, insn, next, 16, s);
            for (int i = 0; i < 4; i++)
                interp_put32(d, i, interp_lane32(s, (i & ~1) | (high ? 1 : 0)));
        } else if (prefix == SSE_F2 && op == 0x12) { // MOVDDUP: broadcast the low qword into both halves
            interp_sse_rm_get(cpu, insn, next, 8, s);
            memcpy(d + 0, s, 8);
            memcpy(d + 8, s, 8);
        } else if (insn->is_mem) { // MOVLPS/MOVLPD load low half, MOVHPS/MOVHPD load high half
            interp_sse_rm_get(cpu, insn, next, 8, s);
            memcpy(d + (high ? 8 : 0), s, 8);
        } else if (high) { // MOVLHPS: destination high half := source LOW half
            interp_xmm_get(cpu, insn->rm_reg, s);
            memcpy(d + 8, s + 0, 8);
        } else { // MOVHLPS: destination low half := source HIGH half
            interp_xmm_get(cpu, insn->rm_reg, s);
            memcpy(d + 0, s + 8, 8);
        }
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOVLPS/MOVLPD (0F 13) and MOVHPS/MOVHPD (0F 17), store direction ------------------------
    case 0x13:
    case 0x17: {
        uint8_t half[16] = {0};
        interp_xmm_get(cpu, destination, d);
        memcpy(half, d + (op == 0x17 ? 8 : 0), 8);
        interp_sse_rm_put(cpu, insn, next, 8, half);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- UNPCKLPS/PD (0F 14) and UNPCKHPS/PD (0F 15) ---------------------------------------------
    // Pure lane interleave, so these ARE the PUNPCK*DQ/QDQ operations under different names: the single-
    // precision forms interleave dwords and the double-precision forms qwords. Sharing the primitive is
    // not a shortcut, it is the architectural definition.
    case 0x14:
    case 0x15:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_punpck(d, s, prefix == SSE_66 ? 8 : 4, op == 0x15);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- MOVAPS/MOVAPD (0F 28/29) and the non-temporal stores (0F 2B) ----------------------------
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

    // ---- MOVMSKPS / MOVMSKPD (0F 50) -------------------------------------------------------------
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
        interp_reg_write(cpu, insn, destination, 4, mask); // 32-bit destination: zero-extends
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- ANDPS/ANDNPS/ORPS/XORPS and their PD forms (0F 54..57) -----------------------------------
    // Bitwise, so the single/double distinction carries no semantic difference at all -- which is exactly
    // why these are in scope while the arithmetic beside them is not.
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int i = 0; i < 16; i++)
            d[i] = op == 0x54 ? (uint8_t)(d[i] & s[i])
                   : op == 0x55 ? (uint8_t)(~d[i] & s[i]) // ANDNPS: NOT destination, then AND
                   : op == 0x56 ? (uint8_t)(d[i] | s[i])
                                : (uint8_t)(d[i] ^ s[i]);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- PUNPCK* low (0F 60/61/62/6C) and high (0F 68/69/6A/6D) ----------------------------------
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x6C:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6D: {
        int lane = (op == 0x60 || op == 0x68) ? 1 : (op == 0x61 || op == 0x69) ? 2 : (op == 0x62 || op == 0x6A) ? 4 : 8;
        // The low/high split is NOT a contiguous range: the byte/word/dword pairs sit at 0x60..0x62 (low)
        // and 0x68..0x6A (high), but the QWORD pair was added later and landed at 0x6C (LOW) / 0x6D (HIGH),
        // i.e. ABOVE the high group. A naive `op >= 0x68` therefore turns PUNPCKLQDQ into PUNPCKHQDQ --
        // which is exactly the `movq`+`punpcklqdq` pointer-duplication idiom glibc's INIT_LIST_HEAD uses,
        // so it corrupts a list head into garbage on the very first use.
        int high = (op >= 0x68 && op <= 0x6A) || op == 0x6D;
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_punpck(d, s, lane, high);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- PACKSSWB / PACKUSWB / PACKSSDW (0F 63/67/6B) --------------------------------------------
    case 0x63:
    case 0x67:
    case 0x6B:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_pack(d, s, op == 0x6B ? 4 : 2, op != 0x67);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- PCMPGTB/W/D (0F 64/65/66) ---------------------------------------------------------------
    case 0x64:
    case 0x65:
    case 0x66:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_pcmpgt(d, s, op == 0x64 ? 1 : op == 0x65 ? 2 : 4);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- MOVD / MOVQ, general register or memory INTO xmm (66 0F 6E) -----------------------------
    // The r/m operand here is an INTEGER operand (a GPR or m32/m64), not an xmm, so it goes through the
    // ordinary integer r/m path. REX.W selects the 64-bit form. The destination's upper bits are ZEROED:
    // this is a move into the register, not a merge.
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

    // ---- MOVDQA (66) / MOVDQU (F3) load (0F 6F) --------------------------------------------------
    case 0x6F:
        if (prefix == SSE_NP)
            return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX MOVQ (no mm[] register file is modelled)");
        if (prefix == SSE_66 && interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_sse_rm_get(cpu, insn, next, 16, d);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- LDDQU (F2 0F F0) ------------------------------------------------------------------------
    // Architecturally identical to MOVDQU: an unaligned 16-byte load. The only difference is a
    // micro-architectural hint (it may over-fetch and realign internally), which has no guest-visible
    // effect, so sharing the MOVDQU path is the definition rather than a shortcut. Memory source only.
    case 0xF0:
        if (prefix != SSE_F2 || !insn->is_mem) return interp_undefined(cpu, insn, pc, "reserved (0F F0)");
        interp_sse_rm_get(cpu, insn, next, 16, d);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- MOVDQA (66) / MOVDQU (F3) store (0F 7F) -------------------------------------------------
    case 0x7F:
        if (prefix == SSE_NP)
            return interp_undefined(cpu, insn, pc, "TODO(amd64-host): MMX MOVQ (no mm[] register file is modelled)");
        if (prefix == SSE_66 && interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, 16, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- MOVD/MOVQ xmm to r/m (66 0F 7E), and MOVQ xmm load (F3 0F 7E) ---------------------------
    case 0x7E:
        if (prefix == SSE_F3) { // MOVQ xmm, xmm/m64 -- a LOAD, upper 64 bits zeroed
            interp_sse_rm_get(cpu, insn, next, 8, s);
            memset(d, 0, 16);
            memcpy(d, s, 8);
            interp_xmm_put(cpu, destination, d);
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (prefix == SSE_66) { // MOVD/MOVQ r/m32/64, xmm -- an integer destination
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

    // ---- PSHUFD (66) / PSHUFHW (F3) / PSHUFLW (F2) (0F 70) ---------------------------------------
    case 0x70: {
        interp_sse_rm_get(cpu, insn, next, 16, s);
        unsigned control = (unsigned)(insn->imm & 0xff);
        memset(d, 0, 16);
        if (prefix == SSE_66) {
            for (int i = 0; i < 4; i++)
                interp_put32(d, i, interp_lane32(s, (int)((control >> (2 * i)) & 3)));
        } else if (prefix == SSE_F3) { // PSHUFHW: shuffle the HIGH four words, copy the low qword
            memcpy(d, s, 8);
            for (int i = 0; i < 4; i++)
                interp_put16(d, 4 + i, interp_lane16(s, 4 + (int)((control >> (2 * i)) & 3)));
        } else if (prefix == SSE_F2) { // PSHUFLW: shuffle the LOW four words, copy the high qword
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

    // ---- the shift-by-immediate group (0F 71/72/73) ----------------------------------------------
    // ModRM.reg is the sub-opcode, not a register, and the operand is always the xmm named by ModRM.rm.
    case 0x71:
    case 0x72:
    case 0x73: {
        int sub = insn->reg & 7;
        unsigned count = (unsigned)(insn->imm & 0xff);
        int lane = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        interp_xmm_get(cpu, insn->rm_reg, d);
        if (op == 0x73 && (sub == 3 || sub == 7)) // PSRLDQ / PSLLDQ: whole-register BYTE shift
            interp_pshift_bytes(d, count, sub == 3);
        else if (sub == 2)
            interp_pshift(d, lane, count, 1, 0); // PSRLW/D/Q
        else if (sub == 4 && op != 0x73)
            interp_pshift(d, lane, count, 1, 1); // PSRAW/D only: SSE2 has no PSRAQ, and 0F 73 /4 is an
                                                 // AVX-512 encoding. Rejecting it below is deliberate --
                                                 // computing a 64-bit arithmetic shift here would answer a
                                                 // question the guest never legally asked.
        else if (sub == 6)
            interp_pshift(d, lane, count, 0, 0); // PSLLW/D/Q
        else
            return interp_undefined(cpu, insn, pc, "unallocated SSE shift-group sub-opcode");
        interp_xmm_put(cpu, insn->rm_reg, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- PCMPEQB/W/D (0F 74/75/76) ---------------------------------------------------------------
    case 0x74:
    case 0x75:
    case 0x76:
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_pcmpeq(d, s, op == 0x74 ? 1 : op == 0x75 ? 2 : 4);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- PINSRW (66 0F C4) and PEXTRW (66 0F C5) -------------------------------------------------
    case 0xC4: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t value = interp_rm_read(cpu, insn, &operand, 2); // GPR low 16 bits, or m16
        interp_xmm_get(cpu, destination, d);
        interp_put16(d, (int)(insn->imm & 7), (uint16_t)value);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xC5:
        interp_xmm_get(cpu, insn->rm_reg, s); // the source is always an xmm register in this encoding
        interp_reg_write(cpu, insn, destination, 4, interp_lane16(s, (int)(insn->imm & 7)));
        cpu->rip = next;
        return STEP_NEXT;

    // ---- SHUFPS / SHUFPD (0F C6) -----------------------------------------------------------------
    case 0xC6: {
        unsigned control = (unsigned)(insn->imm & 0xff);
        uint8_t out[16];
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        if (prefix == SSE_66) { // SHUFPD: one bit per qword
            interp_put64(out, 0, interp_lane64(d, (int)(control & 1)));
            interp_put64(out, 1, interp_lane64(s, (int)((control >> 1) & 1)));
        } else { // SHUFPS: low two dwords from the destination, high two from the source
            interp_put32(out, 0, interp_lane32(d, (int)(control & 3)));
            interp_put32(out, 1, interp_lane32(d, (int)((control >> 2) & 3)));
            interp_put32(out, 2, interp_lane32(s, (int)((control >> 4) & 3)));
            interp_put32(out, 3, interp_lane32(s, (int)((control >> 6) & 3)));
        }
        interp_xmm_put(cpu, destination, out);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- the shift-by-xmm forms (0F D1/D2/D3 right, E1/E2 arithmetic, F1/F2/F3 left) -------------
    // The count is the FULL low 64 bits of the source, not its low 8 -- a source qword of 0x100 is a
    // count of 256 and zeroes the register, so it must not be truncated into a count of 0.
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

    // ---- MOVQ xmm/m64 store (66 0F D6) -----------------------------------------------------------
    case 0xD6: {
        uint8_t half[16] = {0};
        interp_xmm_get(cpu, destination, d);
        memcpy(half, d, 8);
        if (insn->is_mem) {
            interp_store_bytes(interp_ea(cpu, insn, next), half, 8);
        } else {
            // Register destination: MOVQ zeroes the upper 64 bits rather than merging.
            interp_xmm_put(cpu, insn->rm_reg, half);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- PMOVMSKB (66 0F D7) ---------------------------------------------------------------------
    // The reduction every strlen/memchr/strcmp ends with: gather the top bit of each of the sixteen bytes
    // into a general register so ordinary integer code can branch on it.
    case 0xD7: {
        interp_xmm_get(cpu, insn->rm_reg, s);
        uint64_t mask = 0;
        for (int i = 0; i < 16; i++)
            mask |= (uint64_t)((s[i] >> 7) & 1) << i;
        interp_reg_write(cpu, insn, destination, 4, mask);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOVNTDQ (66 0F E7): an aligned store, the hint carries no architectural effect -----------
    case 0xE7:
        if (interp_sse_unaligned(cpu, insn, next)) return interp_guest_trap(cpu, pc, 11, 128);
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_put(cpu, insn, next, 16, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- the bitwise logical group (0F DB/DF/EB/EF) ----------------------------------------------
    case 0xDB: // PAND
    case 0xDF: // PANDN
    case 0xEB: // POR
    case 0xEF: // PXOR
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        for (int i = 0; i < 16; i++)
            d[i] = op == 0xDB ? (uint8_t)(d[i] & s[i])
                   : op == 0xDF ? (uint8_t)(~d[i] & s[i]) // PANDN: NOT destination, then AND
                   : op == 0xEB ? (uint8_t)(d[i] | s[i])
                                : (uint8_t)(d[i] ^ s[i]);
        interp_xmm_put(cpu, destination, d);
        cpu->rip = next;
        return STEP_NEXT;

    // ---- packed integer add/subtract, plain and saturating ---------------------------------------
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

    // ---- unsigned/signed min and max (0F DA/DE/EA/EE) --------------------------------------------
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

    // ---- packed multiplies (0F D5/E4/E5/F4/F5) ---------------------------------------------------
    case 0xD5: // PMULLW: low 16 bits of each 16x16 product
    case 0xE4: // PMULHUW: high 16 bits, unsigned
    case 0xE5: // PMULHW: high 16 bits, signed
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

    case 0xF4: { // PMULUDQ: unsigned 32x32 -> 64, from dwords 0 and 2
        uint8_t out[16];
        interp_xmm_get(cpu, destination, d);
        interp_sse_rm_get(cpu, insn, next, 16, s);
        interp_put64(out, 0, (uint64_t)interp_lane32(d, 0) * (uint64_t)interp_lane32(s, 0));
        interp_put64(out, 1, (uint64_t)interp_lane32(d, 2) * (uint64_t)interp_lane32(s, 2));
        interp_xmm_put(cpu, destination, out);
        cpu->rip = next;
        return STEP_NEXT;
    }

    case 0xF5: { // PMADDWD: signed 16x16 products summed in pairs into dwords
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

    case 0xF6: { // PSADBW: sum of absolute byte differences, per 8-byte half, into words 0 and 4
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

    // ---- rounding averages (0F E0/E3) ------------------------------------------------------------
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

// ---------------------------------------------------------------------------------------------------
// The 0F (two-byte) opcode map.
// ---------------------------------------------------------------------------------------------------

// Name the legacy-SSE region so a residual gap reports as "SSE" rather than as a bare opcode byte. The
// list is the 0F-map SSE/SSE2 opcode space, i.e. everything not claimed by an integer arm below;
// interp_step_sse() above implements the data-movement, bitwise and integer-SIMD part of it.
static int interp_is_legacy_sse(uint8_t op) {
    if (op >= 0x10 && op <= 0x17) return 1; // movups/movss/movlps/movhps/unpck*
    if (op >= 0x28 && op <= 0x2F) return 1; // movaps/movntps/cvt*/ucomiss/comiss
    if (op >= 0x50 && op <= 0x6D) return 1; // movmsk/sqrt/and/or/xor/arith/pack/punpck
    if (op >= 0x6E && op <= 0x7F) return 1; // movd/movdqa/pshuf/pcmp/movq
    if (op >= 0xD0 && op <= 0xFF) return 1; // psrl/psra/psll/pmul/pand/por/pxor/padd/psub/pavg/pmovmskb
    if (op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6) return 1; // cmpps/pinsrw/pextrw/shufps
    return 0;
}

static int interp_step_two_byte(struct cpu *cpu, struct insn *insn, uint64_t pc, uint64_t next) {
    uint8_t op = insn->op;

    // ---- Jcc rel32 (0F 80..8F) -------------------------------------------------------------------
    if (op >= 0x80 && op <= 0x8F) {
        cpu->rip = interp_cond(cpu, op & 0xf) ? next + (uint64_t)insn->imm : next;
        cpu->reason = R_BRANCH;
        return STEP_END;
    }

    // ---- SETcc r/m8 (0F 90..9F) ------------------------------------------------------------------
    if (op >= 0x90 && op <= 0x9F) {
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_rm_write(cpu, insn, &operand, 1, (uint64_t)(interp_cond(cpu, op & 0xf) ? 1 : 0));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- CMOVcc r, r/m (0F 40..4F) ---------------------------------------------------------------
    if (op >= 0x40 && op <= 0x4F) {
        interp_operand operand = interp_rm(cpu, insn, next);
        // The load happens whether or not the condition holds -- CMOVcc reads its source operand
        // unconditionally on real hardware, so a CMOV from an unmapped address faults either way.
        uint64_t source = interp_rm_read(cpu, insn, &operand, insn->opsize);
        // And the DESTINATION is always written: `cmovcc r32, r/m32` zero-extends into the full 64-bit
        // register even when the move does not happen. Writing the old value back through
        // interp_reg_write is what reproduces that, and skipping it is a classic emulator bug that leaves
        // stale high bits in a pointer.
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         interp_cond(cpu, op & 0xf) ? source : interp_reg_read(cpu, insn, insn->reg, insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- BSWAP r (0F C8..CF) ---------------------------------------------------------------------
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
    // ---- SYSCALL (0F 05) -------------------------------------------------------------------------
    // THE RIP ADVANCE CONVENTION. Unlike the AArch64 guest -- whose PC still points at the SVC when the
    // kernel is entered, so its dispatcher does `pc += 4` after service() -- this frontend pre-advances
    // rip PAST the two `0F 05` bytes before exiting, and interp_dispatch.h's R_SYSCALL arm therefore does
    // NO advance of its own (it only clears cpu->redirect, which execve/sigreturn set when they wrote rip
    // themselves). The JIT establishes that convention at emit time; this is the same convention, so the
    // shared reason hook serves both backends unchanged.
    case 0x05:
        return interp_exit(cpu, next, R_SYSCALL);

    // ---- CPUID (0F A2) ---------------------------------------------------------------------------
    case 0xA2:
        return interp_exit(cpu, next, R_CPUID); // hl_x86_cpuid fills rax/rbx/rcx/rdx; rip already = next

    // ---- UD1 / UD2 (0F B9 / 0F 0B): a deliberate trap, not an engine gap ------------------------
    case 0x0B:
    case 0xB9:
        return interp_guest_trap(cpu, pc, 4 /*SIGILL*/, 2 /*ILL_ILLOPN*/);

    // ---- Multi-byte NOPs and hint instructions --------------------------------------------------
    // 0F 1F is the canonical multi-byte NOP; 0F 0D is prefetchw; 0F 18..0F 1E are the prefetch and
    // reserved-hint space, which includes ENDBR64 (F3 0F 1E FA). All are architecturally hints with no
    // register or memory effect, so retiring them is the whole implementation.
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

    // ---- EMMS (0F 77): no MMX tag word exists in this model --------------------------------------
    case 0x77:
        cpu->rip = next;
        return STEP_NEXT;

    // ---- 0F 01: the system-instruction group, of which user space uses three --------------------
    case 0x01:
        if (insn->has_modrm && insn->modrm == 0xF9) { // RDTSCP: EDX:EAX = counter, ECX = TSC_AUX (0)
            uint64_t counter = now_ns();
            interp_reg_write(cpu, insn, RAX, 4, counter & UINT64_C(0xffffffff));
            interp_reg_write(cpu, insn, RDX, 4, counter >> 32);
            interp_reg_write(cpu, insn, RCX, 4, 0);
            cpu->rip = next;
            return STEP_NEXT;
        }
        if (insn->has_modrm && insn->modrm == 0xD0) { // XGETBV(ecx=0): XCR0 = x87 + SSE, deliberately no AVX
            // Matching cpuid.c, which withholds AVX so guest glibc takes its SSE paths. A guest that sees
            // an AVX bit here and no AVX in CPUID (or the reverse) takes a code path neither backend
            // implements, so the two must agree.
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

    // ---- RDTSC (0F 31) --------------------------------------------------------------------------
    // A monotonic nanosecond counter, which is what cpuid.c already advertises via
    // CPUID.80000007H:EDX[8] (invariant TSC). Using the engine's clock rather than a host RDTSC keeps this
    // host-neutral and keeps the value consistent with what clock_gettime reports to the same guest.
    case 0x31: {
        uint64_t counter = now_ns();
        interp_reg_write(cpu, insn, RAX, 4, counter & UINT64_C(0xffffffff));
        interp_reg_write(cpu, insn, RDX, 4, counter >> 32);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOVZX / MOVSX (0F B6/B7 zero-extend, BE/BF sign-extend) --------------------------------
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
        // The DESTINATION width governs how the extended value lands: a 32-bit destination zeroes bits
        // 63:32, a 16-bit destination (under 0x66) merges and preserves bits 63:16.
        interp_reg_write(cpu, insn, insn->reg, insn->opsize, value);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- IMUL r, r/m (0F AF) --------------------------------------------------------------------
    case 0xAF: {
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, insn->opsize);
        interp_reg_write(cpu, insn, insn->reg, insn->opsize,
                         interp_imul_truncating(cpu, interp_reg_read(cpu, insn, insn->reg, insn->opsize), source,
                                                insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- SHLD / SHRD (0F A4 imm8, A5 by CL, AC imm8, AD by CL) ----------------------------------
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

    // ---- BT / BTS / BTR / BTC ------------------------------------------------------------------
    // Register-index forms 0F A3/AB/B3/BB; immediate forms 0F BA /4../7. CF receives the tested bit; x86
    // leaves OF/SF/AF/PF undefined and ZF unchanged, so only C is written.
    //
    // For a MEMORY operand the bit index is not masked: it selects a bit anywhere in memory relative to
    // the effective address, and a negative index reaches BELOW it. Resolving to a byte address plus a
    // bit-in-byte is exactly equivalent to the architectural "operand-size window" formulation and is
    // what makes a locked BTS a single-byte atomic instead of a wide one.
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
            int64_t byte_offset = index >> 3; // arithmetic shift: a negative index reaches below the EA
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

    // ---- BSF / TZCNT (0F BC) and BSR / LZCNT (0F BD) -------------------------------------------
    // The F3 prefix selects the BMI/ABM count forms, which are NOT the same instruction: tzcnt agrees
    // with bsf for a nonzero source but lzcnt is a leading-ZERO COUNT while bsr is a bit INDEX. Conflating
    // them silently corrupts BMI codegen, so they are separate arms.
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
        } else { // BSF / BSR: ZF = (source == 0), destination UNDEFINED (left unchanged) in that case
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

    // ---- POPCNT (F3 0F B8) --------------------------------------------------------------------
    case 0xB8: {
        if (!insn->rep) return interp_undefined(cpu, insn, pc, "0F B8 without the F3 prefix (JMPE)");
        int width = insn->opsize;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t source = interp_rm_read(cpu, insn, &operand, width) & interp_mask(width);
        uint64_t result = (uint64_t)__builtin_popcountll(source);
        // POPCNT sets ZF from the SOURCE and clears every other flag.
        interp_flags_nzcv(cpu, 0, source == 0, 0, 0);
        // POPCNT clears PF. The lane holds a byte whose EVEN parity IS PF, so clearing PF means storing an
        // ODD-parity byte -- 1 is the cheapest one.
        cpu->pf = 1;
        cpu->af = 0;
        interp_reg_write(cpu, insn, insn->reg, width, result);
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- CMPXCHG (0F B0/B1) -------------------------------------------------------------------
    case 0xB0:
    case 0xB1: {
        int width = (op & 1) ? insn->opsize : 1;
        interp_operand operand = interp_rm(cpu, insn, next);
        uint64_t accumulator = interp_reg_read(cpu, insn, RAX, width);
        uint64_t source = interp_reg_read(cpu, insn, insn->reg, width);
        uint64_t observed;
        if (operand.is_memory && insn->lock) {
            // A real compare-exchange, so a concurrent guest thread cannot slip between the compare and
            // the store. The width-specific helper loop in interp_locked_rmw cannot express "swap only
            // if equal", so do the CAS here.
            uint64_t host_address = hl_x86_guest_pointer(operand.address);
            void *pointer = (void *)(uintptr_t)host_address;
            int swapped = 0;
            interp_access_begin();
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
            } else { // split lock: serialise under the hashed spinlock, like interp_locked_rmw does
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
        // Flags come from the comparison itself (CMP accumulator, destination), not just ZF.
        (void)interp_alu_sub(cpu, accumulator, observed, 0, width);
        if ((observed & interp_mask(width)) != (accumulator & interp_mask(width)))
            interp_reg_write(cpu, insn, RAX, width, observed); // Intel: on mismatch the accumulator reloads
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- XADD (0F C0/C1) ----------------------------------------------------------------------
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
            // WRITE ORDER MATTERS, and only here. x86 defines XADD as `TEMP := SRC+DEST; SRC := DEST;
            // DEST := TEMP`, i.e. the SUM lands last -- so `xadd %ax, %ax`, where SRC and DEST are the SAME
            // register, must leave the SUM, not the pre-image. Writing the register first and the r/m second
            // reproduces that and is indistinguishable from either order whenever the two operands differ.
            interp_reg_write(cpu, insn, insn->reg, width, old);
            interp_rm_write(cpu, insn, &operand, width, sum);
        }
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- MOVNTI (0F C3): a non-temporal store, architecturally an ordinary store ---------------
    case 0xC3: {
        if (!insn->is_mem) return interp_undefined(cpu, insn, pc, "MOVNTI with a register destination (#UD)");
        interp_operand operand = interp_rm(cpu, insn, next);
        interp_store(operand.address, insn->opsize, interp_reg_read(cpu, insn, insn->reg, insn->opsize));
        cpu->rip = next;
        return STEP_NEXT;
    }

    // ---- 0F C7: CMPXCHG8B / CMPXCHG16B ---------------------------------------------------------
    case 0xC7: {
        if ((insn->reg & 7) != 1 || !insn->is_mem)
            return interp_undefined(cpu, insn, pc, "0F C7 group (RDRAND/RDSEED/VMPTRLD)");
        interp_operand operand = interp_rm(cpu, insn, next);
        if (insn->opsize == 8) {
            // cmpxchg16b: hand the REBASED address to the shared helper, which performs the 128-bit
            // compare-exchange under a hashed spinlock and sets ZF. rip is already past the instruction.
            cpu->x87_ea = hl_x86_guest_pointer(operand.address);
            return interp_exit(cpu, next, R_CMPXCHG16);
        }
        // cmpxchg8b: EDX:EAX vs m64; on a match store ECX:EBX and set ZF, else reload EDX:EAX. Only ZF is
        // architecturally affected.
        uint64_t expected = ((cpu->r[RDX] & UINT64_C(0xffffffff)) << 32) | (cpu->r[RAX] & UINT64_C(0xffffffff));
        uint64_t desired = ((cpu->r[RCX] & UINT64_C(0xffffffff)) << 32) | (cpu->r[RBX] & UINT64_C(0xffffffff));
        uint64_t observed;
        int equal;
        if (insn->lock) {
            uint64_t host_address = hl_x86_guest_pointer(operand.address);
            uint64_t probe = expected;
            interp_access_begin();
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

    // ---- 0F AE: the fences, plus the FXSAVE/MXCSR group --------------------------------------
    case 0xAE: {
        int sub = insn->reg & 7;
        if (sub >= 5 && !insn->is_mem) {
            // LFENCE / MFENCE / SFENCE. On an x86-64 host the C accesses around them already have x86-TSO
            // ordering, so the fence is a no-op; interp_tso_fence is where a weakly-ordered host would
            // supply a real barrier, which is why it is called rather than elided.
            interp_tso_fence();
            cpu->rip = next;
            return STEP_NEXT;
        }
        if ((sub == 0 || sub == 1) && insn->is_mem) {
            // fxsave / fxrstor: the shared helper handles the modeled x87 stack and the MXCSR/xmm area.
            cpu->x87_ea = hl_x86_guest_pointer(interp_ea(cpu, insn, next));
            return interp_exit(cpu, next, sub == 0 ? R_FXSAVE : R_FXRSTOR);
        }
#if defined(HL_HOST_CPU_X86_64)
        if ((sub == 2 || sub == 3) && insn->is_mem) {
            // LDMXCSR (/2) and STMXCSR (/3). The guest MXCSR lives in the HOST MXCSR on this backend -- see
            // the "SSE / SSE2 FLOATING POINT" section -- so these are a load and a store of that register
            // with no projection in between. Read the memory operand BEFORE touching the host register so a
            // faulting operand leaves the guest's rounding mode and sticky flags exactly as they were.
            if (sub == 3) {
                uint32_t live = _mm_getcsr();
                interp_store(interp_ea(cpu, insn, next), 4, live);
            } else {
                uint32_t loaded = (uint32_t)interp_load(interp_ea(cpu, insn, next), 4);
                // MASK the word. LDMXCSR raises #GP on any bit outside the CPU's MXCSR_MASK, and this value
                // came from GUEST memory: unmasked, an odd or malicious guest word would kill the ENGINE
                // (the host #GP arrives as a fatal SIGSEGV in engine code) where real hardware faults the
                // GUEST. Exactly the trap x87state.c's fxrstor arm documents; 0xffff keeps every
                // architecturally defined field, DAZ included, and drops only the reserved high half.
                _mm_setcsr(loaded & 0xffffu);
            }
            cpu->rip = next;
            return STEP_NEXT;
        }
#endif
        // XSAVE/XRSTOR (/4,/5 with a memory operand) name the extended-state area, which this model has no
        // layout for (there is no AVX state in cpu->v[] terms that XSAVE's XCR0-driven format would describe).
        return interp_undefined(cpu, insn, pc, "TODO(amd64-host): XSAVE/XRSTOR (0F AE)");
    }

    default: break;
    }

    // Legacy SSE/SSE2. Unlike VEX (R_AVX) and the 0F38/0F3A escape maps (R_SSE3B) there is no shared C
    // emulator to route to, so it is implemented in this file; interp_step_sse owns the data-movement,
    // bitwise and integer-SIMD space and declines anything left (which is the floating-point arithmetic
    // and the MMX encodings, each of which reports itself specifically).
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

// ---------------------------------------------------------------------------------------------------
// The persistent translated-code cache.
//
// The pcache stores HOST CODE plus the arena-relative bookkeeping that makes it relocatable. This backend
// produces no host code, and its arena holds descriptors whose only content is a guest PC -- restoring
// either direction across backends would hand one of them the other's representation. So the answer here
// is a clean, permanent MISS and a no-op save: never load, never write, never touch the file.
//
// The one thing that must be exactly right is IDENTITY. pcache_engine_id feeds HL_HOST_CPU_ISA to
// hl_identity_configuration, so an identity computed on this host can never collide with one computed by
// the AArch64 backend, even for the same guest binary and the same engine build. That matters beyond the
// cache: linux_abi/checkpoint.c stamps this same engine id into a checkpoint image and validates it on
// restore, so without the host-ISA term a checkpoint written by the JIT could be accepted here (or the
// reverse) and the restored cpu would be resumed against translations that do not exist.
// ---------------------------------------------------------------------------------------------------

static int g_pcache_forked;     // set in a fork child by linux_abi/fork.c; nothing here reads it
static int g_force_base_failed; // latched by the ELF loader when a fixed-VA image map falls back

static uint64_t pcache_engine_id(void) {
    uint64_t hash = 1469598103934665603ull;
    uint64_t self = hl_identity_source(&g_jit_services, g_self_path);
    for (const char *p = __DATE__ " " __TIME__; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 1099511628211ull;
    }
    hash ^= self;
    hash *= 1099511628211ull;
    // No emitted-code mode bits to mix in (no inline fast syscall, no inline clock, no slim spill): those
    // select between shapes of GENERATED code and there is none. The host-ISA term is the load-bearing one.
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
    return 0; // MISS -> the dispatcher "translates" fresh, which for this backend is a descriptor bump
}

static void pcache_save(void) {
    // Deliberately empty. Writing a file this backend can never load, keyed by an identity no other
    // backend will accept, would cost I/O and invite a future reader to believe a warm path exists.
}

static void pcache_directory_close(void) {
}

static void pcache_note_fixed_img(uint64_t base, uint64_t span) {
    // The pcache records the fixed-image spans to decide which restored block-map entries are revivable.
    // Nothing is revivable here, so there is nothing to record.
    (void)base;
    (void)span;
}

// ---------------------------------------------------------------------------------------------------
// ARM64 emitter stubs, and why this file has to carry them.
//
// The per-class lowering files under lower/ are compiled INDEPENDENTLY into libhl-translator (see
// IR_SOURCES in CMakeLists.txt), not #included into the unity TU -- so they are built on every host, and
// each one calls the ARM64 emitters (e_ldr, e_rrr, emit_exit_const, hl_x86_emit_spill, ...) that only the
// AArch64 arm of core/target/x86_64.c defines. On an AArch64 host that is invisible: the linker pulls an
// archive member in only when something references a symbol in it, and the AArch64 build satisfies every
// reference from emit.c.
//
// On this host it is not invisible. core/target/x86_64.c's engine_global_init unconditionally calls
// hl_x86_rep_set_store_commit() and hl_x86_rep_set_access_validators() -- runtime configuration hooks that
// happen to live in lower/repstr.c, the same object as hl_x86_lower_repstr, its ARM64 emitter. Referencing
// the setters pulls the object in, and the emitter half's undefined references then fail the link. Nothing
// this backend does can avoid that: the calls are in a file it does not own.
//
// So the honest fix is to make the symbols real and unreachable. The lowering entry points that would call
// them (hl_x86_lower_repstr and friends) are never invoked here -- this backend never translates -- so
// these bodies are dead by construction, and aborting is what turns a hypothetical future call into a
// located crash rather than silent nonsense. Each says the same thing: something asked for ARM64.
//
// PROPER FIX, for whoever owns the branch: split the host-neutral runtime halves of the lower/ files
// (hl_x86_rep_movs, hl_x86_rep_stos and the two setters above) into their own translation unit, or gate
// lower/*.c out of IR_SOURCES on a non-AArch64 host once nothing references them. Either removes this
// section entirely, and the second one also lets the interpreter reuse the bulk rep helpers, which is a
// real speedup it currently has to decline.
// ---------------------------------------------------------------------------------------------------

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

// These two return a value, so they cannot use the macro above.
int emit_soft_memory_active(void) {
    // Asked at TRANSLATE time by the lowering files to decide whether to emit soft-memory guards. Nothing
    // is emitted here and the interpreter resolves logical mappings inline, so the truthful answer is "no
    // guards" -- and unlike the stubs above this one is worth answering rather than aborting, because a
    // future reader may legitimately call it from a shared path.
    return 0;
}

uint32_t *hl_x86_emit_cursor(void) {
    interp_no_emitter("hl_x86_emit_cursor");
    return NULL;
}
