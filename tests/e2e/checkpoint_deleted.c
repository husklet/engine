#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int prepare_output(const char *release) {
    char output[1024];
    if (snprintf(output, sizeof output, "%s.output", release) >= (int)sizeof output) return -1;
    int fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

int main(int argc, char **argv) {
    static const char initial[] = "before";
    char deleted[1024], content[sizeof initial];
    int fd;
    if (argc != 2 || prepare_output(argv[1]) != 0) return 2;
    if (snprintf(deleted, sizeof deleted, "%s.deleted", argv[1]) >= (int)sizeof deleted) return 2;
    fd = open(deleted, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, initial, sizeof initial - 1) != (ssize_t)(sizeof initial - 1) ||
        lseek(fd, 2, SEEK_SET) != 2 || unlink(deleted) != 0)
        return 3;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 4;
    if (lseek(fd, 0, SEEK_CUR) != 2 || pread(fd, content, sizeof initial - 1, 0) != (ssize_t)(sizeof initial - 1) ||
        memcmp(content, initial, sizeof initial - 1) != 0)
        return 5;
    if (write(fd, "X", 1) != 1 || pread(fd, content, sizeof initial - 1, 0) != (ssize_t)(sizeof initial - 1) ||
        memcmp(content, "beXore", sizeof initial - 1) != 0)
        return 6;
    dprintf(STDOUT_FILENO, "DELETED-RESTORED\n");
    close(fd);
    return 0;
}
