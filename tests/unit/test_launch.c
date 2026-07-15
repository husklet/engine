#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "../../src/core/launch.h"
#include "../../src/core/options.h"
#include "hl/config.h"
#include "hl/host_services.h"
#include "hl/linux_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]);

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]) {
    (void)host;
    (void)box;
    (void)rootfs;
    (void)argc;
    (void)argv;
    return 99;
}

static uint32_t add_string(char *pool, size_t *cursor, const char *value) {
    uint32_t offset = (uint32_t)*cursor;
    size_t size = strlen(value) + 1;
    memcpy(pool + *cursor, value, size);
    *cursor += size;
    return offset;
}

static int write_launch(const char *path) {
    char pool[256] = {0};
    hl_launch_config config = {0};
    size_t cursor = 1;
    int fd;

    config.magic = HL_CONFIG_MAGIC;
    config.header_size = sizeof config;
    config.abi = HL_CONFIG_ABI;
    config.uid = -1;
    config.gid = -1;
    config.cpu_limit = 2;
    config.arguments_offset = (uint32_t)cursor;
    memcpy(pool + cursor, "guest", 6);
    cursor += 7; // argument terminator plus list terminator
    config.hostname_offset = add_string(pool, &cursor, "typed-host");
    config.volumes_offset = add_string(pool, &cursor, "/host:/guest:ro");
    config.lower_layers_offset = add_string(pool, &cursor, "/lower/one:/lower/two");
    config.network_namespace_offset = add_string(pool, &cursor, "box-network");
    config.pool_size = (uint32_t)cursor;
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return -1;
    if (write(fd, &config, sizeof config) != (ssize_t)sizeof config || write(fd, pool, cursor) != (ssize_t)cursor) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int inspect_launch(const char *rootfs, uint32_t argc, char *const argv[], const hl_options *options) {
    (void)rootfs;
    HL_CHECK(argc == 1 && strcmp(argv[0], "guest") == 0);
    HL_CHECK(strcmp(hl_option_get("HL_HOSTNAME"), "typed-host") == 0);
    HL_CHECK(strcmp(hl_options_get(options, "HL_CPUS"), "2") == 0);
    HL_CHECK(strcmp(hl_options_get(options, "HL_HOSTNAME"), "typed-host") == 0);
    HL_CHECK(strcmp(hl_options_get(options, "HL_VOLUMES"), "/host:/guest:ro") == 0);
    HL_CHECK(strcmp(hl_options_get(options, "HL_LOWER"), "/lower/one:/lower/two") == 0);
    HL_CHECK(strcmp(hl_options_get(options, "HL_NETNS"), "box-network") == 0);
    return 37;
}

int main(void) {
    char path[] = "/tmp/hl_launch_XXXXXX";
    static const char malformed[] = "not a launch configuration";
    int fd;

    HL_CHECK(hl_run_config_file(NULL) == 78);
    HL_CHECK(hl_run_config_file("") == 78);
    HL_CHECK(hl_option_set("HL_HOSTNAME", "caller-host", 1) == 0);
    fd = mkstemp(path);
    HL_CHECK(fd >= 0);
    HL_CHECK(write(fd, malformed, sizeof(malformed)) == (ssize_t)sizeof(malformed));
    HL_CHECK(close(fd) == 0);
    HL_CHECK(hl_run_config_file(path) == 78);
    HL_CHECK(strcmp(hl_option_get("HL_HOSTNAME"), "caller-host") == 0);
    errno = 0;
    HL_CHECK(access(path, F_OK) == -1 && errno == ENOENT);

    HL_CHECK(write_launch(path) == 0);
    HL_CHECK(hl_run_config_file_with(path, inspect_launch) == 37);
    /* Applying the wire is scoped and transactional; caller state survives the run. */
    HL_CHECK(strcmp(hl_option_get("HL_HOSTNAME"), "caller-host") == 0);
    hl_option_reset();
    return EXIT_SUCCESS;
}
