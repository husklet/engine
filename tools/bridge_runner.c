#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ATTEMPTS = 3, TOTAL_TIMEOUT_SECONDS = 75 };

static void stop(pid_t child) {
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int run(char **arguments, time_t deadline, int *status) {
    struct timespec now, tick = {0, 10000000};
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        int nested_status;
        pid_t command;
        (void)setpgid(0, 0);
        command = fork();
        if (command < 0) _exit(125);
        if (command == 0) {
            long maximum = sysconf(_SC_OPEN_MAX);
            (void)unsetenv("MAKEFLAGS");
            (void)unsetenv("MFLAGS");
            if (maximum < 0 || maximum > 4096) maximum = 4096;
            for (int descriptor = 3; descriptor < maximum; ++descriptor) (void)close(descriptor);
            execvp(arguments[0], arguments);
            _exit(127);
        }
        while (waitpid(command, &nested_status, 0) < 0)
            if (errno != EINTR) _exit(125);
        if (WIFEXITED(nested_status)) _exit(WEXITSTATUS(nested_status));
        if (WIFSIGNALED(nested_status)) {
            (void)signal(WTERMSIG(nested_status), SIG_DFL);
            (void)raise(WTERMSIG(nested_status));
        }
        _exit(125);
    }
    (void)setpgid(child, child);
    for (;;) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) return 0;
        if (waited < 0 && errno != EINTR) { stop(child); return -1; }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { stop(child); return -1; }
        if (now.tv_sec >= deadline) { stop(child); return 1; }
        (void)nanosleep(&tick, NULL);
    }
}

int main(int argc, char **argv) {
    struct timespec started;
    const struct timespec identity_settle = {1, 0};
    if (argc < 3) {
        fprintf(stderr, "usage: bridge-runner BRIDGE COMMAND [ARG...]\n");
        return 2;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return 1;
    (void)nanosleep(&identity_settle, NULL);
    for (int attempt = 1; attempt <= ATTEMPTS; ++attempt) {
        int status = 0;
        int outcome = run(argv + 1, started.tv_sec + TOTAL_TIMEOUT_SECONDS, &status);
        if (outcome == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            if (attempt > 1) fprintf(stderr, "bridge-runner: admitted after %d attempts\n", attempt);
            return 0;
        }
        if (outcome == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 18 && attempt < ATTEMPTS) {
            const struct timespec firewall_backoff = {1, 0};
            fprintf(stderr, "bridge-runner: transient bridge exit 18; retry %d/%d\n", attempt + 1, ATTEMPTS);
            (void)nanosleep(&firewall_backoff, NULL);
            continue;
        }
        if (outcome == 1)
            fprintf(stderr, "bridge-runner: total timeout after %d seconds (%d attempts)\n",
                    TOTAL_TIMEOUT_SECONDS, attempt);
        else if (outcome < 0) perror("bridge-runner");
        else if (WIFEXITED(status)) fprintf(stderr, "bridge-runner: command exited %d\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) fprintf(stderr, "bridge-runner: command received signal %d\n", WTERMSIG(status));
        return 1;
    }
    return 1;
}
