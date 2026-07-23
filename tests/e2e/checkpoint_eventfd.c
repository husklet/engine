#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/wait.h>
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
    if (argc != 2 || prepare_output(argv[1]) != 0) return 2;
    int counter = eventfd(7, EFD_NONBLOCK | EFD_CLOEXEC);
    int alias = dup(counter);
    int semaphore = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    int inherited = eventfd(0, EFD_NONBLOCK);
    if (counter < 0 || alias < 0 || semaphore < 0 ||
        inherited < 0 || !(fcntl(counter, F_GETFD) & FD_CLOEXEC) || (fcntl(alias, F_GETFD) & FD_CLOEXEC))
        return 3;
    pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        dprintf(STDOUT_FILENO, "READY 2\n");
        while (access(argv[1], F_OK) != 0)
            if (errno != ENOENT) _exit(20);
        uint64_t eleven = 11;
        _exit(write(inherited, &eleven, sizeof eleven) == 8 ? 0 : 21);
    }
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 4;

    uint64_t value = 0;
    if (read(alias, &value, sizeof value) != 8 || value != 7) return 5;
    if (read(counter, &value, sizeof value) >= 0 || errno != EAGAIN) return 6;
    value = 5;
    if (write(counter, &value, sizeof value) != 8 || read(alias, &value, sizeof value) != 8 || value != 5)
        return 7;
    for (int i = 0; i < 3; i++)
        if (read(semaphore, &value, sizeof value) != 8 || value != 1) return 8;
    if (read(semaphore, &value, sizeof value) >= 0 || errno != EAGAIN) return 9;
    if (!(fcntl(counter, F_GETFD) & FD_CLOEXEC) || (fcntl(alias, F_GETFD) & FD_CLOEXEC)) return 10;
    struct pollfd ready = {.fd = inherited, .events = POLLIN};
    if (poll(&ready, 1, 5000) != 1 || !(ready.revents & POLLIN) ||
        read(inherited, &value, sizeof value) != 8 || value != 11)
        return 11;
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 12;
    close(counter);
    value = 2;
    ssize_t written = write(alias, &value, sizeof value);
    int write_errno = errno;
    ssize_t received = written == 8 ? read(alias, &value, sizeof value) : -1;
    int read_errno = errno;
    if (written != 8 || received != 8 || value != 2) {
        dprintf(STDERR_FILENO, "eventfd alias written=%zd werrno=%d received=%zd rerrno=%d value=%llu\n",
                written, write_errno, received, read_errno, (unsigned long long)value);
        return 13;
    }
    close(alias);
    close(semaphore);
    close(inherited);
    dprintf(STDOUT_FILENO, "EVENTFD-RESTORED\n");
    return 0;
}
