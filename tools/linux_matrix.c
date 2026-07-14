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

static int run_suite(const char *engine, const char *binary_root, const char *suite_root) {
    char manifest[1024], *line = NULL;
    size_t capacity = 0, passed = 0, unsupported = 0, excluded = 0;
    ssize_t length;
    FILE *file;
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
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
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
            if (!has_token(fields[2], "x86_64")) {
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
                snprintf(golden, sizeof(golden), "%s/%s", suite_root, fields[4]) >= (int)sizeof(golden) ||
                run_case(engine, guest, golden, expected_exit) != 0) {
                free(line);
                fclose(file);
                return 1;
            }
            passed++;
            continue;
        }
        if (strncmp(fields[11], "excluded-", 9) == 0 || !has_token(fields[4], "x86_64")) {
            excluded++;
            continue;
        }
        if (strcmp(fields[11], "active") != 0 || parse_exit(fields[8], &expected_exit) != 0) {
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
        } else if (strncmp(fields[5], "prebuilt:", 9) == 0 && source_size != 0 && source_size < sizeof(binary)) {
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
        if (run_case(engine, guest, golden, expected_exit) != 0) {
            free(line);
            fclose(file);
            return 1;
        }
        passed++;
    }
    free(line);
    if (fclose(file) != 0) return 1;
    printf("linux-matrix: %zu active x86-64 cases passed; %zu require typed launch; %zu excluded or other ISA\n",
           passed, unsupported, excluded);
    return passed == 0;
}

int main(int argc, char **argv) {
    int failed = 0;
    if (argc == 5 && strcmp(argv[1], "--suite") == 0)
        return run_suite(argv[2], argv[3], argv[4]) ? EXIT_FAILURE : EXIT_SUCCESS;
    if (argc < 5 || (argc - 2) % 3 != 0) {
        fprintf(stderr, "usage: %s ENGINE GUEST GOLDEN EXIT [GUEST GOLDEN EXIT ...]\n"
                        "       %s --suite ENGINE BIN_ROOT SUITE_ROOT\n", argv[0], argv[0]);
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
