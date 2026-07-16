#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "launch.h"

enum { TOTAL_TIMEOUT_SECONDS = 25 };

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
            hl_launch_hygiene();
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
    int status = 0;
    int outcome = run(argv + 1, started.tv_sec + TOTAL_TIMEOUT_SECONDS, &status);
    if (outcome == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (outcome == 1)
        fprintf(stderr, "bridge-runner: timeout after %d seconds\n", TOTAL_TIMEOUT_SECONDS);
    else if (outcome < 0) perror("bridge-runner");
    else if (WIFEXITED(status)) fprintf(stderr, "bridge-runner: command exited %d\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) fprintf(stderr, "bridge-runner: command received signal %d\n", WTERMSIG(status));
    return 1;
}
