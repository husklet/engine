#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int run_one(const char *path) {
    const struct timespec tick = {0, 10000000};
    unsigned int elapsed_ms = 0;
    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    while (elapsed_ms < 5000) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
            fprintf(stderr, "%s: failed status=%d\n", path, status);
            return 1;
        }
        if (result == -1 && errno != EINTR) {
            perror("waitpid");
            return 1;
        }
        nanosleep(&tick, NULL);
        elapsed_ms += 10;
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    fprintf(stderr, "%s: timed out\n", path);
    return 1;
}

int main(int argc, char **argv) {
    int failed = 0;
    int i;
    if (argc < 2) return 2;
    for (i = 1; i < argc; ++i)
        failed |= run_one(argv[i]);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
