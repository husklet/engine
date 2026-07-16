#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ATTEMPTS = 3, TIMEOUT_SECONDS = 60 };

static void stop(pid_t child) {
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int run(char **arguments, int *status) {
    struct timespec start, now, tick = {0, 10000000};
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        (void)setpgid(0, 0);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    (void)setpgid(child, child);
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) { stop(child); return -1; }
    for (;;) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) return 0;
        if (waited < 0 && errno != EINTR) { stop(child); return -1; }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { stop(child); return -1; }
        if (now.tv_sec - start.tv_sec >= TIMEOUT_SECONDS) { stop(child); return 1; }
        (void)nanosleep(&tick, NULL);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: bridge-runner BRIDGE COMMAND [ARG...]\n");
        return 2;
    }
    for (int attempt = 1; attempt <= ATTEMPTS; ++attempt) {
        int status = 0;
        int outcome = run(argv + 1, &status);
        if (outcome == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
        if (outcome == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 18 && attempt < ATTEMPTS) {
            const struct timespec firewall_backoff = {2, 0};
            fprintf(stderr, "bridge-runner: transient bridge exit 18; retry %d/%d\n", attempt + 1, ATTEMPTS);
            (void)nanosleep(&firewall_backoff, NULL);
            continue;
        }
        if (outcome == 1) fprintf(stderr, "bridge-runner: command timed out after %d seconds\n", TIMEOUT_SECONDS);
        else if (outcome < 0) perror("bridge-runner");
        else if (WIFEXITED(status)) fprintf(stderr, "bridge-runner: command exited %d\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) fprintf(stderr, "bridge-runner: command received signal %d\n", WTERMSIG(status));
        return 1;
    }
    return 1;
}
