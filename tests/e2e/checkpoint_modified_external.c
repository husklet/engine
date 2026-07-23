#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int prepare_io(const char *release) {
    char output[1024];
    if (snprintf(output, sizeof output, "%s.output", release) >= (int)sizeof output) return -1;
    int fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd != STDIN_FILENO) close(fd);
    return 0;
}

int main(int argc, char **argv) {
    char external[1024], content[8] = {0};
    if (argc != 2 || prepare_io(argv[1]) != 0 ||
        snprintf(external, sizeof external, "%s.external", argv[1]) >= (int)sizeof external)
        return 2;
    int fd = open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "before", 6) != 6) return 3;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 4;
    if (pread(fd, content, 5, 0) != 5 || memcmp(content, "after", 5) != 0) return 5;
    dprintf(STDOUT_FILENO, "MODIFIED-EXTERNAL-RESTORED\n");
    return 0;
}
