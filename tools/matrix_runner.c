#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

enum { CASE_MAX = 256, FIELD_MAX = 512, OUTPUT_MAX = 1024 * 1024, ERROR_MAX = 64 * 1024, TIMEOUT_MS = 30000 };

typedef enum case_isa { ISA_AARCH64, ISA_X86_64, ISA_BOTH } case_isa;
typedef struct suite_case {
    char name[128];
    char source[256];
    char expected[256];
    char environment[256];
    case_isa isa;
    int expected_exit;
    int needs_rootfs;
} suite_case;
typedef struct capture {
    unsigned char *output;
    size_t output_size;
    unsigned char *error;
    size_t error_size;
    int wait_status;
} capture;

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

static int load_manifest(const char *root, suite_case cases[CASE_MAX], size_t *case_count, size_t *excluded) {
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
        while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r')) line[--size] = 0;
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
            if (*case_count == CASE_MAX || !relative_path(fields[0]) || source_size < 3 ||
                strcmp(fields[0] + source_size - 2, ".c") != 0 || strcmp(fields[2], "aarch64,x86_64") != 0 ||
                !relative_path(fields[4]) || strncmp(fields[4], "golden/", 7) != 0 ||
                parse_exit(fields[3], &cases[*case_count].expected_exit) != 0)
                goto invalid;
            cases[*case_count].isa = ISA_BOTH;
            cases[*case_count].needs_rootfs = 0;
            cases[*case_count].environment[0] = 0;
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
        if (strcmp(fields[11], "excluded-replaced") == 0) {
            (*excluded)++;
            continue;
        }
        if (strcmp(fields[11], "active") != 0 || *case_count == CASE_MAX || !relative_path(fields[2]) ||
            !relative_path(fields[9]) || strncmp(fields[9], "expected/", 9) != 0 || strcmp(fields[6], "-") != 0 ||
            !valid_environment(fields[7]) || parse_exit(fields[8], &cases[*case_count].expected_exit) != 0)
            goto invalid;
        cases[*case_count].needs_rootfs = strstr(fields[10], "alpine-rootfs") != NULL;
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
}

static int drain(int fd, unsigned char *buffer, size_t *size, size_t limit, int *eof) {
    for (;;) {
        ssize_t count;
        if (*size == limit) return 1;
        count = read(fd, buffer + *size, limit - *size);
        if (count > 0) { *size += (size_t)count; continue; }
        if (count == 0) { *eof = 1; return 0; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno != EINTR) return 1;
    }
}

static void terminate(pid_t child) {
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int install_environment(const char *encoded) {
    char copy[256], names[256] = {0}, *cursor;
    size_t names_size = 0;
    if (*encoded == 0) return 0;
    memcpy(copy, encoded, strlen(encoded) + 1);
    cursor = copy;
    while (cursor != NULL) {
        char *next = strchr(cursor, ';');
        char *equals = strchr(cursor, '=');
        if (next != NULL) *next++ = 0;
        if (equals == NULL) return 1;
        *equals++ = 0;
        if (setenv(cursor, equals, 1) != 0) return 1;
        if (names_size != 0) names[names_size++] = ':';
        if (names_size + strlen(cursor) >= sizeof names) return 1;
        memcpy(names + names_size, cursor, strlen(cursor));
        names_size += strlen(cursor);
        names[names_size] = 0;
        cursor = next;
    }
    /* OrbStack's mac bridge forwards only variables named by ORBENV. */
    return setenv("ORBENV", names, 1) != 0;
}

static int run_guest(const char *bridge, const char *engine, const char *guest, const char *rootfs,
                     const char *environment, capture *result) {
    int output_pipe[2], error_pipe[2], output_eof = 0, error_eof = 0, exited = 0;
    uint64_t deadline;
    pid_t child;
    memset(result, 0, sizeof(*result));
    result->output = malloc(OUTPUT_MAX);
    result->error = malloc(ERROR_MAX);
    if (result->output == NULL || result->error == NULL || pipe(output_pipe) != 0 || pipe(error_pipe) != 0) return 1;
    child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        (void)setpgid(0, 0);
        close(output_pipe[0]); close(error_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(error_pipe[1], STDERR_FILENO) < 0) _exit(127);
        close(output_pipe[1]); close(error_pipe[1]);
        if (install_environment(environment) != 0) _exit(127);
        if (rootfs != NULL)
            execlp(bridge, bridge, engine, "--rootfs", rootfs, guest, (char *)NULL);
        else
            execlp(bridge, bridge, engine, guest, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(child, child);
    close(output_pipe[1]); close(error_pipe[1]);
    if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(error_pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        terminate(child); return 1;
    }
    deadline = monotonic_ms() + TIMEOUT_MS;
    while (!exited || !output_eof || !error_eof) {
        struct pollfd descriptors[2] = {{output_pipe[0], POLLIN | POLLHUP, 0}, {error_pipe[0], POLLIN | POLLHUP, 0}};
        pid_t waited;
        if (monotonic_ms() >= deadline) { terminate(child); close(output_pipe[0]); close(error_pipe[0]); return 2; }
        if (poll(descriptors, 2, 10) < 0 && errno != EINTR) { terminate(child); return 1; }
        if (drain(output_pipe[0], result->output, &result->output_size, OUTPUT_MAX, &output_eof) != 0 ||
            drain(error_pipe[0], result->error, &result->error_size, ERROR_MAX, &error_eof) != 0) {
            terminate(child); return 1;
        }
        if (!exited) {
            waited = waitpid(child, &result->wait_status, WNOHANG);
            if (waited == child) exited = 1;
            else if (waited < 0 && errno != EINTR) { terminate(child); return 1; }
        }
    }
    close(output_pipe[0]); close(error_pipe[0]);
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
    if (output < 0) { close(input); return 1; }
    for (;;) {
        ssize_t count = read(input, buffer, sizeof buffer);
        size_t offset = 0;
        if (count == 0) break;
        if (count < 0) { if (errno == EINTR) continue; close(input); close(output); return 1; }
        while (offset < (size_t)count) {
            ssize_t written = write(output, buffer + offset, (size_t)count - offset);
            if (written < 0) { if (errno == EINTR) continue; close(input); close(output); return 1; }
            offset += (size_t)written;
        }
    }
    return close(input) != 0 || close(output) != 0;
}

static int stage_rootfs(const char *binary_root, const char *guest, char rootfs[1024]) {
    char bin[1024], dev[1024], pts[1024], tmp[1024], staged[1024];
    if (snprintf(rootfs, 1024, "%s/.rootfs-XXXXXX", binary_root) >= 1024 || mkdtemp(rootfs) == NULL ||
        snprintf(bin, sizeof bin, "%s/bin", rootfs) >= (int)sizeof bin ||
        snprintf(dev, sizeof dev, "%s/dev", rootfs) >= (int)sizeof dev ||
        snprintf(pts, sizeof pts, "%s/dev/pts", rootfs) >= (int)sizeof pts ||
        snprintf(tmp, sizeof tmp, "%s/tmp", rootfs) >= (int)sizeof tmp ||
        snprintf(staged, sizeof staged, "%s/bin/guest", rootfs) >= (int)sizeof staged ||
        mkdir(bin, 0755) != 0 || mkdir(dev, 0755) != 0 || mkdir(pts, 0755) != 0 || mkdir(tmp, 01777) != 0 ||
        copy_file(guest, staged) != 0)
        return 1;
    return 0;
}

static void remove_rootfs(const char *rootfs) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/bin/guest", rootfs) < (int)sizeof path) (void)unlink(path);
    if (snprintf(path, sizeof path, "%s/dev/pts", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/bin", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/dev", rootfs) < (int)sizeof path) (void)rmdir(path);
    if (snprintf(path, sizeof path, "%s/tmp", rootfs) < (int)sizeof path) (void)rmdir(path);
    (void)rmdir(rootfs);
}

static void capture_free(capture *result) { free(result->output); free(result->error); }
static int exit_matches(const capture *result, int expected) {
    return WIFEXITED(result->wait_status) && WEXITSTATUS(result->wait_status) == expected;
}
static void diagnostic(const suite_case *item, const char *isa, const char *reason, const capture *result) {
    fprintf(stderr, "matrix-runner: %s [%s] %s", item->name, isa, reason);
    if (result != NULL) {
        size_t index, shown = result->output_size > 64 ? 64 : result->output_size;
        fprintf(stderr, ": wait=0x%x stdout=%zuB hex=", result->wait_status, result->output_size);
        for (index = 0; index < shown; ++index) fprintf(stderr, "%02x", result->output[index]);
        if (shown < result->output_size) fputs("...", stderr);
    }
    if (result != NULL && result->error_size != 0) {
        size_t shown = result->error_size > 240 ? 240 : result->error_size;
        fprintf(stderr, " stderr="); (void)fwrite(result->error, 1, shown, stderr);
    }
    fputc('\n', stderr);
}

static int run_one(const suite_case *item, const char *bridge, const char *engine, const char *binary_root,
                   const char *suite_root, const char *isa, capture *result) {
    char guest[1024], expected_path[1024], binary[256], rootfs[1024] = {0};
    unsigned char *expected;
    size_t expected_size, length = strlen(item->source);
    int status;
    if (length < 3 || strcmp(item->source + length - 2, ".c") != 0 || length - 2 >= sizeof(binary)) return 1;
    memcpy(binary, item->source, length - 2); binary[length - 2] = 0;
    if (snprintf(guest, sizeof(guest), "%s/%s", binary_root, binary) >= (int)sizeof(guest) ||
        snprintf(expected_path, sizeof(expected_path), "%s/%s", suite_root, item->expected) >= (int)sizeof(expected_path) ||
        read_file(expected_path, &expected, &expected_size) != 0) {
        fprintf(stderr, "matrix-runner: %s input path/read failure\n", item->name); return 1;
    }
    if (item->needs_rootfs && stage_rootfs(binary_root, guest, rootfs) != 0) {
        fprintf(stderr, "matrix-runner: %s rootfs staging failure\n", item->name);
        free(expected); return 1;
    }
    /* A bare name is resolved through the guest rootfs PATH without bridge-side path translation. */
    status = run_guest(bridge, engine, item->needs_rootfs ? "guest" : guest,
                       item->needs_rootfs ? rootfs : NULL, item->environment, result);
    if (item->needs_rootfs) remove_rootfs(rootfs);
    if (status != 0 || !exit_matches(result, item->expected_exit) || result->output_size != expected_size ||
        memcmp(result->output, expected, expected_size) != 0) {
        diagnostic(item, isa, status == 2 ? "timeout" : "exit/stdout mismatch", result);
        free(expected); return 1;
    }
    free(expected); return 0;
}

int main(int argc, char **argv) {
    suite_case cases[CASE_MAX];
    size_t count, excluded, index;
    if (argc != 7) {
        fprintf(stderr, "usage: matrix-runner BRIDGE AARCH64_ENGINE AARCH64_BIN_ROOT X86_64_ENGINE X86_64_BIN_ROOT SUITE_ROOT\n");
        return 2;
    }
    if (load_manifest(argv[6], cases, &count, &excluded) != 0) return 1;
    for (index = 0; index < count; ++index) {
        capture a = {0}, x = {0};
        if ((cases[index].isa == ISA_AARCH64 || cases[index].isa == ISA_BOTH) &&
            run_one(&cases[index], argv[1], argv[2], argv[3], argv[6], "aarch64", &a) != 0) { capture_free(&a); return 1; }
        if ((cases[index].isa == ISA_X86_64 || cases[index].isa == ISA_BOTH) &&
            run_one(&cases[index], argv[1], argv[4], argv[5], argv[6], "x86_64", &x) != 0) {
            capture_free(&a); capture_free(&x); return 1;
        }
        if (cases[index].isa == ISA_BOTH &&
            (a.output_size != x.output_size || memcmp(a.output, x.output, a.output_size) != 0)) {
            diagnostic(&cases[index], "cross-ISA", "stdout mismatch", &x);
            capture_free(&a); capture_free(&x); return 1;
        }
        capture_free(&a); capture_free(&x);
    }
    printf("matrix-runner: %zu active cases passed; %zu replaced variants excluded\n", count, excluded);
    return 0;
}
