#ifndef HL_LINUX_ABI_HOST_WAIT_H
#define HL_LINUX_ABI_HOST_WAIT_H

/*
 * <sys/wait.h> for this layer.  Same construction and the same REAL/SHAPE/
 * REFUSAL labelling as host_mman.h and host_poll.h.
 *
 * The split here is unusually sharp, and it is the reason this file is worth
 * reading rather than skimming:
 *
 *   THE MACROS ARE SHAPE, AND THEY ARE REAL WORK.  WIFEXITED and the rest are
 *   not thin wrappers over a host call -- they are the DECODER for a wait
 *   status word, and on this layer that word is very often one this layer
 *   SYNTHESIZED rather than one a host handed back.  syscall/proc.c's wait4
 *   rewrites the low byte of a reaped status into Linux's termsig encoding,
 *   sets the 0x80 core bit from the guest's own RLIMIT_CORE, substitutes the
 *   0xffff continued sentinel, and reconstructs a signalled status out of a
 *   child that actually _exit()ed (the signal-death relay).  Every one of those
 *   steps writes Linux's encoding by hand, and the guest then decodes it with
 *   its own libc's macros.
 *
 *   So these macros MUST carry Linux's encoding and not a host one:
 *
 *       bits 0-6   termination signal (0 => exited)
 *       bit  7     core dumped
 *       bits 8-15  exit code, or the stop signal when bits 0-7 are 0x7f
 *       0x7f       in the low byte: stopped
 *       0xffff     whole word: continued
 *
 *   A host-shaped variant would be a silent data bug of the worst kind -- it
 *   would compile, run, and disagree with the guest about whether its child
 *   died -- and there is no host-shaped variant to reach for anyway, because
 *   Windows has no wait status word at all.  A process exit code here is a
 *   32-bit DWORD from GetExitCodeProcess with no signal, no stop, and no core
 *   bit in it.
 *
 *   THE CALLS ARE REFUSALS.  Nothing wires Windows process reaping to this
 *   layer yet.  The pieces exist on the host side (a HANDLE, WaitForSingle-
 *   Object, GetExitCodeProcess) and the host seam already models a process exit
 *   as a kind plus a value -- the Linux and macOS host backends fill
 *   process_exit_kind/process_exit_value that way -- but the layer here holds a
 *   pid_t, and on Windows a pid is not a process handle: you cannot wait on one
 *   without first opening it, and nothing in this layer owns that handle table.
 *   That is the same population-(2) gap host_mman.h names for descriptors, one
 *   namespace over.
 *
 *   What a refusal must NOT be here is 0 or the requested pid.  waitpid()
 *   returning 0 means "WNOHANG, nothing to report", which turns every reaper
 *   loop into a spin; returning the pid with an uninitialised status word hands
 *   the guest a fabricated death.  ECHILD is nearly as bad as either -- it is
 *   the specific, load-bearing answer "you have no such child", which a shell
 *   or a runtime treats as authoritative.  ENOSYS it is.
 */

#if !defined(_WIN32)

#include <sys/wait.h>

#else /* Windows */

/* Two sibling seams, both pulled in so this header stands on its own rather
 * than only after the unity TU's force-include has run:
 *
 *   host_signal.h -- siginfo_t, which waitid reports through, and the CLD_*
 *                    codes its si_code carries.
 *   host_proc.h   -- struct rusage and id_t, which wait3/wait4/waitid take.
 *                    Owning them there rather than here keeps ONE definition of
 *                    each; <sys/wait.h> gets them from elsewhere on a POSIX
 *                    host too. */
#include "host_proc.h"
#include "host_signal.h"

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

/* ---- SHAPE: wait option bits.  Linux values. ----------------------------
 * Deliberately ALL of them, including the __W* thread-selection bits that have
 * no portable form: syscall/proc.c translates the guest's option word bit by
 * bit into the host's, and the mapping table is only readable if every name it
 * mentions exists. */
#define WNOHANG 0x00000001
#define WUNTRACED 0x00000002
#define WSTOPPED WUNTRACED
#define WEXITED 0x00000004
#define WCONTINUED 0x00000008
#define WNOWAIT 0x01000000

#define __WNOTHREAD 0x20000000
#define __WALL 0x40000000
#define __WCLONE 0x80000000

/* ---- SHAPE: the status-word decoders.  Linux's encoding, see above. ------
 * WIFSIGNALED's expression is glibc's: ((signed char)(((status) & 0x7f) + 1) >> 1) > 0
 * is true for a low byte in 1..0x7e and false for both 0 (exited) and 0x7f
 * (stopped), which is exactly the set a plain `!= 0 && != 0x7f` would give but
 * kept in the form the guest's own libc uses, so the two agree by construction
 * on the boundary values. */
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WTERMSIG(status) ((status) & 0x7f)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WCOREDUMP(status) ((status) & 0x80)

#define WIFEXITED(status) (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) (((signed char)(((status) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WIFCONTINUED(status) ((status) == 0xffff)

#define W_EXITCODE(ret, sig) (((ret) << 8) | (sig))
#define W_STOPCODE(sig) (((sig) << 8) | 0x7f)
#define WCOREFLAG 0x80

/* ---- SHAPE: waitid's id selectors.  Linux values. -----------------------
 * P_PIDFD is included because syscall/rare.c's waitid validates the guest's
 * idtype against 0..3 and then RESOLVES a pidfd to a pid before calling the
 * host with P_PID -- so the name is part of the vocabulary even though no host
 * accepts it. */
typedef enum { P_ALL = 0, P_PID = 1, P_PGID = 2, P_PIDFD = 3 } idtype_t;

/* ---- REFUSAL: the reaping calls.  See the header note. ------------------ */

static inline pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid;
    (void)status;
    (void)options;
    errno = ENOSYS;
    return (pid_t)-1;
}

static inline pid_t wait(int *status) {
    (void)status;
    errno = ENOSYS;
    return (pid_t)-1;
}

static inline pid_t wait3(int *status, int options, struct rusage *usage) {
    (void)status;
    (void)options;
    (void)usage;
    errno = ENOSYS;
    return (pid_t)-1;
}

static inline pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    (void)pid;
    (void)status;
    (void)options;
    (void)usage;
    errno = ENOSYS;
    return (pid_t)-1;
}

/* REFUSAL.  waitid additionally reports through a siginfo_t, and leaving that
 * buffer untouched on failure is part of the contract the callers rely on:
 * syscall/rare.c memsets its siginfo before the call and tests si_pid != 0
 * afterwards to decide whether a child was actually reaped, so a refusal reads
 * as "no reap" there and never as a phantom one. */
static inline int waitid(idtype_t type, id_t id, siginfo_t *info, int options) {
    (void)type;
    (void)id;
    (void)info;
    (void)options;
    errno = ENOSYS;
    return -1;
}

#endif /* _WIN32 */

#endif
