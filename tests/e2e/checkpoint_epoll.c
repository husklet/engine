#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

enum { DATA_EDGE = 0x11, DATA_ONESHOT = 0x22, DATA_INOTIFY = 0x33, DATA_SECOND = 0x44, CAPACITY_FDS = 1100 };

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

static int wait_data(int epoll, uint64_t expected) {
    struct epoll_event events[8];
    for (int attempt = 0; attempt < 500; ++attempt) {
        int count = epoll_wait(epoll, events, 8, 10);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        for (int index = 0; index < count; ++index)
            if (events[index].data.u64 == expected) return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    char directory[1024], before[1100], capacity_path[1100];
    int capacity_fds[CAPACITY_FDS];
    int edge[2], oneshot[2];
    if (argc != 2 || prepare_output(argv[1]) != 0 || pipe2(edge, O_NONBLOCK | O_CLOEXEC) != 0 ||
        pipe2(oneshot, O_NONBLOCK) != 0 ||
        snprintf(directory, sizeof directory, "%s.epoll-watch", argv[1]) >= (int)sizeof directory ||
        snprintf(before, sizeof before, "%s/before", directory) >= (int)sizeof before || mkdir(directory, 0700) != 0)
        return 2;
    if (snprintf(capacity_path, sizeof capacity_path, "%s/capacity", directory) >= (int)sizeof capacity_path)
        return 2;
    capacity_fds[0] = open(capacity_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (capacity_fds[0] < 0) return 2;
    for (int index = 1; index < CAPACITY_FDS; ++index) {
        capacity_fds[index] = dup(capacity_fds[0]);
        if (capacity_fds[index] < 0) return 2;
    }
    int epoll = epoll_create1(EPOLL_CLOEXEC);
    int alias = dup(epoll);
    int second = epoll_create1(0);
    int edge_alias = dup(edge[0]);
    int inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = inotify_add_watch(inotify, directory, IN_CREATE);
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = DATA_EDGE};
    if (epoll < 0 || alias < 0 || second < 0 || edge_alias < 0 || inotify < 0 || wd < 0 ||
        epoll_ctl(epoll, EPOLL_CTL_ADD, edge[0], &event) != 0)
        return 3;
    event = (struct epoll_event){.events = EPOLLIN | EPOLLET, .data.u64 = DATA_SECOND};
    if (epoll_ctl(second, EPOLL_CTL_ADD, edge[0], &event) != 0) return 3;
    event = (struct epoll_event){.events = EPOLLIN | EPOLLONESHOT, .data.u64 = DATA_ONESHOT};
    if (epoll_ctl(alias, EPOLL_CTL_ADD, oneshot[0], &event) != 0 || write(oneshot[1], "o", 1) != 1 ||
        wait_data(epoll, DATA_ONESHOT) != 0)
        return 4;
    event = (struct epoll_event){.events = EPOLLIN, .data.u64 = DATA_INOTIFY};
    if (epoll_ctl(epoll, EPOLL_CTL_ADD, inotify, &event) != 0 || write(edge[1], "e", 1) != 1) return 5;
    int file = open(before, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0 || close(file) != 0) return 5;
    close(edge[0]);
    edge[0] = edge_alias;
    dprintf(STDOUT_FILENO, "READY 1 epoll=%d alias=%d\n", epoll, alias);
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 6;
    if (!(fcntl(epoll, F_GETFD) & FD_CLOEXEC) || (fcntl(alias, F_GETFD) & FD_CLOEXEC) ||
        fcntl(capacity_fds[CAPACITY_FDS - 1], F_GETFD) < 0)
        return 7;
    close(epoll);
    epoll = alias;
    int saw_edge = 0, saw_inotify = 0, saw_oneshot = 0;
    for (int attempt = 0; attempt < 100 && (!saw_edge || !saw_inotify); ++attempt) {
        struct epoll_event events[8];
        int count = epoll_wait(alias, events, 8, 10);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return 8;
        for (int index = 0; index < count; ++index) {
            saw_edge |= events[index].data.u64 == DATA_EDGE;
            saw_inotify |= events[index].data.u64 == DATA_INOTIFY;
            saw_oneshot |= events[index].data.u64 == DATA_ONESHOT;
        }
        if (saw_oneshot) {
            dprintf(STDERR_FILENO, "epoll restore leaked disabled oneshot\n");
            return 8;
        }
    }
    if (!saw_edge || !saw_inotify || saw_oneshot || wait_data(second, DATA_SECOND) != 0) {
        dprintf(STDERR_FILENO, "epoll restore readiness edge=%d inotify=%d oneshot=%d\n", saw_edge, saw_inotify,
                saw_oneshot);
        return 8;
    }
    char byte;
    unsigned char notifications[512];
    if (read(edge[0], &byte, 1) != 1 || read(inotify, notifications, sizeof notifications) <= 0) return 9;
    event = (struct epoll_event){.events = EPOLLIN | EPOLLONESHOT, .data.u64 = DATA_ONESHOT};
    if (epoll_ctl(alias, EPOLL_CTL_MOD, oneshot[0], &event) != 0 || wait_data(epoll, DATA_ONESHOT) != 0 ||
        read(oneshot[0], &byte, 1) != 1)
        return 10;
    close(epoll);
    close(second);
    close(inotify);
    close(edge[0]);
    close(edge[1]);
    close(oneshot[0]);
    close(oneshot[1]);
    for (int index = 0; index < CAPACITY_FDS; ++index) close(capacity_fds[index]);
    unlink(before);
    unlink(capacity_path);
    rmdir(directory);
    dprintf(STDOUT_FILENO, "EPOLL-RESTORED\n");
    return 0;
}
