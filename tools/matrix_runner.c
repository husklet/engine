#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ABI_CASE_MAX = 128, OUTPUT_MAX = 1024 * 1024, ERROR_MAX = 64 * 1024, TIMEOUT_MS = 30000 };

typedef struct abi_case {
    char source[128];
    char golden[160];
    int expected_exit;
} abi_case;

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

static int safe_name(const char *name, const char *suffix) {
    size_t name_size = strlen(name);
    size_t suffix_size = strlen(suffix);
    return name_size > suffix_size && name[0] != '/' && strstr(name, "..") == NULL && strchr(name, '/') == NULL &&
           strcmp(name + name_size - suffix_size, suffix) == 0;
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

static int load_manifest(const char *root, abi_case cases[ABI_CASE_MAX], size_t *case_count) {
    char path[1024];
    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/manifest.tsv", root) >= (int)sizeof(path)) return 1;
    file = fopen(path, "r");
    if (file == NULL) return 1;
    *case_count = 0;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        char *fields[7];
        char *cursor;
        size_t index;
        if (size == 0 || line[0] == '#') continue;
        while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r'))
            line[--size] = 0;
        cursor = line;
        for (index = 0; index < 7; ++index) {
            fields[index] = cursor;
            cursor = index == 6 ? NULL : strchr(cursor, '\t');
            if (index != 6 && cursor == NULL) goto invalid;
            if (cursor != NULL) *cursor++ = 0;
        }
        if (cursor != NULL || strchr(fields[6], '\t') != NULL || *case_count == ABI_CASE_MAX ||
            !safe_name(fields[0], ".c") || strcmp(fields[2], "aarch64,x86_64") != 0 ||
            strncmp(fields[4], "golden/", 7) != 0 || !safe_name(fields[4] + 7, ".out") ||
            parse_exit(fields[3], &cases[*case_count].expected_exit) != 0)
            goto invalid;
        for (index = 0; index < *case_count; ++index)
            if (strcmp(cases[index].source, fields[0]) == 0 || strcmp(cases[index].golden, fields[4]) == 0)
                goto invalid;
        if (snprintf(cases[*case_count].source, sizeof(cases[*case_count].source), "%s", fields[0]) >=
                (int)sizeof(cases[*case_count].source) ||
            snprintf(cases[*case_count].golden, sizeof(cases[*case_count].golden), "%s", fields[4]) >=
                (int)sizeof(cases[*case_count].golden))
            goto invalid;
        (*case_count)++;
    }
    free(line);
    fclose(file);
    return *case_count == 0;
invalid:
    fprintf(stderr, "matrix-runner: invalid manifest row near %zu\n", *case_count + 1);
    free(line);
    fclose(file);
    return 1;
}

static int registered(const abi_case *cases, size_t count, const char *name, int golden) {
    size_t index;
    for (index = 0; index < count; ++index) {
        const char *registered_name = golden ? cases[index].golden + 7 : cases[index].source;
        if (strcmp(registered_name, name) == 0) return 1;
    }
    return 0;
}

static int validate_directory(const char *path, const abi_case *cases, size_t count, const char *suffix, int golden) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    size_t found = 0;
    if (directory == NULL) return 1;
    while ((entry = readdir(directory)) != NULL) {
        size_t size = strlen(entry->d_name);
        size_t suffix_size = strlen(suffix);
        if (size <= suffix_size || strcmp(entry->d_name + size - suffix_size, suffix) != 0) continue;
        if (!registered(cases, count, entry->d_name, golden)) {
            fprintf(stderr, "matrix-runner: unregistered file %s/%s\n", path, entry->d_name);
            closedir(directory);
            return 1;
        }
        found++;
    }
    closedir(directory);
    return found != count;
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

static void terminate(pid_t child) {
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int run_guest(const char *bridge, const char *engine, const char *guest, capture *result) {
    int output_pipe[2];
    int error_pipe[2];
    int output_eof = 0;
    int error_eof = 0;
    int exited = 0;
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
        close(output_pipe[0]);
        close(error_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(error_pipe[1], STDERR_FILENO) < 0) _exit(127);
        close(output_pipe[1]);
        close(error_pipe[1]);
        execlp(bridge, bridge, engine, guest, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(child, child);
    close(output_pipe[1]);
    close(error_pipe[1]);
    if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(error_pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        terminate(child);
        return 1;
    }
    deadline = monotonic_ms() + TIMEOUT_MS;
    while (!exited || !output_eof || !error_eof) {
        struct pollfd descriptors[2] = {{output_pipe[0], POLLIN | POLLHUP, 0}, {error_pipe[0], POLLIN | POLLHUP, 0}};
        pid_t waited;
        if (monotonic_ms() >= deadline) {
            terminate(child);
            close(output_pipe[0]);
            close(error_pipe[0]);
            return 2;
        }
        if (poll(descriptors, 2, 10) < 0 && errno != EINTR) {
            terminate(child);
            return 1;
        }
        if (drain(output_pipe[0], result->output, &result->output_size, OUTPUT_MAX, &output_eof) != 0 ||
            drain(error_pipe[0], result->error, &result->error_size, ERROR_MAX, &error_eof) != 0) {
            terminate(child);
            return 1;
        }
        if (!exited) {
            waited = waitpid(child, &result->wait_status, WNOHANG);
            if (waited == child)
                exited = 1;
            else if (waited < 0 && errno != EINTR) {
                terminate(child);
                return 1;
            }
        }
    }
    close(output_pipe[0]);
    close(error_pipe[0]);
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

static void capture_free(capture *result) {
    free(result->output);
    free(result->error);
}

static int exit_matches(const capture *result, int expected) {
    return WIFEXITED(result->wait_status) && WEXITSTATUS(result->wait_status) == expected;
}

static void diagnostic(const char *name, const char *isa, const char *reason, const capture *result) {
    fprintf(stderr, "matrix-runner: %s [%s] %s", name, isa, reason);
    if (result != NULL && result->error_size != 0) {
        size_t shown = result->error_size > 240 ? 240 : result->error_size;
        fprintf(stderr, ": stderr=");
        (void)fwrite(result->error, 1, shown, stderr);
    }
    fputc('\n', stderr);
}

int main(int argc, char **argv) {
    abi_case cases[ABI_CASE_MAX];
    size_t count;
    size_t index;
    char golden_directory[1024];
    if (argc != 7) {
        fprintf(
            stderr,
            "usage: matrix-runner BRIDGE AARCH64_ENGINE AARCH64_GUEST_DIR X86_64_ENGINE X86_64_GUEST_DIR SUITE_ROOT\n");
        return 2;
    }
    if (load_manifest(argv[6], cases, &count) != 0 ||
        snprintf(golden_directory, sizeof(golden_directory), "%s/golden", argv[6]) >= (int)sizeof(golden_directory) ||
        validate_directory(argv[6], cases, count, ".c", 0) != 0 ||
        validate_directory(golden_directory, cases, count, ".out", 1) != 0) {
        fprintf(stderr, "matrix-runner: manifest/source/golden registration mismatch\n");
        return 1;
    }
    for (index = 0; index < count; ++index) {
        capture aarch64;
        capture x86_64;
        unsigned char *golden;
        size_t golden_size;
        char name[128];
        char aarch64_guest[1024];
        char x86_64_guest[1024];
        char golden_path[1024];
        size_t source_size = strlen(cases[index].source);
        memcpy(name, cases[index].source, source_size - 2);
        name[source_size - 2] = 0;
        if (snprintf(aarch64_guest, sizeof(aarch64_guest), "%s/%s", argv[3], name) >= (int)sizeof(aarch64_guest) ||
            snprintf(x86_64_guest, sizeof(x86_64_guest), "%s/%s", argv[5], name) >= (int)sizeof(x86_64_guest) ||
            snprintf(golden_path, sizeof(golden_path), "%s/%s", argv[6], cases[index].golden) >=
                (int)sizeof(golden_path) ||
            read_file(golden_path, &golden, &golden_size) != 0) {
            fprintf(stderr, "matrix-runner: %s input path/read failure\n", name);
            return 1;
        }
        int a_status = run_guest(argv[1], argv[2], aarch64_guest, &aarch64);
        if (a_status != 0 || !exit_matches(&aarch64, cases[index].expected_exit) ||
            aarch64.output_size != golden_size || memcmp(aarch64.output, golden, golden_size) != 0) {
            diagnostic(name, "aarch64", a_status == 2 ? "timeout" : "exit/stdout mismatch", &aarch64);
            free(golden);
            capture_free(&aarch64);
            return 1;
        }
        int x_status = run_guest(argv[1], argv[4], x86_64_guest, &x86_64);
        if (x_status != 0 || !exit_matches(&x86_64, cases[index].expected_exit) || x86_64.output_size != golden_size ||
            memcmp(x86_64.output, golden, golden_size) != 0 || x86_64.output_size != aarch64.output_size ||
            memcmp(x86_64.output, aarch64.output, aarch64.output_size) != 0) {
            diagnostic(name, "x86_64", x_status == 2 ? "timeout" : "exit/stdout/cross-ISA mismatch", &x86_64);
            free(golden);
            capture_free(&aarch64);
            capture_free(&x86_64);
            return 1;
        }
        free(golden);
        capture_free(&aarch64);
        capture_free(&x86_64);
    }
    printf("matrix-runner: %zu cases passed on aarch64 and x86_64\n", count);
    return 0;
}
