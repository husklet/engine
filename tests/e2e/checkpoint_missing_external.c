#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static int released(const char *path) {
    if (access(path, F_OK) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

static int prepare_io(const char *release) {
    char output[1024];
    if (snprintf(output, sizeof output, "%s.output", release) >= (int)sizeof output) return -1;
    int descriptor = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor < 0 || dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0) return -1;
    if (descriptor > STDERR_FILENO) close(descriptor);
    descriptor = open("/dev/null", O_RDONLY);
    if (descriptor < 0 || dup2(descriptor, STDIN_FILENO) < 0) return -1;
    if (descriptor != STDIN_FILENO) close(descriptor);
    return 0;
}

int main(int argc, char **argv) {
    char external[1024];
    if (argc != 2 || snprintf(external, sizeof external, "%s.external", argv[1]) >= (int)sizeof external) return 2;
    if (prepare_io(argv[1]) != 0) return 2;
    pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        int fd = open(external, O_RDONLY | O_CREAT, 0600);
        if (fd < 0) return 4;
        dprintf(STDOUT_FILENO, "READY 1\n");
        int ready;
        while ((ready = released(argv[1])) == 0) usleep(1000);
        if (ready < 0) return 5;
        dprintf(STDOUT_FILENO, "CHILD-RESTORED\n");
        close(fd);
        return 0;
    }
    dprintf(STDOUT_FILENO, "READY 2\n");
    int ready;
    while ((ready = released(argv[1])) == 0) usleep(1000);
    if (ready < 0) return 7;
    dprintf(STDOUT_FILENO, "PARENT-RESTORED\n");
    return 0;
}
