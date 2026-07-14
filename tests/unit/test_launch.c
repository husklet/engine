#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "../../src/core/launch.h"
#include "../../src/core/options.h"
#include "hl/config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int hl_run_linux_guest(const char *rootfs, int argc, char *const argv[]);

int hl_run_linux_guest(const char *rootfs, int argc, char *const argv[]) {
    (void)rootfs;
    (void)argc;
    (void)argv;
    return 99;
}

static int write_launch(const char *path, const char *bridge) {
    char pool[96] = {0};
    hl_launch_config config = {0};
    size_t cursor = 1;
    int fd;

    config.magic = HL_CONFIG_MAGIC;
    config.header_size = sizeof config;
    config.abi = HL_CONFIG_ABI;
    config.uid = -1;
    config.gid = -1;
    config.gpu_enabled = 1;
    config.arguments_offset = (uint32_t)cursor;
    memcpy(pool + cursor, "guest", 6);
    cursor += 7; // argument terminator plus list terminator
    if (bridge) {
        config.gpu_bridge_name_offset = (uint32_t)cursor;
        memcpy(pool + cursor, bridge, strlen(bridge) + 1);
        cursor += strlen(bridge) + 1;
    }
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

    hl_option_reset();
    HL_CHECK(write_launch(path, NULL) == 0);
    HL_CHECK(hl_run_config_file(path) == 99);
    HL_CHECK(hl_option_get("HL_GPU_IOSURFACE") != NULL);
    HL_CHECK(hl_option_get("HL_GPU_BRIDGE_NAME") == NULL);

    hl_option_reset();
    HL_CHECK(write_launch(path, "org.example.presentation") == 0);
    HL_CHECK(hl_run_config_file(path) == 99);
    HL_CHECK(strcmp(hl_option_get("HL_GPU_BRIDGE_NAME"), "org.example.presentation") == 0);
    hl_option_reset();
    return EXIT_SUCCESS;
}
