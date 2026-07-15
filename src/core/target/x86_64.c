#include "namespace.h"

// hl/core/target -- x86-64 Linux guest target composition.
//
// This unity translation unit wires the x86-64 translator frontend to the shared Linux ABI,
// container, host-service, and engine layers. Architecture-specific translation remains under
// translator/guest/x86_64; Linux executable loading and process construction belong to linux_abi.

// jit86.c — an x86-64-guest JIT (x86-64 -> ARM64) for Linux guests on macOS/arm64.
//
// Sibling of runtime/jit/jit.c (which is aarch64->aarch64). See DESIGN.md for the
// full "what breaks / what doesn't" rationale. Short version:
//
//   * The ISA-AGNOSTIC scaffolding (code cache, guest-PC->host-code map, direct-
//     branch chaining, the run_block/block_return trampolines, the Linux->macOS
//     syscall bodies, the Linux ELF loader, rootfs path rewriting) is COPIED+ADAPTED from
//     jit.c. We can't refactor jit.c (it's under active dev), so we duplicate.
//   * The FRONT-END is new: an x86-64 decoder + per-opcode ARM64 codegen, replacing
//     jit.c's "copy the instruction verbatim" core (which only works same-arch).
//
// Register model (the win from x86 having only 16 GPRs, see DESIGN.md §4):
//   guest  rax rcx rdx rbx rsp rbp rsi rdi  r8..r15
//   host    x0  x1  x2  x3  x4  x5  x6  x7  x8..x15   (guest reg# == host reg#)
//   cpu ptr : x28 (PINNED for the whole block)   scratch : x16,x17   forbidden: x18
//   flags   : ARM nzcv saved/restored to cpu->nzcv (exact for cmp/test->jcc, §9)
//
// Status: BOOTSTRAP. Implements enough to run a freestanding write+exit guest and a
// growing slice toward simple busybox. Unknown opcodes print their bytes and exit —
// that is the iterative workflow (run -> see unimpl -> add it -> repeat).

#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <poll.h>
#include "../../host/native_compat.h"
#include "../../host/native_context.h"
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdatomic.h>

#include "hl/engine.h"
#include "hl/linux_abi.h"
#include "../launch.h"
#include "../options.h"
#include "../cli.h"
#include "native.h"
#include "services.h"
#include "../bus.h"
#include "../engine_result.h"
#include "../../linux_abi/bus.h"

/* Instance-scoped host seam supplied by hl_engine. CLI launches retain their native-host path with NULL. */
static hl_target_services g_target_services;
#define g_host_services (g_target_services.injected)
#define g_jit_services (g_target_services.bound)
static hl_status g_engine_result_status;
static hl_linux_abi *g_linux_box;

hl_status hl_run_linux_guest_status(void) {
    return g_engine_result_status;
}

static uint64_t g_host_launch_monotonic_ns;

#include "../../translator/guest/x86_64/cpu.h"
#include "../../translator/guest/x86_64/frame.h"
#include "../../translator/guest/x86_64/abi.h"      // cpu-interface seam (G_* contract + sysmap + normalize)
#include "../../translator/guest/x86_64/dispatch.h" // x86 dispatch seam for the SHARED engine/dispatch.c
#define HL_GUEST_STAT_SIZE HL_LINUX_STAT_X86_64_SIZE
#define HL_GUEST_STAT_ENCODE hl_linux_stat_encode_x86_64
#define HL_GUEST_BOUND_STAT hl_linux_stat_x86_64
#include "../../linux_abi/guest_stat.h"
#undef HL_GUEST_BOUND_STAT
#undef HL_GUEST_STAT_ENCODE
#undef HL_GUEST_STAT_SIZE

// Byte size of the guest `struct stat` stat.c writes -- the shared stat syscalls (os/linux/syscall/
// fs.c cases 79/80) validate exactly this many guest bytes before filling the buffer (EFAULT guard).
#define GUEST_LINUX_STAT_BYTES 144

#include "../../linux_abi/container/state.c" // SHARED: container globals (rootfs/cwd/netns/ids/fd tables)
#include "../../linux_abi/fdcache.h"
#include "../../linux_abi/container/vfs/gmap.h"
#include "../../translator/guest/x86_64/glue.c" // x86-only engine globals the shared cache.c omits
#include "../../translator/cache.c"             // SHARED translator: code cache + block map

static const hl_host_services *effective_host_services(void) {
    return hl_target_services_effective(&g_target_services);
}

#include "../../translator/guest/x86_64/emit.c" // x86 engine: arm64 emitters + SSE + x87
#include "../../translator/guest/x86_64/address.h"

static void address_addi(void *context, int rd, int rn, unsigned immediate, int sf, int shift) {
    (void)context;
    if (shift)
        e_addi_sh(rd, rn, immediate, sf, shift);
    else
        e_addi(rd, rn, immediate, sf);
}

static void address_subi(void *context, int rd, int rn, unsigned immediate, int sf, int shift) {
    (void)context;
    if (shift)
        e_subi_sh(rd, rn, immediate, sf, shift);
    else
        e_subi(rd, rn, immediate, sf);
}

static void address_movconst(void *context, int rd, uint64_t value) {
    (void)context;
    e_movconst(rd, value);
}

static void address_addreg(void *context, int rd, int rn, int rm, int sf, int shift) {
    (void)context;
    e_rrr(A_ADD, rd, rn, rm, sf, shift);
}

static void address_lsr(void *context, int rd, int rn, int shift, int sf) {
    (void)context;
    e_lsr_i(rd, rn, shift, sf);
}

static void address_movreg(void *context, int rd, int rn, int sf) {
    (void)context;
    e_mov_rr(rd, rn, sf);
}

static void address_movzero(void *context, int rd, uint32_t immediate, int shift) {
    (void)context;
    e_movz(rd, immediate, shift);
}

static void address_uxt(void *context, int rd, int rn, int bytes) {
    (void)context;
    e_uxt(rd, rn, bytes);
}

static void address_load_cpu(void *context, int rt, int offset) {
    (void)context;
    e_ldr(rt, 28, offset);
}

static void address_load_scaled(void *context, int width, int rt, int rn, unsigned offset) {
    (void)context;
    e_load_uoff(width, rt, rn, offset);
}

static void address_load_unscaled(void *context, int width, int rt, int rn, int offset) {
    (void)context;
    e_ldur(width, rt, rn, offset);
}

static void address_load(void *context, int width, int rt, int rn) {
    (void)context;
    e_load(width, rt, rn);
}

static void address_bus_guard(void *context, int reg, uint64_t size, uint64_t pc) {
    (void)context;
    emit_bus_guard(reg, size, pc);
}

static uintptr_t address_branch_placeholder(void *context) {
    (void)context;
    uint32_t *placeholder = (uint32_t *)g_cp;
    emit32(0);
    return (uintptr_t)placeholder;
}

static void address_patch_cbnz(void *context, uintptr_t token, int reg) {
    (void)context;
    uint32_t *placeholder = (uint32_t *)token;
    *placeholder = UINT32_C(0xB5000000) |
                   (((uint32_t)(((uint8_t *)g_cp - (uint8_t *)placeholder) / 4) & UINT32_C(0x7FFFF)) << 5) |
                   (uint32_t)reg;
}

static const hl_x86_address_emitter address_emitter = {address_addi,          address_subi,
                                                       address_movconst,      address_addreg,
                                                       address_lsr,           address_movreg,
                                                       address_movzero,       address_uxt,
                                                       address_load_cpu,      address_load_scaled,
                                                       address_load_unscaled, address_load,
                                                       address_bus_guard,     address_branch_placeholder,
                                                       address_patch_cbnz};

static hl_x86_address_state address_state(void) {
    return (hl_x86_address_state){NULL,   &address_emitter, g_nonpie_lo, g_nonpie_hi,           g_nonpie_bias,
                                  OFF_FS, OFF_GS,           !noeaopt(),  jit_guest_bus_active()};
}

void emit_ea_core(struct insn *insn, uint64_t next, int bias) {
    hl_x86_address_state state = address_state();
    hl_x86_address_emit(&state, insn, next, bias);
}

void emit_ea(struct insn *insn, uint64_t next) {
    emit_ea_core(insn, next, 1);
}

int ea_imm_fold(struct insn *insn, int width, int *rn, int *offset) {
    hl_x86_address_state state = address_state();
    return hl_x86_address_fold(&state, insn, width, rn, offset);
}

void emit_load_mem(struct insn *insn, uint64_t next, int width, int rt) {
    hl_x86_address_state state = address_state();
    hl_x86_address_load(&state, insn, next, width, rt);
}

#include "../../translator/guest/x86_64/translate.c" // x86-64 translate_block + trampolines
#include "../../translator/guest/x86_64/cache.c"     // persistent translated-code cache (HL_PCACHE=1)
#include "../../linux_abi/thread.c"                  // SHARED: clone->pthread, per-thread cpu, futex
#include "../../linux_abi/signal.c"                  // SHARED: signal delivery driver + translation

static int x86_signal_cache_contains(void *context, uint64_t pc) {
    (void)context;
    return jit_pc_in_retained_cache(pc);
}

static uint64_t x86_signal_handler(void *context, int signal_number) {
    (void)context;
    return g_sigact[signal_number].handler;
}

static hl_x86_signal_queue x86_signal_queue(void) {
    return (hl_x86_signal_queue){x86_signal_handler, NULL, g_sigcode, g_sigaddr, &g_pending};
}

static void build_signal_frame(struct cpu *c, int sig) {
    hl_x86_signal_state state = {
        .handler = g_sigact[sig].handler,
        .flags = g_sigact[sig].flags,
        .mask = g_sigact[sig].mask,
        .code = &g_sigcode[sig],
        .value = &g_sigval[sig],
        .address = &g_sigaddr[sig],
        .pid = &g_sigpid[sig],
        .uid = &g_siguid[sig],
        .sigreturn_pc = SIGRETURN_PC,
        .trace = g_trace,
    };
    hl_x86_signal_build(c, sig, &state);
}

static void do_sigreturn(struct cpu *c) {
    hl_x86_signal_restore(c);
}

static int sigframe_capture_fault(struct cpu *c, void *native_context) {
    return hl_x86_signal_capture(c, native_context, x86_signal_cache_contains, NULL);
}

static void sigframe_resume_dispatch(struct cpu *c, void *native_context) {
    hl_x86_signal_resume(c, native_context, (uintptr_t)block_return);
}

static int fastclk_fault_fixup(siginfo_t *info, void *native_context) {
    struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
    return hl_x86_signal_fast_clock_fault(c, (uintptr_t)(info != NULL ? info->si_addr : NULL), native_context);
}

static int raise_guest_de(struct cpu *c) {
    hl_x86_signal_queue queue = x86_signal_queue();
    return hl_x86_signal_raise_divide(c, &queue);
}

static int raise_guest_trap(struct cpu *c) {
    hl_x86_signal_queue queue = x86_signal_queue();
    return hl_x86_signal_raise_trap(c, &queue);
}

static uint64_t nzcv_to_eflags(uint64_t nzcv) {
    return hl_x86_signal_nzcv_to_eflags(nzcv);
}

static uint64_t eflags_to_nzcv(uint64_t eflags) {
    return hl_x86_signal_eflags_to_nzcv(eflags);
}

#include "../../linux_abi/container/vfs.c"   // SHARED: rootfs jail, overlay, /proc synth, stat
#include "../../linux_abi/container/netns.c" // SHARED: sockets, loopback netns, termios
static void load_elf(const char *path, struct loaded *out);
static int elf_interp(const char *path, char *out, size_t n);
static uint64_t build_stack(int argc, char **argv, struct loaded *lm, uint64_t at_base);

static int64_t legacy_time_seconds(void *context) {
    (void)context;
    return (int64_t)time(NULL);
}

static int legacy_set_alarm(void *context, uint64_t seconds, uint64_t *remaining_seconds) {
    struct itimerval next = {{0, 0}, {0, 0}};
    struct itimerval previous = {{0, 0}, {0, 0}};
    (void)context;
    next.it_value.tv_sec = (time_t)seconds;
    if (setitimer(ITIMER_REAL, &next, &previous) < 0) return errno;
    *remaining_seconds = (uint64_t)previous.it_value.tv_sec + (previous.it_value.tv_usec != 0 ? 1u : 0u);
    return 0;
}

#include "../../linux_abi/syscall/dispatch.c"  // SHARED: the canonical syscall layer
#include "../../linux_abi/sentry.c"            // untrusted-guest isolation: SPSC ring + sentry split (g_untrusted)
#include "../../translator/guest/x86_64/avx.c" // AVX/AVX2/AVX-512 emulation
#include "../dispatch.c"                       // SHARED engine: run_guest loop (x86 drives it via dispatch.h;
// keeps its own run_block/block_return in translate.c, G_OWN_TRAMPOLINES)
#include "../../linux_abi/x86.c" // Linux x86-64 ELF loader + stack + fault handlers

// ---- entry + main ----
// ---------------- entry ----------------
// Fork-server refactor: the original guest entry inlined container init, engine init,
// (pthread key + MAP_JIT arena + signal handlers + trace env), and (3) per-launch load+run. The
// resident engine server pays (1)+(2) once and shares them COW with every forked worker, so
// those two phases are factored into container_init()/engine_global_init(). engine_global_init()
// is idempotent (g_engine_inited) so the standalone path is byte-for-byte unchanged: standalone
// hl_run_linux_guest() composes container_init -> engine_global_init -> load_program -> run_loaded in the
// exact original order, with the identical operations in each phase.
static int g_engine_inited;

static int container_init(const char *rootfs) {
    hl_gmap_bind_limits(&g_limits);
    // PID ns: only containers (rootfs) get PID 1. Record the init's real host pid so the shared Linux
    // personality can virtualize just the init's identity (getpid()==1, host pgid<->guest pgid 1) and
    // pass real child pids straight through -- this is what makes bash job control (setpgid / TIOCSPGRP)
    // work on x86-64 the way it already does on aarch64. Without it g_init_hostpid stayed 0, getpid()
    // returned the real host pid, and bash's setpgid(0,1)/tcsetpgrp targeted host pid 1 (launchd) -> the
    // foreground command got SIGTTOU/SIGTTIN-stopped ("[N]+ Stopped  ls") instead of running.
    if (rootfs) g_init_hostpid = getpid();
    // cross-engine-process cgroup accounting: a FRESH shared slot table for THIS container init, inherited
    // by every guest fork (see state.c). Per-container so sibling forkserver workers never share a total.
    if (rootfs) acct_container_reset();
    container_read_resource_env(); // Docker CPU, read-only-root, and ulimit values from centralized HL options.
    // The final typed launch hands the container model to the engine as HL options, not as the
    // --hostname/--mem-max/--pids-max CLI flags. aarch64's container_init() already reads these options;
    // (linux_aarch64.c); x86-64 did not, so a `docker run --hostname h` on x86 dropped the hostname
    // (uname/gethostname/`/etc/hostname` returned "jit") and --memory/--pids-limit were ignored. The
    // out-of-process SpawnConfig::script() path passes them as CLI flags, which is why the default test
    // matrix missed this. Guard on the CLI value (only fill when the flag path left it unset), matching
    // aarch64, so a genuine --hostname flag still wins.
    {
        const char *h = hl_option_get("HL_HOSTNAME");
        if (h && h[0] && !g_hostname[0]) {
            strncpy(g_hostname, h, 64);
            g_hostname[64] = 0;
        }
        const char *m = hl_option_get("HL_MEM_MAX");
        if (m && m[0] && !g_mem_max) g_mem_max = parse_size(m);
        const char *p = hl_option_get("HL_PIDS_MAX");
        if (p && p[0] && !g_pids_max) g_pids_max = hl_parse_id("HL_PIDS_MAX", p);
    }
    if (rootfs && rootfs[0]) { // the shared container jails against the canonical rootfs + its dir fd
        g_rootfs = (char *)rootfs;
        if (root_handle_bind(g_rootfs) != 0) return -1;
        container_populate_dev();        // /dev/{fd,stdin,stdout,stderr,ptmx,pts,shm,console,...} the unpacker stripped
        container_populate_machine_id(); // /etc/machine-id agreeing with boot_id (if image ships none)
        // Container identity = root (0) by default; HL_UID/HL_GID or typed launch fields override it.
        const char *eu = hl_option_get("HL_UID");
        if (eu && g_uid < 0) g_uid = hl_parse_id("HL_UID", eu);
        const char *eg = hl_option_get("HL_GID");
        if (eg && g_gid < 0) g_gid = hl_parse_id("HL_GID", eg);
        if (g_uid < 0) g_uid = 0;
        if (g_gid < 0) g_gid = 0;
    }
    if (!rootfs && root_handle_bind("/") != 0) return -1;
    {
        // HL_NETNS is a short key (not a path) used to derive abstract-socket and IPC identities.
        // The daemon and both guest ISAs share it across exec; the private-loopback directory is derived from it.
        // Inherit the key when supplied, otherwise mint one from the process id.
        const char *ns = hl_option_get("HL_NETNS");
        char key[40];
        if (ns && ns[0])
            snprintf(key, sizeof key, "%.39s", ns);
        else
            snprintf(key, sizeof key, "%d", (int)getpid());
        namespace_key_set(key);
        snprintf(g_netns, sizeof g_netns, "/tmp/.hl-net-%.40s", key);
        if (hl_target_services_make_directory(&g_target_services, g_netns, 0700) == 0 && !(ns && ns[0]))
            hl_option_set("HL_NETNS", key, 1);
    }
    {
        const char *vs =
            hl_option_get("HL_VOLUMES"); // bind-mount volumes (env path; bridge usually can't pass env, so --vol too)
        if (vs && vs[0]) {
            char tmp[2048];
            snprintf(tmp, sizeof tmp, "%s", vs);
            char *sv;
            for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                add_vol(t);
        }
    }
    {
        const char *pub = hl_option_get("HL_PUBLISH");
        if (pub && pub[0] && !g_nportmap) parse_publish(pub);
    } // docker -p (inherit across exec)
    {
        const char *ls = hl_option_get("HL_LOWER"); // overlay lower layers (inherit across exec)
        if (ls && ls[0] && !g_nlower) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s", ls);
            char *sv;
            // colon-separated (highest first), UNIFIED with linux_aarch64.c and the Rust
            // engine launch-wire joiner (`src/launch/wire.rs`, `lowers.join(":")`). This
            // target historically split on ',', so multi-lower typed launches mis-split here.
            for (char *t = strtok_r(tmp, ":", &sv); t; t = strtok_r(NULL, ":", &sv))
                add_lower(t);
        }
    }
    if (g_rootfs && chdir(g_rootfs) != 0) return -1; // guest cwd "/" maps to the rootfs root
    // Docker -w / initial working directory: start the guest in HL_CWD (must be reachable inside the
    // container -- typically a bind-mounted volume). confine() normalizes + clamps it to the rootfs.
    const char *icwd = hl_option_get("HL_CWD");
    if (icwd && icwd[0]) confine(icwd, g_cwd, sizeof g_cwd);
    // derive the run user's supplementary group set from the image rootfs (runc additionalGids), after
    // g_uid/g_gid + the overlay lowers are resolved, so getgroups(2) and /proc/self/status Groups: report it.
    if (g_rootfs) container_parse_groups();
    return 0;
}

// W3D: idempotent engine init (pthread key + MAP_JIT arena + trace env + fault handlers). Returns 0
// on success, nonzero exit code on failure. First call wins; later calls are no-ops (g_engine_inited),
// so the resident parent pays this once and the standalone path runs it exactly as before.
static int engine_global_init(void) {
    if (hl_target_services_bind(&g_target_services) != 0) return 1;
    if (g_engine_inited) return 0;
    if (pthread_key_create(&g_cpu_key, NULL) != 0) {
        perror("pthread_key_create");
        return 1;
    }
    // macOS host services own code mappings and post-fork alias repair. NODUALMAP keeps the single-MAP_JIT
    // compatibility mode; the default remains a stable dual RW/RX mapping.
    if (jit_cache_init() != 0) {
        g_engine_result_status = hl_fatal_status(&g_jit_fatal);
        return 70;
    }
    {
        hl_fdcache_binding binding = {hl_target_services_bound(&g_target_services),
                                      &g_vfs_namespace,
                                      &g_nvols,
                                      g_rootfs_canon,
                                      &g_rootfs_canon_len,
                                      g_fdpath,
                                      1024,
                                      &g_threaded,
                                      hl_option_get("HL_FSGEN_FILE")};
        if (hl_fdcache_bind(&binding) != 0) {
            fprintf(stderr, "hl-engine: unable to initialize filesystem caches\n");
            return 1;
        }
    }
    g_trace = 0;
    g_systrace = 0;
    g_prof = 0;
    g_fwdskip = 8;
    g_notier2x = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO;
    extern void jit86_lazyguard(int, siginfo_t *, void *);
    sa.sa_sigaction = jit86_lazyguard;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    // Untrusted-guest isolation (the sentry process-split). OFF by default -> trusted path unchanged.
    g_untrusted = hl_option_get("HL_UNTRUSTED") != NULL;
    g_sentry_sandbox = hl_option_get("HL_SANDBOX") != NULL;
    // ptrace tracer/tracee coordination arena -- mmap the shared region ONCE here, BEFORE any guest
    // fork, so every descendant guest process inherits the same physical pages. Inert until a guest ptraces.
    ptrace_arena_init();
    g_engine_inited = 1;
    return 0;
}

// W3D: load main program + (optional) interp, recording the load base/entry/at_base into *lm/*li.
// Used both by the standalone path and by the fork-server's parent preload (so the COW-inherited
// image is byte-identical and the warm worker re-runs from the same entry at the same base). The
// gb/pb/ib buffers are static because g_exe_path = prog points into gb and must outlive this call.
static const char *load_program(const char *prog, struct loaded *lm, struct loaded *li, uint64_t *jump,
                                uint64_t *at_base, int *have_interp) {
    static char gb[1024];
    prog = find_in_path(prog, gb, sizeof gb);   // bare "sh" (docker) -> "/bin/sh" via the container PATH
    if (!g_comm_store[0]) set_guest_comm(prog); // record the pre-shebang name; preload lands here
    g_exe_path = prog;
    // /proc/self/exe must be the ABSOLUTE, CANONICAL guest path of the loaded image: a RELATIVE guest
    // invocation ("./x" from a harness) or an entry symlink otherwise leaks into the link value, and
    // glibc static-pie ASSERTS on it at startup ("dl-origin.c: linkval[0]=='/'"). Static: the
    // value must outlive this call, like gb above.
    static char bootexe[4200];
    exe_canon(prog, bootexe, sizeof bootexe);
    g_exe_path = bootexe;

    static char pb[4200];
    const char *prog_host = xresolve_overlay(prog, pb, sizeof pb); // upper, then lowers (pure --lower image)
    // opt8: load the guest image + interp at FIXED VAs so the translated arena is byte-identical across
    // runs (one-shot g_force_base, cleared inside load_elf). Only when the persistent cache is enabled.
    if (g_pcache) g_force_base = PC_IMG_BASE;
    load_elf(prog_host, lm);
    g_loadbase = lm->base;
    *jump = lm->entry;
    *at_base = 0;
    *have_interp = 0;
    const char *interp_host = NULL;
    char interp[256];
    if (elf_interp(prog_host, interp, sizeof interp) == 0) {
        static char ib[4200];
        interp_host = xresolve_overlay(interp, ib, sizeof ib);
        if (g_pcache) g_force_base = PC_INTERP_BASE;
        load_elf(interp_host, li);
        *jump = li->entry;
        *at_base = li->base;
        *have_interp = 1;
    }
    // opt8: key the cache by the identity (dev/ino/size/mtime) of the guest binary AND its interpreter,
    // plus the argv[0] basename -- a multicall binary (busybox) runs a different applet per
    // argv[0], and with the exec re-key each image epoch persists its own arena under its own key.
    if (g_pcache) g_pc_binid = pcache_make_id(prog_host, interp_host, prog);
    return prog;
}

// W3D: fresh per-launch guest run from a loaded image. Allocates a private heap + stack + cpu and
// runs from `jump`. Shared by standalone/cold and warm-worker paths (which restore a
// pristine COW image first, then calls this against the parent-preloaded base). Body is the original
// common execution tail, including calibration and diagnostic output, so standalone
// behavior is byte-identical.
static int run_loaded(int argc, char *const argv[], struct loaded *lm, uint64_t jump, uint64_t at_base) {
    uint64_t heap;
    if (hl_gmap_map_anonymous(0, 256u << 20, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE, HL_HOST_MEMORY_PRIVATE,
                              &heap) != HL_STATUS_OK)
        return 70;
    brk_lo = brk_cur = heap;
    brk_hi = brk_lo + (256u << 20);

    struct cpu c;
    memset(&c, 0, sizeof c);
    c.fpcw = 0x037f; // x87 default control word (round-to-nearest, all exceptions masked, 64-bit precision)
    c.r[RSP] = build_stack(argc, (char **)argv, lm, at_base); // rsp -> argc
    c.r[RDX] = 0;                                             // rtld_fini = 0
    c.rip = jump;

    proc_reg_publish(g_exe_path, argc, argv); // publish this process into the /proc table
    if (g_untrusted) sentry_init();           // fork the host-authority sentry + (optionally) confine the worker
    run_guest(&c);
    if (g_untrusted) sentry_shutdown(); // signal quit + waitpid (reap, no orphan)
    if (g_fast_count)
        fprintf(stderr, "[fastsys] enabled=%d inline-served=%llu\n", g_fastsys, (unsigned long long)g_fast_count);
    if (g_prof)
        fprintf(stderr, "[prof] dispatcher round-trips=%llu  IBTC fills=%llu  (IBTC %s)\n",
                (unsigned long long)g_disp_n, (unsigned long long)g_ibtc_fill, g_noibtc ? "OFF" : "ON");
    return c.exit_code;
}

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argument_count,
                       char *const argv[]) {
    int argc;
    g_engine_result_status = HL_STATUS_OK;
    if (argument_count > (uint32_t)INT_MAX) return 2;
    argc = (int)argument_count;
    hl_target_services_inject(&g_target_services, host);
    hl_gmap_bind_host(host);
    g_linux_box = box;
    jit_guest_bus_bind(hl_linux_bus_fault, hl_linux_bus_active(), hl_linux_bus_generation());
    hl_linux_bus_set_change_callback(jit_guest_bus_changed, NULL);
    hl_linux_file_events_set_callback(bound_mapping_journal_apply, NULL);
    hl_linux_bus_set_transition_callbacks(jit_guest_bus_transition_begin, jit_guest_bus_transition_end, NULL);
    g_host_launch_monotonic_ns = 0;
    if (host != NULL) {
        hl_host_result now;
        if (hl_host_services_validate(host, HL_HOST_CAP_CLOCK) != HL_STATUS_OK) return 70;
        now = host->clock->monotonic_ns(host->context);
        if (now.status != HL_STATUS_OK) return 70;
        g_host_launch_monotonic_ns = now.value;
    }
    if (bound_shadow_activate() != 0) return 70;
    if (argc < 1 || !argv || !argv[0]) return 2;
    // Persistent translated-code cache: enabled only by the centralized HL_PCACHE option.
    g_coldprof = 0;
    g_pcache = hl_option_get("HL_PCACHE") != NULL;
    if (container_init(rootfs) != 0) return 70;
    int rc = engine_global_init();
    if (rc) return rc;
    // Initial-exec shebang handling -- mirror of linux_aarch64.c (and execve case 221) via the shared
    // resolve_shebang_chain(). The container entry may itself be a "#!" script (redis/postgres'
    // docker-entrypoint.sh), and that script's interpreter may ITSELF be a "#!" script (nested, Linux
    // binfmt_script). load_elf has no ELF-magic/#! check, so without this it parses the script text as a
    // bogus ELF (e_machine garbage) and faults before any guest syscall runs. Resolve the whole chain,
    // rewriting argv to [finalInterp, ..., scriptpath, args...] and loading the FINAL interpreter. A
    // non-shebang ELF entry falls straight through unchanged (argc/argv untouched -> byte-identical).
    static char sb_gb[1024], sb_pb[4200], sb_fhb[4200];
    static char sb_store[SHEBANG_MAX * 2][256];
    static char *sb_argv[256];
    const char *sb_prog = find_in_path(argv[0], sb_gb, sizeof sb_gb); // bare "sh" -> "/bin/sh" via PATH
    set_guest_comm(sb_prog); // Linux comm = basename of the exec NAME (stays the script's for a shebang entry)
    const char *sb_prog_host = xresolve_overlay(sb_prog, sb_pb, sizeof sb_pb);
    int sb_argc = 0;
    sb_argv[sb_argc++] = (char *)sb_prog;
    for (int i = 1; i < argc && sb_argc < 255; i++)
        sb_argv[sb_argc++] = (char *)argv[i];
    sb_argv[sb_argc] = NULL;
    const char *sb_finalhost;
    int sb_new =
        resolve_shebang_chain(sb_argv, sb_argc, 256, sb_prog_host, sb_store, sb_fhb, sizeof sb_fhb, &sb_finalhost);
    if (sb_new < 0) {
        fprintf(stderr, "hl-engine: too many nested #! interpreters (ELOOP): %s\n", argv[0]);
        return 40; // ELOOP
    }
    if (sb_new != sb_argc) { // a shebang chain resolved -> run the final interpreter, not the script
        argc = sb_new;
        argv = (char *const *)sb_argv;
    }
    struct loaded lm, li;
    uint64_t jump, at_base;
    int have_interp;
    /* Calibration selects emitted code, so it must precede cache identity
       construction and lookup. */
    s1_calibrate();
    load_program(argv[0], &lm, &li, &jump, &at_base, &have_interp); // (sets g_pc_binid + fixed bases when g_pcache)
    if (g_pcache) {
        g_pc_entry = jump;
        int hit = pcache_load(jump); // graceful MISS on any stale/corrupt/truncated cache -> translate fresh
        if (g_coldprof) fprintf(stderr, "[pcache] %s reloc=%d\n", hit ? "HIT (translation skipped)" : "MISS", g_nreloc);
        if (hl_fatal_status(&g_jit_fatal) != HL_STATUS_OK) {
            g_engine_result_status = hl_fatal_status(&g_jit_fatal);
            pcache_directory_close();
            return 70;
        }
    }
    int ec = run_loaded(argc, argv, &lm, jump, at_base);
    if (hl_fatal_status(&g_jit_fatal) == HL_STATUS_OK)
        pcache_save(); // exit via syscall 93 returns here; syscall 94 saves before _exit (idempotent atomic rename)
    if (hl_fatal_status(&g_jit_fatal) != HL_STATUS_OK) {
        g_engine_result_status = hl_fatal_status(&g_jit_fatal);
        ec = 70;
    }
    pcache_directory_close();
    return ec;
}

// resident engine fork server (server/client/worker), shared with the AArch64 target and driven
// through the container-init/engine-init/load/run seam defined above.
// x86-only knobs: the warm re-run must re-point g_loadbase, and the x86 container model chdir()s the
// engine process into the rootfs (container_init does; the warm path must match it per request).
#define FSRV_SET_LOADBASE(b) (g_loadbase = (b))
#define FSRV_WARM_CHDIR_ROOTFS()                                                                                       \
    do {                                                                                                               \
        if (g_rootfs) {                                                                                                \
            if (chdir(g_rootfs)) {}                                                                                    \
        }                                                                                                              \
    } while (0)
#include "../../linux_abi/fork.c"

void hl_target_runtime_init(void) {
    futex_table_init();
    jit86_install_sync_fault_guards();
    poslk_init();
    ipc_init();
    eventfd_count_init();
    fdvis_init();
    ts_init();
}

// The engine entry point uses the public HL prefix so the runtime can be linked as a library and launched
// by an in-process fork()+call; the thin `main` shim below keeps the standalone binary (used by the test
// harness) launching identically.
int hl_engine_entry(int argc, char **argv);

static int hl_standalone_run(const char *rootfs, uint32_t argc, char *const argv[], const hl_options *options,
                             const char *result_path) {
    return hl_native_engine_run(HL_GUEST_ISA_X86_64, rootfs, argc, argv, options, result_path);
}
#ifndef HL_ENGINE_NO_MAIN
int main(int argc, char **argv) {
    return hl_engine_entry(argc, argv);
}
#endif
int hl_engine_entry(int argc, char **argv) {
    hl_cli_route route = hl_cli_route_parse(argc, argv);
    int ai = 1;
    const char *rootfs = NULL;
    static char self[4200];
    hl_option_reset();
    if (realpath(argv[0], self))
        g_self_path = self;
    else
        g_self_path = argv[0];
    // Final-product launch: the host provides one serialized, validated HL config file.
    if (route.mode == HL_CLI_CONFIG) return hl_run_config_file_with(route.config_path, hl_standalone_run);
    // W3D fork-server dispatch (gated; standalone path untouched when neither flag is present):
    //   --server SOCK [--rootfs DIR] [--prewarm PROG] : run the resident engine server
    //   --client SOCK [--rootfs DIR] PROG [args...]   : forward a launch request to that server
    if (route.mode == HL_CLI_SERVER) return hl_server_main(argc, argv);
    if (route.mode == HL_CLI_CLIENT) return hl_client_main(argc, argv);
    while (ai + 1 < argc) { // --rootfs DIR / --vol guest:host (repeatable)
        if (strcmp(argv[ai], "--rootfs") == 0) {
            rootfs = argv[ai + 1];
            ai += 2;
        } else if (strcmp(argv[ai], "--vol") == 0) {
            add_vol(argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--publish") == 0 || strcmp(argv[ai], "-p") == 0) { // docker -p H:C (port-map)
            parse_publish(argv[ai + 1]);
            hl_option_set("HL_PUBLISH", argv[ai + 1], 1);
            ai += 2;
        } else if (strcmp(argv[ai], "--lower") == 0) {
            add_lower(argv[ai + 1]);
            ai += 2;
        } // overlay read-only layer
        else if (strcmp(argv[ai], "--hostname") == 0) { // docker --hostname -> uname/gethostname + /etc/hostname
            strncpy(g_hostname, argv[ai + 1], 64);
            g_hostname[64] = 0;
            ai += 2;
        } else if (strcmp(argv[ai], "--mem-max") == 0) { // docker --memory -> brk/mmap charge + /proc reporting
            g_mem_max = parse_size(argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--pids-max") == 0) { // docker --pids-limit -> pids.max reporting
            g_pids_max = hl_parse_id("--pids-max", argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--uid") == 0) { // docker --user uid (USER-ns uid); else container default 0
            g_uid = hl_parse_id("--uid", argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--gid") == 0) {
            g_gid = hl_parse_id("--gid", argv[ai + 1]);
            ai += 2;
        } else
            break;
    }
    if (ai >= argc) {
        fprintf(stderr, "usage: %s [--rootfs DIR] [--vol guest:host]... [-p H:C]... <x86-64-elf> [args...]\n", argv[0]);
        return 2;
    }
    return hl_standalone_run(rootfs, (uint32_t)(argc - ai), argv + ai, NULL, NULL);
}
