/* Guest fixture captured MID-SIGNAL-DELIVERY.
 *
 * Every other signal case here checkpoints a process with signals pending or blocked. This one is frozen
 * with a handler frame LIVE on an alternate stack: struct cpu carries alt_sp/alt_size/alt_flags, sig_depth,
 * sig_defer, sig_defer_stack[] and sig_frame_sp[], and nothing exercised any of them across a restore.
 *
 * The handler blocks inside itself until the host publishes the release file, so the capture lands inside it.
 * On restore the handler has to still be on the alternate stack, still hold its own locals, still run under
 * the mask delivery installed -- and its return has to hand control back to the interrupted main flow with
 * main's own state intact. Each of those is a separate line of output, so a failure names which one.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WITNESS_TAG 0xfeedface00000000ull

static const char *g_release;
static volatile sig_atomic_t g_failed;
static volatile unsigned long g_handler_witness;
static volatile sig_atomic_t g_handler_done;
/* HEAP, not .bss. This fixture is non-PIE (hl_guest_pair's default linkage) and an alternate signal stack
 * inside a non-PIE image's .bss is not deliverable today -- the engine faults writing the handler frame to
 * the unbiased address. Reproducer and analysis are in docs/amd64-host-findings.md; that defect is in guest
 * signal delivery, not in checkpoint, so this case does not carry it. */
static char *g_altstack;
#define ALTSTACK_SIZE (1 << 16)

static void fail(const char *why) {
    dprintf(STDERR_FILENO, "handler fixture: %s\n", why);
    g_failed = 1;
}

static void handler(int sig, siginfo_t *info, void *context) {
    /* Handler-frame state the restore has to carry: locals the compiler must keep on the alternate stack. */
    volatile unsigned long tag = WITNESS_TAG | (unsigned long)(unsigned)info->si_value.sival_int;
    volatile unsigned long spins = 0;
    stack_t current;
    sigset_t now;
    (void)context;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(g_release, F_OK) != 0) {
        if (errno != ENOENT) {
            fail("release probe failed");
            return;
        }
        spins++;
    }
    /* --- everything below runs only after the restore --- */
    if (sigaltstack(NULL, &current) != 0 || (current.ss_flags & SS_ONSTACK) == 0)
        fail("not on the alternate stack after restore");
    if (tag != (WITNESS_TAG | (unsigned long)(unsigned)info->si_value.sival_int) || spins == 0)
        fail("handler locals did not survive the restore");
    if (sigprocmask(SIG_BLOCK, NULL, &now) != 0 || !sigismember(&now, sig))
        fail("delivery mask not in force after restore");
    g_handler_witness = spins;
    g_handler_done = 1;
}

static int prepare_output(const char *release) {
    char path[1024];
    int fd;
    if (snprintf(path, sizeof path, "%s.output", release) >= (int)sizeof path) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    if (snprintf(path, sizeof path, "%s.error", release) >= (int)sizeof path) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

int main(int argc, char **argv) {
    /* main's own interrupted state: sigreturn has to restore it, so it is checked after the handler runs. */
    volatile unsigned long resumed = WITNESS_TAG | 0x5a5a5a5aull;
    struct sigaction action;
    stack_t alternate;
    union sigval value = {.sival_int = 0x1234};
    if (argc != 2 || prepare_output(argv[1]) != 0) return 2;
    g_release = argv[1];

    g_altstack = malloc(ALTSTACK_SIZE);
    if (g_altstack == NULL) return 3;
    alternate.ss_sp = g_altstack;
    alternate.ss_size = ALTSTACK_SIZE;
    alternate.ss_flags = 0;
    if (sigaltstack(&alternate, NULL) != 0) return 3;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) return 3;

    if (sigqueue(getpid(), SIGUSR1, value) != 0) return 4;
    /* Linux delivers a self-directed unblocked signal before sigqueue returns; the engine delivers at its
     * next safepoint, so main must offer safepoints rather than assume the handler has already run. Entering
     * the handler suspends this loop, which is then part of the frozen frame -- exactly the point. */
    for (unsigned long spin = 0; spin < 1000000000ul && !g_handler_done; spin++) {
    }

    if (!g_handler_done || g_failed) return 5;
    if (resumed != (WITNESS_TAG | 0x5a5a5a5aull)) {
        fail("interrupted frame did not survive sigreturn");
        return 6;
    }
    if (g_handler_witness == 0) return 7; /* the handler never spun, i.e. it was never the frozen frame */
    {
        stack_t current;
        if (sigaltstack(NULL, &current) != 0 || (current.ss_flags & SS_ONSTACK) != 0 ||
            current.ss_sp != (void *)g_altstack || current.ss_size != ALTSTACK_SIZE)
            return 8; /* the alternate stack registration itself must survive */
    }
    dprintf(STDOUT_FILENO, "HANDLER-RESTORED %lu\n", g_handler_witness);
    return 0;
}
