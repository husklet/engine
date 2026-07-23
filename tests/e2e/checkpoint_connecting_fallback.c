#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int prepare_output(const char *release) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s.output", release) >= (int)sizeof path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    if (snprintf(path, sizeof path, "%s.error", release) >= (int)sizeof path) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

int main(int argc, char **argv) {
    static const uint32_t destinations[] = {0x01010101u, 0x08080808u, 0xcb007101u};
    int connecting = -1;
    if (argc != 2 || prepare_output(argv[1]) != 0) return 2;
    for (size_t index = 0; index < sizeof destinations / sizeof destinations[0]; ++index) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(81),
                                      .sin_addr.s_addr = htonl(destinations[index])};
        errno = 0;
        if (fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof address) < 0 && errno == EINPROGRESS) {
            connecting = fd;
            break;
        }
        if (fd >= 0) close(fd);
    }
    if (connecting < 0) return 3;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 4;
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(connecting, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != ECONNRESET) return 5;
    dprintf(STDOUT_FILENO, "CONNECTING-FALLBACK-RESTORED\n");
    return 0;
}
