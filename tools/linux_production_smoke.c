#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int pipefd[2];
    pid_t child;
    char output[16];
    ssize_t used = 0;
    int status;
    if (argc != 3) return 2;
    if (pipe(pipefd) != 0) return 3;
    child = fork();
    if (child < 0) return 4;
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(125);
        (void)close(pipefd[1]);
        execl(argv[1], argv[1], argv[2], (char *)NULL);
        _exit(126);
    }
    (void)close(pipefd[1]);
    while (used < (ssize_t)sizeof(output)) {
        ssize_t count = read(pipefd[0], output + used, sizeof(output) - (size_t)used);
        if (count > 0) {
            used += count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    (void)close(pipefd[0]);
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return 5;
    if (used != 3 || memcmp(output, "hi\n", 3) != 0) return 6;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) return 7;
    puts("linux production x86-64 write/exit smoke: ok");
    return 0;
}
