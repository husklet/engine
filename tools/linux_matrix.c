/* mingw-w64 keys its declarations off __STRICT_ANSI__ and hides part of them when a POSIX level is
 * asserted, so the Windows arm asks for nothing and takes the toolchain's default surface. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

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
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/wait.h>
#endif

/* ---------------------------------------------------------------------------
 * Host portability shims; the reasoning is the same as in tools/matrix_runner.c,
 * which is this runner's reference. Only spellings are unified here -- every
 * place where a host genuinely cannot answer is handled at the call site.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
/* NT opens in text mode by default and would rewrite \n as \r\n. Every comparison here is byte-exact
 * against a golden captured on Linux, so a text-mode descriptor fails the corpus for a reason that has
 * nothing to do with the guest. */
#define HL_O_BINARY _O_BINARY
#if !defined(O_CLOEXEC)
#define O_CLOEXEC _O_NOINHERIT
#endif
/* No wait status word on NT: the child's exit code IS the status. Only consulted after a normal
 * completion, so an exit code above 255 (a structured exception, e.g. 0xC0000005) cannot match an
 * expected exit in [0,255] and is printed verbatim. */
#define WIFEXITED(status) (1)
#define WEXITSTATUS(status) (status)
#define HL_IO_COUNT(bytes) ((unsigned)(bytes))

/* mingw-w64 supplies mkstemp, mkdtemp and <dirent.h>, but not getline. This is the POSIX 2008 contract, no
 * more: grow the caller's buffer, keep the newline, return the byte count or -1 at end of file. */
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
#define HL_IO_COUNT(bytes) (bytes)
#endif

/* Per-case hang detector and its host-backend scale; tools/matrix_runner.c is the reference and the knob is
   identical in all six runners. 20s assumes a JIT host; without one the SIGKILL
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
   per-case budget floored at STALL_FLOOR_MS, so at scale 1 it is >= the wall budget and cannot fire.

   "UNANSWERABLE COUNTS AS PROGRESS" IS A POSIX-ARM RULE AND DOES NOT CROSS TO WINDOWS, for the same reason
   spelled out at length in tools/matrix_runner.c: there the CPU source is /proc (absent means the runner is
   somewhere it was never meant to be) or nothing at all (Darwin, where the detector is simply off and no
   worse than before). Here the source is a job object THIS runner created and still holds, so a query that
   fails is a defect in this runner -- and a detector that answers "progress" to it is not a weaker
   detector, it is an absent one wearing a detector's name. The Windows arm therefore FAILS THE CASE, and
   refuses to start at all if the source cannot be read once up front. */
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
#elif !defined(_WIN32)
static long long tree_cpu_ticks(long root) {
    (void)root;
    /* No portable per-tree CPU accounting; off beats output-only, which would be stricter than today. */
    return -1;
}
#endif

#if defined(_WIN32)
/* CPU consumed by every process in the case's job object, in 100ns units, or -1 with `error` set. Exact
 * rather than reconstructed: a job contains every descendant by construction, keeps the time of processes
 * that have already exited, and cannot be escaped by setsid. Monotone, so a sum that has not moved proves
 * nothing in the tree ran. */
static long long job_cpu_ticks(HANDLE job, unsigned long *error) {
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

/* One read of the CPU source, from an empty job, before any case runs. An empty job answers 0 rather than
 * failing, so the only way this fails is that the primitive is unavailable -- one line at the top of the log
 * instead of a corpus of identical per-case failures. */
static int stall_source_selftest(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    unsigned long error = 0;
    long long ticks;
    if (job == NULL) {
        fprintf(stderr,
                "linux-matrix: cannot create a job object (error %lu); it is both the kill switch for a "
                "runaway case and the stall detector's CPU source. Refusing to run rather than running with "
                "an inert hang detector.\n",
                GetLastError());
        return 1;
    }
    ticks = job_cpu_ticks(job, &error);
    (void)CloseHandle(job);
    if (ticks < 0) {
        fprintf(stderr,
                "linux-matrix: the stall detector's CPU source is unreadable on this host "
                "(QueryInformationJobObject error %lu on an empty job). Refusing to run rather than running "
                "with an inert hang detector.\n",
                error);
        return 1;
    }
    return 0;
}
#endif

static uint64_t output_bytes(int descriptor) {
    struct stat status;
    if (fstat(descriptor, &status) != 0 || status.st_size < 0) return 0;
    return (uint64_t)status.st_size;
}

static uint64_t milliseconds(void) {
#if defined(_WIN32)
    /* GetTickCount64, not clock_gettime: mingw-w64's clock_gettime lives in libwinpthread, and every use
     * here is a millisecond difference against a deadline. Monotonic, and 64-bit so it does not wrap. */
    return (uint64_t)GetTickCount64();
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
#endif
}

static int same_file(int actual, const char *expected_path) {
    unsigned char actual_buffer[4096], expected_buffer[4096];
    int expected = open(expected_path, O_RDONLY | O_CLOEXEC | HL_O_BINARY);
    if (expected < 0 || lseek(actual, 0, SEEK_SET) < 0) {
        if (expected >= 0) close(expected);
        return 0;
    }
    for (;;) {
        ssize_t an = read(actual, actual_buffer, HL_IO_COUNT(sizeof(actual_buffer)));
        ssize_t en = read(expected, expected_buffer, HL_IO_COUNT(sizeof(expected_buffer)));
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

#if !defined(_WIN32)
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
#else
/* ---------------------------------------------------------------------------
 * The Windows supervision arm, primitive for primitive against the POSIX one
 * above:
 *
 *   fork + execl    -> CreateProcessA, created SUSPENDED so the job can contain
 *                      it before it runs an instruction.
 *   setpgid         -> a job object, which a guest cannot walk out of and which
 *                      contains grandchildren this runner never saw.
 *   waitpid(WNOHANG)-> WaitForSingleObject(process, 10) + GetExitCodeProcess.
 *   kill(-pid,KILL) -> TerminateJobObject: one call, whole tree.
 *   mkstemp+unlink  -> GetTempFileNameA + FILE_FLAG_DELETE_ON_CLOSE, which is
 *                      the same trick (a file with no name left to open) spelled
 *                      the way NT spells it, since NT cannot unlink an open file
 *                      that was not opened for delete.
 *   /proc CPU walk  -> the job's own accounting; see job_cpu_ticks().
 *
 * The one behavioural difference: an unreadable CPU source FAILS THE CASE here.
 * ------------------------------------------------------------------------- */
static int run_case(const char *engine, const char *guest, const char *golden, int expected_exit) {
    const uint64_t budget = (uint64_t)CASE_TIMEOUT_MS * timeout_scale;
    const uint64_t stall_budget = stall_timeout_ms();
    SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    char directory[MAX_PATH], temporary[MAX_PATH], command[2600];
    HANDLE capture = INVALID_HANDLE_VALUE, job = NULL;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    unsigned long cpu_error = 0;
    uint64_t start, stall_stamp, stall_sampled, stall_bytes = 0;
    long long stall_ticks = -1;
    DWORD exit_code = 0, path_length;
    int output = -1, status = 0, timed_out = 0, stalled = 0, undetectable = 0, written;
    path_length = GetTempPathA((DWORD)sizeof directory, directory);
    if (path_length == 0 || path_length >= sizeof directory || GetTempFileNameA(directory, "hlm", 0, temporary) == 0) {
        fprintf(stderr, "linux-matrix: cannot create a capture file under the host temp directory\n");
        return 1;
    }
    /* DELETE_ON_CLOSE is this arm's `unlink` immediately after `mkstemp`: the file has no reachable name
       once the last handle closes, whether the case passes, fails or is killed. */
    capture = CreateFileA(temporary, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (capture == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "linux-matrix: cannot open capture file %s (error %lu)\n", temporary, GetLastError());
        return 1;
    }
    /* The CRT descriptor takes ownership of the handle, so close(output) is the only release needed; the raw
       handle is still readable from it for the child's stdout. */
    /* _O_BINARY only: _open_osfhandle takes the access mode from the handle, and _O_TEXT (its default when
       nothing is asked for) would strip the \r of any guest byte stream that contains one. */
    output = _open_osfhandle((intptr_t)capture, _O_BINARY);
    if (output < 0) {
        (void)CloseHandle(capture);
        fprintf(stderr, "linux-matrix: cannot bind capture file to a descriptor\n");
        return 1;
    }
    /* Both arguments quoted: CreateProcess re-splits one string with the CRT's rules, and a build directory
       on this host routinely contains a space. */
    written = snprintf(command, sizeof command, "\"%s\" \"%s\"", engine, guest);
    if (written < 0 || (size_t)written >= sizeof command) {
        close(output);
        return 1;
    }
    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) {
        close(output);
        fprintf(stderr, "linux-matrix: cannot create a job object (error %lu)\n", GetLastError());
        return 1;
    }
    memset(&limits, 0, sizeof limits);
    /* The kill switch: closing the job ends anything the engine started, whether or not it was waited for. */
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    memset(&startup, 0, sizeof startup);
    memset(&process, 0, sizeof process);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = (HANDLE)_get_osfhandle(output);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                        &startup, &process)) {
        fprintf(stderr, "linux-matrix: %s: cannot launch %s (error %lu)\n", guest, engine, GetLastError());
        (void)CloseHandle(job);
        close(output);
        return 1;
    }
    /* Assign before resume: a child that is already running can spawn a grandchild outside the job, which
       would be both an escaped kill switch and a hole in the CPU accounting the stall detector reads. */
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == (DWORD)-1) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)CloseHandle(process.hThread);
        (void)CloseHandle(process.hProcess);
        (void)CloseHandle(job);
        close(output);
        fprintf(stderr, "linux-matrix: %s: cannot contain the launched engine in a job object\n", guest);
        return 1;
    }
    start = milliseconds();
    stall_stamp = start;
    stall_sampled = start;
    for (;;) {
        uint64_t now = milliseconds();
        DWORD waited = WaitForSingleObject(process.hProcess, 10);
        if (waited == WAIT_OBJECT_0) {
            if (GetExitCodeProcess(process.hProcess, &exit_code)) status = (int)exit_code;
            break;
        }
        if (waited != WAIT_TIMEOUT) {
            (void)TerminateJobObject(job, 1);
            (void)WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        if (now - start >= budget) {
            timed_out = 1;
            (void)TerminateJobObject(job, 1);
            (void)WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        if (stall_budget != 0 && now - stall_sampled >= STALL_SAMPLE_MS) {
            uint64_t bytes = output_bytes(output);
            long long ticks = job_cpu_ticks(job, &cpu_error);
            stall_sampled = now;
            if (ticks < 0) {
                /* Not "progress". See the note on the stall detector above: this source is a job object this
                   process created and still holds, so a failed query is a defect here -- and answering
                   "progress" would silently disarm the detector for the whole run. */
                undetectable = 1;
                (void)TerminateJobObject(job, 1);
                (void)WaitForSingleObject(process.hProcess, 5000);
                break;
            }
            if (bytes != stall_bytes || ticks != stall_ticks) {
                stall_bytes = bytes;
                stall_ticks = ticks;
                stall_stamp = now;
            } else if (now - stall_stamp >= stall_budget) {
                stalled = 1;
                (void)TerminateJobObject(job, 1);
                (void)WaitForSingleObject(process.hProcess, 5000);
                break;
            }
        }
    }
    (void)CloseHandle(process.hThread);
    (void)CloseHandle(process.hProcess);
    (void)CloseHandle(job);
    if (undetectable)
        fprintf(stderr,
                "%s: STALL DETECTOR UNANSWERABLE -- the case's job-object CPU accounting could not be read "
                "(QueryInformationJobObject error %lu), so a hang would have gone undetected for the rest of "
                "this run. Failing the case rather than continuing with an inert detector.\n",
                guest, cpu_error);
    else if (stalled)
        fprintf(stderr,
                "%s: HUNG -- no stdout and no CPU in its process tree for %llums, with the %llums per-case budget "
                "unexpired (this is a hang, not slow execution)\n",
                guest, (unsigned long long)stall_budget, (unsigned long long)budget);
    else if (timed_out) {
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
#endif

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
#if defined(_WIN32)
    /* Also before any case runs: prove the hang detector's CPU source answers at all. */
    if (stall_source_selftest() != 0) return 2;
#endif
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
