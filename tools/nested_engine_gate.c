#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_file(const char *path, char **data, size_t *size) {
    struct stat metadata;
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 || metadata.st_size < 0) {
        if (descriptor >= 0) close(descriptor);
        return -1;
    }
    size_t length = (size_t)metadata.st_size;
    char *content = malloc(length + 1);
    if (!content) {
        close(descriptor);
        return -1;
    }
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(descriptor, content + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            free(content);
            close(descriptor);
            return -1;
        }
    }
    close(descriptor);
    content[offset] = '\0';
    *data = content;
    *size = offset;
    return 0;
}

static int under_tree(const char *path, const char *tree) {
    size_t length = strlen(tree);
    return strncmp(path, tree, length) == 0 && path[length] == '/';
}

static void print_chain(char *const chain[]) {
    for (size_t index = 0; chain[index]; index++)
        fprintf(stderr, "%s%s", index ? " " : "", chain[index]);
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr,
                "usage: %s <cross-tree|-> <fix-command> <expected-stdout> <expected-status> -- "
                "<engine>... <guest>\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[5], "--") != 0) {
        fprintf(stderr, "nested-engine: expected `--` before the chain, got `%s`\n", argv[5]);
        return 2;
    }
    if (argc - 6 < 3) {
        fputs("nested-engine: a chain needs at least two engines and a guest\n", stderr);
        return 2;
    }

    char *end = NULL;
    errno = 0;
    long wanted = strtol(argv[4], &end, 10);
    if (errno || !end || *end || wanted < 0 || wanted > 255) {
        fprintf(stderr, "nested-engine: invalid expected status `%s`\n", argv[4]);
        return 2;
    }

    for (int index = 6; index < argc; index++) {
        if (access(argv[index], X_OK) == 0) continue;
        if (strcmp(argv[1], "-") != 0 && under_tree(argv[index], argv[1])) {
            fprintf(stderr,
                    "nested-engine: SKIPPED, this gate did NOT run and is NOT green.\n"
                    "nested-engine:   missing: %s\n"
                    "nested-engine: It comes from the foreign-host cross build tree %s, built with a second\n"
                    "nested-engine: toolchain that no default build of this tree produces. Build it with:\n"
                    "nested-engine:   %s\n",
                    argv[index], argv[1], argv[2]);
            return 77;
        }
        fprintf(stderr, "nested-engine: %s is missing or not executable; build this tree first\n", argv[index]);
        return 1;
    }

    char *expected = NULL;
    size_t expected_size = 0;
    if (read_file(argv[3], &expected, &expected_size) != 0) {
        fprintf(stderr, "nested-engine: cannot read expected stdout %s: %s\n", argv[3], strerror(errno));
        return 1;
    }

    char *output = NULL;
    size_t output_size = 0;
    hl_process_result_t result;
    if (hl_process_run(&argv[6], NULL, 0, &output, &output_size, &result) != 0) {
        fprintf(stderr, "nested-engine: cannot run chain: %s\n", strerror(errno));
        free(expected);
        free(output);
        return 1;
    }
    if (!result.exited || result.exit_code != wanted) {
        fputs("nested-engine: ", stderr);
        print_chain(&argv[6]);
        if (result.exec_errno)
            fprintf(stderr, "\nnested-engine: exec failed: %s\n", strerror(result.exec_errno));
        else if (result.signaled)
            fprintf(stderr, "\nnested-engine: signal %d, expected exit %ld\n", result.signal, wanted);
        else
            fprintf(stderr, "\nnested-engine: exit %d, expected %ld\n", result.exit_code, wanted);
        free(expected);
        free(output);
        return 1;
    }
    if (output_size != expected_size || memcmp(output, expected, expected_size) != 0) {
        fputs("nested-engine: ", stderr);
        print_chain(&argv[6]);
        fprintf(stderr, "\nnested-engine: stdout differs from %s\n", argv[3]);
        free(expected);
        free(output);
        return 1;
    }

    fputs("nested-engine: ", stdout);
    for (int index = 6; index < argc; index++)
        fprintf(stdout, "%s%s", index == 6 ? "" : " ", argv[index]);
    fprintf(stdout, " -> exit %ld, stdout matches %s\n", wanted, argv[3]);
    free(expected);
    free(output);
    return 0;
}
