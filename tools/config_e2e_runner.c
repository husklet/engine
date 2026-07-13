#define _POSIX_C_SOURCE 200809L

#include "hl/config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int write_full(int fd, const void *buffer, size_t size) {
    const unsigned char *cursor = buffer;
    while (size != 0) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int make_config(const char *path, const char *guest, uint32_t magic) {
    hl_launch_config config;
    size_t guest_size = strlen(guest) + 1;
    size_t pool_size = 1 + guest_size + 1;
    char *pool = calloc(pool_size, 1);
    int fd;
    int result = -1;
    if (pool == NULL || pool_size > UINT32_MAX) goto done;
    memset(&config, 0, sizeof config);
    config.magic = magic;
    config.pool_size = (uint32_t)pool_size;
    config.header_size = (uint32_t)sizeof config;
    config.abi = HL_CONFIG_ABI;
    config.uid = -1;
    config.gid = -1;
    config.arguments_offset = 1;
    memcpy(pool + 1, guest, guest_size);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) goto done;
    if (write_full(fd, &config, sizeof config) == 0 && write_full(fd, pool, pool_size) == 0) result = 0;
    if (close(fd) != 0) result = -1;
done:
    free(pool);
    return result;
}

static int run_config(const char *bridge, const char *engine, const char *config_path, int expected_exit) {
    const struct timespec tick = {0, 10000000};
    unsigned int elapsed_ms = 0;
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        execlp(bridge, bridge, engine, "--configfile", config_path, (char *)NULL);
        _exit(127);
    }
    while (elapsed_ms < 30000) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            unlink(config_path);
            if (WIFEXITED(status) && WEXITSTATUS(status) == expected_exit) return 0;
            fprintf(stderr, "%s config launch: expected exit %d, status=%d\n", engine, expected_exit, status);
            return 1;
        }
        if (result < 0 && errno != EINTR) return 1;
        nanosleep(&tick, NULL);
        elapsed_ms += 10;
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    unlink(config_path);
    return 1;
}

int main(int argc, char **argv) {
    char directory[1024];
    char path[128];
    uint32_t magic;
    int expected_exit;
    if (argc != 6 || (strcmp(argv[4], "new") != 0 && strcmp(argv[4], "legacy") != 0)) {
        fprintf(stderr, "usage: config-e2e-runner BRIDGE ENGINE GUEST new|legacy EXPECTED_EXIT\n");
        return 2;
    }
    magic = strcmp(argv[4], "new") == 0 ? HL_CONFIG_MAGIC : HL_CONFIG_LEGACY_MAGIC;
    expected_exit = atoi(argv[5]);
    if (getcwd(directory, sizeof directory) == NULL) {
        perror("getcwd");
        return 1;
    }
    if (snprintf(path, sizeof path, "%s/.hl-config-%ld-%u", directory, (long)getpid(), magic) >= (int)sizeof path) {
        fprintf(stderr, "config path is too long\n");
        return 1;
    }
    if (make_config(path, argv[3], magic) != 0) {
        perror("make config");
        unlink(path);
        return 1;
    }
    return run_config(argv[1], argv[2], path, expected_exit);
}
