#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct options {
    const char *label;
    const char *host_os;
    const char *host_release;
    const char *host_arch;
    unsigned long warmups;
    unsigned long samples;
    unsigned long max_cold_us;
    unsigned long max_p99_us;
    int expected_exit;
    char **command;
};

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

/* Nearest-rank percentile.  This deliberately selects an observed sample. */
static unsigned long percentile_index(unsigned long count, unsigned long percentile) {
    return (count * percentile + 99UL) / 100UL - 1UL;
}

static int parse_ulong(const char *text, unsigned long minimum, unsigned long maximum, unsigned long *result) {
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum) return -1;
    *result = value;
    return 0;
}

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --label NAME [--host-os NAME] [--host-release NAME] [--host-arch NAME] "
            "[--warmups N] [--samples N] [--expect CODE] [--max-cold-us N] [--max-p99-us N] "
            "-- COMMAND [ARG ...]\n",
            program);
}

static int parse_options(int argc, char **argv, struct options *options) {
    int i;
    options->label = NULL;
    options->host_os = NULL;
    options->host_release = NULL;
    options->host_arch = NULL;
    options->warmups = 3;
    options->samples = 25;
    options->max_cold_us = 0;
    options->max_p99_us = 0;
    options->expected_exit = 0;
    options->command = NULL;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            if (++i == argc) return -1;
            options->command = &argv[i];
            break;
        }
        if (i + 1 == argc) return -1;
        if (strcmp(argv[i], "--label") == 0) {
            options->label = argv[++i];
        } else if (strcmp(argv[i], "--host-os") == 0) {
            options->host_os = argv[++i];
        } else if (strcmp(argv[i], "--host-release") == 0) {
            options->host_release = argv[++i];
        } else if (strcmp(argv[i], "--host-arch") == 0) {
            options->host_arch = argv[++i];
        } else if (strcmp(argv[i], "--warmups") == 0) {
            if (parse_ulong(argv[++i], 0, 10000, &options->warmups) != 0) return -1;
        } else if (strcmp(argv[i], "--samples") == 0) {
            if (parse_ulong(argv[++i], 3, 10000, &options->samples) != 0) return -1;
        } else if (strcmp(argv[i], "--expect") == 0) {
            unsigned long value;
            if (parse_ulong(argv[++i], 0, 255, &value) != 0) return -1;
            options->expected_exit = (int)value;
        } else if (strcmp(argv[i], "--max-cold-us") == 0) {
            if (parse_ulong(argv[++i], 1, ULONG_MAX / 1000UL, &options->max_cold_us) != 0) return -1;
        } else if (strcmp(argv[i], "--max-p99-us") == 0) {
            if (parse_ulong(argv[++i], 1, ULONG_MAX / 1000UL, &options->max_p99_us) != 0) return -1;
        } else {
            return -1;
        }
    }
    return options->label != NULL && options->label[0] != '\0' && options->command != NULL ? 0 : -1;
}

static int run_once(char *const command[], int expected_exit, uint64_t *elapsed) {
    uint64_t start;
    uint64_t end;
    pid_t child;
    pid_t waited;
    int status = 0;

    start = monotonic_ns();
    if (start == 0) return -1;
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            if (null_fd != STDOUT_FILENO) close(null_fd);
        }
        execvp(command[0], command);
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    end = monotonic_ns();
    if (waited != child || end == 0 || end < start) return -1;
    *elapsed = end - start;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit) {
        if (WIFEXITED(status)) {
            fprintf(stderr, "perf-runner: %s exited %d, expected %d\n", command[0], WEXITSTATUS(status), expected_exit);
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "perf-runner: %s terminated by signal %d\n", command[0], WTERMSIG(status));
        }
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct options options;
    struct utsname host;
    uint64_t *values;
    uint64_t cold;
    uint64_t ignored;
    long double sum = 0.0L;
    unsigned long i;
    uint64_t p99;

    if (parse_options(argc, argv, &options) != 0) {
        usage(argv[0]);
        return 2;
    }
    values = calloc(options.samples, sizeof(*values));
    if (values == NULL) return 1;
    if (run_once(options.command, options.expected_exit, &cold) != 0) goto failed;
    for (i = 0; i < options.warmups; ++i) {
        if (run_once(options.command, options.expected_exit, &ignored) != 0) goto failed;
    }
    for (i = 0; i < options.samples; ++i) {
        if (run_once(options.command, options.expected_exit, &values[i]) != 0) goto failed;
        sum += (long double)values[i];
    }
    qsort(values, options.samples, sizeof(*values), compare_u64);
    p99 = values[percentile_index(options.samples, 99)];
    if (uname(&host) != 0) {
        (void)strcpy(host.sysname, "unknown");
        (void)strcpy(host.release, "unknown");
        (void)strcpy(host.machine, "unknown");
    }
    if (options.host_os != NULL) (void)snprintf(host.sysname, sizeof(host.sysname), "%s", options.host_os);
    if (options.host_release != NULL) (void)snprintf(host.release, sizeof(host.release), "%s", options.host_release);
    if (options.host_arch != NULL) (void)snprintf(host.machine, sizeof(host.machine), "%s", options.host_arch);
    printf("metric=%s unit=us host_os=%s host_release=%s host_arch=%s warmups=%lu samples=%lu "
           "cold=%llu min=%llu median=%llu p90=%llu p99=%llu max=%llu mean=%.0Lf command=%s\n",
           options.label, host.sysname, host.release, host.machine, options.warmups, options.samples,
           (unsigned long long)(cold / 1000), (unsigned long long)(values[0] / 1000),
           (unsigned long long)(values[percentile_index(options.samples, 50)] / 1000),
           (unsigned long long)(values[percentile_index(options.samples, 90)] / 1000),
           (unsigned long long)(p99 / 1000),
           (unsigned long long)(values[options.samples - 1] / 1000), sum / (long double)options.samples / 1000.0L,
           options.command[0]);
    if ((options.max_cold_us != 0 && cold > (uint64_t)options.max_cold_us * UINT64_C(1000)) ||
        (options.max_p99_us != 0 && p99 > (uint64_t)options.max_p99_us * UINT64_C(1000))) {
        fprintf(stderr, "perf-runner: metric %s exceeded threshold (cold=%llu/%luus p99=%llu/%luus)\n",
                options.label, (unsigned long long)(cold / 1000), options.max_cold_us,
                (unsigned long long)(p99 / 1000), options.max_p99_us);
        free(values);
        return 1;
    }
    free(values);
    return 0;

failed:
    free(values);
    return 1;
}
