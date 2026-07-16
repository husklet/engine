#include "hl/activation.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int force_stop_descendants(const char *self, const char *guest) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    uint32_t ready = 0;
    struct timespec delay = {.tv_nsec = 10000000};
    int output[2];
    hl_activation_stdio stdio;
    hl_status status;
    struct pollfd drained;
    char byte;
    int attempt;

    if (pipe(output) != 0) return 2;
    stdio = (hl_activation_stdio){.input = -1, .output = output[1], .error = output[1]};
    status = hl_activation_start_with_stdio(self, HL_GUEST_ISA_AARCH64, guest, &stdio, &process);
    close(output[1]);
    if (status != HL_STATUS_OK) { close(output[0]); return 3; }
    if (hl_activation_kill(process) != HL_STATUS_OK) { close(output[0]); return 4; }
    for (attempt = 0; attempt < 500; ++attempt) {
        status = hl_activation_try_wait(process, &ready, &result);
        if (status != HL_STATUS_OK || ready) break;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
    drained = (struct pollfd){.fd = output[0], .events = POLLIN | POLLHUP};
    if (status == HL_STATUS_OK && ready && poll(&drained, 1, 5000) > 0 && (drained.revents & POLLHUP) != 0 &&
        read(output[0], &byte, 1) == 0)
        attempt = 0;
    else
        attempt = -1;
    hl_activation_process_destroy(process);
    close(output[0]);
    return status == HL_STATUS_OK && ready && attempt == 0 && result.kind == HL_ENGINE_EXIT_SIGNAL &&
                   result.guest_status == 9
               ? 0
               : 5;
}

int main(int argc, char **argv) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    uint32_t ready = 0;
    uint64_t process_id = 0;

    if (hl_activation_start(NULL, HL_GUEST_ISA_AARCH64, NULL, &process) != HL_STATUS_INVALID_ARGUMENT ||
        process != NULL || hl_activation_process_id(NULL, &process_id) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_wait(NULL, &result) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_try_wait(NULL, &ready, &result) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_kill(NULL) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_spawn(NULL, HL_GUEST_ISA_AARCH64, NULL, NULL) != HL_STATUS_INVALID_ARGUMENT)
        return 1;
    hl_activation_process_destroy(NULL);
    if (argc == 2) {
        int stopped = force_stop_descendants(argv[0], argv[1]);
        if (stopped != 0) return stopped;
    }
    puts("installed hl-engine activation package: ok");
    return 0;
}
