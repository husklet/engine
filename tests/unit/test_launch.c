#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "../../src/core/launch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int hl_run_linux_guest(const char *rootfs, int argc, char *const argv[]);

int hl_run_linux_guest(const char *rootfs, int argc, char *const argv[]) {
    (void)rootfs;
    (void)argc;
    (void)argv;
    return 99;
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
    return EXIT_SUCCESS;
}
