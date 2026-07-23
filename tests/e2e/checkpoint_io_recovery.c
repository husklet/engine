#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int prepare_io(const char *release) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s.output", release) >= (int)sizeof path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

static int wait_release(const char *release) {
    while (access(release, F_OK) != 0)
        if (errno != ENOENT) return -1;
    return 0;
}

static int external_path(char *path, size_t size, const char *release) {
    return snprintf(path, size, "%s.external", release) < (int)size ? 0 : -1;
}

static int send_descriptor(int socket_fd, int descriptor) {
    char byte = 'R', control[CMSG_SPACE(sizeof descriptor)];
    struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                             .msg_controllen = sizeof control};
    memset(control, 0, sizeof control);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof descriptor);
    memcpy(CMSG_DATA(header), &descriptor, sizeof descriptor);
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

static int receive_descriptor(int socket_fd) {
    char byte, control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                             .msg_controllen = sizeof control};
    if (recvmsg(socket_fd, &message, 0) != 1 || byte != 'R') return -1;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) return -1;
    int descriptor;
    memcpy(&descriptor, CMSG_DATA(header), sizeof descriptor);
    return descriptor;
}

int main(int argc, char **argv) {
    char external[1024], data[32] = {0};
    if (argc != 3 || prepare_io(argv[1]) != 0 || external_path(external, sizeof external, argv[1]) != 0) return 2;

    if (!strcmp(argv[2], "queued-device") || !strcmp(argv[2], "queued-missing")) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 3;
        int source = !strcmp(argv[2], "queued-device") ? open("/dev/zero", O_RDONLY)
                                                        : open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (source < 0 || send_descriptor(pair[0], source) != 0) return 3;
        close(source);
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0) return 4;
        if (!strcmp(argv[2], "queued-missing")) return 5;
        int restored = receive_descriptor(pair[1]);
        if (restored < 0 || read(restored, data, sizeof data) != sizeof data) return 5;
        for (size_t index = 0; index < sizeof data; ++index)
            if (data[index] != 0) return 5;
        dprintf(STDOUT_FILENO, "IO-QUEUED-DEVICE-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "device")) {
        int zero = open("/dev/zero", O_RDONLY), null = open("/dev/null", O_WRONLY);
        if (zero < 0 || null < 0) return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0 || read(zero, data, sizeof data) != sizeof data ||
            write(null, "discard", 7) != 7)
            return 4;
        for (size_t i = 0; i < sizeof data; ++i)
            if (data[i] != 0) return 5;
        dprintf(STDOUT_FILENO, "IO-DEVICE-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "directory")) {
        if (mkdir(external, 0700) != 0) return 3;
        int directory = open(external, O_RDONLY | O_DIRECTORY);
        if (directory < 0) return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0) return 4;
        int current = openat(directory, "current", O_RDONLY);
        if (current < 0 || read(current, data, 7) != 7 || memcmp(data, "current", 7)) return 5;
        dprintf(STDOUT_FILENO, "IO-DIRECTORY-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "directory-change")) {
        if (mkdir(external, 0700) != 0) return 3;
        pid_t child = fork();
        if (child < 0) return 3;
        if (child == 0) {
            int directory = open(external, O_RDONLY | O_DIRECTORY);
            if (directory < 0) return 3;
            dprintf(STDOUT_FILENO, "READY 1\n");
            if (wait_release(argv[1]) != 0) return 4;
            dprintf(STDOUT_FILENO, "IO-CHILD-RESTORED\n");
            return fstat(directory, &(struct stat){0}) == 0 ? 0 : 5;
        }
        dprintf(STDOUT_FILENO, "READY 2\n");
        if (wait_release(argv[1]) != 0) return 6;
        dprintf(STDOUT_FILENO, "IO-PARENT-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "duplicate")) {
        int first = open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (first < 0 || write(first, "abcdef", 6) != 6 || lseek(first, 0, SEEK_SET) != 0) return 3;
        int second = dup(first);
        char byte;
        if (second < 0 || read(first, &byte, 1) != 1 || byte != 'a') return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0 || read(first, &byte, 1) != 1 || byte != 'b' ||
            read(second, &byte, 1) != 1 || byte != 'c')
            return 4;
        dprintf(STDOUT_FILENO, "IO-DUPLICATE-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "type-change") || !strcmp(argv[2], "permission") ||
        !strcmp(argv[2], "missing-child-strict")) {
        pid_t child = fork();
        if (child < 0) return 3;
        if (child == 0) {
            int fd = open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || write(fd, "child", 5) != 5) return 4;
            dprintf(STDOUT_FILENO, "READY 1\n");
            if (wait_release(argv[1]) != 0) return 5;
            dprintf(STDOUT_FILENO, "IO-CHILD-RESTORED\n");
            return 0;
        }
        dprintf(STDOUT_FILENO, "READY 2\n");
        if (wait_release(argv[1]) != 0) return 6;
        dprintf(STDOUT_FILENO, "IO-PARENT-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "fifo-refusal")) {
        if (mkfifo(external, 0600) != 0) return 3;
        int fifo = open(external, O_RDWR | O_NONBLOCK);
        if (fifo < 0) return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0) return 4;
        return 5;
    }

    if (!strcmp(argv[2], "append")) {
        int append = open(external, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, 0600);
        if (append < 0 || write(append, "guest", 5) != 5) return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0 || write(append, "restored", 8) != 8) return 4;
        int check = open(external, O_RDONLY);
        if (check < 0 || read(check, data, 17) != 17 || memcmp(data, "guesthostrestored", 17)) return 5;
        dprintf(STDOUT_FILENO, "IO-APPEND-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "shortened")) {
        int shortened = open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
        char byte;
        if (shortened < 0 || write(shortened, "longdata", 8) != 8 || lseek(shortened, 8, SEEK_SET) != 8) return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0 || read(shortened, &byte, 1) != 0) return 4;
        dprintf(STDOUT_FILENO, "IO-SHORTENED-RESTORED\n");
        return 0;
    }

    if (!strcmp(argv[2], "repeat")) {
        int repeated = open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (repeated < 0) return 3;
        dprintf(STDOUT_FILENO, "READY 1\n");
        if (wait_release(argv[1]) != 0) return 4;
        dprintf(STDOUT_FILENO, "IO-REPEAT-RESTORED\n");
        return 0;
    }

    int fd = open(external, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "original", 8) != 8) return 3;
    dprintf(STDOUT_FILENO, "READY 1\n");
    if (wait_release(argv[1]) != 0) return 4;
    if (!strcmp(argv[2], "missing-root")) return 5;
    size_t expected = !strcmp(argv[2], "replace") ? 11 : 9;
    const char *value = !strcmp(argv[2], "replace") ? "replacement" : "recreated";
    if (pread(fd, data, expected, 0) != (ssize_t)expected || memcmp(data, value, expected)) return 6;
    dprintf(STDOUT_FILENO, "IO-%s-RESTORED\n", !strcmp(argv[2], "replace") ? "REPLACE" : "RECREATE");
    return 0;
}
