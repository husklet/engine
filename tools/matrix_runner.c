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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hl/config.h"

enum { CASE_MAX = 256, FIELD_MAX = 512, OUTPUT_MAX = 1024 * 1024, ERROR_MAX = 64 * 1024, TIMEOUT_MS = 30000 };

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
            if (*case_count == CASE_MAX || !relative_path(fields[0]) || source_size < 3 ||
                strcmp(fields[0] + source_size - 2, ".c") != 0 || strcmp(fields[2], "aarch64,x86_64") != 0 ||
                !relative_path(fields[4]) || strncmp(fields[4], "golden/", 7) != 0 ||
                parse_exit(fields[3], &cases[*case_count].expected_exit) != 0)
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
        if (strncmp(fields[11], "excluded-", 9) == 0) {
            (*excluded)++;
            continue;
        }
        if (strcmp(fields[11], "active") != 0 || *case_count == CASE_MAX || !relative_path(fields[2]) ||
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
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
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
    if (rootfs != NULL && pool_string(&wire, rootfs, &wire.config.rootfs_offset) != 0) return 1;
    if (*encoded != 0) {
        memcpy(copy, encoded, strlen(encoded) + 1);
        cursor = copy;
        while (cursor != NULL) {
            char *next = strchr(cursor, ';'), *equals = strchr(cursor, '=');
            int option;
            if (next != NULL) *next++ = 0;
            if (equals == NULL) return 1;
            *equals++ = 0;
            option = config_option(&wire, cursor, equals);
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
        int length = declared ? snprintf(volume, sizeof volume, "%s,/tmp:%s", declared, scratch)
                              : snprintf(volume, sizeof volume, "/tmp:%s", scratch);
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
    uint64_t deadline;
    pid_t child;
    memset(result, 0, sizeof(*result));
    result->output = malloc(OUTPUT_MAX);
    result->error = malloc(ERROR_MAX);
    if (result->output == NULL || result->error == NULL || pipe(output_pipe) != 0 || pipe(error_pipe) != 0) return 1;
    if (snprintf(scratch, sizeof scratch, "%s/.matrix-scratch-XXXXXX", binary_root) >= (int)sizeof scratch ||
        mkdtemp(scratch) == NULL)
        return 1;
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
        execlp(bridge, bridge, supervisor, "--capture", capture_output, capture_error, engine, "--configfile",
               config_path, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(child, child);
    close(output_pipe[1]);
    close(error_pipe[1]);
    if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(error_pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        terminate(child);
        (void)unlink(config_path);
        remove_tree(scratch);
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
            unlink(config_path);
            remove_tree(scratch);
            return 2;
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
    fprintf(stderr, "matrix-runner: %s [%s] %s", item->name, isa, reason);
    if (result != NULL) {
        size_t index, shown = result->output_size > 64 ? 64 : result->output_size;
        fprintf(stderr, ": wait=0x%x stdout=%zuB hex=", result->wait_status, result->output_size);
        for (index = 0; index < shown; ++index)
            fprintf(stderr, "%02x", result->output[index]);
        if (shown < result->output_size) fputs("...", stderr);
    }
    if (result != NULL && result->error_size != 0) {
        size_t shown = result->error_size > 240 ? 240 : result->error_size;
        fprintf(stderr, " stderr=");
        (void)fwrite(result->error, 1, shown, stderr);
    }
    fputc('\n', stderr);
}

static int run_one(const suite_case *item, const char *bridge, const char *engine, const char *binary_root,
                   const char *suite_root, const char *isa, capture *result) {
    char guest[1024], expected_path[1024], binary[256], rootfs[1024] = {0};
    unsigned char *expected;
    size_t expected_size, length = strlen(item->source), binary_length = length;
    int status;
    /* Source-built suites use foo.c; fixture suites may name the committed executable itself. */
    if (length >= 2 && strcmp(item->source + length - 2, ".c") == 0) binary_length -= 2;
    if (binary_length == 0 || binary_length >= sizeof(binary)) return 1;
    memcpy(binary, item->source, binary_length);
    binary[binary_length] = 0;
    if (snprintf(guest, sizeof(guest), "%s/%s", binary_root, binary) >= (int)sizeof(guest) ||
        snprintf(expected_path, sizeof(expected_path), "%s/%s", suite_root, item->expected) >=
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
        while (mismatch < common && result->output[mismatch] == expected[mismatch]) ++mismatch;
        if (mismatch < common)
            fprintf(stderr, "matrix-runner: %s [%s] first stdout difference at byte %zu: got=%02x expected=%02x\n",
                    item->name, isa, mismatch, result->output[mismatch], expected[mismatch]);
        else if (result->output_size != expected_size)
            fprintf(stderr, "matrix-runner: %s [%s] stdout length: got=%zu expected=%zu\n", item->name, isa,
                    result->output_size, expected_size);
        diagnostic(item, isa, status == 2 ? "timeout" : "exit/stdout mismatch", result);
        free(expected);
        return 1;
    }
    free(expected);
    return 0;
}

int main(int argc, char **argv) {
    suite_case cases[CASE_MAX];
    size_t count, excluded, index;
    if (argc != 7) {
        fprintf(
            stderr,
            "usage: matrix-runner BRIDGE AARCH64_ENGINE AARCH64_BIN_ROOT X86_64_ENGINE X86_64_BIN_ROOT SUITE_ROOT\n");
        return 2;
    }
    if (load_manifest(argv[6], cases, &count, &excluded) != 0) return 1;
    for (index = 0; index < count; ++index) {
        capture a = {0}, x = {0};
        if ((cases[index].isa == ISA_AARCH64 || cases[index].isa == ISA_BOTH) &&
            run_one(&cases[index], argv[1], argv[2], argv[3], argv[6], "aarch64", &a) != 0) {
            capture_free(&a);
            return 1;
        }
        if ((cases[index].isa == ISA_X86_64 || cases[index].isa == ISA_BOTH) &&
            run_one(&cases[index], argv[1], argv[4], argv[5], argv[6], "x86_64", &x) != 0) {
            capture_free(&a);
            capture_free(&x);
            return 1;
        }
        if (cases[index].isa == ISA_BOTH &&
            (a.output_size != x.output_size || memcmp(a.output, x.output, a.output_size) != 0)) {
            diagnostic(&cases[index], "cross-ISA", "stdout mismatch", &x);
            capture_free(&a);
            capture_free(&x);
            return 1;
        }
        capture_free(&a);
        capture_free(&x);
    }
    printf("matrix-runner: %zu active cases passed; %zu manifest cases excluded\n", count, excluded);
    return 0;
}
