#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
/* _POSIX_C_SOURCE selects the POSIX 2008 surface this runner uses on Linux and Darwin. mingw-w64 keys its
 * own declarations off __STRICT_ANSI__ instead and hides part of them when a POSIX level is asserted, so the
 * Windows arm asks for nothing and takes the toolchain's default surface. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
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
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/wait.h>

#include "launch.h"
#endif

#include "config.h"
#include "hl/config.h"

/* ---------------------------------------------------------------------------
 * Host portability shims.
 *
 * The runner drives three production engines (ELF, Mach-O, PE) and therefore
 * has to run on three hosts. Everything below is a spelling difference only:
 * where a host genuinely cannot answer a question the difference is handled at
 * the call site, loudly, not hidden behind a macro that returns a plausible
 * lie. The one deliberate exception is O_CLOEXEC/O_NOINHERIT, which is a pure
 * rename of the same guarantee.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
/* NT opens in text mode by default, which rewrites \n as \r\n on the way out and
 * swallows \r on the way in. Every comparison this runner makes is byte-exact
 * against a golden captured on Linux, so a text-mode descriptor would fail the
 * whole corpus for a reason that has nothing to do with the guest. */
#define HL_O_BINARY _O_BINARY
#if !defined(O_CLOEXEC)
#define O_CLOEXEC _O_NOINHERIT
#endif
#define hl_mkdir(path, mode) _mkdir(path)
/* read()/write() here take an `unsigned int` count, not a size_t. Every request this runner makes is
 * bounded by OUTPUT_MAX (1 MiB) or a 64 KiB stack buffer, so the narrowing is provably lossless -- but it is
 * spelled out rather than left to an implicit conversion warning nobody reads. */
#define HL_IO_COUNT(bytes) ((unsigned)(bytes))
/* No symlinks are created inside a scratch tree, so lstat's only distinguishing
 * behaviour is unreachable here. */
#define lstat stat
/* Windows has no wait status word: the child's exit code IS the status, and a
 * process killed by the job object below reports the code the kill supplied.
 * The runner only consults these after a normal completion (run_guest returns
 * non-zero for every other outcome), so an exit code above 255 -- a structured
 * exception, e.g. 0xC0000005 -- simply cannot match an expected exit in [0,255]
 * and is printed verbatim by the diagnostic. */
#define WIFEXITED(status) (1)
#define WEXITSTATUS(status) (status)
#if !defined(R_OK)
#define R_OK 4
#endif
#if !defined(W_OK)
#define W_OK 2
#endif

/* mingw-w64 supplies mkstemp, mkdtemp and the whole <dirent.h> family, but not
 * getline. This is the POSIX 2008 contract, no more: grow the caller's buffer,
 * keep the newline, return the byte count or -1 at end of file. */
static ssize_t hl_getline(char **line, size_t *capacity, FILE *file) {
    size_t used = 0;
    if (line == NULL || capacity == NULL || file == NULL) return -1;
    if (*line == NULL || *capacity == 0) {
        char *fresh = realloc(*line, 256);
        if (fresh == NULL) return -1;
        *line = fresh;
        *capacity = 256;
    }
    for (;;) {
        int character = fgetc(file);
        if (character == EOF) {
            if (used == 0) return -1;
            break;
        }
        if (used + 2u > *capacity) {
            size_t grown = *capacity * 2u;
            char *fresh = realloc(*line, grown);
            if (fresh == NULL) return -1;
            *line = fresh;
            *capacity = grown;
        }
        (*line)[used++] = (char)character;
        if (character == '\n') break;
    }
    (*line)[used] = 0;
    return (ssize_t)used;
}

#define getline hl_getline
#else
#define HL_O_BINARY 0
#define hl_mkdir(path, mode) mkdir(path, mode)
#define HL_IO_COUNT(bytes) (bytes)
#endif

/* CASE_MAX bounds the fixed cases[] array in main(); overflow is the harness's limit, not a parse error. */
enum { CASE_MAX = 256, FIELD_MAX = 512, OUTPUT_MAX = 1024 * 1024, ERROR_MAX = 64 * 1024, TIMEOUT_MS = 120000 };

/* Per-case hang detector. 120s suits every suite whose cases are milliseconds of
 * work, but the endurance cases are minutes of real compute: soak/reallocchurn
 * measures 124s (aarch64) and 112s (x86_64) pinned to four CPUs, i.e. at or over
 * the cap on a runner-sized machine, while finishing in a fraction of that on a
 * whole host. Raising the constant for everyone would blunt the detector, so the
 * budget is an explicit opt-in the caller sets for the suites that need it.
 * Values outside [1s, 1h] are ignored rather than trusted. */
static uint64_t suite_case_timeout_ms(void) {
    return hl_tool_config_matrix_timeout_ms(TIMEOUT_MS);
}

/* Interpreter hosts need a larger budget than JIT hosts. CMake validates and
 * compiles the host-specific factor so configuration stays outside the tool. */
#ifndef HL_MATRIX_TIMEOUT_SCALE
#define HL_MATRIX_TIMEOUT_SCALE 1
#endif
#if HL_MATRIX_TIMEOUT_SCALE < 1 || HL_MATRIX_TIMEOUT_SCALE > 100
#error "HL_MATRIX_TIMEOUT_SCALE must be in [1, 100]"
#endif
static const unsigned long timeout_scale = HL_MATRIX_TIMEOUT_SCALE;

static uint64_t case_timeout_ms(void) {
    return suite_case_timeout_ms() * timeout_scale;
}

static void note_timeout_scale(void) {
    if (timeout_scale == 1) return;
    printf("matrix-runner: per-case timeout scaled x%lu to %llums (HL_MATRIX_TIMEOUT_SCALE); this run tolerates "
           "slow-but-correct guest execution and is NOT evidence of speed comparable to an unscaled lane\n",
           timeout_scale, (unsigned long long)case_timeout_ms());
}

/*
 * The stall detector.
 * progress := captured stdout/stderr grew, OR the process tree consumed CPU -- both needed, and neither may
 * come from this runner's pipes (the remote supervisor heartbeats stderr every 250ms). Walk the tree by parent
 * link, not process group: the supervisor puts the engine in a group of its OWN. Unanswerable counts as
 * progress -- missing evidence must never manufacture a hang. The budget is the UNSCALED per-case budget
 * floored at STALL_FLOOR_MS, so at scale 1 it is >= the wall budget and this detector is inert.
 *
 * "UNANSWERABLE COUNTS AS PROGRESS" IS A POSIX-ARM RULE AND IT DOES NOT CROSS TO WINDOWS. It is safe on
 * Linux, where /proc is the CPU source and its absence means the runner is somewhere it was never meant to
 * be; and it is deliberate on Darwin, where there is no per-tree accounting at all and the detector is
 * simply off, which is no worse than the day before it was written. On Windows it would be neither: the CPU
 * source is a job object this runner CREATED and owns, so a query that fails is a defect in this runner or a
 * handle it has lost, not a host that cannot answer -- and a detector that answers "progress" to every such
 * failure is not a weaker detector, it is an ABSENT one wearing a detector's name. That is exactly the
 * five-hour-hang hole this code exists to close, so the Windows arm treats an unanswerable CPU source as a
 * FAILURE OF THE CASE (run_guest status 4, with the Win32 error named), and refuses to start the suite at all
 * if the source cannot be read once before the first case runs. See windows_job_cpu_ticks() and
 * windows_stall_source_selftest() below.
 */
enum { STALL_FLOOR_MS = 60000, STALL_SAMPLE_MS = 1000, STALL_PROCESS_MAX = 4096 };

static uint64_t stall_timeout_ms(void) {
    uint64_t budget = suite_case_timeout_ms();
    if (budget < STALL_FLOOR_MS) budget = STALL_FLOOR_MS;
    /* 0 rather than an unreachable number, so the unscaled case skips the sampling in the poll loop. */
    return budget >= case_timeout_ms() ? 0 : budget;
}

#if defined(__linux__)
typedef struct process_row {
    long pid;
    long parent;
    unsigned long long ticks;
    int descends;
} process_row;

/* User+system ticks for `root` and its live descendants, or -1 when the host cannot answer. Ticks are
   monotone, so a sum that has not moved proves no counted process ran. */
static long long tree_cpu_ticks(long root) {
    /* static: far too large for a frame in the per-case poll loop, and the runner runs one case at a time. */
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
    /* /proc is not ordered parent-before-child, so close over the parent links to a fixed point. */
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
#elif !defined(_WIN32)
static long long tree_cpu_ticks(long root) {
    (void)root;
    /* No portable per-tree CPU accounting; off beats output-only, which would make a Darwin lane stricter
       than it is today. */
    return -1;
}
#endif

#if defined(_WIN32)
/* CPU consumed by every process in the case's job object, in 100ns units, or -1 with `error` set to the
 * Win32 code. This is a STRICTLY BETTER source than the /proc walk it replaces, and the difference is worth
 * stating because it is what lets the Windows arm be strict where the Linux arm cannot be:
 *
 *   * it is exact. The Linux walk enumerates /proc and reconstructs descent from parent links, which loses a
 *     process that exits between readdir and open and truncates at STALL_PROCESS_MAX. A job object is
 *     kernel-maintained containment: every descendant is in it by construction, including ones this runner
 *     never saw.
 *   * it does not forget. TotalUserTime/TotalKernelTime accumulate the time of processes that have ALREADY
 *     EXITED, so a case that churns short-lived children still reads as progress. The tick sum does not.
 *   * it cannot be escaped. A guest that calls setsid/setpgid walks out of a process group; nothing short of
 *     a breakaway limit (which is not set) leaves a job.
 *
 * Monotone, so a sum that has not moved proves no process in the tree ran. */
static long long windows_job_cpu_ticks(HANDLE job, unsigned long *error) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
    DWORD returned = 0;
    memset(&accounting, 0, sizeof accounting);
    if (job == NULL) {
        *error = ERROR_INVALID_HANDLE;
        return -1;
    }
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof accounting,
                                   &returned) ||
        returned != sizeof accounting) {
        *error = GetLastError();
        return -1;
    }
    *error = 0;
    return (long long)((unsigned long long)accounting.TotalUserTime.QuadPart +
                       (unsigned long long)accounting.TotalKernelTime.QuadPart);
}

/* Read the stall detector's CPU source ONCE, from an empty job this runner just made, before any case runs.
 * An empty job answers 0, not an error, so the only way this fails is that the primitive itself is
 * unavailable -- and finding that out here costs one line at the top of the log, whereas finding it out per
 * case costs a corpus of failures that all say the same thing. Nothing is inferred from success beyond "the
 * query works": the per-case query is still checked every time it is made. */
static int windows_stall_source_selftest(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    unsigned long error = 0;
    long long ticks;
    if (job == NULL) {
        fprintf(stderr,
                "matrix-runner: cannot create a job object (error %lu). The job object is this runner's "
                "process-group equivalent AND the stall detector's CPU source; without it a runaway case has "
                "no kill switch and a hung case is not detectable. Refusing to run rather than running with an "
                "inert hang detector.\n",
                GetLastError());
        return 1;
    }
    ticks = windows_job_cpu_ticks(job, &error);
    (void)CloseHandle(job);
    if (ticks < 0) {
        fprintf(stderr,
                "matrix-runner: the stall detector's CPU source is unreadable on this host: "
                "QueryInformationJobObject(JobObjectBasicAccountingInformation) failed with error %lu on an "
                "empty job. Refusing to run rather than running with an inert hang detector.\n",
                error);
        return 1;
    }
    return 0;
}
#endif

/* HL_MATRIX_SCRATCH_DIR backs the guest's /tmp, and the filesystem beneath it decides whether memfd seals and
   statx btime match the goldens; only .github/workflows/linux.yml exports it. Not forced from CMake: some
   suites' goldens were captured against the build tree's own filesystem. */
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
        /* TMPFS_MAGIC spelled out, not from <linux/magic.h>: the tool builds against a bare libc. */
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

#if !defined(_WIN32)
static uint64_t capture_bytes(const char *output_path, const char *error_path) {
    struct stat status;
    uint64_t total = 0;
    if (stat(output_path, &status) == 0 && status.st_size > 0) total += (uint64_t)status.st_size;
    if (stat(error_path, &status) == 0 && status.st_size > 0) total += (uint64_t)status.st_size;
    return total;
}
#endif

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
    int translation_reuse;
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

#if defined(_WIN32)
/* sig_atomic_t is the right type for both arms: the console control handler runs on a thread of its own
 * rather than on this one, but the only thing crossing is a flag and a HANDLE, and the HANDLE is published
 * before the child is resumed and cleared after it is waited for. */
static volatile sig_atomic_t interrupted_signal;
static HANDLE volatile active_job;

/* Ctrl-C / Ctrl-Break / console close / logoff / shutdown. Killing the JOB rather than the child is the
 * point: it takes the engine, the guest, and anything either of them started, which `kill(-pid)` only
 * approximates and only until something calls setsid. Returning FALSE lets the default handler run, so the
 * runner still dies -- this hook exists to make sure it does not leave a guest behind when it does. */
static BOOL WINAPI interrupt_runner(DWORD control_type) {
    HANDLE job = active_job;
    interrupted_signal = (sig_atomic_t)(control_type == CTRL_C_EVENT ? 2 : 15);
    if (job != NULL) (void)TerminateJobObject(job, 1);
    return FALSE;
}

static int install_interrupt_handlers(void) {
    return SetConsoleCtrlHandler(interrupt_runner, TRUE) ? 0 : 1;
}
#else
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
#endif

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
#if defined(_WIN32)
    /* There is no wait(-1) here: NT has no notion of "any child", and a process handle this runner did not
     * open is not waitable at all. The leak this check is looking for cannot survive anyway -- every case
     * runs inside a job object created with KILL_ON_JOB_CLOSE, so closing the job at the end of the case
     * terminates anything the engine left running, whether or not this runner ever knew about it. That is a
     * stronger guarantee than the POSIX arm's, which only reports the leak. What it is NOT is a measurement,
     * so nothing is claimed: `clean` here means "structurally impossible", and the descriptor and thread
     * counts stay unmeasured (-1) exactly as they are on Darwin. */
    int child_clean = 1;
#else
    int child_status;
    pid_t child = waitpid(-1, &child_status, WNOHANG);
    int child_clean = child < 0 && errno == ECHILD;
#endif
    int descriptor_clean = baseline.descriptors < 0 || current.descriptors == baseline.descriptors;
    int thread_clean = baseline.threads < 0 || current.threads == baseline.threads;
    if (child_clean && descriptor_clean && thread_clean) return 1;
    if (hl_tool_config_github_actions())
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
#if defined(_WIN32)
    /* GetTickCount64, not clock_gettime: mingw-w64's clock_gettime lives in libwinpthread, and every use
     * here is a millisecond difference against a deadline. 64-bit, so it does not wrap, and monotonic by
     * definition -- a wall-clock source would let an NTP step forward look like a case that timed out. */
    return (uint64_t)GetTickCount64();
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
#endif
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

/* The compat matrix runs against production engines of more than one object format: the ELF Linux engine
   (test-linux-production-typed), the Mach-O macOS engine (e2e-compat), and eventually a PE Windows engine.
   A handful of cases exercise behavior the macOS engine cannot emulate (deliberate PROT_NONE
   non-enforcement, Darwin-absent child-subreaper, netns bridging, deep JIT re-translate). Those are marked
   `excluded-macos` so they are skipped ONLY when the engine binary under test is Mach-O, while the other
   engines still run and enforce them -- no coverage is lost anywhere else.

   This used to be a BOOLEAN, `engine_is_macho`, whose default arm was `return 1`: ELF meant Linux and
   ANYTHING ELSE meant macOS. A PE image starts `4D 5A` ("MZ"), so a Windows engine classified itself as
   macOS and silently inherited every excluded-macos row -- ~60 cases across the manifests, skipped on the
   first day the lane existed, under a reason (no OFD locks, no F_SETPIPE_SZ, no child-subreaper, BSD pipe
   semantics) that describes Darwin and says nothing whatever about NT. A silent misclassification that
   presents as a green lane is the exact failure this project keeps writing down as unacceptable, so the
   default arm is gone: an unrecognised magic is fatal and names the path and the four bytes it read. */
typedef enum { ENGINE_ELF, ENGINE_MACHO, ENGINE_PE } engine_format;

/* A host may serve one guest ISA and not the other: the Windows lane has a PE engine for x86_64 guests and
 * NO aarch64 engine at all (cmake/Phase2Production.cmake declares one target, not two). Spelling that as the
 * literal engine path `-` is the honest way to say it, and the ONLY thing it is allowed to do is make the
 * absence explicit -- load_manifest() below refuses a suite that contains a case for the missing ISA rather
 * than skipping it. A silently narrower lane that still reports "all cases passed" is the failure this
 * runner keeps being rewritten to prevent; passing `-` to a suite that needs the ISA is a hard error naming
 * the first case that needs it. */
static int engine_absent(const char *engine_path) {
    return strcmp(engine_path, "-") == 0;
}

static int engine_format_of(const char *engine_path, engine_format *out) {
    unsigned char magic[4] = {0};
    int fd = open(engine_path, O_RDONLY | HL_O_BINARY);
    ssize_t got;
    if (fd < 0) {
        fprintf(stderr, "matrix-runner: cannot open engine %s to identify its object format\n", engine_path);
        return 1;
    }
    got = read(fd, magic, sizeof(magic));
    (void)close(fd);
    if (got != (ssize_t)sizeof(magic)) {
        fprintf(stderr, "matrix-runner: engine %s is shorter than a 4-byte magic\n", engine_path);
        return 1;
    }
    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        *out = ENGINE_ELF;
        return 0;
    }
    /* Mach-O 64/32 and their byte-swapped forms, plus the universal (fat) magic. */
    if ((magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
        (magic[0] == 0xce && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
        (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa && magic[3] == 0xcf) ||
        (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa && magic[3] == 0xce) ||
        (magic[0] == 0xca && magic[1] == 0xfe && magic[2] == 0xba && magic[3] == 0xbe) ||
        (magic[0] == 0xbe && magic[1] == 0xba && magic[2] == 0xfe && magic[3] == 0xca)) {
        *out = ENGINE_MACHO;
        return 0;
    }
    if (magic[0] == 0x4d && magic[1] == 0x5a) { /* "MZ" */
        *out = ENGINE_PE;
        return 0;
    }
    fprintf(stderr,
            "matrix-runner: %s is not an ELF, Mach-O or PE image (magic %02x %02x %02x %02x). The manifest's "
            "per-engine exclusions are keyed on the object format, so guessing one would silently apply "
            "another host's exclusion set. Refusing to run.\n",
            engine_path, magic[0], magic[1], magic[2], magic[3]);
    return 1;
}

static const char *engine_format_name(engine_format format) {
    switch (format) {
    case ENGINE_ELF: return "ELF";
    case ENGINE_MACHO: return "Mach-O";
    case ENGINE_PE: return "PE";
    }
    return "?";
}

/* Refuse, by name, the first case that needs an ISA this host has no engine for. Called after a row's `isa`
 * has been decided and before it is counted, so the message can say which case and which ISA. */
static int isa_servable(const suite_case *item, int have_aarch64, int have_x86_64, const char *manifest_path) {
    const char *missing = NULL;
    if (item->isa != ISA_X86_64 && !have_aarch64) missing = "aarch64";
    if (item->isa != ISA_AARCH64 && !have_x86_64) missing = "x86_64";
    if (missing == NULL) return 1;
    fprintf(stderr,
            "matrix-runner: %s: case `%s` runs on %s, and this invocation was given `-` for the %s engine -- "
            "there is no engine on this host that can run it. A suite is run whole or not at all: dropping the "
            "case would report a green suite that silently covers less than its manifest says. Register only "
            "suites every one of whose cases this host can serve.\n",
            manifest_path, item->name, missing, missing);
    return 0;
}

static int load_manifest(const char *root, suite_case cases[CASE_MAX], size_t *case_count, size_t *excluded,
                         engine_format host_format, int have_aarch64, int have_x86_64) {
    char path[1024];
    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    size_t undisposed = 0;
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
            cases[*case_count].translation_reuse = 0;
            cases[*case_count].environment[0] = 0;
            cases[*case_count].argument[0] = 0;
            if (snprintf(cases[*case_count].name, sizeof(cases[*case_count].name), "%s", fields[0]) >=
                    (int)sizeof(cases[*case_count].name) ||
                snprintf(cases[*case_count].source, sizeof(cases[*case_count].source), "%s", fields[0]) >=
                    (int)sizeof(cases[*case_count].source) ||
                snprintf(cases[*case_count].expected, sizeof(cases[*case_count].expected), "%s", fields[4]) >=
                    (int)sizeof(cases[*case_count].expected))
                goto invalid;
            if (!isa_servable(&cases[*case_count], have_aarch64, have_x86_64, path)) goto invalid_reported;
            (*case_count)++;
            continue;
        }
        /* Field 12 is the disposition, and it holds exactly ONE token.
         *
         * `excluded-macos` and `excluded-windows` are PER-ENGINE: each drops out only on the engine whose
         * object format it names, and is parsed and run exactly like an active case on every other engine,
         * so no other host loses coverage. Every other `excluded-*` token drops everywhere.
         *
         * THE ONE-TOKEN CONSTRAINT, recorded here because this is where it bites. There is no way to spell
         * "excluded on macOS AND on Windows": the obvious `excluded-macos,excluded-windows` would match
         * neither per-engine arm, fall into the generic `excluded-` arm, and be skipped on ALL THREE
         * engines -- silently deleting Linux coverage, which is the exact loss the per-engine mechanism was
         * built to prevent. `excluded-known-bug` has the same effect and is not a substitute for the same
         * reason. So a comma is rejected outright below rather than mis-parsed. Widening the column to a
         * comma-separated SET is a small parser change here and in tools/linux_matrix.c, and it should be
         * made the day the first row needs it -- not before, since a format nothing exercises is a format
         * nothing tests. Until then the loud parse error is the guard. */
        if (strchr(fields[11], ',') != NULL) {
            fprintf(stderr,
                    "matrix-runner: %s: disposition `%s` holds more than one token. Column 12 is a single "
                    "token; a comma here would be read as an unknown `excluded-*` and skip the case on the "
                    "Linux engine too. Widen the parser in both runners first.\n",
                    fields[0], fields[11]);
            goto invalid_reported;
        }
        int macos_only = strcmp(fields[11], "excluded-macos") == 0;
        int windows_only = strcmp(fields[11], "excluded-windows") == 0;
        /* Runs here unless this engine is the one the token names. Note what this does NOT do: it does not
         * treat a macOS exclusion as covering Windows. The macOS set is Darwin-shaped -- no OFD locks, no
         * F_SETPIPE_SZ, no child-subreaper, BSD pipe semantics -- and says nothing about NT, where the
         * engine emulates rather than passes through. A PE engine therefore RUNS every excluded-macos row,
         * and if one of them is genuinely impossible on NT that has to be argued for and written down as
         * its own excluded-windows row. Inheriting another host's skips would hide ~60 cases behind a
         * reason that does not apply, on the first day the lane exists. */
        int runs_here = (macos_only && host_format != ENGINE_MACHO) || (windows_only && host_format != ENGINE_PE);
        if (strncmp(fields[11], "excluded-", 9) == 0 && !runs_here) {
            (*excluded)++;
            continue;
        }
        /* Count what a PE engine is inheriting-but-not-honouring, so the runner can say so out loud. */
        if (macos_only && host_format == ENGINE_PE) undisposed++;
        if (*case_count == CASE_MAX) goto overflow;
        if ((strcmp(fields[11], "active") != 0 && !runs_here) || !relative_path(fields[2]) ||
            !relative_path(fields[9]) || strncmp(fields[9], "expected/", 9) != 0 ||
            (strcmp(fields[6], "-") != 0 && strncmp(fields[6], "argv:", 5) != 0) || !valid_environment(fields[7]) ||
            parse_exit(fields[8], &cases[*case_count].expected_exit) != 0)
            goto invalid;
        /* Rootfs shape is explicit manifest metadata; ABI4 always carries the staged root as typed launch data. */
        cases[*case_count].needs_rootfs = strstr(fields[10], "-rootfs") != NULL;
        cases[*case_count].dynamic_rootfs = strstr(fields[10], "dynamic-rootfs") != NULL;
        cases[*case_count].mapping_data_rootfs = strstr(fields[10], "mapping-data-rootfs") != NULL;
        cases[*case_count].translation_reuse = strstr(fields[10], "translation-reuse") != NULL;
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
        if (!isa_servable(&cases[*case_count], have_aarch64, have_x86_64, path)) goto invalid_reported;
        (*case_count)++;
    }
    /* Loud, every run, because the alternative is a silently smaller lane. These cases carry a Darwin
     * exclusion and NO Windows disposition, so the PE engine runs them; some may well be legitimate
     * excluded-windows rows and some may be real defects, and nobody can tell which from a green tick. */
    if (undisposed != 0)
        fprintf(stderr,
                "matrix-runner: %s: %zu case(s) are excluded on macOS and carry no Windows disposition. This "
                "PE engine RUNS them -- a Darwin exclusion is not a Windows one. If one is genuinely "
                "impossible on NT, give it its own excluded-windows row with a reason; do not widen the "
                "macOS row.\n",
                path, undisposed);
    free(line);
    fclose(file);
    return *case_count == 0;
invalid:
    fprintf(stderr, "matrix-runner: invalid manifest row near active case %zu\n", *case_count + 1);
invalid_reported:
    free(line);
    fclose(file);
    return 1;
    /* Not `invalid`: the manifest is fine, so do not send the reader hunting for a malformed row. */
overflow:
    fprintf(stderr,
            "matrix-runner: %s holds more than %d active cases, which is CASE_MAX in tools/matrix_runner.c. The "
            "manifest is not malformed; raise CASE_MAX (the array is a fixed-size main() local) and rebuild.\n",
            path, CASE_MAX);
    free(line);
    fclose(file);
    return 1;
}

#if !defined(_WIN32)
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
#endif

static int read_capture(const char *path, unsigned char *buffer, size_t limit, size_t *size) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | HL_O_BINARY);
    *size = 0;
    if (descriptor < 0) return 1;
    while (*size < limit) {
        ssize_t count = read(descriptor, buffer + *size, HL_IO_COUNT(limit - *size));
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

#if !defined(_WIN32)
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
#endif

typedef struct config_wire {
    hl_launch_config config;
    char pool[2048];
    size_t used;
} config_wire;

static int write_full(int fd, const void *buffer, size_t size) {
    const unsigned char *cursor = buffer;
    while (size != 0) {
        ssize_t written = write(fd, cursor, HL_IO_COUNT(size));
        if (written < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

#if defined(_WIN32)
/* The process domain namespaces a launch's SysV keys, abstract sockets and network namespace against every
 * other launch on the machine; what it needs is uniqueness, not secrecy. BCryptGenRandom would be the
 * cryptographic answer and would put bcrypt.dll in this tool's import table; ProcessPrng likewise. Neither
 * is warranted for a collision domain, so the two words are mixed from three sources that cannot all repeat
 * across two runs on one host: the boot-relative performance counter (different every call), the process id,
 * and a per-call counter (different within one process even if the counter has not ticked). */
static int process_domain(uint64_t identity[2]) {
    static uint64_t sequence;
    LARGE_INTEGER counter;
    FILETIME now;
    uint64_t stamp;
    if (!QueryPerformanceCounter(&counter)) return -1;
    GetSystemTimeAsFileTime(&now);
    stamp = ((uint64_t)now.dwHighDateTime << 32) | (uint64_t)now.dwLowDateTime;
    sequence++;
    identity[0] = (uint64_t)counter.QuadPart ^ (stamp * UINT64_C(0x9e3779b97f4a7c15));
    identity[1] = ((uint64_t)GetCurrentProcessId() << 32) ^ (sequence * UINT64_C(0xc2b2ae3d27d4eb4f)) ^ stamp;
    /* The launch config rejects an all-zero domain, and a zero here would be a silent collision with every
     * other zero rather than a diagnosable failure. */
    return (identity[0] | identity[1]) != 0 ? 0 : -1;
}
#else
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
#endif

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
        const char *debug_log = hl_tool_config_log_selector();
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
                    hl_mkdir(isolated_cache, 0700) != 0)
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
#if defined(_WIN32)
    /* mingw-w64's mkstemp already creates with _S_IREAD|_S_IWRITE and no sharing beyond this process; there
     * is no fchmod, and the POSIX mode bits it would set have no NT meaning. The file is a launch config in
     * the build tree, not a secret. */
    (void)_setmode(fd, _O_BINARY);
    if (write_full(fd, &wire.config, sizeof wire.config) == 0 && write_full(fd, wire.pool, wire.used) == 0 &&
        close(fd) == 0)
        return 0;
#else
    if (fchmod(fd, 0600) == 0 && write_full(fd, &wire.config, sizeof wire.config) == 0 &&
        write_full(fd, wire.pool, wire.used) == 0 && close(fd) == 0)
        return 0;
#endif
    result = errno;
    if (fd >= 0) (void)close(fd);
    (void)unlink(path);
    errno = result;
    return 1;
}

/*
 * Everything a case needs on disk before a process exists: a private scratch directory (which is also the
 * guest's /tmp), the two capture files inside it, and the typed launch config. Shared by both supervision
 * arms, so a change to the workspace cannot apply to one host and not the other. On failure nothing is left
 * behind and the caller returns 1.
 */
static int open_case_workspace(const char *binary_root, const char *guest, const char *argument, const char *rootfs,
                               const char *environment, char scratch[1024], char capture_output[1200],
                               char capture_error[1200], char config_path[1024]) {
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
    const char *scratch_base = hl_tool_config_matrix_scratch();
    int requested = scratch_base != NULL && scratch_base[0] != 0, rejected = 0;
    struct stat base_stat;
    if (!requested || stat(scratch_base, &base_stat) != 0 || !S_ISDIR(base_stat.st_mode) ||
        access(scratch_base, W_OK) != 0) {
        rejected = requested;
        scratch_base = binary_root;
    }
    scratch_observe(scratch_base, requested && !rejected, rejected);
    if (snprintf(scratch, sizeof(char[1024]), "%s/.matrix-scratch-XXXXXX", scratch_base) >= 1024 ||
        mkdtemp(scratch) == NULL)
        return 1;
    if (snprintf(capture_output, sizeof(char[1200]), "%s/stdout", scratch) >= 1200 ||
        snprintf(capture_error, sizeof(char[1200]), "%s/stderr", scratch) >= 1200) {
        remove_tree(scratch);
        return 1;
    }
    if (make_config(binary_root, guest, argument, rootfs, environment, scratch, config_path) != 0) {
        remove_tree(scratch);
        return 1;
    }
    return 0;
}

#if !defined(_WIN32)
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
    if (open_case_workspace(binary_root, guest, argument, rootfs, environment, scratch, capture_output, capture_error,
                            config_path) != 0)
        return 1;
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
        /* Once a second, not every 10ms poll. `exited` guards the drain tail: no tree left to sample. */
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
                /* Unlike the wall-clock path, recover what the guest DID print: a hang is diagnosed from
                   where it got to. */
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
#else
/* ---------------------------------------------------------------------------
 * The Windows supervision arm.
 *
 * Same contract as the POSIX arm above, primitive for primitive:
 *
 *   fork + execlp   -> CreateProcessA, created SUSPENDED so the job below can
 *                      contain it before it runs a single instruction.
 *   setpgid         -> a job object. Strictly stronger: a job cannot be escaped
 *                      by setsid/setpgid, contains grandchildren the runner
 *                      never saw, and KILL_ON_JOB_CLOSE makes "the case is over"
 *                      and "nothing it started survives" the same event.
 *   waitpid(WNOHANG)-> WaitForSingleObject(process, 0) + GetExitCodeProcess.
 *   kill(-pid, ...) -> TerminateJobObject. One call, whole tree, no grace-then-
 *                      force dance: the child here is an engine being abandoned,
 *                      not a service being asked to shut down.
 *   pipe + poll     -> NOTHING. The engine's stdout and stderr are handed the
 *                      capture FILES directly. On POSIX those pipes exist to keep
 *                      a remote supervisor's forwarded stream flowing; there is
 *                      no supervisor on this host, and the capture files were
 *                      always the authoritative bytes (read_capture() overwrites
 *                      whatever the pipes drained). Redirecting straight to them
 *                      removes a copy and the deadlock class that comes with it.
 *   /proc CPU walk  -> the job's own accounting, which is exact rather than
 *                      reconstructed. See windows_job_cpu_ticks().
 *
 * ONE BEHAVIOURAL DIFFERENCE, stated rather than smoothed over: an unreadable
 * CPU source FAILS THE CASE here (status 4). It does not count as progress.
 * ------------------------------------------------------------------------- */

/* Job + process for one case. The job is created before the process and closed after it, so the window in
   which a guest exists outside a job is exactly zero instructions wide. */
typedef struct windows_child {
    HANDLE job;
    HANDLE process;
    HANDLE thread;
} windows_child;

static void windows_child_close(windows_child *child) {
    if (child->thread != NULL) (void)CloseHandle(child->thread);
    /* Closing the last handle to a KILL_ON_JOB_CLOSE job terminates everything still in it. */
    if (child->process != NULL) (void)CloseHandle(child->process);
    if (child->job != NULL) (void)CloseHandle(child->job);
    child->thread = NULL;
    child->process = NULL;
    child->job = NULL;
}

static void windows_child_terminate(windows_child *child) {
    if (child->job != NULL) {
        (void)TerminateJobObject(child->job, 1);
        if (child->process != NULL) (void)WaitForSingleObject(child->process, 5000);
    }
    if (active_job == child->job) active_job = NULL;
    windows_child_close(child);
}

/* Captured bytes read from the OPEN handles rather than by stat()ing the paths. NT updates a file's
   directory entry lazily, so a path-based size can lag the writes an engine has already made -- and a size
   that lags is a size that has not changed, which the stall detector would read as "no output". */
static uint64_t windows_capture_bytes(HANDLE output, HANDLE error) {
    LARGE_INTEGER size;
    uint64_t total = 0;
    if (output != INVALID_HANDLE_VALUE && GetFileSizeEx(output, &size) && size.QuadPart > 0)
        total += (uint64_t)size.QuadPart;
    if (error != INVALID_HANDLE_VALUE && GetFileSizeEx(error, &size) && size.QuadPart > 0)
        total += (uint64_t)size.QuadPart;
    return total;
}

/* A capture file opened for the CHILD to write: inheritable, and opened for append so two handles onto the
   same file (there are not, but the engine may reopen its own stderr) cannot rewind each other. */
static HANDLE windows_capture_open(const char *path) {
    SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    return CreateFileA(path, FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* `<engine> --configfile <config>`, quoted. CreateProcess takes one string and re-splits it with the CRT's
   rules, so every argument is quoted unconditionally: build directories on this host routinely contain a
   space, and an unquoted path silently becomes two arguments and an engine that reports a missing file. */
static int windows_command_line(const char *engine, const char *config_path, char *out, size_t limit) {
    int written = snprintf(out, limit, "\"%s\" --configfile \"%s\"", engine, config_path);
    return written < 0 || (size_t)written >= limit;
}

static int run_guest(const char *bridge, const char *engine, const char *guest, const char *argument,
                     const char *rootfs, const char *environment, const char *binary_root, capture *result) {
    char config_path[1024], scratch[1024], capture_output[1200], capture_error[1200], command[2600];
    uint64_t deadline, stall_budget = stall_timeout_ms(), stall_bytes = 0, stall_stamp, stall_sampled;
    long long stall_ticks = -1;
    unsigned long cpu_error = 0;
    windows_child child = {NULL, NULL, NULL};
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE output_handle = INVALID_HANDLE_VALUE, error_handle = INVALID_HANDLE_VALUE;
    DWORD exit_code = 0;
    int outcome = 1;
    memset(result, 0, sizeof(*result));
    result->output = malloc(OUTPUT_MAX);
    result->error = malloc(ERROR_MAX);
    if (result->output == NULL || result->error == NULL) return 1;
    /* There is no `mac` forwarder on this host and no hl-remote-supervisor built for it, so the only bridge
       this arm can honour is the direct one. Refuse anything else by name rather than ignoring it: a lane
       configured to forward somewhere, silently running locally instead, would report the wrong machine's
       results. */
    if (strcmp(bridge, "env") != 0) {
        fprintf(stderr,
                "matrix-runner: bridge `%s` is not available on this host. The Windows arm launches the engine "
                "directly (there is no forwarder and no remote supervisor here), so `env` is the only bridge it "
                "can honour; running locally under another bridge's name would attribute these results to the "
                "wrong machine.\n",
                bridge);
        return 1;
    }
    if (open_case_workspace(binary_root, guest, argument, rootfs, environment, scratch, capture_output, capture_error,
                            config_path) != 0)
        return 1;
    if (windows_command_line(engine, config_path, command, sizeof command) != 0) goto cleanup;
    output_handle = windows_capture_open(capture_output);
    error_handle = windows_capture_open(capture_error);
    if (output_handle == INVALID_HANDLE_VALUE || error_handle == INVALID_HANDLE_VALUE) goto cleanup;
    child.job = CreateJobObjectW(NULL, NULL);
    if (child.job == NULL) goto cleanup;
    memset(&limits, 0, sizeof limits);
    /* The kill switch, and the reason a runaway case cannot outlive its case: whatever the engine started,
       closing this job ends. */
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) goto cleanup;
    memset(&startup, 0, sizeof startup);
    memset(&process, 0, sizeof process);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_handle;
    startup.hStdError = error_handle;
    /* CREATE_SUSPENDED, then assign, then resume. Assigning after the child is already running leaves a
       window in which it can spawn a grandchild that is outside the job -- which would be both an escaped
       kill switch and a hole in the CPU accounting the stall detector reads. CREATE_NEW_PROCESS_GROUP keeps
       a console Ctrl-C from reaching the engine directly; this runner's handler kills the job instead, so
       there is exactly one path by which a case dies. */
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                        &startup, &process))
        goto cleanup;
    child.process = process.hProcess;
    child.thread = process.hThread;
    if (!AssignProcessToJobObject(child.job, child.process)) {
        windows_child_terminate(&child);
        goto cleanup;
    }
    active_job = child.job;
    if (ResumeThread(child.thread) == (DWORD)-1) {
        windows_child_terminate(&child);
        goto cleanup;
    }
    deadline = monotonic_ms() + case_timeout_ms();
    stall_stamp = monotonic_ms();
    stall_sampled = stall_stamp;
    for (;;) {
        uint64_t now = monotonic_ms();
        DWORD waited;
        if (interrupted_signal != 0 || now >= deadline) {
            windows_child_terminate(&child);
            outcome = 2;
            goto cleanup;
        }
        if (stall_budget != 0 && now - stall_sampled >= STALL_SAMPLE_MS) {
            uint64_t bytes = windows_capture_bytes(output_handle, error_handle);
            long long ticks = windows_job_cpu_ticks(child.job, &cpu_error);
            stall_sampled = now;
            if (ticks < 0) {
                /* THE line this arm exists for. The POSIX walk treats an unanswerable host as progress
                   because on Linux that means /proc is gone and on Darwin it means the source never
                   existed. Here the source is a job object this process created and still holds a handle
                   to, so a failure is a real defect -- and answering "progress" to it would silently
                   disarm the detector for the rest of the run, which is the five-hour hang this code was
                   written to make impossible. Fail the case, name the error, keep going. */
                windows_child_terminate(&child);
                (void)read_capture(capture_output, result->output, OUTPUT_MAX, &result->output_size);
                (void)read_capture(capture_error, result->error, ERROR_MAX, &result->error_size);
                outcome = 4;
                goto cleanup;
            }
            if (bytes != stall_bytes || ticks != stall_ticks) {
                stall_bytes = bytes;
                stall_ticks = ticks;
                stall_stamp = now;
            } else if (now - stall_stamp >= stall_budget) {
                windows_child_terminate(&child);
                /* Unlike the wall-clock path, recover what the guest DID print: a hang is diagnosed from
                   where it got to. */
                (void)read_capture(capture_output, result->output, OUTPUT_MAX, &result->output_size);
                (void)read_capture(capture_error, result->error, ERROR_MAX, &result->error_size);
                outcome = 3;
                goto cleanup;
            }
        }
        /* 10ms, matching the POSIX poll tick, so the deadline and stall sampling have the same granularity
           on both hosts. */
        waited = WaitForSingleObject(child.process, 10);
        if (waited == WAIT_OBJECT_0) break;
        if (waited != WAIT_TIMEOUT) {
            windows_child_terminate(&child);
            goto cleanup;
        }
    }
    if (!GetExitCodeProcess(child.process, &exit_code)) {
        windows_child_terminate(&child);
        goto cleanup;
    }
    result->wait_status = (int)exit_code;
    active_job = NULL;
    /* Handles closed before the captures are read: the engine is gone, and a still-open inheritable write
       handle would keep the file's last extent uncommitted on some filesystems. */
    windows_child_close(&child);
    (void)CloseHandle(output_handle);
    (void)CloseHandle(error_handle);
    output_handle = INVALID_HANDLE_VALUE;
    error_handle = INVALID_HANDLE_VALUE;
    if (read_capture(capture_output, result->output, OUTPUT_MAX, &result->output_size) != 0 ||
        read_capture(capture_error, result->error, ERROR_MAX, &result->error_size) != 0)
        outcome = 1;
    else
        outcome = 0;
cleanup:
    if (outcome == 4)
        fprintf(stderr,
                "matrix-runner: the stall detector could not read the case's job-object CPU accounting "
                "(QueryInformationJobObject error %lu). Treating this as a FAILURE of the case: an "
                "unreadable CPU source makes the hang detector inert, and an inert hang detector is how a "
                "hung case becomes a lane that runs until CI's own wall clock kills it.\n",
                cpu_error);
    if (output_handle != INVALID_HANDLE_VALUE) (void)CloseHandle(output_handle);
    if (error_handle != INVALID_HANDLE_VALUE) (void)CloseHandle(error_handle);
    windows_child_close(&child);
    active_job = NULL;
    (void)unlink(config_path); /* Engine normally unlinks immediately; covers pre-launch failure. */
    remove_tree(scratch);
    return outcome;
}
#endif

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
    int input = open(source, O_RDONLY | HL_O_BINARY), output = -1;
    if (input < 0) return 1;
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL | HL_O_BINARY, 0755);
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
            ssize_t written = write(output, buffer + offset, HL_IO_COUNT((size_t)count - offset));
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
        if (hl_mkdir(path, 0755) != 0 && errno != EEXIST) return 1;
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
        snprintf(staged, sizeof staged, "%s/bin/guest", rootfs) >= (int)sizeof staged || hl_mkdir(bin, 0755) != 0 ||
        hl_mkdir(dev, 0755) != 0 || hl_mkdir(pts, 0755) != 0 || hl_mkdir(tmp, 01777) != 0 ||
        copy_file(guest, staged) != 0)
        return 1;
    if (mapping_data) {
        unsigned char bytes[12288];
        int descriptor;
        memset(bytes, 0x2a, sizeof(bytes));
        if (snprintf(data, sizeof data, "%s/data", rootfs) >= (int)sizeof data) return 1;
        descriptor = open(data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | HL_O_BINARY, 0600);
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

/*
 * The guest emits a marker immediately before each exit_group. With launches
 * serialized by waitpid, the next translate log belongs to that marked
 * process. A no-exec child has already inherited and executed the warmed
 * corpus, so its total translation count bounds the work rebuilt after fork.
 * Exec children remain correctness/control samples and are not budgeted.
 */
static int translation_reuse_matches(const suite_case *item, const char *isa, const capture *result) {
    enum { REUSE_BLOCK_BUDGET = 128 };

    const char marker[] = "[cache-reuse] kind=";
    const char blocks[] = "[hl:translate] blocks=";
    char *text, *line, *save = NULL;
    uint64_t parent = 0, noexec_max = 0, exec_max = 0;
    unsigned fork_count = 0, clone_count = 0, exec_count = 0;
    int kind = 0;
    if (!item->translation_reuse || !hl_tool_config_matrix_translation_reuse()) return 1;
    text = malloc(result->error_size + 1u);
    if (text == NULL) return 0;
    memcpy(text, result->error, result->error_size);
    text[result->error_size] = 0;
    for (line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        const char *found = strstr(line, marker);
        if (found != NULL) {
            const char *name = found + sizeof marker - 1u;
            kind = strcmp(name, "fork") == 0     ? 1
                   : strcmp(name, "clone3") == 0 ? 2
                   : strcmp(name, "exec") == 0   ? 3
                   : strcmp(name, "parent") == 0 ? 4
                                                 : 0;
            continue;
        }
        found = strstr(line, blocks);
        if (found != NULL && kind != 0) {
            char *end = NULL;
            uint64_t count = strtoull(found + sizeof blocks - 1u, &end, 10);
            if (end == found + sizeof blocks - 1u) continue;
            if (kind == 1) {
                fork_count++;
                if (count > noexec_max) noexec_max = count;
            } else if (kind == 2) {
                clone_count++;
                if (count > noexec_max) noexec_max = count;
            } else if (kind == 3) {
                exec_count++;
                if (count > exec_max) exec_max = count;
            } else {
                parent = count;
            }
            kind = 0;
        }
    }
    free(text);
    fprintf(stderr,
            "matrix-runner: %s [%s] translation reuse parent=%llu noexec_max=%llu budget=%u "
            "exec_max=%llu samples=%u/%u/%u\n",
            item->name, isa, (unsigned long long)parent, (unsigned long long)noexec_max, REUSE_BLOCK_BUDGET,
            (unsigned long long)exec_max, fork_count, clone_count, exec_count);
    return parent != 0 && fork_count == 4 && clone_count == 4 && exec_count == 4 && noexec_max <= REUSE_BLOCK_BUDGET;
}

static void diagnostic(const suite_case *item, const char *isa, const char *reason, const capture *result) {
    if (hl_tool_config_github_actions())
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

/* A bare "timeout" cannot separate "hung" from "scale too low". Name the budget only when scaled. */
static const char *timeout_reason(void) {
    static char text[96];
    if (timeout_scale == 1) return "timeout";
    (void)snprintf(text, sizeof text, "timeout after %llums (HL_MATRIX_TIMEOUT_SCALE=%lu)",
                   (unsigned long long)case_timeout_ms(), timeout_scale);
    return text;
}

/* A distinct verdict from a timeout: the case did not run out of time, it stopped doing anything. */
static const char *stall_reason(void) {
    static char text[192];
    (void)snprintf(text, sizeof text,
                   "HUNG: no guest output and no CPU anywhere in its process tree for %llums (the %llums per-case "
                   "budget had NOT expired, so this is a hang, not slow execution)",
                   (unsigned long long)stall_timeout_ms(), (unsigned long long)case_timeout_ms());
    return text;
}

#if defined(_WIN32)
/* A third verdict, distinct from both "hung" and "timed out": the detector itself could not answer. The
   detail is already on stderr from run_guest; this is the one-line reason that lands in the per-case
   summary and, on CI, in the ::error annotation. */
static const char *stall_unanswerable_reason(void) {
    return "STALL DETECTOR UNANSWERABLE: the case's job-object CPU accounting could not be read, so a hang "
           "would have been undetectable for the rest of this run";
}
#endif

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
    char *saved_log = NULL;
    int measure_reuse = item->translation_reuse && hl_tool_config_matrix_translation_reuse();
    if (measure_reuse) {
        const char *current = hl_tool_config_log_selector();
        if (current != NULL) saved_log = strdup(current);
        if (setenv("HL_LOG", "translate", 1) != 0) {
            free(expected);
            free(saved_log);
            return 1;
        }
    }
    status = run_guest(bridge, engine, item->needs_rootfs ? "/bin/guest" : guest, item->argument,
                       item->needs_rootfs ? rootfs : NULL, item->environment, binary_root, result);
    if (measure_reuse) {
        if (saved_log != NULL)
            (void)setenv("HL_LOG", saved_log, 1);
        else
            (void)unsetenv("HL_LOG");
        free(saved_log);
    }
    if (item->needs_rootfs) remove_rootfs(rootfs);
    int reuse_ok = translation_reuse_matches(item, isa, result);
    if (status != 0 || !exit_matches(result, item->expected_exit) || result->output_size != expected_size ||
        memcmp(result->output, expected, expected_size) != 0 || !reuse_ok) {
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
#if defined(_WIN32)
                   : status == 4 ? stall_unanswerable_reason()
#endif
                   : measure_reuse && !reuse_ok ? "translation reuse threshold exceeded"
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
    /* Per-ISA tallies: one run of one suite has to answer "how did each backend do". */
    size_t aarch64_selected = 0, aarch64_failures = 0, x86_64_selected = 0, x86_64_failures = 0;
    size_t cross_compared = 0, cross_failures = 0, cross_skipped = 0;
    int legs_aarch64 = 0, legs_x86_64 = 0, failed_aarch64 = 0, failed_x86_64 = 0, failed_cross = 0,
        failed_resources = 0;
    const char *only = NULL;
    unsigned long repetitions = 1;
    unsigned long repetition;
    resource_baseline baseline;
    engine_format host_format;
    int have_aarch64, have_x86_64;
    if (install_interrupt_handlers() != 0) return 1;
    note_timeout_scale();
#if defined(_WIN32)
    /* Also before any case runs: prove the hang detector's CPU source answers at all. A detector that
       silently cannot see CPU is worse than no detector, because the lane still claims to have one. */
    if (windows_stall_source_selftest() != 0) return 2;
#endif
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
    /* The disposition of a manifest row is keyed on the OBJECT FORMAT of the engine under test, so the
     * format has to be identified before a single row is parsed -- and identified, not guessed. Both
     * engines are sniffed rather than only the aarch64 one, because "both engines are the same format" was
     * an assumption nothing checked; a mixed pair would silently apply one host's exclusion set to the
     * other's engine. */
    have_aarch64 = !engine_absent(argv[2]);
    have_x86_64 = !engine_absent(argv[4]);
    if (!have_aarch64 && !have_x86_64) {
        fprintf(stderr, "matrix-runner: both engines were given as `-`; there is nothing to run\n");
        return 2;
    }
    {
        engine_format aarch64_format = ENGINE_ELF;
        engine_format x86_64_format = ENGINE_ELF;
        if (have_aarch64 && engine_format_of(argv[2], &aarch64_format) != 0) return 2;
        if (have_x86_64 && engine_format_of(argv[4], &x86_64_format) != 0) return 2;
        if (!have_aarch64) aarch64_format = x86_64_format;
        if (!have_x86_64) x86_64_format = aarch64_format;
        if (aarch64_format != x86_64_format) {
            fprintf(stderr,
                    "matrix-runner: the two engines are different object formats (%s: %s, %s: %s). The "
                    "manifest's per-engine exclusions are one set for both, so this pairing would apply one "
                    "host's skips to the other host's engine.\n",
                    argv[2], engine_format_name(aarch64_format), argv[4], engine_format_name(x86_64_format));
            return 2;
        }
        host_format = aarch64_format;
    }
    if (load_manifest(argv[6], cases, &count, &excluded, host_format, have_aarch64, have_x86_64) != 0) return 1;
    baseline = resource_measure();
    for (index = 0; index < count; ++index) {
        if (interrupted_signal != 0) return 128 + interrupted_signal;
        if (only != NULL && strcmp(only, cases[index].name) != 0) continue;
        selected++;
        /* BOTH ISAs always run: chaining them hid divergence on exactly the host where the backends differ.
         * Verdict unchanged -- either ISA failing still fails the case. */
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
            /* One greppable (case, ISA) line: the per-leg diagnostics interleave with engine stderr. */
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
    /* Repeated by the summary, which is what a reader takes away. */
    if (timeout_scale != 1)
        printf("matrix-runner: per-case timeout was scaled x%lu; this run's timing is not comparable to an "
               "unscaled lane\n",
               timeout_scale);
    /* Unconditional and fixed-shape; "not comparable" is a gap in the identity evidence, not a pass. */
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
