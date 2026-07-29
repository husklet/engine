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

#include "launch.h"

enum { HL_E2E_OUTPUT_LIMIT = 1024 * 1024 };

/* Per-case guest budget and its host-backend scale; tools/matrix_runner.c is the reference and the knob is
   identical in all six runners. 30s assumes a JIT host; without one a correct
   case is killed and reported as a hang. Scale 1 is bit-for-bit the unscaled runner; a bad value is refused,
   not rounded to 1. */
enum { CASE_TIMEOUT_MS = 30000, TIMEOUT_SCALE_MAX = 100 };

static unsigned long timeout_scale = 1;

static unsigned int case_timeout_ms(void) {
    return (unsigned int)((unsigned long)CASE_TIMEOUT_MS * timeout_scale);
}

static int load_timeout_scale(void) {
    const char *value = getenv("HL_MATRIX_TIMEOUT_SCALE");
    char *end = NULL;
    unsigned long parsed;
    if (value == NULL || *value == 0) return 0;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    /* strtoul() accepts a sign, wrapping "-1" to ULONG_MAX, so the first character is checked directly. */
    if (value[0] < '0' || value[0] > '9' || errno != 0 || end == value || *end != 0 || parsed < 1 ||
        parsed > TIMEOUT_SCALE_MAX) {
        fprintf(stderr,
                "e2e-runner: HL_MATRIX_TIMEOUT_SCALE=\"%s\" is not a decimal factor in [1, %d]; refusing to run "
                "rather than silently using the unscaled per-case timeout\n",
                value, TIMEOUT_SCALE_MAX);
        return 1;
    }
    timeout_scale = parsed;
    /* On stdout, in a PASSING case: a green scaled lane must not read as evidence of comparable speed. */
    if (timeout_scale == 1) return 0;
    printf("e2e-runner: per-case timeout scaled x%lu to %ums (HL_MATRIX_TIMEOUT_SCALE); this run tolerates "
           "slow-but-correct guest execution and is NOT evidence of speed comparable to an unscaled lane\n",
           timeout_scale, case_timeout_ms());
    return 0;
}

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
        hl_launch_hygiene();
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
    if (load_timeout_scale() != 0) return 2;
    expected_exit = atoi(argv[4]);
    if (argc == 6) {
        /* The oracle is a HOST-native binary run without the engine, so the scale never applies to it; the
           unscaled budget keeps the hang detector sharp where it still works. */
        status = run_process(NULL, NULL, argv[5], CASE_TIMEOUT_MS, &oracle);
        if (status != 0 || !exit_matches(&oracle, expected_exit)) {
            fprintf(stderr, "native oracle %s failed or timed out (status=%d raw=%d)\n", argv[5], status,
                    oracle.status);
            return 1;
        }
    } else {
        memset(&oracle, 0, sizeof oracle);
    }
    status = run_process(argv[1], argv[2], argv[3], case_timeout_ms(), &guest);
    if (guest.output_size != 0) (void)fwrite(guest.output, 1, guest.output_size, stdout);
    if (status != 0 || !exit_matches(&guest, expected_exit)) {
        fprintf(stderr, "%s running %s: expected exit %d, status=%d raw=%d\n", argv[2], argv[3], expected_exit, status,
                guest.status);
        /* Only when scaled, so unscaled failure output stays verbatim rather than naming a budget. */
        if (status == 2 && timeout_scale != 1)
            fprintf(stderr, "%s running %s: timed out after %ums (HL_MATRIX_TIMEOUT_SCALE=%lu)\n", argv[2], argv[3],
                    case_timeout_ms(), timeout_scale);
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
