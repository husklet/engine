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

static int make_config(const char *path, const char *guest) {
    static const char volume[] = "/tmp:/tmp";
    hl_launch_config config;
    size_t guest_size = strlen(guest) + 1;
    size_t volume_size = sizeof volume;
    size_t pool_size = 1 + guest_size + 1 + volume_size;
    char *pool = calloc(pool_size, 1);
    int fd;
    int result = -1;
    if (pool == NULL || pool_size > UINT32_MAX) goto done;
    memset(&config, 0, sizeof config);
    config.magic = HL_CONFIG_MAGIC;
    config.pool_size = (uint32_t)pool_size;
    config.header_size = (uint32_t)sizeof config;
    config.abi = HL_CONFIG_ABI;
    config.uid = -1;
    config.gid = -1;
    config.arguments_offset = 1;
    memcpy(pool + 1, guest, guest_size);
    config.volumes_offset = (uint32_t)(1 + guest_size + 1);
    memcpy(pool + config.volumes_offset, volume, volume_size);
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
    char path[1200];
    int expected_exit, repetitions = 1;
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: config-e2e-runner BRIDGE ENGINE GUEST EXPECTED_EXIT [REPETITIONS]\n");
        return 2;
    }
    expected_exit = atoi(argv[4]);
    if (argc == 6) {
        char *end = NULL;
        long parsed;
        errno = 0;
        parsed = strtol(argv[5], &end, 10);
        if (errno != 0 || end == argv[5] || *end != 0 || parsed < 1 || parsed > 10000) return 2;
        repetitions = (int)parsed;
    }
    if (getcwd(directory, sizeof directory) == NULL) {
        perror("getcwd");
        return 1;
    }
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        if (snprintf(path, sizeof path, "%s/.hl-config-%ld-%d", directory, (long)getpid(), iteration) >=
            (int)sizeof path) {
            fprintf(stderr, "config path is too long\n");
            return 1;
        }
        if (make_config(path, argv[3]) != 0) {
            perror("make config");
            unlink(path);
            return 1;
        }
        if (run_config(argv[1], argv[2], path, expected_exit) != 0) return 1;
    }
    return 0;
}
