#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
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

/* Per-case hang detector and its host-backend scale; tools/matrix_runner.c is the reference, docs/ci-green.md
   the reasoning, and the knob is identical in all six runners. 20s assumes a JIT host; without one the SIGKILL
   below reports slow-but-correct as a hang. Scale 1 is bit-for-bit the unscaled runner, failure text
   included. */
enum { CASE_TIMEOUT_MS = 20000, TIMEOUT_SCALE_MAX = 100 };

static unsigned long timeout_scale = 1;

static int load_timeout_scale(void) {
    const char *value = getenv("HL_MATRIX_TIMEOUT_SCALE");
    char *end = NULL;
    unsigned long parsed;
    if (value == NULL || *value == 0) return 0;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    /* strtoul() accepts a sign, wrapping "-1" to ULONG_MAX, so the first character is checked directly. */
    if (value[0] < '0' || value[0] > '9' || errno != 0 || end == value || *end != 0 || parsed < 1 ||
        parsed > TIMEOUT_SCALE_MAX) {
        fprintf(stderr,
                "linux-matrix: HL_MATRIX_TIMEOUT_SCALE=\"%s\" is not a decimal factor in [1, %d]; refusing to run "
                "rather than silently using the unscaled per-case timeout\n",
                value, TIMEOUT_SCALE_MAX);
        return 1;
    }
    timeout_scale = parsed;
    /* On stdout, in a PASSING run: a green scaled lane must not read as evidence of comparable speed. */
    if (timeout_scale == 1) return 0;
    printf("linux-matrix: per-case timeout scaled x%lu to %llums (HL_MATRIX_TIMEOUT_SCALE); this run tolerates "
           "slow-but-correct guest execution and is NOT evidence of speed comparable to an unscaled lane\n",
           timeout_scale, (unsigned long long)((uint64_t)CASE_TIMEOUT_MS * timeout_scale));
    return 0;
}

/* The stall detector; same one, same arithmetic, as tools/matrix_runner.c. The budget is the UNSCALED
   per-case budget floored at STALL_FLOOR_MS, so at scale 1 it is >= the wall budget and cannot fire. */
enum { STALL_FLOOR_MS = 60000, STALL_SAMPLE_MS = 1000, STALL_PROCESS_MAX = 4096 };

static uint64_t stall_timeout_ms(void) {
    uint64_t budget = CASE_TIMEOUT_MS;
    if (budget < STALL_FLOOR_MS) budget = STALL_FLOOR_MS;
    return budget >= (uint64_t)CASE_TIMEOUT_MS * timeout_scale ? 0 : budget;
}

#if defined(__linux__)
typedef struct process_row {
    long pid;
    long parent;
    unsigned long long ticks;
    int descends;
} process_row;

/* User+system ticks for `root` and its live descendants, or -1 when the host will not say. Descent by parent
   link, not process group: a guest may setpgid. Unknown counts as progress. */
static long long tree_cpu_ticks(long root) {
    static process_row rows[STALL_PROCESS_MAX]; /* Too large for a frame in the per-case wait loop. */
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    size_t count = 0, index;
    long long total = 0;
    unsigned pass;
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char path[64], text[1024], *tail, *end = NULL;
        int descriptor;
        ssize_t got;
        long pid, parent;
        unsigned long long user_ticks, system_ticks;
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
        errno = 0;
        pid = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == NULL || *end != 0) continue;
        if (count == STALL_PROCESS_MAX) {
            (void)closedir(directory);
            return -1;
        }
        if (snprintf(path, sizeof path, "/proc/%ld/stat", pid) >= (int)sizeof path) continue;
        descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) continue;
        got = read(descriptor, text, sizeof text - 1u);
        (void)close(descriptor);
        if (got <= 0) continue;
        text[got] = 0;
        /* Field 2 (comm) may contain spaces and parentheses: parse after the LAST ')'. */
        tail = strrchr(text, ')');
        if (tail == NULL) continue;
        if (sscanf(tail + 1, " %*c %ld %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &parent, &user_ticks,
                   &system_ticks) != 3)
            continue;
        rows[count].pid = pid;
        rows[count].parent = parent;
        rows[count].ticks = user_ticks + system_ticks;
        rows[count].descends = pid == root;
        count++;
    }
    (void)closedir(directory);
    for (pass = 0; pass < 32; ++pass) {
        int changed = 0;
        for (index = 0; index < count; ++index) {
            size_t other;
            if (rows[index].descends) continue;
            for (other = 0; other < count; ++other)
                if (rows[other].descends && rows[other].pid == rows[index].parent) {
                    rows[index].descends = 1;
                    changed = 1;
                    break;
                }
        }
        if (!changed) break;
    }
    for (index = 0; index < count; ++index)
        if (rows[index].descends) total += (long long)rows[index].ticks;
    return total;
}
#else
static long long tree_cpu_ticks(long root) {
    (void)root;
    /* No portable per-tree CPU accounting; off beats output-only, which would be stricter than today. */
    return -1;
}
#endif

static uint64_t output_bytes(int descriptor) {
    struct stat status;
    if (fstat(descriptor, &status) != 0 || status.st_size < 0) return 0;
    return (uint64_t)status.st_size;
}

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
    const uint64_t budget = (uint64_t)CASE_TIMEOUT_MS * timeout_scale;
    const uint64_t stall_budget = stall_timeout_ms();
    char temporary[] = "/tmp/hl-linux-matrix-XXXXXX";
    int output = mkstemp(temporary);
    int status = 0, timed_out = 0, stalled = 0;
    pid_t child;
    uint64_t start, stall_stamp, stall_sampled, stall_bytes = 0;
    long long stall_ticks = -1;
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
    stall_stamp = start;
    stall_sampled = start;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        uint64_t now;
        if (result == child) break;
        if (result < 0 && errno != EINTR) {
            perror("waitpid");
            close(output);
            return 1;
        }
        now = milliseconds();
        if (now - start >= budget) {
            timed_out = 1;
            (void)kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        /* Once a second: the /proc walk is not free. Unknown CPU (ticks < 0) counts as progress. */
        if (stall_budget != 0 && now - stall_sampled >= STALL_SAMPLE_MS) {
            uint64_t bytes = output_bytes(output);
            long long ticks = tree_cpu_ticks((long)child);
            stall_sampled = now;
            if (ticks < 0 || bytes != stall_bytes || ticks != stall_ticks) {
                stall_bytes = bytes;
                stall_ticks = ticks;
                stall_stamp = now;
            } else if (now - stall_stamp >= stall_budget) {
                stalled = 1;
                (void)kill(-child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                break;
            }
        }
        (void)nanosleep(&tick, NULL);
    }
    /* A distinct verdict: the case did not run out of time, it stopped doing anything. */
    if (stalled)
        fprintf(stderr,
                "%s: HUNG -- no stdout and no CPU in its process tree for %llums, with the %llums per-case budget "
                "unexpired (this is a hang, not slow execution)\n",
                guest, (unsigned long long)stall_budget, (unsigned long long)budget);
    else if (timed_out) {
        /* Name the budget only when scaled, so unscaled failure text is unchanged. */
        if (timeout_scale == 1)
            fprintf(stderr, "%s: timed out\n", guest);
        else
            fprintf(stderr, "%s: timed out after %llums (HL_MATRIX_TIMEOUT_SCALE=%lu)\n", guest,
                    (unsigned long long)budget, timeout_scale);
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit)
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

static int has_token(const char *list, const char *token) {
    size_t token_size = strlen(token);
    const char *cursor = list;
    while ((cursor = strstr(cursor, token)) != NULL) {
        if ((cursor == list || cursor[-1] == ',') && (cursor[token_size] == 0 || cursor[token_size] == ',')) return 1;
        cursor += token_size;
    }
    return 0;
}

static int parse_exit(const char *text, int *value) {
    char *end = NULL;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || parsed < 0 || parsed > 255) return 1;
    *value = (int)parsed;
    return 0;
}

/* Whole-suite sweep: record a failing case and continue, as tools/matrix_runner.c does. Returning on the
   first failure reduced a 21-suite lane's report to one line; any failure still exits non-zero. */
static int run_suite(const char *engine, const char *binary_root, const char *suite_root) {
    char manifest[1024], *line = NULL;
    const char *architecture = strrchr(binary_root, '/');
    size_t capacity = 0, passed = 0, failed = 0, unsupported = 0, excluded = 0;
    ssize_t length;
    FILE *file;
    architecture = architecture == NULL ? binary_root : architecture + 1;
    if (strcmp(architecture, "aarch64") != 0 && strcmp(architecture, "x86_64") != 0) {
        fprintf(stderr, "linux-matrix: binary root must end in aarch64 or x86_64: %s\n", binary_root);
        return 1;
    }
    if (snprintf(manifest, sizeof(manifest), "%s/manifest.tsv", suite_root) >= (int)sizeof(manifest) ||
        (file = fopen(manifest, "r")) == NULL) {
        perror("linux matrix manifest");
        return 1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *fields[13], *cursor;
        size_t count = 0, source_size;
        int expected_exit;
        char guest[1024], golden[1024], binary[512];
        if (length == 0 || line[0] == '#') continue;
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = 0;
        cursor = line;
        while (count < 13) {
            fields[count++] = cursor;
            cursor = strchr(cursor, '\t');
            if (cursor == NULL) break;
            *cursor++ = 0;
        }
        if (cursor != NULL || (count != 7 && count != 13)) {
            fprintf(stderr, "linux-matrix: invalid manifest %s\n", manifest);
            free(line);
            fclose(file);
            return 1;
        }
        if (count == 7) {
            source_size = strlen(fields[0]);
            if (!has_token(fields[2], architecture)) {
                excluded++;
                continue;
            }
            if (parse_exit(fields[3], &expected_exit) != 0 || source_size < 3 ||
                strcmp(fields[0] + source_size - 2, ".c") != 0 || source_size - 2 >= sizeof(binary)) {
                fprintf(stderr, "linux-matrix: invalid legacy row %s\n", fields[0]);
                free(line);
                fclose(file);
                return 1;
            }
            memcpy(binary, fields[0], source_size - 2);
            binary[source_size - 2] = 0;
            if (snprintf(guest, sizeof(guest), "%s/%s", binary_root, binary) >= (int)sizeof(guest) ||
                snprintf(golden, sizeof(golden), "%s/%s", suite_root, fields[4]) >= (int)sizeof(golden)) {
                fprintf(stderr, "linux-matrix: path too long for %s\n", fields[0]);
                free(line);
                fclose(file);
                return 1;
            }
            if (run_case(engine, guest, golden, expected_exit) != 0)
                failed++;
            else
                passed++;
            continue;
        }
        /* `excluded-macos` and `excluded-windows` are PER-ENGINE dispositions: each is skipped only on the
           engine whose object format it names (tools/matrix_runner.c sniffs that format). This runner
           always drives the ELF Linux production engine, so it must RUN both as active -- otherwise Linux
           would silently lose coverage of them. Every other excluded-* disposition drops out everywhere.

           Field 12 holds exactly ONE token, so "excluded on macOS AND on Windows" is unspellable today. A
           comma is rejected rather than mis-parsed: `excluded-macos,excluded-windows` matches neither
           per-engine arm, falls into the generic `excluded-` arm, and would be skipped HERE too, silently
           deleting the Linux coverage the per-engine mechanism exists to keep. Widening the column to a
           comma-separated set is a small change here and in matrix_runner.c, to be made the day a row needs
           it; until then this error is the guard. */
        if (strchr(fields[11], ',') != NULL) {
            fprintf(stderr,
                    "linux-matrix: %s: disposition `%s` holds more than one token; column 12 is a single "
                    "token and a comma would silently skip this case on the Linux engine too\n",
                    fields[0], fields[11]);
            free(line);
            fclose(file);
            return 1;
        }
        int host_specific = strcmp(fields[11], "excluded-macos") == 0 || strcmp(fields[11], "excluded-windows") == 0;
        if ((strncmp(fields[11], "excluded-", 9) == 0 && !host_specific) || !has_token(fields[4], architecture)) {
            excluded++;
            continue;
        }
        if ((strcmp(fields[11], "active") != 0 && !host_specific) || parse_exit(fields[8], &expected_exit) != 0) {
            fprintf(stderr, "linux-matrix: invalid active row %s\n", fields[0]);
            free(line);
            fclose(file);
            return 1;
        }
        /* Typed launch data is covered only after the Linux config path is production-safe. */
        if (strcmp(fields[6], "-") != 0 || strcmp(fields[7], "-") != 0 || strstr(fields[10], "-rootfs") != NULL) {
            unsupported++;
            continue;
        }
        source_size = strlen(fields[2]);
        if (source_size >= 3 && strcmp(fields[2] + source_size - 2, ".c") == 0) {
            if (source_size - 2 >= sizeof(binary)) {
                fprintf(stderr, "linux-matrix: source path too long %s\n", fields[2]);
                free(line);
                fclose(file);
                return 1;
            }
            memcpy(binary, fields[2], source_size - 2);
            binary[source_size - 2] = 0;
        } else if (source_size != 0 && source_size < sizeof(binary)) {
            /* A source without a .c suffix names the built artifact directly -- a prebuilt
               corpus binary, or a fixture the build produces (e.g. a symlink). Mirrors
               matrix_runner.c, which strips .c when present and otherwise uses the name. */
            memcpy(binary, fields[2], source_size + 1);
        } else {
            fprintf(stderr, "linux-matrix: invalid source %s\n", fields[2]);
            free(line);
            fclose(file);
            return 1;
        }
        if (snprintf(guest, sizeof(guest), "%s/%s", binary_root, binary) >= (int)sizeof(guest) ||
            snprintf(golden, sizeof(golden), "%s/%s", suite_root, fields[9]) >= (int)sizeof(golden)) {
            fprintf(stderr, "linux-matrix: path too long for %s\n", fields[0]);
            free(line);
            fclose(file);
            return 1;
        }
        if (run_case(engine, guest, golden, expected_exit) != 0)
            failed++;
        else
            passed++;
    }
    free(line);
    if (fclose(file) != 0) return 1;
    /* What RAN, FAILED and was skipped: a pass count alone cannot say where a suite stopped. */
    printf("linux-matrix: %s: %zu of %zu active %s cases passed, %zu FAILED; %zu skipped (%zu require typed launch, "
           "%zu excluded or other ISA)\n",
           suite_root, passed, passed + failed, architecture, failed, unsupported + excluded, unsupported, excluded);
    /* Scaled counts are not comparable to an unscaled lane. */
    if (timeout_scale != 1)
        printf("linux-matrix: per-case timeout was scaled x%lu; this run's timing is not comparable to an "
               "unscaled lane\n",
               timeout_scale);
    /* `passed == 0` stays a failure: a suite that selected nothing is a registration bug. */
    return failed != 0 || passed == 0;
}

int main(int argc, char **argv) {
    int failed = 0;
    /* Before any case runs, so a malformed scale is one line at the top, not a verdict. */
    if (load_timeout_scale() != 0) return 2;
    if (argc == 5 && strcmp(argv[1], "--suite") == 0)
        return run_suite(argv[2], argv[3], argv[4]) ? EXIT_FAILURE : EXIT_SUCCESS;
    if (argc < 5 || (argc - 2) % 3 != 0) {
        fprintf(stderr,
                "usage: %s ENGINE GUEST GOLDEN EXIT [GUEST GOLDEN EXIT ...]\n"
                "       %s --suite ENGINE BIN_ROOT SUITE_ROOT\n",
                argv[0], argv[0]);
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
