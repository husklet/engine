#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { HL_E2E_OUTPUT_LIMIT = 1024 * 1024 };

typedef struct hl_e2e_result {
    char *output;
    size_t output_size;
    int status;
} hl_e2e_result;

static int drain_output(int fd, hl_e2e_result *result) {
    for (;;) {
        ssize_t count;
        if (result->output_size == HL_E2E_OUTPUT_LIMIT) return 1;
        count = read(fd, result->output + result->output_size, HL_E2E_OUTPUT_LIMIT - result->output_size);
        if (count > 0) {
            result->output_size += (size_t)count;
            continue;
        }
        if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno != EINTR) return 1;
    }
}

static int run_process(const char *bridge, const char *engine, const char *guest, unsigned int timeout_ms,
                       hl_e2e_result *result) {
    const struct timespec tick = {0, 10000000};
    unsigned int elapsed_ms = 0;
    int output_pipe[2];
    pid_t child;
    memset(result, 0, sizeof(*result));
    result->output = malloc(HL_E2E_OUTPUT_LIMIT);
    if (result->output == NULL || pipe(output_pipe) != 0) return 1;
    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(output_pipe[1]);
        if (bridge != NULL)
            execlp(bridge, bridge, engine, guest, (char *)NULL);
        else
            execl(guest, guest, (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) < 0) return 1;
    while (elapsed_ms < timeout_ms) {
        pid_t waited;
        if (drain_output(output_pipe[0], result) != 0) return 1;
        waited = waitpid(child, &result->status, WNOHANG);
        if (waited == child) {
            int flags = fcntl(output_pipe[0], F_GETFL);
            if (flags >= 0) (void)fcntl(output_pipe[0], F_SETFL, flags & ~O_NONBLOCK);
            if (drain_output(output_pipe[0], result) != 0) return 1;
            close(output_pipe[0]);
            return 0;
        }
        if (waited < 0 && errno != EINTR) {
            perror("waitpid");
            return 1;
        }
        nanosleep(&tick, NULL);
        elapsed_ms += 10;
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(output_pipe[0]);
    return 2;
}

static int exit_matches(const hl_e2e_result *result, int expected_exit) {
    return WIFEXITED(result->status) && WEXITSTATUS(result->status) == expected_exit;
}

int main(int argc, char **argv) {
    hl_e2e_result oracle;
    hl_e2e_result guest;
    int expected_exit;
    int status;
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: e2e-runner BRIDGE ENGINE GUEST EXPECTED_EXIT [NATIVE_ORACLE]\n");
        return 2;
    }
    expected_exit = atoi(argv[4]);
    if (argc == 6) {
        status = run_process(NULL, NULL, argv[5], 30000, &oracle);
        if (status != 0 || !exit_matches(&oracle, expected_exit)) {
            fprintf(stderr, "native oracle %s failed or timed out (status=%d raw=%d)\n", argv[5], status,
                    oracle.status);
            return 1;
        }
    } else {
        memset(&oracle, 0, sizeof oracle);
    }
    status = run_process(argv[1], argv[2], argv[3], 30000, &guest);
    if (guest.output_size != 0) (void)fwrite(guest.output, 1, guest.output_size, stdout);
    if (status != 0 || !exit_matches(&guest, expected_exit)) {
        fprintf(stderr, "%s running %s: expected exit %d, status=%d raw=%d\n", argv[2], argv[3], expected_exit, status,
                guest.status);
        return 1;
    }
    if (argc == 6 &&
        (guest.output_size != oracle.output_size || memcmp(guest.output, oracle.output, guest.output_size) != 0)) {
        fprintf(stderr, "%s running %s: stdout differs from native oracle %s\n", argv[2], argv[3], argv[5]);
        return 1;
    }
    free(oracle.output);
    free(guest.output);
    return 0;
}
