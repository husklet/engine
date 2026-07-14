#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "../../src/core/launch.h"
#include "hl/config.h"
#include "hl/host_services.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int hl_run_linux_guest(const hl_host_services *host, const char *rootfs, uint32_t argc, char *const argv[]);

int hl_run_linux_guest(const hl_host_services *host, const char *rootfs, uint32_t argc, char *const argv[]) {
    (void)host;
    (void)rootfs;
    (void)argc;
    (void)argv;
    return 99;
}

static int write_launch(const char *path) {
    char pool[96] = {0};
    hl_launch_config config = {0};
    size_t cursor = 1;
    int fd;

    config.magic = HL_CONFIG_MAGIC;
    config.header_size = sizeof config;
    config.abi = HL_CONFIG_ABI;
    config.uid = -1;
    config.gid = -1;
    config.arguments_offset = (uint32_t)cursor;
    memcpy(pool + cursor, "guest", 6);
    cursor += 7; // argument terminator plus list terminator
    config.pool_size = (uint32_t)cursor;
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return -1;
    if (write(fd, &config, sizeof config) != (ssize_t)sizeof config || write(fd, pool, cursor) != (ssize_t)cursor) {
        close(fd);
        return -1;
    }
    return close(fd);
}

int main(void) {
    char path[] = "/tmp/hl_launch_XXXXXX";
    static const char malformed[] = "not a launch configuration";
    int fd;

    HL_CHECK(hl_run_config_file(NULL) == 78);
    HL_CHECK(hl_run_config_file("") == 78);
    fd = mkstemp(path);
    HL_CHECK(fd >= 0);
    HL_CHECK(write(fd, malformed, sizeof(malformed)) == (ssize_t)sizeof(malformed));
    HL_CHECK(close(fd) == 0);
    HL_CHECK(hl_run_config_file(path) == 78);
    errno = 0;
    HL_CHECK(access(path, F_OK) == -1 && errno == ENOENT);

    HL_CHECK(write_launch(path) == 0);
    HL_CHECK(hl_run_config_file(path) == 99);
    return EXIT_SUCCESS;
}
