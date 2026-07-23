#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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
    static const char before[] = "before";
    static const char after[] = "-after";
    int descriptors[2], status;
    pid_t child;
    if (argc != 2 || prepare_output(argv[1]) != 0 || pipe(descriptors) != 0) return 2;
    if (write(descriptors[1], before, sizeof before - 1) != (ssize_t)(sizeof before - 1)) return 3;
    child = fork();
    if (child < 0) return 4;
    if (child == 0) {
        char bytes[sizeof before + sizeof after - 1];
        size_t used = 0;
        close(descriptors[1]);
        dprintf(STDOUT_FILENO, "READY 1\n");
        while (access(argv[1], F_OK) != 0)
            if (errno != ENOENT) return 5;
        while (used < sizeof bytes) {
            ssize_t count = read(descriptors[0], bytes + used, sizeof bytes - used);
            if (count > 0) used += (size_t)count;
            else if (count == 0) break;
            else if (errno != EINTR) return 6;
        }
        if (used != sizeof bytes - 1 || memcmp(bytes, "before-after", sizeof bytes - 1) != 0) return 7;
        dprintf(STDOUT_FILENO, "PIPE-RESTORED\n");
        return 21;
    }
    close(descriptors[0]);
    dprintf(STDOUT_FILENO, "READY 2\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 8;
    if (write(descriptors[1], after, sizeof after - 1) != (ssize_t)(sizeof after - 1)) return 9;
    close(descriptors[1]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 21) return 10;
    return 0;
}
