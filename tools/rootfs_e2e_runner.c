#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int make_parents(char *path) {
    for (char *cursor = path + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
        *cursor = '/';
    }
    return 0;
}

static int copy_file(const char *source, const char *destination, mode_t mode) {
    char path[4096];
    unsigned char buffer[65536];
    int input = -1;
    int output = -1;
    int result = -1;
    if (snprintf(path, sizeof path, "%s", destination) >= (int)sizeof path || make_parents(path) != 0) goto done;
    input = open(source, O_RDONLY);
    if (input < 0) goto done;
    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (output < 0) goto done;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof buffer);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            goto done;
        }
        for (ssize_t offset = 0; offset < count;) {
            ssize_t written = write(output, buffer + offset, (size_t)(count - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                goto done;
            }
            offset += written;
        }
    }
    if (fchmod(output, mode) != 0) goto done;
    result = 0;
done:
    if (output >= 0 && close(output) != 0) result = -1;
    if (input >= 0 && close(input) != 0) result = -1;
    return result;
}

static int stage_rootfs(const char *rootfs, const char *guest, const char *loader, const char *libc,
                        const char *loader_path) {
    char destination[4096];
    char data_path[4096];
    char root_path[4096];
    int fd;
    static const char data[] = "rootfs-data";
    if (snprintf(root_path, sizeof root_path, "%s", rootfs) >= (int)sizeof root_path || make_parents(root_path) != 0 ||
        (mkdir(rootfs, 0755) != 0 && errno != EEXIST))
        return -1;
    if (snprintf(destination, sizeof destination, "%s/bin/hl-dynamic", rootfs) >= (int)sizeof destination ||
        copy_file(guest, destination, 0755) != 0)
        return -1;
    if (snprintf(destination, sizeof destination, "%s%s", rootfs, loader_path) >= (int)sizeof destination ||
        copy_file(loader, destination, 0755) != 0)
        return -1;
    if (snprintf(destination, sizeof destination, "%s/lib/libc.so.6", rootfs) >= (int)sizeof destination ||
        copy_file(libc, destination, 0755) != 0)
        return -1;
    if (snprintf(data_path, sizeof data_path, "%s/etc/hl-dynamic-data", rootfs) >= (int)sizeof data_path ||
        make_parents(data_path) != 0)
        return -1;
    fd = open(data_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write(fd, data, sizeof data - 1) != (ssize_t)(sizeof data - 1)) {
        close(fd);
        return -1;
    }
    if (close(fd) != 0) return -1;
    if (snprintf(data_path, sizeof data_path, "%s/tmp/hl-dynamic-result", rootfs) >= (int)sizeof data_path ||
        make_parents(data_path) != 0)
        return -1;
    if (unlink(data_path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static int run_guest(const char *bridge, const char *engine, const char *rootfs, const char *expected) {
    const struct timespec tick = {0, 10000000};
    char result_path[4096];
    char output[4096];
    size_t output_size = 0;
    unsigned int elapsed_ms = 0;
    int fd;
    pid_t child;
    int status = 0;
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        /* The Linux-to-macOS bridge rewrites absolute Linux-looking argv paths to its mounted
         * macOS counterpart. Pass a bare guest name so the engine resolves it against guest PATH. */
        execlp(bridge, bridge, engine, "--rootfs", rootfs, "hl-dynamic", "probe", (char *)NULL);
        _exit(127);
    }
    while (elapsed_ms < 30000) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) return -1;
        nanosleep(&tick, NULL);
        elapsed_ms += 10;
    }
    if (elapsed_ms >= 30000) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        fprintf(stderr, "%s dynamic guest timed out\n", engine);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s dynamic guest: status=%d\n", engine, status);
        return -1;
    }
    if (snprintf(result_path, sizeof result_path, "%s/tmp/hl-dynamic-result", rootfs) >= (int)sizeof result_path)
        return -1;
    fd = open(result_path, O_RDONLY);
    if (fd < 0) return -1;
    while (output_size + 1 < sizeof output) {
        ssize_t count = read(fd, output + output_size, sizeof output - output_size - 1);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        output_size += (size_t)count;
    }
    output[output_size] = '\0';
    if (close(fd) != 0) return -1;
    size_t expected_size = strlen(expected);
    if (output_size != expected_size + 1 || memcmp(output, expected, expected_size) != 0 ||
        output[expected_size] != '\n') {
        fprintf(stderr, "%s dynamic guest: status=%d output=%s", engine, status, output);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 9) {
        fprintf(stderr, "usage: rootfs-e2e-runner BRIDGE ENGINE ROOTFS GUEST LOADER LIBC LOADER_PATH EXPECTED\n");
        return 2;
    }
    if (stage_rootfs(argv[3], argv[4], argv[5], argv[6], argv[7]) != 0) {
        perror("stage rootfs");
        return 1;
    }
    return run_guest(argv[1], argv[2], argv[3], argv[8]) == 0 ? 0 : 1;
}
