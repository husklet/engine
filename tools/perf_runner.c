#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int run_once(const char *bridge, const char *engine, const char *guest, int expected_exit, uint64_t *elapsed) {
    uint64_t start = monotonic_ns();
    pid_t child = fork();
    int status;
    if (child < 0) return 1;
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            close(null_fd);
        }
        execlp(bridge, bridge, engine, guest, (char *)NULL);
        _exit(127);
    }
    do {
        if (waitpid(child, &status, 0) == child) break;
    } while (errno == EINTR);
    *elapsed = monotonic_ns() - start;
    return !(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit);
}

int main(int argc, char **argv) {
    uint64_t *samples;
    uint64_t sum = 0;
    uint64_t cold_ns;
    unsigned long iterations;
    unsigned long i;
    int expected;
    if (argc != 6) {
        fprintf(stderr, "usage: perf-runner BRIDGE ENGINE GUEST EXPECTED_EXIT ITERATIONS\n");
        return 2;
    }
    expected = atoi(argv[4]);
    iterations = strtoul(argv[5], NULL, 10);
    if (iterations < 3 || iterations > 10000) return 2;
    samples = calloc(iterations, sizeof(*samples));
    if (samples == NULL) return 1;
    if (run_once(argv[1], argv[2], argv[3], expected, &cold_ns)) {
        free(samples);
        return 1;
    }
    for (i = 0; i < iterations; ++i) {
        if (run_once(argv[1], argv[2], argv[3], expected, &samples[i])) {
            free(samples);
            return 1;
        }
        sum += samples[i];
    }
    qsort(samples, iterations, sizeof(*samples), compare_u64);
    printf("engine=%s guest=%s iterations=%lu cold_us=%llu min_us=%llu median_us=%llu p95_us=%llu mean_us=%llu\n",
           argv[2], argv[3], iterations, (unsigned long long)(cold_ns / 1000), (unsigned long long)(samples[0] / 1000),
           (unsigned long long)(samples[iterations / 2] / 1000),
           (unsigned long long)(samples[(iterations * 95) / 100] / 1000),
           (unsigned long long)((sum / iterations) / 1000));
    free(samples);
    return 0;
}
