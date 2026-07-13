#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int run_guest(const char *bridge, const char *engine, const char *guest, int expected_exit) {
    const struct timespec tick = {0, 10000000};
    unsigned int elapsed_ms = 0;
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        execlp(bridge, bridge, engine, guest, (char *)NULL);
        _exit(127);
    }
    while (elapsed_ms < 30000) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == expected_exit) return 0;
            fprintf(stderr, "%s running %s: expected exit %d, status=%d\n", engine, guest, expected_exit, status);
            return 1;
        }
        if (result < 0 && errno != EINTR) {
            perror("waitpid");
            return 1;
        }
        nanosleep(&tick, NULL);
        elapsed_ms += 10;
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    fprintf(stderr, "%s running %s: timeout\n", engine, guest);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: e2e-runner BRIDGE ENGINE GUEST EXPECTED_EXIT\n");
        return 2;
    }
    return run_guest(argv[1], argv[2], argv[3], atoi(argv[4]));
}
