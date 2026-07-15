#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CAPTURE_SIZE = 20000 };

static int write_repeated(int descriptor, unsigned char value) {
    unsigned char buffer[1024];
    size_t remaining = CAPTURE_SIZE;
    memset(buffer, value, sizeof buffer);
    while (remaining != 0) {
        size_t amount = remaining < sizeof buffer ? remaining : sizeof buffer;
        ssize_t count = write(descriptor, buffer, amount);
        if (count < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (count == 0) return 1;
        remaining -= (size_t)count;
    }
    return 0;
}

static int verify_capture(const char *path, unsigned char value) {
    unsigned char buffer[1024];
    size_t total = 0;
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return 1;
    for (;;) {
        ssize_t count = read(descriptor, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) break;
        if (count == 0) {
            close(descriptor);
            return total != CAPTURE_SIZE;
        }
        for (ssize_t index = 0; index < count; ++index)
            if (buffer[index] != value) {
                close(descriptor);
                return 1;
            }
        total += (size_t)count;
    }
    close(descriptor);
    return 1;
}

static uint64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

int main(int argc, char **argv) {
    int heartbeat[2], status = 0;
    pid_t child;
    if (argc == 2 && strcmp(argv[1], "--emit") == 0)
        return write_repeated(STDOUT_FILENO, 'o') || write_repeated(STDERR_FILENO, 'e');
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
    {
        char output[] = "/tmp/hl-supervisor-out-XXXXXX";
        char error[] = "/tmp/hl-supervisor-err-XXXXXX";
        int output_fd = mkstemp(output), error_fd = mkstemp(error);
        pid_t capture;
        if (output_fd < 0 || error_fd < 0) return 9;
        close(output_fd);
        close(error_fd);
        if (unlink(output) != 0 || unlink(error) != 0) return 10;
        capture = fork();
        if (capture < 0) return 11;
        if (capture == 0) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd < 0 || dup2(null_fd, STDERR_FILENO) < 0) _exit(126);
            close(null_fd);
            execl(argv[1], argv[1], "--capture", output, error, argv[0], "--emit", (char *)NULL);
            _exit(127);
        }
        if (waitpid(capture, &status, 0) != capture || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            verify_capture(output, 'o') != 0 || verify_capture(error, 'e') != 0) {
            unlink(output);
            unlink(error);
            return 12;
        }
        unlink(output);
        unlink(error);
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
