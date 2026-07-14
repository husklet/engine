#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

int main(int argc, char **argv) {
    int heartbeat[2], status = 0;
    pid_t child;
    if (argc != 2 || pipe(heartbeat) != 0) return 1;
    {
        uint64_t started = now_ms();
        pid_t fast = fork();
        if (fast < 0) return 6;
        if (fast == 0) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd < 0 || dup2(null_fd, STDERR_FILENO) < 0) _exit(126);
            close(null_fd);
            execl(argv[1], argv[1], "/usr/bin/true", (char *)NULL);
            _exit(127);
        }
        if (waitpid(fast, &status, 0) != fast || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 7;
        if (now_ms() - started >= 200) return 8;
    }
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        close(heartbeat[0]);
        if (dup2(heartbeat[1], STDERR_FILENO) < 0) _exit(126);
        close(heartbeat[1]);
        execl(argv[1], argv[1], "/bin/sleep", "30", (char *)NULL);
        _exit(127);
    }
    close(heartbeat[1]);
    {
        char byte;
        if (read(heartbeat[0], &byte, 1) != 1 || byte != '\036') return 3;
    }
    close(heartbeat[0]); /* Simulate the local bridge disappearing. */
    uint64_t deadline = now_ms() + 3000;
    while (waitpid(child, &status, WNOHANG) == 0 && now_ms() < deadline) {
        struct timespec pause = {0, 10000000};
        (void)nanosleep(&pause, NULL);
    }
    if (waitpid(child, &status, WNOHANG) == 0) {
        (void)kill(child, SIGKILL);
        return 4;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 124 ? 0 : 5;
}
