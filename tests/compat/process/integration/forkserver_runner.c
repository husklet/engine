#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct result {
    char output[16384];
    size_t size;
    int status;
};

static int wait_child(pid_t pid, unsigned timeout_ms, int *status) {
    const struct timespec tick = {0, 10000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        pid_t got = waitpid(pid, status, WNOHANG);
        if (got == pid) return 0;
        if (got < 0 && errno != EINTR) return -1;
        nanosleep(&tick, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return -1;
}

static int run_guest(const char *bridge, const char *engine, const char *socket_path, const char *guest,
                     const char *command, const char *argument, const char *input, struct result *result) {
    int input_pipe[2], output_pipe[2];
    pid_t pid;
    memset(result, 0, sizeof(*result));
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(input_pipe[1]);
        close(output_pipe[0]);
        if (dup2(input_pipe[0], STDIN_FILENO) < 0 || dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(output_pipe[1], STDERR_FILENO) < 0)
            _exit(127);
        close(input_pipe[0]);
        close(output_pipe[1]);
        if (chdir("/tmp") != 0) _exit(127);
        if (socket_path != NULL) {
            if (argument != NULL)
                execlp(bridge, bridge, engine, "--client", socket_path, guest, command, argument, (char *)NULL);
            else
                execlp(bridge, bridge, engine, "--client", socket_path, guest, command, (char *)NULL);
        } else if (argument != NULL) {
            execlp(bridge, bridge, engine, guest, command, argument, (char *)NULL);
        } else {
            execlp(bridge, bridge, engine, guest, command, (char *)NULL);
        }
        _exit(127);
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    if (input != NULL && write(input_pipe[1], input, strlen(input)) != (ssize_t)strlen(input)) return -1;
    close(input_pipe[1]);
    while (result->size < sizeof(result->output)) {
        ssize_t count = read(output_pipe[0], result->output + result->size, sizeof(result->output) - result->size);
        if (count > 0) {
            result->size += (size_t)count;
            continue;
        }
        if (count == 0) break;
        if (errno != EINTR) return -1;
    }
    close(output_pipe[0]);
    return wait_child(pid, 30000, &result->status);
}

static pid_t start_server(const char *bridge, const char *engine, const char *socket_path, const char *prewarm) {
    const struct timespec tick = {0, 10000000};
    pid_t pid;
    unlink(socket_path);
    pid = fork();
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        if (prewarm != NULL)
            execlp(bridge, bridge, engine, "--server", socket_path, "--prewarm", prewarm, (char *)NULL);
        else
            execlp(bridge, bridge, engine, "--server", socket_path, (char *)NULL);
        _exit(127);
    }
    for (unsigned elapsed = 0; elapsed < 10000; elapsed += 10) {
        struct stat st;
        int status;
        if (stat(socket_path, &st) == 0 && S_ISSOCK(st.st_mode)) return pid;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        nanosleep(&tick, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static void stop_server(pid_t pid, const char *socket_path) {
    int status;
    kill(pid, SIGTERM);
    if (wait_child(pid, 3000, &status) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    unlink(socket_path);
}

static int same(const struct result *a, const struct result *b) {
    return a->status == b->status && a->size == b->size && memcmp(a->output, b->output, a->size) == 0;
}

static void report_difference(const char *label, const struct result *first, const struct result *second) {
    if (same(first, second)) return;
    fprintf(stderr, "%s mismatch: first status=%d size=%zu, second status=%d size=%zu\n", label, first->status,
            first->size, second->status, second->size);
    if (first->size != 0) fprintf(stderr, "%s first output: %.*s\n", label, (int)first->size, first->output);
    if (second->size != 0) fprintf(stderr, "%s second output: %.*s\n", label, (int)second->size, second->output);
}

static int shell_status(int status) {
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv) {
    char socket_path[256];
    char cwd[192];
    struct result cold, served, warm1, warm2;
    pid_t server;
    int identity = 0, stdio = 0, exitcode = 0, fatal = 0, warm = 0;
    char summary[128], expected[128];
    FILE *golden;
    if (argc != 5) {
        fprintf(stderr, "usage: forkserver-runner BRIDGE ENGINE GUEST GOLDEN\n");
        return 2;
    }
    if (getcwd(cwd, sizeof cwd) == NULL ||
        snprintf(socket_path, sizeof socket_path, "%s/build/hl-fsrv-%ld.sock", cwd, (long)getpid()) >=
            (int)sizeof socket_path)
        return 1;
    server = start_server(argv[1], argv[2], socket_path, NULL);
    if (server < 0) return 1;
    if (run_guest(argv[1], argv[2], NULL, argv[3], "id", NULL, NULL, &cold) == 0 &&
        run_guest(argv[1], argv[2], socket_path, argv[3], "id", NULL, NULL, &served) == 0)
        identity = same(&cold, &served) && WIFEXITED(cold.status) && WEXITSTATUS(cold.status) == 42;
    if (!identity) report_difference("identity", &cold, &served);
    if (run_guest(argv[1], argv[2], NULL, argv[3], "stdin", NULL, "forkserver-stdin\n", &cold) == 0 &&
        run_guest(argv[1], argv[2], socket_path, argv[3], "stdin", NULL, "forkserver-stdin\n", &served) == 0)
        stdio = same(&cold, &served) && WIFEXITED(cold.status) && WEXITSTATUS(cold.status) == 0;
    if (!stdio) report_difference("stdio", &cold, &served);
    if (run_guest(argv[1], argv[2], NULL, argv[3], "exit", "17", NULL, &cold) == 0 &&
        run_guest(argv[1], argv[2], socket_path, argv[3], "exit", "17", NULL, &served) == 0)
        exitcode = same(&cold, &served) && WIFEXITED(cold.status) && WEXITSTATUS(cold.status) == 17;
    if (!exitcode) report_difference("exit", &cold, &served);
    if (run_guest(argv[1], argv[2], NULL, argv[3], "segv", NULL, NULL, &cold) == 0 &&
        run_guest(argv[1], argv[2], socket_path, argv[3], "segv", NULL, NULL, &served) == 0)
        fatal = cold.size == served.size && memcmp(cold.output, served.output, cold.size) == 0 &&
                shell_status(cold.status) == 128 + SIGSEGV && shell_status(served.status) == 128 + SIGSEGV;
    if (!fatal) report_difference("fatal", &cold, &served);
    stop_server(server, socket_path);

    server = start_server(argv[1], argv[2], socket_path, argv[3]);
    if (server < 0) return 1;
    if (run_guest(argv[1], argv[2], socket_path, argv[3], "id", NULL, NULL, &warm1) == 0 &&
        run_guest(argv[1], argv[2], socket_path, argv[3], "id", NULL, NULL, &warm2) == 0)
        warm = same(&warm1, &warm2) && WIFEXITED(warm1.status) && WEXITSTATUS(warm1.status) == 42;
    if (!warm) report_difference("warm", &warm1, &warm2);
    stop_server(server, socket_path);
    snprintf(summary, sizeof summary, "forkserver identity=%d stdio=%d exit=%d fatal=%d warm=%d\n", identity,
             stdio, exitcode, fatal, warm);
    fputs(summary, stdout);
    golden = fopen(argv[4], "r");
    if (golden == NULL || fgets(expected, sizeof expected, golden) == NULL || fclose(golden) != 0 ||
        strcmp(summary, expected) != 0)
        return 1;
    return identity && stdio && exitcode && fatal && warm ? 0 : 1;
}
