#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <poll.h>
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
    int realtime = SIGRTMIN + 2;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, realtime);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) return 3;
    sigset_t fd_mask;
    sigemptyset(&fd_mask);
    sigaddset(&fd_mask, SIGUSR2);
    int signal_fd = signalfd(-1, &fd_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    int signal_alias = signal_fd >= 0 ? dup(signal_fd) : -1;
    if (signal_fd < 0 || signal_alias < 0 || (fcntl(signal_alias, F_GETFD) & FD_CLOEXEC) != 0 ||
        (fcntl(signal_fd, F_GETFD) & FD_CLOEXEC) == 0)
        return 3;
    union sigval first = {.sival_int = 111};
    union sigval second = {.sival_int = 222};
    union sigval standard = {.sival_int = 77};
    if (sigqueue(getpid(), realtime, first) != 0 || sigqueue(getpid(), realtime, second) != 0 ||
        sigqueue(getpid(), SIGUSR1, standard) != 0 || kill(getpid(), SIGUSR2) != 0)
        return 4;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 5;

    struct timespec timeout = {2, 0};
    struct pollfd poll_fd = {signal_alias, POLLIN, 0};
    struct signalfd_siginfo signal_info;
    memset(&signal_info, 0, sizeof signal_info);
    int poll_result = poll(&poll_fd, 1, 2000);
    ssize_t signal_read = read(signal_alias, &signal_info, sizeof signal_info);
    if (poll_result != 1 || signal_read != (ssize_t)sizeof signal_info || signal_info.ssi_signo != SIGUSR2 ||
        (fcntl(signal_alias, F_GETFD) & FD_CLOEXEC) != 0 || (fcntl(signal_fd, F_GETFD) & FD_CLOEXEC) == 0) {
        dprintf(STDERR_FILENO, "signalfd restore poll=%d read=%lld signo=%u flags=%x/%x errno=%d\n",
                poll_result, (long long)signal_read, signal_info.ssi_signo, fcntl(signal_fd, F_GETFD),
                fcntl(signal_alias, F_GETFD), errno);
        return 6;
    }
    siginfo_t info[3];
    memset(info, 0, sizeof info);
    int signal[3];
    for (int index = 0; index < 3; ++index) signal[index] = sigtimedwait(&mask, &info[index], &timeout);
    if (signal[0] != SIGUSR1 || info[0].si_value.sival_int != 77 || signal[1] != realtime ||
        info[1].si_value.sival_int != 111 || signal[2] != realtime || info[2].si_value.sival_int != 222) {
        dprintf(STDERR_FILENO, "signal restore signo=%d/%d/%d value=%d/%d/%d errno=%d\n", signal[0],
                signal[1], signal[2], info[0].si_value.sival_int, info[1].si_value.sival_int,
                info[2].si_value.sival_int, errno);
        return 6;
    }
    dprintf(STDOUT_FILENO, "SIGNAL-RESTORED\n");
    close(signal_alias);
    close(signal_fd);
    return 0;
}
