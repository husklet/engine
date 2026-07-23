#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
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

static int wait_event(int instance, int expected_wd, const char *name) {
    unsigned char buffer[4096];
    for (int attempt = 0; attempt < 500; attempt++) {
        ssize_t count = read(instance, buffer, sizeof buffer);
        if (count < 0 && errno == EAGAIN) {
            usleep(10000);
            continue;
        }
        if (count < 0) return -1;
        for (size_t offset = 0; offset + sizeof(struct inotify_event) <= (size_t)count;) {
            struct inotify_event *event = (struct inotify_event *)(buffer + offset);
            if (event->wd == expected_wd && (event->mask & IN_CREATE) && event->len &&
                strcmp(event->name, name) == 0)
                return 0;
            offset += sizeof *event + event->len;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    char directory[1024], before[1100], after[1100];
    if (argc != 2 || prepare_output(argv[1]) != 0 ||
        snprintf(directory, sizeof directory, "%s.watch", argv[1]) >= (int)sizeof directory ||
        snprintf(before, sizeof before, "%s/before", directory) >= (int)sizeof before ||
        snprintf(after, sizeof after, "%s/after", directory) >= (int)sizeof after || mkdir(directory, 0700) != 0)
        return 2;
    int instance = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = inotify_add_watch(instance, directory, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    int alias = dup(instance);
    int file = open(before, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (instance < 0 || wd < 0 || alias < 0 || file < 0 || close(file) != 0 ||
        !(fcntl(instance, F_GETFD) & FD_CLOEXEC) || (fcntl(alias, F_GETFD) & FD_CLOEXEC))
        return 3;
    dprintf(STDOUT_FILENO, "READY 1 instance=%d alias=%d wd=%d\n", instance, alias, wd);
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 4;
    if (!(fcntl(instance, F_GETFD) & FD_CLOEXEC) || (fcntl(alias, F_GETFD) & FD_CLOEXEC) ||
        wait_event(alias, wd, "before") != 0)
        return 5;
    {
        unsigned char empty[64];
        if (read(instance, empty, sizeof empty) >= 0 || errno != EAGAIN) return 5;
    }
    file = open(after, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0 || close(file) != 0 || wait_event(instance, wd, "after") != 0) return 6;
    if (inotify_rm_watch(alias, wd) != 0) return 7;
    close(instance);
    close(alias);
    unlink(before);
    unlink(after);
    rmdir(directory);
    dprintf(STDOUT_FILENO, "INOTIFY-RESTORED\n");
    return 0;
}
