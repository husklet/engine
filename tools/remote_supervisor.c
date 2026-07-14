#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t cancelled;
static volatile sig_atomic_t child_changed;

static void cancel_handler(int signal_number) {
    (void)signal_number;
    cancelled = 1;
}

static void child_handler(int signal_number) {
    (void)signal_number;
    child_changed = 1;
}

static void terminate_group(pid_t child) {
    struct timespec pause = {0, 10000000};
    int status;
    (void)kill(-child, SIGTERM);
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) return;
        (void)nanosleep(&pause, NULL);
    }
    (void)kill(-child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

int main(int argc, char **argv) {
    struct sigaction action = {0};
    struct timespec heartbeat = {0, 250000000};
    pid_t child;
    int status = 0;
    if (argc < 2) return 125;
    action.sa_handler = cancel_handler;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    action.sa_handler = child_handler;
    (void)sigaction(SIGCHLD, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    child = fork();
    if (child < 0) return 125;
    if (child == 0) {
        (void)setpgid(0, 0);
        execv(argv[1], &argv[1]);
        _exit(127);
    }
    (void)setpgid(child, child);
    for (;;) {
        child_changed = 0;
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) {
            terminate_group(child);
            return 125;
        }
        if (cancelled || write(STDERR_FILENO, "\036", 1) != 1) {
            terminate_group(child);
            return 124;
        }
        if (!child_changed) (void)nanosleep(&heartbeat, NULL);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        (void)signal(WTERMSIG(status), SIG_DFL);
        (void)raise(WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 125;
}
