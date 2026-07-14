#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static int same_file(int actual, const char *expected_path) {
    unsigned char actual_buffer[4096], expected_buffer[4096];
    int expected = open(expected_path, O_RDONLY | O_CLOEXEC);
    if (expected < 0 || lseek(actual, 0, SEEK_SET) < 0) {
        if (expected >= 0) close(expected);
        return 0;
    }
    for (;;) {
        ssize_t an = read(actual, actual_buffer, sizeof(actual_buffer));
        ssize_t en = read(expected, expected_buffer, sizeof(expected_buffer));
        if (an < 0 && errno == EINTR) continue;
        if (en < 0 && errno == EINTR) continue;
        if (an < 0 || en < 0 || an != en || (an > 0 && memcmp(actual_buffer, expected_buffer, (size_t)an) != 0)) {
            close(expected);
            return 0;
        }
        if (an == 0) {
            close(expected);
            return 1;
        }
    }
}

static int run_case(const char *engine, const char *guest, const char *golden, int expected_exit) {
    const struct timespec tick = {0, 10000000};
    char temporary[] = "/tmp/hl-linux-matrix-XXXXXX";
    int output = mkstemp(temporary);
    int status = 0, timed_out = 0;
    pid_t child;
    uint64_t start;
    if (output < 0 || unlink(temporary) != 0) {
        perror("matrix output");
        if (output >= 0) close(output);
        return 1;
    }
    child = fork();
    if (child < 0) {
        perror("fork");
        close(output);
        return 1;
    }
    if (child == 0) {
        (void)setpgid(0, 0);
        if (dup2(output, STDOUT_FILENO) < 0) _exit(126);
        close(output);
        execl(engine, engine, guest, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(child, child);
    start = milliseconds();
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) {
            perror("waitpid");
            close(output);
            return 1;
        }
        if (milliseconds() - start >= UINT64_C(20000)) {
            timed_out = 1;
            (void)kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        (void)nanosleep(&tick, NULL);
    }
    if (timed_out) fprintf(stderr, "%s: timed out\n", guest);
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit)
        fprintf(stderr, "%s: exit mismatch status=%d expected=%d\n", guest, status, expected_exit);
    else if (!same_file(output, golden))
        fprintf(stderr, "%s: stdout differs from %s\n", guest, golden);
    else {
        printf("PASS %s\n", guest);
        close(output);
        return 0;
    }
    close(output);
    return 1;
}

int main(int argc, char **argv) {
    int failed = 0;
    if (argc < 5 || (argc - 2) % 3 != 0) {
        fprintf(stderr, "usage: %s ENGINE GUEST GOLDEN EXIT [GUEST GOLDEN EXIT ...]\n", argv[0]);
        return 2;
    }
    for (int index = 2; index < argc; index += 3) {
        char *end = NULL;
        long expected = strtol(argv[index + 2], &end, 10);
        if (end == argv[index + 2] || *end != '\0' || expected < 0 || expected > 255) return 2;
        failed |= run_case(argv[1], argv[index], argv[index + 1], (int)expected);
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
