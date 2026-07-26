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

/*
 * Per-case hang detector and its host-backend scale. 20s is calibrated against a JIT host, because until now
 * every supported host CPU had one: on an ARM64 host every case in every suite this runner drives is
 * milliseconds of guest work, so "not finished in 20s" could only mean hung. On an x86_64 host it cannot mean
 * that. Neither guest frontend has an amd64 back end -- both emit ARM64 -- so that host's backend decodes and
 * executes instead of emitting, at roughly 10-50x the cost of the ARM64-host JIT (docs/amd64-host.md section
 * 3). The SIGKILL below would then convert slow-but-correct into a report of a hang, which is both false and
 * indistinguishable from the real thing.
 *
 * So the budget is CASE_TIMEOUT_MS times a scale the caller sets once where the lanes are registered
 * (cmake/Phase3Gates.cmake, through the helper that declares it in cmake/Phase3Compat.cmake), not something
 * inferred here: whether the engine this runner execs interprets is a
 * property of that binary, and guessing it from inside the harness would couple the harness to backend
 * internals and re-tune itself silently whenever they changed. tools/matrix_runner.c carries the same knob,
 * reads the same variable, and validates it the same way -- the two runners drive the same guests and must not
 * disagree about how long a guest is allowed to take.
 *
 * Unset or empty is 1, and at 1 every path below -- including the failure text -- is bit-for-bit what it was
 * before the knob existed. A malformed or out-of-range value is refused rather than rounded back to 1, because
 * a silent fallback presents as a suite full of unexplained timeouts: exactly what this exists to prevent. The
 * ceiling is twice the worst factor docs/amd64-host.md predicts, so a value mistyped in milliseconds is
 * rejected instead of disabling the detector.
 */
enum { CASE_TIMEOUT_MS = 20000, TIMEOUT_SCALE_MAX = 100 };

static unsigned long timeout_scale = 1;

static int load_timeout_scale(void) {
    const char *value = getenv("HL_MATRIX_TIMEOUT_SCALE");
    char *end = NULL;
    unsigned long parsed;
    if (value == NULL || *value == 0) return 0;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    /* strtoul() skips leading blanks and accepts a sign, wrapping "-1" into ULONG_MAX, so the first character
       is inspected directly: a negative or padded scale has to be a rejection, not a huge budget. */
    if (value[0] < '0' || value[0] > '9' || errno != 0 || end == value || *end != 0 || parsed < 1 ||
        parsed > TIMEOUT_SCALE_MAX) {
        fprintf(stderr,
                "linux-matrix: HL_MATRIX_TIMEOUT_SCALE=\"%s\" is not a decimal factor in [1, %d]; refusing to run "
                "rather than silently using the unscaled per-case timeout\n",
                value, TIMEOUT_SCALE_MAX);
        return 1;
    }
    timeout_scale = parsed;
    /* On stdout, in the output of a PASSING run: a green lane with a scaled budget must not read as evidence
       that guest execution is comparably fast, and the run's own log is where a reader will look. An explicit
       x1 announces nothing, because it IS the unscaled run. */
    if (timeout_scale == 1) return 0;
    printf("linux-matrix: per-case timeout scaled x%lu to %llums (HL_MATRIX_TIMEOUT_SCALE); this run tolerates "
           "slow-but-correct guest execution and is NOT evidence of speed comparable to an unscaled lane\n",
           timeout_scale, (unsigned long long)((uint64_t)CASE_TIMEOUT_MS * timeout_scale));
    return 0;
}

/*
 * The stall detector. tools/matrix_runner.c carries the same one, for the same reason and with the same
 * arithmetic; the two runners drive the same guests and must not disagree about when a case is hung. The
 * long-form reasoning lives beside stall_timeout_ms() there. In brief:
 *
 * Scaling the wall-clock budget stopped slow-but-correct being reported as a hang, and left nothing
 * bounding a real one -- at scale 30 this runner's budget is 600s per case, and the production-full lanes
 * are 21 suites of them. Slow and hung differ in kind, not degree: an interpreted guest is 10-50x slower
 * per unit of work and not one bit more idle. So the detector measures PROGRESS -- the case's stdout grew,
 * or something in its process tree consumed CPU -- and kills only when both have been absent for the stall
 * budget.
 *
 * That budget is the UNSCALED per-case budget with a floor of STALL_FLOOR_MS, which makes it >= the wall
 * budget whenever the scale is 1: on an unscaled lane the wall clock always fires first and this code
 * cannot change a verdict. The floor is 6x the longest deliberate sleep in the whole guest corpus (10s),
 * and it is a margin against idleness rather than against speed -- the axis a slower host does not move
 * along.
 */
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

/* User+system clock ticks charged to `root` and its live descendants, or -1 when the host will not say.
   Descent by parent link rather than by process group: a guest is free to call setpgid, and a process that
   leaves the group would otherwise look like a process that stopped running. Unknown counts as progress,
   so missing evidence can never manufacture a hang. */
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
        /* Field 2 is comm: parenthesised, and free to contain spaces and parentheses. The last ')' is the
           only correct place to start parsing. */
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
    /* No portable per-tree CPU accounting, so the detector stays off rather than running on the output
       signal alone -- which would make such a host stricter than it is today. */
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
        /* Sampled once a second: a /proc walk is cheap but not free, and a stall is measured in tens of
           seconds. Unknown CPU (ticks < 0) counts as progress. */
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
    if (stalled)
        /* A distinct verdict from a timeout, and it must read as one: the case did not run out of time, it
           stopped doing anything at all while time remained. */
        fprintf(stderr,
                "%s: HUNG -- no stdout and no CPU in its process tree for %llums, with the %llums per-case budget "
                "unexpired (this is a hang, not slow execution)\n",
                guest, (unsigned long long)stall_budget, (unsigned long long)budget);
    else if (timed_out) {
        /* Naming the budget matters only when it is not the built-in one; at scale 1 the text stays verbatim so
           an unscaled lane's failure output is unchanged by the existence of the knob. */
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

/*
 * Whole-suite sweep.
 *
 * This used to return on the FIRST failing case, unlike tools/matrix_runner.c, which records a failure and
 * carries on. The production-full-{aarch64,x86_64} lanes are 21 suites each of exactly this runner, so one
 * broken case anywhere in a suite reduced that suite's entire report to a single line -- and a lane whose
 * job is to measure a corpus cannot be allowed to stop measuring at the first thing it finds. Nothing
 * about the VERDICT changes: any failure still exits non-zero. Only the amount of evidence does.
 */
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
        /* `excluded-macos` is a PER-ENGINE disposition: the case is skipped only on the Mach-O macOS
           engine (matrix_runner.c). This runner always drives the ELF Linux production engine, so it must
           RUN excluded-macos rows as active -- otherwise Linux would silently lose coverage of them. Every
           other excluded-* disposition drops out on both engines. */
        int macos_only = strcmp(fields[11], "excluded-macos") == 0;
        if ((strncmp(fields[11], "excluded-", 9) == 0 && !macos_only) || !has_token(fields[4], architecture)) {
            excluded++;
            continue;
        }
        if ((strcmp(fields[11], "active") != 0 && !macos_only) || parse_exit(fields[8], &expected_exit) != 0) {
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
    /* The summary reports what RAN, what FAILED and what was skipped, in one line. It previously named
       only the pass count, which was accurate solely because the first failure ended the run -- so a
       reader could not tell a 47-case suite that passed from one that stopped at case 12, and the skip
       counts sat beside a pass count they were not proportional to. */
    printf("linux-matrix: %s: %zu of %zu active %s cases passed, %zu FAILED; %zu skipped (%zu require typed launch, "
           "%zu excluded or other ISA)\n",
           suite_root, passed, passed + failed, architecture, failed, unsupported + excluded, unsupported, excluded);
    /* The summary line is what a reader takes away, and on a scaled host the same case count does not mean what
       it means on an unscaled one. */
    if (timeout_scale != 1)
        printf("linux-matrix: per-case timeout was scaled x%lu; this run's timing is not comparable to an "
               "unscaled lane\n",
               timeout_scale);
    /* `passed == 0` is retained as a failure: a suite that selected nothing at all is a registration bug,
       and it looked identical to a clean sweep before anything counted the cases it did not run. */
    return failed != 0 || passed == 0;
}

int main(int argc, char **argv) {
    int failed = 0;
    /* Before any case runs, so a malformed scale is one line at the top of the log rather than a verdict on a
       suite that was measured against a budget nobody chose. */
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
