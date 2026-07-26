#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/statfs.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "launch.h"

#include "hl/config.h"

/* CASE_MAX bounds the fixed cases[] array in main(). The largest manifest in the tree is
   tests/compat/completeness/manifest.tsv at 167 active rows, so 256 leaves ~90 rows of headroom; the
   overflow path below names the limit rather than blaming the manifest, because a manifest that parses
   perfectly is not a parse error and reporting it as one sends a reader to the wrong file. */
enum { CASE_MAX = 256, FIELD_MAX = 512, OUTPUT_MAX = 1024 * 1024, ERROR_MAX = 64 * 1024, TIMEOUT_MS = 120000 };

/* Per-case hang detector. 120s suits every suite whose cases are milliseconds of
 * work, but the endurance cases are minutes of real compute: soak/reallocchurn
 * measures 124s (aarch64) and 112s (x86_64) pinned to four CPUs, i.e. at or over
 * the cap on a runner-sized machine, while finishing in a fraction of that on a
 * whole host. Raising the constant for everyone would blunt the detector, so the
 * budget is an explicit opt-in the caller sets for the suites that need it.
 * Values outside [1s, 1h] are ignored rather than trusted. */
static uint64_t suite_case_timeout_ms(void) {
    const char *value = getenv("HL_MATRIX_CASE_TIMEOUT_MS");
    char *end = NULL;
    unsigned long parsed;
    if (value == NULL || *value == 0) return TIMEOUT_MS;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != 0 || parsed < 1000 || parsed > 3600000) return TIMEOUT_MS;
    return (uint64_t)parsed;
}

/*
 * Host-backend timeout scale.
 *
 * Every budget above is calibrated against a JIT, because until now every supported host CPU had one, which
 * made "a case that has not finished in 120s is hung" a safe inference. On an x86_64 host it is not: neither
 * guest frontend has an amd64 back end -- both emit ARM64 -- so the backend there decodes and executes rather
 * than emitting, and guest execution costs roughly 10-50x what the ARM64-host JIT costs (docs/amd64-host.md
 * section 3). A correct-but-interpreted case would trip the detector and be killed by the process-group kill
 * below, reporting the one thing that is NOT true -- a hang -- and destroying the output that would have
 * distinguished the two. Slow and hung must not share a diagnostic.
 *
 * The scale is therefore supplied by the caller, once, where the lanes are registered
 * (cmake/Phase3Compat.cmake), and is deliberately NOT detected here. This runner cannot detect it honestly:
 * "is my backend an interpreter" is a property of the engine binary it execs, and reading that out of the
 * engine would couple the harness to backend internals and silently re-tune itself the day those internals
 * change. One environment variable set next to the test registration is greppable and reviewable; an
 * inference made inside the harness is neither.
 *
 * It MULTIPLIES HL_MATRIX_CASE_TIMEOUT_MS rather than replacing it, because the two answer different
 * questions: the absolute value is how much work THIS SUITE does (.github/workflows/linux.yml gives
 * compat-soak 600s), the scale is how much slower THIS HOST executes any of it. Both apply, so both compose.
 *
 * Unset or empty means 1 and every code path below is then bit-for-bit what it was before this existed --
 * including the diagnostic strings, so an unscaled lane's output cannot change at all. Anything else must be
 * decimal digits naming a factor in [1, TIMEOUT_SCALE_MAX], and a value outside that is a hard refusal rather
 * than a fallback to 1: falling back would present as a suite full of unexplained per-case timeouts, which is
 * precisely the diagnostic this exists to prevent. The ceiling is low on purpose -- 100 is twice the worst
 * factor docs/amd64-host.md predicts -- so that a value mistyped in milliseconds (20000) is rejected instead
 * of quietly disabling the hang detector for the rest of the run. A configuration that is slower still for
 * some other reason (a sanitized or -O0 engine) belongs in HL_MATRIX_CASE_TIMEOUT_MS, which is the axis that
 * means "this run does more work".
 */
enum { TIMEOUT_SCALE_MAX = 100 };

static unsigned long timeout_scale = 1;

static uint64_t case_timeout_ms(void) {
    return suite_case_timeout_ms() * timeout_scale;
}

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
                "matrix-runner: HL_MATRIX_TIMEOUT_SCALE=\"%s\" is not a decimal factor in [1, %d]; refusing to "
                "run rather than silently using the unscaled per-case timeout\n",
                value, TIMEOUT_SCALE_MAX);
        return 1;
    }
    timeout_scale = parsed;
    /* Announced on stdout, in the output of a PASSING run: a green lane with a scaled budget must not be
       readable as evidence that guest execution is comparably fast, and the run's own log is the only place a
       reader will look. An explicit x1 announces nothing, because it IS the unscaled run -- so setting the
       variable to 1 by hand is indistinguishable from not setting it, which is the honest reading. */
    if (timeout_scale == 1) return 0;
    printf("matrix-runner: per-case timeout scaled x%lu to %llums (HL_MATRIX_TIMEOUT_SCALE); this run tolerates "
           "slow-but-correct guest execution and is NOT evidence of speed comparable to an unscaled lane\n",
           timeout_scale, (unsigned long long)case_timeout_ms());
    return 0;
}

/*
 * The stall detector: a hang is a lack of PROGRESS, not a lack of time.
 *
 * Scaling the wall-clock budget above fixed the false positive (slow-but-correct reported as a hang) and
 * created a false negative in its place: at HL_MATRIX_TIMEOUT_SCALE=30 the budget is 3600s per case, and
 * compat-soak's HL_MATRIX_CASE_TIMEOUT_MS=600000 makes it 5 HOURS. Nothing bounded a real hang any more,
 * and there was a real one to bound: the x86_64 futex-across-fork cases were observed sitting in
 * do_wait/futex_do_wait indefinitely at ZERO CPU (that engine defect is since fixed; the signature is what
 * the class looks like, and the class is what this detects).
 *
 * A larger or smaller number cannot separate the two cases, because they differ in kind rather than in
 * degree. An interpreted guest is 10-50x slower per unit of work; it is not 10-50x more idle. So measure
 * progress directly and give it its own, much shorter budget:
 *
 *   progress := the guest's captured stdout/stderr grew, OR the case's process tree consumed CPU.
 *
 * Both signals are needed. Output alone would kill a correct case that computes for minutes before
 * printing (soak/reallocchurn does exactly that), and CPU alone would kill a correct case blocked on a
 * timer. Together they are only both absent when nothing in the launch is running or producing anything,
 * which is the definition of the hang this exists to catch. Note that neither signal can be taken from the
 * runner's own pipes: tools/remote_supervisor.c writes a heartbeat byte to its stderr every 250ms, so pipe
 * traffic proves the SUPERVISOR is alive and says nothing about the guest. The capture files are the
 * guest's real output, and the process tree is where its real work happens.
 *
 * The tree is walked by parent-child descent from the runner's own child rather than by process group,
 * because the supervisor deliberately puts the engine in a group of its OWN (setpgid(0,0) in the forked
 * child there) -- a pgid scan finds the supervisor and misses every process that does the work. A
 * descendant that has been reparented away (a deliberately orphaned daemon) is not counted; its output
 * still is. Anything the host will not answer -- a non-Linux host, an unreadable /proc, more processes
 * than the table holds -- reports "unknown", which counts as progress. Missing evidence must never be
 * allowed to manufacture a hang.
 *
 * The budget is DERIVED, not invented: it is the UNSCALED per-case budget (suite_case_timeout_ms(), i.e.
 * 120s, or whatever HL_MATRIX_CASE_TIMEOUT_MS says this suite is worth), with a floor of STALL_FLOOR_MS.
 * Two consequences, both wanted:
 *
 *   * On any lane where the scale is 1 the stall budget is >= the wall budget, so the wall clock always
 *     fires first and the detector is inert BY CONSTRUCTION. The aarch64 and Darwin lanes therefore cannot
 *     change behaviour, and that is a property of the arithmetic rather than a claim about it.
 *   * A suite that says it does more work (compat-soak's 600s) automatically gets a longer stall budget
 *     too. That is the honest coupling: the one knob that means "this suite is big" already means "this
 *     suite may legitimately go quiet for longer". No second environment variable exists to get wrong.
 *
 * Why the floor cannot fire on a correct case: firing needs STALL_FLOOR_MS (60s) in which the whole
 * process tree consumed zero CPU ticks AND wrote zero bytes. The longest deliberate sleep anywhere in the
 * corpus is 10s (the 100s/1000s values in tests/compat/time are timers armed far into the future and read
 * back, never waited on), so the margin is at least 6x, and it is a margin against idleness rather than
 * against speed -- the axis a slower host does not move along.
 */
enum { STALL_FLOOR_MS = 60000, STALL_SAMPLE_MS = 1000, STALL_PROCESS_MAX = 4096 };

static uint64_t stall_timeout_ms(void) {
    uint64_t budget = suite_case_timeout_ms();
    if (budget < STALL_FLOOR_MS) budget = STALL_FLOOR_MS;
    /* Inert whenever the wall clock would fire no later -- the unscaled case. Returning 0 rather than a
       number that can never be reached keeps the sampling out of the poll loop entirely there. */
    return budget >= case_timeout_ms() ? 0 : budget;
}

#if defined(__linux__)
typedef struct process_row {
    long pid;
    long parent;
    unsigned long long ticks;
    int descends;
} process_row;

/* Total user+system clock ticks charged to `root` and its live descendants, or -1 when the host cannot
   answer. Ticks are monotone per process, so a sum that has not moved is proof no counted process ran. */
static long long tree_cpu_ticks(long root) {
    /* static: 4096 rows is far too large for a frame in the per-case poll loop, and the runner is
       single-threaded (one case at a time, by construction of main()). */
    static process_row rows[STALL_PROCESS_MAX];
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    size_t count = 0, index;
    long long total = 0;
    unsigned pass;
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char path[64], text[1024], *tail;
        int descriptor;
        ssize_t got;
        long pid, parent;
        unsigned long long user_ticks, system_ticks;
        char *end = NULL;
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
        errno = 0;
        pid = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == NULL || *end != 0) continue;
        if (count == STALL_PROCESS_MAX) {
            (void)closedir(directory);
            return -1; /* Truncated is unknown, and unknown counts as progress. */
        }
        if (snprintf(path, sizeof path, "/proc/%ld/stat", pid) >= (int)sizeof path) continue;
        descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) continue; /* Exited between readdir and open; its ticks are simply not seen. */
        got = read(descriptor, text, sizeof text - 1u);
        (void)close(descriptor);
        if (got <= 0) continue;
        text[got] = 0;
        /* Field 2 is comm, which is parenthesised and may itself contain spaces and parentheses, so the
           only correct place to start parsing is after the LAST ')'. */
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
    /* Transitive closure over the parent links. /proc is not ordered parent-before-child, so this is
       repeated until it stops changing; the trees here are a handful of levels deep. */
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
    /* No portable per-tree CPU accounting. Reporting "unknown" disables the detector rather than leaving
       it running on the output signal alone, which would make a Darwin lane STRICTER than it is today. */
    return -1;
}
#endif

/*
 * HL_MATRIX_SCRATCH_DIR, and why a silent fallback is a trap.
 *
 * The guest's /tmp is a per-case scratch directory, so the filesystem BENEATH it decides whether memfd
 * seals, statx btime and friends behave as the goldens (captured on Linux) say they must. The variable is
 * exported by .github/workflows/linux.yml and by nothing else, so a developer running `ctest -L
 * compat-syscall` on an ext4 build tree fails syscall/memfd-seals on both ISAs for a reason that has
 * nothing to do with the engine, and the run says nothing about why.
 *
 * It is deliberately NOT set from CMake -- see the note in cmake/Phase3Compat.cmake. Some suites' goldens
 * were captured against the build tree's own filesystem (linux.yml unsets it for compat-core-syscall for
 * exactly that reason), so a build system that forced tmpfs on every suite would break those cases to fix
 * these. Instead the runner records what it actually used and says so WHEN A RUN FAILS -- the one moment
 * the information is worth anything, and the one place it cannot add noise to a green lane.
 */
static char scratch_base_used[1024];
static int scratch_overridden;
static int scratch_override_rejected;
static int scratch_is_tmpfs = -1; /* 1 yes, 0 no, -1 the host will not say */

static void scratch_observe(const char *base, int overridden, int rejected) {
    if (scratch_base_used[0] != 0) return; /* First case only; every case uses the same base. */
    (void)snprintf(scratch_base_used, sizeof scratch_base_used, "%s", base);
    scratch_overridden = overridden;
    scratch_override_rejected = rejected;
#if defined(__linux__)
    {
        struct statfs info;
        /* TMPFS_MAGIC, spelled out rather than pulled from <linux/magic.h> so the tool keeps building
           against a bare libc header set. */
        if (statfs(base, &info) == 0) scratch_is_tmpfs = (unsigned long long)info.f_type == 0x01021994ULL ? 1 : 0;
    }
#endif
}

static void scratch_note(void) {
    if (scratch_override_rejected)
        fprintf(stderr,
                "matrix-runner: NOTE: HL_MATRIX_SCRATCH_DIR was set but is not a writable directory; the guest's "
                "/tmp fell back to %s\n",
                scratch_base_used);
    if (scratch_is_tmpfs != 0 || scratch_overridden) return;
    fprintf(stderr,
            "matrix-runner: NOTE: the guest's /tmp was backed by %s, which is NOT tmpfs. Cases asserting "
            "tmpfs-only behaviour (memfd seals, statx btime) fail on any other filesystem for that reason alone, "
            "on BOTH ISAs. Set HL_MATRIX_SCRATCH_DIR to a tmpfs (e.g. /dev/shm) before reading those failures as "
            "engine defects; CI does this in .github/workflows/linux.yml.\n",
            scratch_base_used);
}

static uint64_t capture_bytes(const char *output_path, const char *error_path) {
    struct stat status;
    uint64_t total = 0;
    if (stat(output_path, &status) == 0 && status.st_size > 0) total += (uint64_t)status.st_size;
    if (stat(error_path, &status) == 0 && status.st_size > 0) total += (uint64_t)status.st_size;
    return total;
}

#ifndef AARCH64_DYNAMIC_LOADER
#define AARCH64_DYNAMIC_LOADER ""
#define AARCH64_DYNAMIC_LIBC ""
#define X86_64_DYNAMIC_LOADER ""
#define X86_64_DYNAMIC_LIBC ""
#endif

typedef enum case_isa { ISA_AARCH64, ISA_X86_64, ISA_BOTH } case_isa;

typedef struct suite_case {
    char name[128];
    char source[256];
    char expected[256];
    char environment[256];
    char argument[256];
    case_isa isa;
    int expected_exit;
    int needs_rootfs;
    int dynamic_rootfs;
    int mapping_data_rootfs;
} suite_case;

typedef struct capture {
    unsigned char *output;
    size_t output_size;
    unsigned char *error;
    size_t error_size;
    int wait_status;
} capture;

typedef struct resource_baseline {
    long descriptors;
    long threads;
} resource_baseline;

static volatile sig_atomic_t interrupted_signal;
static volatile sig_atomic_t active_group;

static void interrupt_runner(int signal_number) {
    sig_atomic_t group = active_group;
    interrupted_signal = signal_number;
    if (group > 0) (void)kill(-(pid_t)group, SIGTERM);
}

static int install_interrupt_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = interrupt_runner;
    if (sigemptyset(&action.sa_mask) != 0) return 1;
    return sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0 ||
           sigaction(SIGHUP, &action, NULL) != 0;
}

static long count_directory_entries(const char *path) {
#if defined(__linux__)
    DIR *directory = opendir(path);
    struct dirent *entry;
    long count = 0;
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
    return closedir(directory) == 0 ? count : -1;
#else
    (void)path;
    return -1;
#endif
}

static resource_baseline resource_measure(void) {
    resource_baseline measured = {count_directory_entries("/proc/self/fd"), count_directory_entries("/proc/self/task")};
    return measured;
}

static int resources_restored(resource_baseline baseline, const suite_case *item) {
    resource_baseline current = resource_measure();
    int child_status;
    pid_t child = waitpid(-1, &child_status, WNOHANG);
    int child_clean = child < 0 && errno == ECHILD;
    int descriptor_clean = baseline.descriptors < 0 || current.descriptors == baseline.descriptors;
    int thread_clean = baseline.threads < 0 || current.threads == baseline.threads;
    if (child_clean && descriptor_clean && thread_clean) return 1;
    if (getenv("GITHUB_ACTIONS") != NULL)
        fprintf(stderr,
                "::error title=Compatibility resource leak (%s)::children=%s descriptors=%ld/%ld "
                "threads=%ld/%ld\n",
                item->name, child_clean ? "clean" : "live", baseline.descriptors, current.descriptors, baseline.threads,
                current.threads);
    fprintf(stderr, "matrix-runner: %s resource leak: children=%s descriptors=%ld/%ld threads=%ld/%ld\n", item->name,
            child_clean ? "clean" : "live", baseline.descriptors, current.descriptors, baseline.threads,
            current.threads);
    return 0;
}

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static int relative_path(const char *path) {
    const char *part = path;
    if (*path == 0 || *path == '/' || strstr(path, "//") != NULL) return 0;
    while ((part = strstr(part, "..")) != NULL) {
        if ((part == path || part[-1] == '/') && (part[2] == 0 || part[2] == '/')) return 0;
        part += 2;
    }
    return 1;
}

static int parse_exit(const char *text, int *value) {
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || *text == 0 || *end != 0 || parsed < 0 || parsed > 255) return 1;
    *value = (int)parsed;
    return 0;
}

static int valid_environment(const char *text) {
    const char *cursor = text;
    if (strcmp(text, "-") == 0) return 1;
    if (*text == 0 || strlen(text) >= sizeof(((suite_case *)0)->environment)) return 0;
    while (*cursor) {
        const char *equals = strchr(cursor, '=');
        const char *end = strchr(cursor, ';');
        const char *name;
        if (end == NULL) end = cursor + strlen(cursor);
        if (equals == NULL || equals == cursor || equals >= end) return 0;
        for (name = cursor; name < equals; ++name)
            if (!((*name >= 'A' && *name <= 'Z') || (*name >= '0' && *name <= '9') || *name == '_')) return 0;
        cursor = *end == ';' ? end + 1 : end;
        if (*end == ';' && *cursor == 0) return 0;
    }
    return 1;
}

/* The compat matrix runs against two production engines: the ELF Linux engine (test-linux-production-typed)
   and the Mach-O macOS engine (e2e-compat). A handful of cases exercise behavior the macOS engine cannot
   emulate (deliberate PROT_NONE non-enforcement, Darwin-absent child-subreaper, netns bridging, deep JIT
   re-translate). Those are marked `excluded-macos` so they are skipped ONLY when the engine binary under
   test is Mach-O, while the Linux engine still runs and enforces them -- no Linux coverage is lost. */
static int engine_is_macho(const char *engine_path) {
    unsigned char magic[4] = {0};
    int fd = open(engine_path, O_RDONLY);
    ssize_t got;
    if (fd < 0) return 0;
    got = read(fd, magic, sizeof(magic));
    (void)close(fd);
    if (got != (ssize_t)sizeof(magic)) return 0;
    /* ELF -> Linux engine; anything else (Mach-O 0xFEEDFACF / fat 0xCAFEBABE) -> treat as macOS. */
    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return 0;
    return 1;
}

static int load_manifest(const char *root, suite_case cases[CASE_MAX], size_t *case_count, size_t *excluded,
                         int host_macho) {
    char path[1024];
    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/manifest.tsv", root) >= (int)sizeof(path)) return 1;
    file = fopen(path, "r");
    if (file == NULL) return 1;
    *case_count = 0;
    *excluded = 0;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        char *fields[13];
        char *cursor;
        size_t field_count = 0;
        if (size == 0 || line[0] == '#') continue;
        while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r'))
            line[--size] = 0;
        cursor = line;
        while (field_count < 13) {
            fields[field_count++] = cursor;
            cursor = strchr(cursor, '\t');
            if (cursor == NULL) break;
            *cursor++ = 0;
        }
        if (cursor != NULL || (field_count != 7 && field_count != 13)) goto invalid;
        if (field_count == 7) {
            size_t source_size = strlen(fields[0]);
            if (*case_count == CASE_MAX) goto overflow;
            if (!relative_path(fields[0]) || source_size < 3 || strcmp(fields[0] + source_size - 2, ".c") != 0 ||
                strcmp(fields[2], "aarch64,x86_64") != 0 || !relative_path(fields[4]) ||
                strncmp(fields[4], "golden/", 7) != 0 || parse_exit(fields[3], &cases[*case_count].expected_exit) != 0)
                goto invalid;
            cases[*case_count].isa = ISA_BOTH;
            cases[*case_count].needs_rootfs = 0;
            cases[*case_count].environment[0] = 0;
            cases[*case_count].argument[0] = 0;
            if (snprintf(cases[*case_count].name, sizeof(cases[*case_count].name), "%s", fields[0]) >=
                    (int)sizeof(cases[*case_count].name) ||
                snprintf(cases[*case_count].source, sizeof(cases[*case_count].source), "%s", fields[0]) >=
                    (int)sizeof(cases[*case_count].source) ||
                snprintf(cases[*case_count].expected, sizeof(cases[*case_count].expected), "%s", fields[4]) >=
                    (int)sizeof(cases[*case_count].expected))
                goto invalid;
            (*case_count)++;
            continue;
        }
        /* excluded-macos is per-engine: it drops out only on the Mach-O (macOS) engine; on the ELF
           (Linux) engine it is parsed and run exactly like an active case so Linux keeps enforcing it. */
        int macos_only = strcmp(fields[11], "excluded-macos") == 0;
        if (strncmp(fields[11], "excluded-", 9) == 0 && !(macos_only && !host_macho)) {
            (*excluded)++;
            continue;
        }
        if (*case_count == CASE_MAX) goto overflow;
        if ((strcmp(fields[11], "active") != 0 && !macos_only) || !relative_path(fields[2]) ||
            !relative_path(fields[9]) || strncmp(fields[9], "expected/", 9) != 0 ||
            (strcmp(fields[6], "-") != 0 && strncmp(fields[6], "argv:", 5) != 0) || !valid_environment(fields[7]) ||
            parse_exit(fields[8], &cases[*case_count].expected_exit) != 0)
            goto invalid;
        /* Rootfs shape is explicit manifest metadata; ABI4 always carries the staged root as typed launch data. */
        cases[*case_count].needs_rootfs = strstr(fields[10], "-rootfs") != NULL;
        cases[*case_count].dynamic_rootfs = strstr(fields[10], "dynamic-rootfs") != NULL;
        cases[*case_count].mapping_data_rootfs = strstr(fields[10], "mapping-data-rootfs") != NULL;
        cases[*case_count].argument[0] = 0;
        if (strncmp(fields[6], "argv:", 5) == 0 &&
            snprintf(cases[*case_count].argument, sizeof(cases[*case_count].argument), "%s", fields[6] + 5) >=
                (int)sizeof(cases[*case_count].argument))
            goto invalid;
        if (strcmp(fields[4], "aarch64") == 0)
            cases[*case_count].isa = ISA_AARCH64;
        else if (strcmp(fields[4], "x86_64") == 0)
            cases[*case_count].isa = ISA_X86_64;
        else if (strcmp(fields[4], "aarch64,x86_64") == 0)
            cases[*case_count].isa = ISA_BOTH;
        else
            goto invalid;
        if (snprintf(cases[*case_count].name, sizeof(cases[*case_count].name), "%s", fields[0]) >=
                (int)sizeof(cases[*case_count].name) ||
            snprintf(cases[*case_count].source, sizeof(cases[*case_count].source), "%s", fields[2]) >=
                (int)sizeof(cases[*case_count].source) ||
            snprintf(cases[*case_count].environment, sizeof(cases[*case_count].environment), "%s",
                     strcmp(fields[7], "-") == 0 ? "" : fields[7]) >= (int)sizeof(cases[*case_count].environment) ||
            snprintf(cases[*case_count].expected, sizeof(cases[*case_count].expected), "%s", fields[9]) >=
                (int)sizeof(cases[*case_count].expected))
            goto invalid;
        (*case_count)++;
    }
    free(line);
    fclose(file);
    return *case_count == 0;
invalid:
    fprintf(stderr, "matrix-runner: invalid manifest row near active case %zu\n", *case_count + 1);
    free(line);
    fclose(file);
    return 1;
    /* Overflow used to fall into `invalid` above, so a manifest that parses perfectly was reported as a
       parse error -- sending the reader to the manifest to look for a malformed row that is not there.
       The limit is the harness's, so the harness says so, and names the file it was reading. */
overflow:
    fprintf(stderr,
            "matrix-runner: %s holds more than %d active cases, which is CASE_MAX in tools/matrix_runner.c. The "
            "manifest is not malformed; raise CASE_MAX (the array is a fixed-size main() local) and rebuild.\n",
            path, CASE_MAX);
    free(line);
    fclose(file);
    return 1;
}

static int drain(int fd, unsigned char *buffer, size_t *size, size_t limit, int *eof) {
    for (;;) {
        ssize_t count;
        if (*size == limit) return 1;
        count = read(fd, buffer + *size, limit - *size);
        if (count > 0) {
            *size += (size_t)count;
            continue;
        }
        if (count == 0) {
            *eof = 1;
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno != EINTR) return 1;
    }
}

static int read_capture(const char *path, unsigned char *buffer, size_t limit, size_t *size) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    *size = 0;
    if (descriptor < 0) return 1;
    while (*size < limit) {
        ssize_t count = read(descriptor, buffer + *size, limit - *size);
        if (count > 0) {
            *size += (size_t)count;
            continue;
        }
        if (count == 0) {
            close(descriptor);
            return 0;
        }
        if (errno != EINTR) break;
    }
    if (*size == limit) {
        unsigned char extra;
        ssize_t count;
        do {
            count = read(descriptor, &extra, 1);
        } while (count < 0 && errno == EINTR);
        if (count == 0) {
            close(descriptor);
            return 0;
        }
    }
    close(descriptor);
    return 1;
}

static void terminate(pid_t child) {
    const struct timespec tick = {0, 10000000};
    (void)kill(-child, SIGTERM);
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        pid_t waited = waitpid(child, NULL, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) goto done;
        if (waited < 0 && errno != EINTR) break;
        (void)nanosleep(&tick, NULL);
    }
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
done:
    if (active_group == (sig_atomic_t)child) active_group = 0;
}

typedef struct config_wire {
    hl_launch_config config;
    char pool[2048];
    size_t used;
} config_wire;

static int write_full(int fd, const void *buffer, size_t size) {
    const unsigned char *cursor = buffer;
    while (size != 0) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int process_domain(uint64_t identity[2]) {
    unsigned char *output = (unsigned char *)identity;
    size_t offset = 0;
    int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return -1;
    while (offset < sizeof(uint64_t) * 2u) {
        ssize_t count = read(descriptor, output + offset, sizeof(uint64_t) * 2u - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            (void)close(descriptor);
            return -1;
        }
    }
    if (close(descriptor) != 0) return -1;
    return (identity[0] | identity[1]) != 0 ? 0 : -1;
}

static void remove_tree(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) {
        (void)unlink(path);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[1200];
        struct stat status;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >= (int)sizeof child) continue;
        if (lstat(child, &status) == 0 && S_ISDIR(status.st_mode))
            remove_tree(child);
        else
            (void)unlink(child);
    }
    (void)closedir(directory);
    (void)rmdir(path);
}

static int pool_string(config_wire *wire, const char *value, uint32_t *offset) {
    size_t size = strlen(value) + 1;
    if (wire->used + size > sizeof wire->pool || wire->used > UINT32_MAX) return 1;
    *offset = (uint32_t)wire->used;
    memcpy(wire->pool + wire->used, value, size);
    wire->used += size;
    return 0;
}

static int config_option(config_wire *wire, const char *name, const char *value) {
    if (strcmp(name, "HL_NET_ISOLATE") == 0) {
        if (strcmp(value, "1") != 0) return 1;
        wire->config.network_isolated = 1;
        // ABI12 validate (config.c) rejects the config unless network_transport agrees with the
        // network_isolated flag; setting the flag alone leaves transport at VIRTUAL (0) and the launch
        // is rejected as malformed. Pin the transport to ISOLATED so the two fields stay consistent.
        wire->config.network_transport = HL_CONFIG_NETWORK_ISOLATED;
    } else if (strcmp(name, "HL_CPUS") == 0) {
        char *end;
        unsigned long parsed;
        errno = 0;
        parsed = strtoul(value, &end, 10);
        if (errno != 0 || *value == 0 || *end != 0 || parsed == 0 || parsed > UINT32_MAX) return 1;
        wire->config.cpu_limit = (uint32_t)parsed;
    } else if (strcmp(name, "HL_MEM_MAX") == 0) {
        char *end;
        unsigned long long parsed;
        errno = 0;
        parsed = strtoull(value, &end, 10);
        if (errno != 0 || *value == 0 || *end != 0 || parsed == 0) return 1;
        wire->config.memory_limit = (uint64_t)parsed;
    } else if (strcmp(name, "HL_UID") == 0 || strcmp(name, "HL_GID") == 0) {
        char *end;
        unsigned long parsed;
        errno = 0;
        parsed = strtoul(value, &end, 10);
        if (errno != 0 || *value == 0 || *end != 0 || parsed > INT32_MAX) return 1;
        if (name[3] == 'U')
            wire->config.uid = (int32_t)parsed;
        else
            wire->config.gid = (int32_t)parsed;
    } else if (strcmp(name, "HL_ROOTFS_RO") == 0) {
        if (strcmp(value, "1") != 0) return 1;
        wire->config.rootfs_read_only = 1;
    } else if (strcmp(name, "HL_SANDBOX") == 0) {
        if (strcmp(value, "1") != 0) return 1;
        wire->config.sandbox = HL_CONFIG_SANDBOX_ENABLED;
    } else if (strcmp(name, "HL_UNTRUSTED") == 0) {
        if (strcmp(value, "1") != 0) return 1;
        wire->config.sandbox = HL_CONFIG_UNTRUSTED_ONLY;
    } else if (strcmp(name, "HL_ULIMITS") == 0) {
        return pool_string(wire, value, &wire->config.limits_offset);
    } else if (strcmp(name, "HL_VOLUMES") == 0) {
        return pool_string(wire, value, &wire->config.volumes_offset);
    } else if (strcmp(name, "HL_NETNS") == 0) {
        return pool_string(wire, value, &wire->config.network_namespace_offset);
    } else if (strcmp(name, "HL_NETBR") == 0) {
        return pool_string(wire, value, &wire->config.network_bridge_offset);
    } else if (strcmp(name, "HL_IP") == 0) {
        return pool_string(wire, value, &wire->config.ip_offset);
    } else if (strcmp(name, "HL_PCACHE_DIR") == 0) {
        // ABI4 enables the persistent translation cache by supplying its directory; production launch
        // deliberately does not ingest ambient HL_* environment variables.
        if (*value == 0) return 1;
        return pool_string(wire, value, &wire->config.translation_cache_offset);
    } else {
        return 2;
    }
    return 0;
}

static int make_config(const char *binary_root, const char *guest, const char *argument, const char *rootfs,
                       const char *encoded, const char *scratch, char path[1024]) {
    config_wire wire;
    char copy[256], guest_environment[512] = {0}, *cursor;
    size_t environment_size = 0;
    int fd = -1, result = 1;
    memset(&wire, 0, sizeof wire);
    wire.used = 1; /* Offset zero is the canonical absent string. */
    wire.config.magic = HL_CONFIG_MAGIC;
    wire.config.header_size = sizeof wire.config;
    wire.config.abi = HL_CONFIG_ABI;
    wire.config.uid = -1;
    wire.config.gid = -1;
    if (process_domain(wire.config.process_domain) != 0) return 1;
    {
        const char *debug_log = getenv("HL_LOG");
        if (debug_log != NULL && *debug_log != 0 && pool_string(&wire, debug_log, &wire.config.debug_log_offset) != 0)
            return 1;
    }
    if (rootfs != NULL && pool_string(&wire, rootfs, &wire.config.rootfs_offset) != 0) return 1;
    if (*encoded != 0) {
        memcpy(copy, encoded, strlen(encoded) + 1);
        cursor = copy;
        while (cursor != NULL) {
            char *next = strchr(cursor, ';'), *equals = strchr(cursor, '=');
            char isolated_cache[1024];
            const char *option_value;
            int option;
            if (next != NULL) *next++ = 0;
            if (equals == NULL) return 1;
            *equals++ = 0;
            option_value = equals;
            /*
             * Translation runs in the macOS engine, so HL_PCACHE_DIR is a
             * host path rather than a guest path covered by the per-case
             * /tmp volume below.  A literal /tmp path in a manifest otherwise
             * collides across parallel runners and can consume stale code
             * from another build.  Keep the manifest as the opt-in signal,
             * but always isolate its storage inside this case's unique host
             * scratch directory.
             */
            if (scratch != NULL && strcmp(cursor, "HL_PCACHE_DIR") == 0) {
                if (snprintf(isolated_cache, sizeof isolated_cache, "%s/pcache", scratch) >=
                        (int)sizeof isolated_cache ||
                    mkdir(isolated_cache, 0700) != 0)
                    return 1;
                option_value = isolated_cache;
            }
            option = config_option(&wire, cursor, option_value);
            if (option == 1 || (option == 2 && strncmp(cursor, "HL_", 3) == 0)) return 1;
            if (option == 2) {
                size_t record = strlen(cursor) + 1 + strlen(equals);
                if (environment_size != 0) guest_environment[environment_size++] = '\n';
                if (environment_size + record + 1 > sizeof guest_environment) return 1;
                memcpy(guest_environment + environment_size, cursor, strlen(cursor));
                environment_size += strlen(cursor);
                guest_environment[environment_size++] = '=';
                memcpy(guest_environment + environment_size, equals, strlen(equals));
                environment_size += strlen(equals);
                guest_environment[environment_size] = 0;
            }
            cursor = next;
        }
    }
    if (scratch != NULL) {
        char volume[1600];
        const char *declared = wire.config.volumes_offset ? wire.pool + wire.config.volumes_offset : NULL;
        int length =
            declared ? snprintf(volume, sizeof volume, "%s,%s:%s,/tmp:%s", declared, binary_root, binary_root, scratch)
                     : snprintf(volume, sizeof volume, "%s:%s,/tmp:%s", binary_root, binary_root, scratch);
        if (length < 0 || length >= (int)sizeof volume ||
            pool_string(&wire, volume, &wire.config.volumes_offset) != 0 ||
            pool_string(&wire, "/tmp", &wire.config.working_directory_offset) != 0)
            return 1;
    }
    if (environment_size != 0 && pool_string(&wire, guest_environment, &wire.config.environment_offset) != 0) return 1;
    /* argv is a NUL-separated vector terminated by an additional NUL. */
    if (pool_string(&wire, guest, &wire.config.arguments_offset) != 0) return 1;
    if (*argument != 0 && wire.used + strlen(argument) + 1 <= sizeof wire.pool) {
        memcpy(wire.pool + wire.used, argument, strlen(argument) + 1);
        wire.used += strlen(argument) + 1;
    } else if (*argument != 0) {
        return 1;
    }
    if (wire.used == sizeof wire.pool) return 1;
    wire.pool[wire.used++] = 0;
    wire.config.pool_size = (uint32_t)wire.used;
    if (snprintf(path, 1024, "%s/.matrix-config-XXXXXX", binary_root) >= 1024) return 1;
    fd = mkstemp(path);
    if (fd < 0) return 1;
    if (fchmod(fd, 0600) == 0 && write_full(fd, &wire.config, sizeof wire.config) == 0 &&
        write_full(fd, wire.pool, wire.used) == 0 && close(fd) == 0)
        return 0;
    result = errno;
    if (fd >= 0) (void)close(fd);
    (void)unlink(path);
    errno = result;
    return 1;
}

static int run_guest(const char *bridge, const char *engine, const char *guest, const char *argument,
                     const char *rootfs, const char *environment, const char *binary_root, capture *result) {
    int output_pipe[2], error_pipe[2], output_eof = 0, error_eof = 0, exited = 0;
    char config_path[1024], scratch[1024], supervisor[1024], capture_output[1200], capture_error[1200];
    uint64_t deadline, stall_budget = stall_timeout_ms(), stall_bytes = 0, stall_stamp, stall_sampled;
    long long stall_ticks = -1;
    pid_t child;
    memset(result, 0, sizeof(*result));
    result->output = malloc(OUTPUT_MAX);
    result->error = malloc(ERROR_MAX);
    if (result->output == NULL || result->error == NULL || pipe(output_pipe) != 0 || pipe(error_pipe) != 0) return 1;
    {
        /*
         * The guest scratch is mapped as the guest's /tmp volume, so its backing
         * filesystem determines whether statx-btime, memfd seals, and related
         * tmpfs-only behaviour are observable. By default we place it under
         * binary_root (the case dir in the build tree), which keeps local `make`
         * on a tmpfs tree correct. On CI where the build tree lives on ext4, the
         * scratch base can be overridden with HL_MATRIX_SCRATCH_DIR pointing at a
         * mounted tmpfs. The override is a HOST path; the engine's guest-side
         * special-casing of /tmp and /dev/shm does not apply to it.
         */
        const char *scratch_base = getenv("HL_MATRIX_SCRATCH_DIR");
        int requested = scratch_base != NULL && scratch_base[0] != 0, rejected = 0;
        struct stat base_stat;
        if (!requested || stat(scratch_base, &base_stat) != 0 || !S_ISDIR(base_stat.st_mode) ||
            access(scratch_base, W_OK) != 0) {
            rejected = requested;
            scratch_base = binary_root;
        }
        scratch_observe(scratch_base, requested && !rejected, rejected);
        if (snprintf(scratch, sizeof scratch, "%s/.matrix-scratch-XXXXXX", scratch_base) >= (int)sizeof scratch ||
            mkdtemp(scratch) == NULL)
            return 1;
    }
    if (snprintf(capture_output, sizeof capture_output, "%s/stdout", scratch) >= (int)sizeof capture_output ||
        snprintf(capture_error, sizeof capture_error, "%s/stderr", scratch) >= (int)sizeof capture_error) {
        remove_tree(scratch);
        return 1;
    }
    if (make_config(binary_root, guest, argument, rootfs, environment, scratch, config_path) != 0) {
        remove_tree(scratch);
        return 1;
    }
    {
        const char *slash = strrchr(engine, '/');
        size_t directory_size = slash == NULL ? 0 : (size_t)(slash - engine);
        if (directory_size == 0 || directory_size + sizeof("/hl-remote-supervisor") > sizeof supervisor) {
            (void)unlink(config_path);
            remove_tree(scratch);
            return 1;
        }
        memcpy(supervisor, engine, directory_size);
        memcpy(supervisor + directory_size, "/hl-remote-supervisor", sizeof("/hl-remote-supervisor"));
    }
    child = fork();
    if (child < 0) {
        (void)unlink(config_path);
        remove_tree(scratch);
        return 1;
    }
    if (child == 0) {
        (void)setpgid(0, 0);
        close(output_pipe[0]);
        close(error_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(error_pipe[1], STDERR_FILENO) < 0) _exit(127);
        close(output_pipe[1]);
        close(error_pipe[1]);
        hl_launch_hygiene();
        execlp(bridge, bridge, supervisor, "--capture", capture_output, capture_error, engine, "--configfile",
               config_path, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(child, child);
    active_group = (sig_atomic_t)child;
    close(output_pipe[1]);
    close(error_pipe[1]);
    if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(error_pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        terminate(child);
        (void)unlink(config_path);
        remove_tree(scratch);
        return 1;
    }
    deadline = monotonic_ms() + case_timeout_ms();
    stall_stamp = monotonic_ms();
    stall_sampled = stall_stamp;
    while (!exited || !output_eof || !error_eof) {
        struct pollfd descriptors[2] = {{output_pipe[0], POLLIN | POLLHUP, 0}, {error_pipe[0], POLLIN | POLLHUP, 0}};
        uint64_t now = monotonic_ms();
        pid_t waited;
        if (interrupted_signal != 0 || now >= deadline) {
            terminate(child);
            close(output_pipe[0]);
            close(error_pipe[0]);
            unlink(config_path);
            remove_tree(scratch);
            return 2;
        }
        /* Sampled once a second rather than on every 10ms poll: a /proc walk is cheap but not free, and a
           stall is measured in tens of seconds. `exited` guards the tail of the loop where the child is
           already reaped and only the pipes are being drained -- there is no tree left to sample. */
        if (stall_budget != 0 && !exited && now - stall_sampled >= STALL_SAMPLE_MS) {
            uint64_t bytes = capture_bytes(capture_output, capture_error);
            long long ticks = tree_cpu_ticks((long)child);
            stall_sampled = now;
            if (ticks < 0 || bytes != stall_bytes || ticks != stall_ticks) {
                stall_bytes = bytes;
                stall_ticks = ticks;
                stall_stamp = now;
            } else if (now - stall_stamp >= stall_budget) {
                terminate(child);
                close(output_pipe[0]);
                close(error_pipe[0]);
                /* Unlike the wall-clock path, recover whatever the guest DID print before it stopped: a
                   hang is diagnosed from where it got to, and this path can only be reached on a lane
                   whose budget was scaled, so no existing lane's failure text is affected. */
                (void)read_capture(capture_output, result->output, OUTPUT_MAX, &result->output_size);
                (void)read_capture(capture_error, result->error, ERROR_MAX, &result->error_size);
                unlink(config_path);
                remove_tree(scratch);
                return 3;
            }
        }
        if (poll(descriptors, 2, 10) < 0 && errno != EINTR) {
            terminate(child);
            (void)unlink(config_path);
            remove_tree(scratch);
            return 1;
        }
        if (drain(output_pipe[0], result->output, &result->output_size, OUTPUT_MAX, &output_eof) != 0 ||
            drain(error_pipe[0], result->error, &result->error_size, ERROR_MAX, &error_eof) != 0) {
            terminate(child);
            (void)unlink(config_path);
            remove_tree(scratch);
            return 1;
        }
        if (!exited) {
            waited = waitpid(child, &result->wait_status, WNOHANG);
            if (waited == child)
                exited = 1;
            else if (waited < 0 && errno != EINTR) {
                terminate(child);
                (void)unlink(config_path);
                remove_tree(scratch);
                return 1;
            }
        }
    }
    active_group = 0;
    close(output_pipe[0]);
    close(error_pipe[0]);
    if (read_capture(capture_output, result->output, OUTPUT_MAX, &result->output_size) != 0 ||
        read_capture(capture_error, result->error, ERROR_MAX, &result->error_size) != 0) {
        (void)unlink(config_path);
        remove_tree(scratch);
        return 1;
    }
    (void)unlink(config_path); /* Engine normally unlinks immediately; covers pre-exec failure. */
    remove_tree(scratch);
    return 0;
}

static int read_file(const char *path, unsigned char **data, size_t *size) {
    FILE *file = fopen(path, "rb");
    long length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
        return 1;
    *data = malloc((size_t)length + 1u);
    if (*data == NULL || fread(*data, 1, (size_t)length, file) != (size_t)length || fclose(file) != 0) return 1;
    *size = (size_t)length;
    return 0;
}

static int copy_file(const char *source, const char *destination) {
    unsigned char buffer[64 * 1024];
    int input = open(source, O_RDONLY), output = -1;
    if (input < 0) return 1;
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (output < 0) {
        close(input);
        return 1;
    }
    for (;;) {
        ssize_t count = read(input, buffer, sizeof buffer);
        size_t offset = 0;
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            close(input);
            close(output);
            return 1;
        }
        while (offset < (size_t)count) {
            ssize_t written = write(output, buffer + offset, (size_t)count - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                close(input);
                close(output);
                return 1;
            }
            offset += (size_t)written;
        }
    }
    return close(input) != 0 || close(output) != 0;
}

static int make_parents(char *path) {
    char *cursor;
    for (cursor = path + 1; *cursor != 0; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = 0;
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return 1;
        *cursor = '/';
    }
    return 0;
}

static int stage_rootfs(const char *binary_root, const char *guest, const char *isa, int dynamic, int mapping_data,
                        char rootfs[1024]) {
    char bin[1024], dev[1024], pts[1024], tmp[1024], staged[1024], data[1024], loader[1024], libc[1024];
    const char *loader_source = strcmp(isa, "aarch64") == 0 ? AARCH64_DYNAMIC_LOADER : X86_64_DYNAMIC_LOADER;
    const char *libc_source = strcmp(isa, "aarch64") == 0 ? AARCH64_DYNAMIC_LIBC : X86_64_DYNAMIC_LIBC;
    const char *loader_guest =
        strcmp(isa, "aarch64") == 0 ? "/lib/ld-linux-aarch64.so.1" : "/lib64/ld-linux-x86-64.so.2";
    if (snprintf(rootfs, 1024, "%s/.rootfs-XXXXXX", binary_root) >= 1024 || mkdtemp(rootfs) == NULL ||
        snprintf(bin, sizeof bin, "%s/bin", rootfs) >= (int)sizeof bin ||
        snprintf(dev, sizeof dev, "%s/dev", rootfs) >= (int)sizeof dev ||
        snprintf(pts, sizeof pts, "%s/dev/pts", rootfs) >= (int)sizeof pts ||
        snprintf(tmp, sizeof tmp, "%s/tmp", rootfs) >= (int)sizeof tmp ||
        snprintf(staged, sizeof staged, "%s/bin/guest", rootfs) >= (int)sizeof staged || mkdir(bin, 0755) != 0 ||
        mkdir(dev, 0755) != 0 || mkdir(pts, 0755) != 0 || mkdir(tmp, 01777) != 0 || copy_file(guest, staged) != 0)
        return 1;
    if (mapping_data) {
        unsigned char bytes[12288];
        int descriptor;
        memset(bytes, 0x2a, sizeof(bytes));
        if (snprintf(data, sizeof data, "%s/data", rootfs) >= (int)sizeof data) return 1;
        descriptor = open(data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor < 0 || write(descriptor, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes) ||
            close(descriptor) != 0)
            return 1;
    }
    if (!dynamic) return 0;
    if (*loader_source == 0 || *libc_source == 0 ||
        snprintf(loader, sizeof loader, "%s%s", rootfs, loader_guest) >= (int)sizeof loader ||
        snprintf(libc, sizeof libc, "%s/lib/libc.so.6", rootfs) >= (int)sizeof libc || make_parents(loader) != 0 ||
        make_parents(libc) != 0 || copy_file(loader_source, loader) != 0 || copy_file(libc_source, libc) != 0)
        return 1;
    return 0;
}

static void remove_rootfs(const char *rootfs) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/lib/ld-linux-aarch64.so.1", rootfs) < (int)sizeof path) (void)unlink(path);
    if (snprintf(path, sizeof path, "%s/lib64/ld-linux-x86-64.so.2", rootfs) < (int)sizeof path) (void)unlink(path);
    if (snprintf(path, sizeof path, "%s/lib/libc.so.6", rootfs) < (int)sizeof path) (void)unlink(path);
    if (snprintf(path, sizeof path, "%s/bin/guest", rootfs) < (int)sizeof path) (void)unlink(path);
    if (snprintf(path, sizeof path, "%s/data", rootfs) < (int)sizeof path) (void)unlink(path);
    if (snprintf(path, sizeof path, "%s/dev/pts", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/bin", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/dev", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/tmp", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/lib64", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/lib", rootfs) < (int)sizeof path) (void)rmdir(path);
    (void)rmdir(rootfs);
}

static void capture_free(capture *result) {
    free(result->output);
    free(result->error);
}

static int exit_matches(const capture *result, int expected) {
    return WIFEXITED(result->wait_status) && WEXITSTATUS(result->wait_status) == expected;
}

static void diagnostic(const suite_case *item, const char *isa, const char *reason, const capture *result) {
    if (getenv("GITHUB_ACTIONS") != NULL)
        fprintf(stderr, "::error title=Compatibility failure (%s %s)::%s\n", item->name, isa, reason);
    fprintf(stderr, "matrix-runner: %s [%s] %s", item->name, isa, reason);
    if (result != NULL) {
        size_t index, shown = result->output_size > 64 ? 64 : result->output_size;
        fprintf(stderr, ": wait=0x%x stdout=%zuB hex=", result->wait_status, result->output_size);
        for (index = 0; index < shown; ++index)
            fprintf(stderr, "%02x", result->output[index]);
        if (shown < result->output_size) fputs("...", stderr);
    }
    if (result != NULL && result->error_size != 0) {
        size_t shown = result->error_size > 4096 ? 4096 : result->error_size;
        size_t offset = result->error_size - shown;
        fprintf(stderr, result->error_size > shown ? " stderr_tail=" : " stderr=");
        (void)fwrite(result->error + offset, 1, shown, stderr);
    }
    fputc('\n', stderr);
}

/* A bare "timeout" cannot be read on a host whose budget is not the default: it does not say whether the case
   hung or whether the scale is simply set too low for it. Name the budget that expired -- but only when it is
   scaled, so an unscaled lane's failure text stays exactly the string it has always been. */
static const char *timeout_reason(void) {
    static char text[96];
    if (timeout_scale == 1) return "timeout";
    (void)snprintf(text, sizeof text, "timeout after %llums (HL_MATRIX_TIMEOUT_SCALE=%lu)",
                   (unsigned long long)case_timeout_ms(), timeout_scale);
    return text;
}

/* A stall is a different verdict from a timeout and must read as one: the case did not run out of time, it
   stopped doing anything. Naming both budgets is what lets a reader tell "hung" from "the scale is too
   low" without re-running anything. */
static const char *stall_reason(void) {
    static char text[192];
    (void)snprintf(text, sizeof text,
                   "HUNG: no guest output and no CPU anywhere in its process tree for %llums (the %llums per-case "
                   "budget had NOT expired, so this is a hang, not slow execution)",
                   (unsigned long long)stall_timeout_ms(), (unsigned long long)case_timeout_ms());
    return text;
}

static int run_one(const suite_case *item, const char *bridge, const char *engine, const char *binary_root,
                   const char *suite_root, const char *isa, capture *result) {
    char guest[1024], expected_path[1024], binary[256], rootfs[1024] = {0};
    unsigned char *expected;
    const char *source_name = strrchr(item->source, '/');
    size_t expected_size, length, binary_length;
    int status;
    /*
     * Most suite binaries preserve source subdirectories, while imported corpus sources are flattened with
     * $(notdir). Prefer the source-relative output and fall back to its basename so both Makefile layouts are
     * addressable without encoding build-system paths into the manifest.
     */
    length = strlen(item->source);
    binary_length = length;
    /* Source-built suites use foo.c; fixture suites may name the committed executable itself. */
    if (length >= 2 && strcmp(item->source + length - 2, ".c") == 0) binary_length -= 2;
    if (binary_length == 0 || binary_length >= sizeof(binary)) return 1;
    memcpy(binary, item->source, binary_length);
    binary[binary_length] = 0;
    if (snprintf(guest, sizeof(guest), "%s/%s", binary_root, binary) >= (int)sizeof(guest)) return 1;
    if (access(guest, R_OK) != 0 && source_name != NULL) {
        source_name++;
        length = strlen(source_name);
        binary_length = length;
        if (length >= 2 && strcmp(source_name + length - 2, ".c") == 0) binary_length -= 2;
        if (binary_length == 0 || binary_length >= sizeof(binary)) return 1;
        memcpy(binary, source_name, binary_length);
        binary[binary_length] = 0;
        if (snprintf(guest, sizeof(guest), "%s/%s", binary_root, binary) >= (int)sizeof(guest)) return 1;
    }
    /* An unbuilt guest otherwise surfaces as an opaque engine "execution failed status=6" with empty stdout;
       name it so a missing build registration is not mistaken for a runtime abort. */
    if (!item->needs_rootfs && access(guest, R_OK) != 0) {
        fprintf(stderr, "matrix-runner: %s [%s] guest binary missing: %s\n", item->name, isa, guest);
        return 1;
    }
    if (snprintf(expected_path, sizeof(expected_path), "%s/%s", suite_root, item->expected) >=
            (int)sizeof(expected_path) ||
        read_file(expected_path, &expected, &expected_size) != 0) {
        fprintf(stderr, "matrix-runner: %s input path/read failure\n", item->name);
        return 1;
    }
    if (item->needs_rootfs &&
        stage_rootfs(binary_root, guest, isa, item->dynamic_rootfs, item->mapping_data_rootfs, rootfs) != 0) {
        fprintf(stderr, "matrix-runner: %s rootfs staging failure\n", item->name);
        free(expected);
        return 1;
    }
    /* A bare name is resolved through the guest rootfs PATH without bridge-side path translation. */
    status = run_guest(bridge, engine, item->needs_rootfs ? "/bin/guest" : guest, item->argument,
                       item->needs_rootfs ? rootfs : NULL, item->environment, binary_root, result);
    if (item->needs_rootfs) remove_rootfs(rootfs);
    if (status != 0 || !exit_matches(result, item->expected_exit) || result->output_size != expected_size ||
        memcmp(result->output, expected, expected_size) != 0) {
        size_t common = result->output_size < expected_size ? result->output_size : expected_size;
        size_t mismatch = 0;
        while (mismatch < common && result->output[mismatch] == expected[mismatch])
            ++mismatch;
        if (mismatch < common)
            fprintf(stderr, "matrix-runner: %s [%s] first stdout difference at byte %zu: got=%02x expected=%02x\n",
                    item->name, isa, mismatch, result->output[mismatch], expected[mismatch]);
        else if (result->output_size != expected_size)
            fprintf(stderr, "matrix-runner: %s [%s] stdout length: got=%zu expected=%zu\n", item->name, isa,
                    result->output_size, expected_size);
        diagnostic(item, isa,
                   status == 2   ? timeout_reason()
                   : status == 3 ? stall_reason()
                                 : "exit/stdout mismatch",
                   result);
        free(expected);
        return 1;
    }
    free(expected);
    return 0;
}

int main(int argc, char **argv) {
    suite_case cases[CASE_MAX];
    size_t count, excluded, index, selected = 0, failures = 0;
    /* Per-ISA tallies. One run of one suite has to answer "how did each backend do", which is the whole
       point of a two-ISA matrix and was unanswerable while the second leg was conditional on the first. */
    size_t aarch64_selected = 0, aarch64_failures = 0, x86_64_selected = 0, x86_64_failures = 0;
    size_t cross_compared = 0, cross_failures = 0, cross_skipped = 0;
    int legs_aarch64 = 0, legs_x86_64 = 0, failed_aarch64 = 0, failed_x86_64 = 0, failed_cross = 0,
        failed_resources = 0;
    const char *only = NULL;
    unsigned long repetitions = 1;
    unsigned long repetition;
    resource_baseline baseline;
    if (install_interrupt_handlers() != 0) return 1;
    /* Before any case runs, so a malformed scale is one line at the top of the log instead of a verdict on a
       suite that was measured against a budget nobody chose. */
    if (load_timeout_scale() != 0) return 2;
    if (argc == 8) {
        only = argv[7];
    } else if (argc == 9 || argc == 10) {
        char *end = NULL;
        if (strcmp(argv[7], "--repeat") != 0) goto usage;
        errno = 0;
        repetitions = strtoul(argv[8], &end, 10);
        if (errno != 0 || end == argv[8] || *end != '\0' || repetitions == 0 || repetitions > 10000) goto usage;
        if (argc == 10) only = argv[9];
    } else if (argc != 7) {
    usage:
        fprintf(stderr,
                "usage: matrix-runner BRIDGE AARCH64_ENGINE AARCH64_BIN_ROOT X86_64_ENGINE X86_64_BIN_ROOT SUITE_ROOT "
                "[--repeat N] [CASE]\n");
        return 2;
    }
    /*
     * The guest maps /tmp to a fresh per-case scratch directory, so a binary root UNDER /tmp cannot resolve
     * inside the guest: cases that canonicalize argv[0] then fail with a diagnostic that looks like an engine
     * bug (BUILD=/tmp/...). Reject it up front instead.
     */
    if (strncmp(argv[3], "/tmp/", 5) == 0 || strncmp(argv[5], "/tmp/", 5) == 0) {
        fprintf(stderr, "matrix-runner: binary root under /tmp is unusable -- the guest maps /tmp to its own "
                        "scratch; use a build directory outside /tmp\n");
        return 2;
    }
    if (load_manifest(argv[6], cases, &count, &excluded, engine_is_macho(argv[2])) != 0) return 1;
    baseline = resource_measure();
    for (index = 0; index < count; ++index) {
        if (interrupted_signal != 0) return 128 + interrupted_signal;
        if (only != NULL && strcmp(only, cases[index].name) != 0) continue;
        selected++;
        /*
         * BOTH ISAs always run. They used to be chained -- the x86_64 leg was conditional on the aarch64
         * leg having passed -- which meant one run could never report per-ISA numbers, and, worse, that a
         * host where the two backends have DIVERGED is exactly the host where the divergence is invisible:
         * the first ISA to fail suppressed the other's result and the cross-ISA identity check with it.
         * The verdict is deliberately unchanged (a case failing on either ISA still fails the case, and
         * the identity check still requires two passing legs to have something to compare), so this is a
         * measurement fix and not a scoring change.
         */
        legs_aarch64 = cases[index].isa != ISA_X86_64;
        legs_x86_64 = cases[index].isa != ISA_AARCH64;
        failed_aarch64 = 0;
        failed_x86_64 = 0;
        failed_cross = 0;
        failed_resources = 0;
        for (repetition = 0; repetition < repetitions; ++repetition) {
            if (interrupted_signal != 0) return 128 + interrupted_signal;
            capture a = {0}, x = {0};
            // Keep-going: a failing case emits its own ::error diagnostic (above, in run_one) but does not
            // abort the run -- record it and move to the next case so ONE run reports every failure, not just
            // the first. The run still exits non-zero when any case failed (checked after the loop).
            int case_failed;
            if (legs_aarch64 && run_one(&cases[index], argv[1], argv[2], argv[3], argv[6], "aarch64", &a) != 0)
                failed_aarch64 = 1;
            if (legs_x86_64 && run_one(&cases[index], argv[1], argv[4], argv[5], argv[6], "x86_64", &x) != 0)
                failed_x86_64 = 1;
            if (!failed_aarch64 && !failed_x86_64 && cases[index].isa == ISA_BOTH &&
                (a.output_size != x.output_size || memcmp(a.output, x.output, a.output_size) != 0)) {
                diagnostic(&cases[index], "cross-ISA", "stdout mismatch", &x);
                failed_cross = 1;
            }
            capture_free(&a);
            capture_free(&x);
            case_failed = failed_aarch64 || failed_x86_64 || failed_cross;
            if (!case_failed && !resources_restored(baseline, &cases[index])) {
                failed_resources = 1;
                case_failed = 1;
            }
            if (case_failed) break; // stop repeating this case; continue with the rest of the suite
        }
        if (legs_aarch64) {
            aarch64_selected++;
            aarch64_failures += (size_t)failed_aarch64;
        }
        if (legs_x86_64) {
            x86_64_selected++;
            x86_64_failures += (size_t)failed_x86_64;
        }
        if (cases[index].isa == ISA_BOTH) {
            if (failed_aarch64 || failed_x86_64)
                cross_skipped++;
            else if (failed_cross)
                cross_failures++;
            else
                cross_compared++;
        }
        if (failed_aarch64 || failed_x86_64 || failed_cross || failed_resources) {
            failures++;
            /* One line per failing case naming the LEG, because the per-leg diagnostics above are
               interleaved with the engine's own stderr and a reader scanning a 3000-case log needs the
               (case, ISA) pair to be greppable on its own. */
            fprintf(stderr, "matrix-runner: %s FAILED on %s\n", cases[index].name,
                    failed_aarch64 && failed_x86_64 ? "BOTH ISAs"
                    : failed_aarch64                ? (legs_x86_64 ? "aarch64 only (x86_64 passed)" : "aarch64")
                    : failed_x86_64                 ? (legs_aarch64 ? "x86_64 only (aarch64 passed)" : "x86_64")
                    : failed_cross                  ? "cross-ISA stdout identity (both ISAs passed on their own)"
                                                    : "host resource restoration (both ISAs passed)");
        }
    }
    if (only != NULL && selected == 0) {
        fprintf(stderr, "matrix-runner: unknown active case %s\n", only);
        return 2;
    }
    /* Repeated at the end as well as the start: the summary line is what a reader takes away from a run, and on
       a scaled host neither a pass nor a failure count means what the same numbers mean on an unscaled one. */
    if (timeout_scale != 1)
        printf("matrix-runner: per-case timeout was scaled x%lu; this run's timing is not comparable to an "
               "unscaled lane\n",
               timeout_scale);
    /* Unconditional, on stdout, in a fixed shape: a per-ISA number is the deliverable of a two-ISA matrix,
       and printing it only on failure would make a green run the one place the numbers are missing. The
       cross-ISA line reports what was COMPARED separately from what could not be, because "not compared"
       is a gap in the identity evidence, not a pass. */
    printf("matrix-runner: per-ISA: aarch64 %zu/%zu passed, x86_64 %zu/%zu passed; cross-ISA identity %zu "
           "compared, %zu mismatched, %zu not comparable (a leg failed)\n",
           aarch64_selected - aarch64_failures, aarch64_selected, x86_64_selected - x86_64_failures, x86_64_selected,
           cross_compared, cross_failures, cross_skipped);
    if (failures != 0) {
        fprintf(stderr, "matrix-runner: %zu of %zu selected case(s) FAILED; %zu excluded\n", failures, selected,
                excluded);
        scratch_note();
        return 1;
    }
    printf("matrix-runner: %zu active cases passed with %lu repetition(s); %zu manifest cases excluded\n", selected,
           repetitions, excluded);
    return 0;
}
