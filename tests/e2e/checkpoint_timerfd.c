#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
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
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int alias = dup(timer);
    int disarmed = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    int shared = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    int report[2];
    struct itimerspec setting = {.it_interval = {0, 20000000}, .it_value = {0, 10000000}};
    struct itimerspec shared_setting = {.it_value = {0, 10000000}};
    if (timer < 0 || alias < 0 || disarmed < 0 || shared < 0 || pipe(report) != 0 ||
        timerfd_settime(timer, 0, &setting, NULL) != 0 || timerfd_settime(shared, 0, &shared_setting, NULL) != 0 ||
        !(fcntl(timer, F_GETFD) & FD_CLOEXEC) || (fcntl(alias, F_GETFD) & FD_CLOEXEC))
        return 3;
    pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        close(report[0]);
        dprintf(STDOUT_FILENO, "READY 2\n");
        while (access(argv[1], F_OK) != 0)
            if (errno != ENOENT) _exit(20);
        struct pollfd child_ready = {.fd = shared, .events = POLLIN};
        uint64_t child_value = 0;
        int result = poll(&child_ready, 1, 5000) == 1 && read(shared, &child_value, 8) == 8 && child_value == 1;
        _exit(write(report[1], &result, sizeof result) == sizeof result ? 0 : 21);
    }
    close(report[1]);
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 4;
    struct pollfd ready = {.fd = alias, .events = POLLIN};
    uint64_t expirations = 0;
    if (poll(&ready, 1, 5000) != 1 || read(alias, &expirations, 8) != 8 || expirations < 1) return 5;
    struct itimerspec current;
    if (timerfd_gettime(timer, &current) != 0 || current.it_interval.tv_sec != 0 ||
        current.it_interval.tv_nsec != 20000000)
        return 6;
    errno = 0;
    if (read(disarmed, &expirations, 8) >= 0 || errno != EAGAIN) return 7;
    int child_result = 0, status = 0;
    if (read(report[0], &child_result, sizeof child_result) != sizeof child_result || child_result != 1 ||
        waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 9;
    errno = 0;
    if (read(shared, &expirations, 8) >= 0 || errno != EAGAIN) return 10;
    close(timer);
    ready.revents = 0;
    if (poll(&ready, 1, 5000) != 1 || read(alias, &expirations, 8) != 8 || expirations < 1) return 8;
    close(alias);
    close(disarmed);
    close(shared);
    close(report[0]);
    dprintf(STDOUT_FILENO, "TIMERFD-RESTORED\n");
    return 0;
}
