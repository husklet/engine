#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#define MFD_ALLOW_SEALING 0x0002u
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_WRITE 0x0008
#endif

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

static int receive_exact(int fd, const char *expected, int message) {
    char buffer[128];
    size_t wanted = strlen(expected);
    for (int attempt = 0; attempt < 500; ++attempt) {
        ssize_t count = recv(fd, buffer, sizeof buffer, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            usleep(10000);
            continue;
        }
        if (count < 0) return -1;
        if (message) return (size_t)count == wanted && memcmp(buffer, expected, wanted) == 0 ? 0 : -1;
        if ((size_t)count != wanted || memcmp(buffer, expected, wanted) != 0) return -1;
        return 0;
    }
    return -1;
}

static int send_fd(int socket_fd, int fd) {
    char byte = 'R';
    struct iovec iov = {&byte, 1};
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof fd);
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

static int receive_fd(int socket_fd) {
    char byte;
    struct iovec iov = {&byte, 1};
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    if (recvmsg(socket_fd, &message, 0) != 1 || byte != 'R') return -1;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    int fd = -1;
    if (header == NULL || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int))) {
        dprintf(STDERR_FILENO, "receive_fd header=%p level=%d type=%d len=%llu flags=%x control=%llu\n",
                (void *)header, header ? header->cmsg_level : -1, header ? header->cmsg_type : -1,
                (unsigned long long)(header ? header->cmsg_len : 0), message.msg_flags,
                (unsigned long long)message.msg_controllen);
        return -1;
    }
    memcpy(&fd, CMSG_DATA(header), sizeof fd);
    return fd;
}

int main(int argc, char **argv) {
    int stream[2], packet[2], rights[2], event_rights[2], timer_rights[2], memfd_rights[2], pipe_rights[2];
    int pipe_write_rights[2], shared_pipe[2];
    int signal_rights[2], inotify_rights[2], epoll_rights[2], epoll_pipe[2];
    char rights_path[1024];
    if (argc != 2 || prepare_output(argv[1]) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, stream) != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, packet) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, event_rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, timer_rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, memfd_rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_write_rights) != 0 || pipe(shared_pipe) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, signal_rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, inotify_rights) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, epoll_rights) != 0 || pipe(epoll_pipe) != 0 ||
        snprintf(rights_path, sizeof rights_path, "%s.rights", argv[1]) >= (int)sizeof rights_path)
        return 2;
    int shared_file = open(rights_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (shared_file < 0 || write(shared_file, "shared-offset-content", 21) != 21 ||
        lseek(shared_file, 0, SEEK_SET) != 0 || unlink(rights_path) != 0)
        return 2;
    int queued_eventfd = -1;
    uint64_t event_value = 9;
    pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        close(stream[0]);
        close(packet[0]);
        close(rights[0]);
        close(event_rights[0]);
        close(timer_rights[0]);
        close(memfd_rights[0]);
        close(pipe_rights[0]);
        close(pipe_write_rights[0]);
        close(shared_pipe[1]);
        close(signal_rights[0]);
        close(inotify_rights[0]);
        close(epoll_rights[0]);
        close(epoll_pipe[1]);
        sigset_t signal_mask;
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) != 0) return 4;
        close(shared_file);
        if (send(stream[1], "to-parent", 9, 0) != 9 || send(packet[1], "cp-one", 6, 0) != 6 ||
            send(packet[1], "cp-two", 6, 0) != 6)
            return 4;
        dprintf(STDOUT_FILENO, "READY 2\n");
        while (access(argv[1], F_OK) != 0)
            if (errno != ENOENT) return 5;
        int received_file = receive_fd(rights[1]);
        int received_eventfd = receive_fd(event_rights[1]);
        int received_timerfd = receive_fd(timer_rights[1]);
        int received_memfd = receive_fd(memfd_rights[1]);
        int received_pipe = receive_fd(pipe_rights[1]);
        int received_pipe_writer = receive_fd(pipe_write_rights[1]);
        int received_signalfd = receive_fd(signal_rights[1]);
        int received_inotify = receive_fd(inotify_rights[1]);
        int received_epoll = receive_fd(epoll_rights[1]);
        int stream_result = receive_exact(stream[1], "to-child", 0);
        int go_result = receive_exact(rights[1], "G", 0);
        int packet_one_result = receive_exact(packet[1], "pc-one", 1);
        int packet_two_result = receive_exact(packet[1], "pc-two", 1);
        if (received_file < 0 || received_eventfd < 0 || received_timerfd < 0 || received_memfd < 0 ||
            received_pipe < 0 || received_pipe_writer < 0 || received_signalfd < 0 || received_inotify < 0 ||
            received_epoll < 0 ||
            stream_result != 0 || go_result != 0 ||
            packet_one_result != 0 || packet_two_result != 0) {
            dprintf(STDERR_FILENO, "queued rights file=%d event=%d stream=%d go=%d packet=%d/%d errno=%d\n",
                    received_file, received_eventfd, stream_result, go_result, packet_one_result,
                    packet_two_result, errno);
            return 6;
        }
        char tail[32] = {0};
        ssize_t tail_read = read(received_file, tail, sizeof tail - 1);
        if (tail_read != 14 || strcmp(tail, "offset-content") != 0) {
            dprintf(STDERR_FILENO, "queued file read=%lld tail=%s errno=%d\n", (long long)tail_read, tail, errno);
            return 6;
        }
        uint64_t restored_event = 0;
        ssize_t event_read = read(received_eventfd, &restored_event, sizeof restored_event);
        if (event_read != sizeof restored_event || restored_event != event_value) {
            dprintf(STDERR_FILENO, "queued eventfd fd=%d read=%lld value=%llu expected=%llu errno=%d\n",
                    received_eventfd, (long long)event_read, (unsigned long long)restored_event,
                    (unsigned long long)event_value, errno);
            return 6;
        }
        uint64_t restored_timer = 0;
        ssize_t timer_read = read(received_timerfd, &restored_timer, sizeof restored_timer);
        if (timer_read != sizeof restored_timer || restored_timer == 0) {
            dprintf(STDERR_FILENO, "queued timerfd fd=%d read=%lld value=%llu errno=%d\n",
                    received_timerfd, (long long)timer_read, (unsigned long long)restored_timer, errno);
            return 6;
        }
        char memfd_data[16] = {0};
        int memfd_seals = fcntl(received_memfd, F_GET_SEALS);
        errno = 0;
        ssize_t sealed_write = pwrite(received_memfd, "X", 1, 0);
        int sealed_errno = errno;
        if (pread(received_memfd, memfd_data, 13, 0) != 13 || strcmp(memfd_data, "queued-memfd") != 0 ||
            memfd_seals != (F_SEAL_SEAL | F_SEAL_WRITE) || sealed_write != -1 || sealed_errno != EPERM) {
            dprintf(STDERR_FILENO, "queued memfd fd=%d data=%s seals=%x write=%lld errno=%d\n",
                    received_memfd, memfd_data, memfd_seals, (long long)sealed_write, sealed_errno);
            return 6;
        }
        char pipe_data[16] = {0};
        ssize_t pipe_read = read(received_pipe, pipe_data, 11);
        ssize_t pipe_eof = read(received_pipe, pipe_data + 12, 1);
        if (pipe_read != 11 || memcmp(pipe_data, "queued-pipe", 11) != 0 || pipe_eof != 0) {
            dprintf(STDERR_FILENO, "queued pipe fd=%d read=%lld data=%s eof=%lld errno=%d\n",
                    received_pipe, (long long)pipe_read, pipe_data, (long long)pipe_eof, errno);
            return 6;
        }
        char shared_pipe_data[16] = {0};
        if (write(received_pipe_writer, "linked-pipe", 11) != 11 ||
            read(shared_pipe[0], shared_pipe_data, 11) != 11 ||
            memcmp(shared_pipe_data, "linked-pipe", 11) != 0) {
            dprintf(STDERR_FILENO, "queued pipe writer=%d data=%s errno=%d\n", received_pipe_writer,
                    shared_pipe_data, errno);
            return 6;
        }
        struct signalfd_siginfo signal_info;
        struct pollfd signal_poll = {received_signalfd, POLLIN, 0};
        memset(&signal_info, 0, sizeof signal_info);
        if (kill(getpid(), SIGUSR1) != 0 || poll(&signal_poll, 1, 2000) != 1 ||
            read(received_signalfd, &signal_info, sizeof signal_info) != (ssize_t)sizeof signal_info ||
            signal_info.ssi_signo != SIGUSR1) {
            dprintf(STDERR_FILENO, "queued signalfd fd=%d signo=%u revents=%x errno=%d\n",
                    received_signalfd, signal_info.ssi_signo, signal_poll.revents, errno);
            return 6;
        }
        char queued_watch_dir[1024], queued_watch_file[1100];
        if (snprintf(queued_watch_dir, sizeof queued_watch_dir, "%s.queued-inotify", argv[1]) >=
                (int)sizeof queued_watch_dir ||
            snprintf(queued_watch_file, sizeof queued_watch_file, "%s/created", queued_watch_dir) >=
                (int)sizeof queued_watch_file)
            return 6;
        int created = open(queued_watch_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        struct pollfd inotify_poll = {received_inotify, POLLIN, 0};
        unsigned char inotify_buffer[512];
        ssize_t inotify_count = -1;
        if (created >= 0) close(created);
        if (created < 0 || poll(&inotify_poll, 1, 2000) != 1 ||
            (inotify_count = read(received_inotify, inotify_buffer, sizeof inotify_buffer)) <= 0 ||
            !(((struct inotify_event *)inotify_buffer)->mask & IN_CREATE)) {
            dprintf(STDERR_FILENO, "queued inotify fd=%d poll=%x count=%zd errno=%d\n", received_inotify,
                    inotify_poll.revents, inotify_count, errno);
            return 6;
        }
        unlink(queued_watch_file);
        rmdir(queued_watch_dir);
        struct epoll_event restored_epoll_event;
        memset(&restored_epoll_event, 0, sizeof restored_epoll_event);
        char epoll_byte = 0;
        if (epoll_wait(received_epoll, &restored_epoll_event, 1, 2000) != 1 ||
            restored_epoll_event.data.u64 != UINT64_C(0x45504f4c4c) ||
            !(restored_epoll_event.events & EPOLLIN) || read(epoll_pipe[0], &epoll_byte, 1) != 1 ||
            epoll_byte != 'E') {
            dprintf(STDERR_FILENO, "queued epoll fd=%d events=%x data=%llx byte=%c errno=%d\n", received_epoll,
                    restored_epoll_event.events, (unsigned long long)restored_epoll_event.data.u64, epoll_byte,
                    errno);
            return 6;
        }
        close(received_file);
        close(received_eventfd);
        close(received_timerfd);
        close(received_memfd);
        close(received_pipe);
        close(received_pipe_writer);
        close(received_signalfd);
        close(received_inotify);
        close(received_epoll);
        close(rights[1]);
        close(event_rights[1]);
        close(timer_rights[1]);
        close(memfd_rights[1]);
        close(pipe_rights[1]);
        close(pipe_write_rights[1]);
        close(shared_pipe[0]);
        close(signal_rights[1]);
        close(inotify_rights[1]);
        close(epoll_rights[1]);
        close(epoll_pipe[0]);
        return 0;
    }
    close(stream[1]);
    close(packet[1]);
    close(rights[1]);
    close(event_rights[1]);
    close(timer_rights[1]);
    close(memfd_rights[1]);
    close(pipe_rights[1]);
    close(pipe_write_rights[1]);
    close(shared_pipe[0]);
    close(signal_rights[1]);
    close(inotify_rights[1]);
    close(epoll_rights[1]);
    queued_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int queued_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    int queued_memfd = (int)syscall(SYS_memfd_create, "queued-checkpoint", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    int queued_pipe[2];
    sigset_t queued_signal_mask;
    sigemptyset(&queued_signal_mask);
    sigaddset(&queued_signal_mask, SIGUSR1);
    int queued_signalfd = signalfd(-1, &queued_signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    char queued_watch_dir[1024];
    if (snprintf(queued_watch_dir, sizeof queued_watch_dir, "%s.queued-inotify", argv[1]) >=
            (int)sizeof queued_watch_dir ||
        mkdir(queued_watch_dir, 0700) != 0)
        return 7;
    int queued_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int queued_inotify_wd = queued_inotify >= 0 ? inotify_add_watch(queued_inotify, queued_watch_dir, IN_CREATE) : -1;
    int queued_epoll = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event queued_epoll_event = {.events = EPOLLIN, .data.u64 = UINT64_C(0x45504f4c4c)};
    struct itimerspec timer_spec = {.it_value = {.tv_nsec = 1}};
    if (queued_eventfd < 0 || write(queued_eventfd, &event_value, sizeof event_value) != sizeof event_value)
        return 7;
    if (queued_timerfd < 0) {
        dprintf(STDERR_FILENO, "queued timerfd create failed errno=%d\n", errno);
        return 7;
    }
    if (queued_memfd < 0 || write(queued_memfd, "queued-memfd", 13) != 13 ||
        fcntl(queued_memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_WRITE) != 0)
        return 7;
    if (pipe(queued_pipe) != 0 || write(queued_pipe[1], "queued-pipe", 11) != 11) return 7;
    if (queued_signalfd < 0 || queued_inotify < 0 || queued_inotify_wd < 0 || queued_epoll < 0 ||
        epoll_ctl(queued_epoll, EPOLL_CTL_ADD, epoll_pipe[0], &queued_epoll_event) != 0)
        return 7;
    if (timerfd_settime(queued_timerfd, 0, &timer_spec, NULL) != 0) {
        dprintf(STDERR_FILENO, "queued timerfd arm fd=%d failed errno=%d\n", queued_timerfd, errno);
        return 7;
    }
    usleep(10000);
    int alias = dup(stream[0]);
    if (alias < 0 || (fcntl(alias, F_GETFD) & FD_CLOEXEC) || !(fcntl(stream[0], F_GETFD) & FD_CLOEXEC) ||
        send_fd(rights[0], shared_file) != 0 || send_fd(event_rights[0], queued_eventfd) != 0 ||
        send(stream[0], "to-child", 8, 0) != 8 ||
        send(packet[0], "pc-one", 6, 0) != 6 ||
        send(packet[0], "pc-two", 6, 0) != 6)
        return 7;
    if (send_fd(timer_rights[0], queued_timerfd) != 0) {
        dprintf(STDERR_FILENO, "queued timerfd send fd=%d failed errno=%d\n", queued_timerfd, errno);
        return 7;
    }
    if (send_fd(memfd_rights[0], queued_memfd) != 0) return 7;
    if (send_fd(pipe_rights[0], queued_pipe[0]) != 0) return 7;
    if (send_fd(pipe_write_rights[0], shared_pipe[1]) != 0) return 7;
    if (send_fd(signal_rights[0], queued_signalfd) != 0) return 7;
    if (send_fd(inotify_rights[0], queued_inotify) != 0) return 7;
    if (send_fd(epoll_rights[0], queued_epoll) != 0 || write(epoll_pipe[1], "E", 1) != 1) return 7;
    close(queued_eventfd);
    close(queued_timerfd);
    close(queued_memfd);
    close(queued_pipe[0]);
    close(queued_pipe[1]);
    close(shared_pipe[1]);
    close(queued_signalfd);
    close(queued_inotify);
    close(queued_epoll);
    close(epoll_pipe[0]);
    close(epoll_pipe[1]);
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 8;
    if ((fcntl(alias, F_GETFD) & FD_CLOEXEC) || !(fcntl(stream[0], F_GETFD) & FD_CLOEXEC)) return 9;
    close(stream[0]);
    char prefix[8];
    if (read(shared_file, prefix, 7) != 7 || send(rights[0], "G", 1, 0) != 1) return 10;
    int stream_ok = receive_exact(alias, "to-parent", 0);
    int packet_one_ok = receive_exact(packet[0], "cp-one", 1);
    int packet_two_ok = receive_exact(packet[0], "cp-two", 1);
    if (stream_ok != 0 || packet_one_ok != 0 || packet_two_ok != 0) {
        dprintf(STDERR_FILENO, "socketpair parent queue stream=%d packet1=%d packet2=%d\n", stream_ok,
                packet_one_ok, packet_two_ok);
        return 10;
    }
    int status;
    pid_t waited = waitpid(child, &status, 0);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        dprintf(STDERR_FILENO, "socketpair child waited=%d expected=%d status=%x exit=%d\n", (int)waited,
                (int)child, status, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 11;
    }
    close(alias);
    close(packet[0]);
    close(rights[0]);
    close(event_rights[0]);
    close(timer_rights[0]);
    close(memfd_rights[0]);
    close(pipe_rights[0]);
    close(pipe_write_rights[0]);
    close(signal_rights[0]);
    close(inotify_rights[0]);
    close(epoll_rights[0]);
    close(shared_file);
    dprintf(STDOUT_FILENO, "SOCKETPAIR-RESTORED\n");
    return 0;
}
