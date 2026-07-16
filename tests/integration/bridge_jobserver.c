#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int descriptors[2], status;
    pid_t child;
    if (argc < 5 || pipe(descriptors) != 0) return 2;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        if (dup2(descriptors[0], 3) != 3 || dup2(descriptors[1], 4) != 4 ||
            setenv("MAKEFLAGS", "-j2 --jobserver-auth=3,4", 1) != 0 || setenv("MFLAGS", "-j2", 1) != 0)
            _exit(2);
        execv(argv[1], argv + 1);
        _exit(127);
    }
    close(descriptors[0]);
    close(descriptors[1]);
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return 2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
