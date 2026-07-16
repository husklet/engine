#include "hl/activation.h"
#include "hl/config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int config(const char *guest, char path[64]) {
    hl_launch_config launch = {0};
    char pool[4096] = {0};
    size_t guest_size = strlen(guest) + 1;
    int descriptor;
    if (guest_size + 2 > sizeof(pool)) return -1;
    strcpy(path, "/tmp/hl-activation.XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    memcpy(pool + 1, guest, guest_size);
    launch.magic = HL_CONFIG_MAGIC;
    launch.abi = HL_CONFIG_ABI;
    launch.header_size = sizeof(launch);
    launch.pool_size = (uint32_t)(guest_size + 2);
    launch.uid = -1;
    launch.gid = -1;
    launch.arguments_offset = 1;
    if (write(descriptor, &launch, sizeof(launch)) != sizeof(launch) ||
        write(descriptor, pool, launch.pool_size) != launch.pool_size || close(descriptor) != 0) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    return 0;
}

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

static int forward_signal(const char *self, const char *guest, int signal_number, const char expected[13]) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    hl_activation_stdio stdio;
    uint64_t process_id = 0;
    char output[13];
    size_t used = 0;
    int descriptors[2];
    hl_status status;
    char config_path[64];
    if (config(guest, config_path) != 0) return 6;
    if (pipe(descriptors) != 0) return 6;
    stdio = (hl_activation_stdio){.input = -1, .output = descriptors[1], .error = descriptors[1]};
    status = hl_activation_start_with_stdio(self, HL_GUEST_ISA_AARCH64, config_path, &stdio, &process);
    close(descriptors[1]);
    if (status != HL_STATUS_OK || hl_activation_process_id(process, &process_id) != HL_STATUS_OK) {
        close(descriptors[0]);
        hl_activation_process_destroy(process);
        return 7;
    }
    while (used < 5) {
        ssize_t count = read(descriptors[0], output + used, 5 - used);
        if (count <= 0) { close(descriptors[0]); hl_activation_process_destroy(process); return 8; }
        used += (size_t)count;
    }
    if (memcmp(output, "READY", 5) != 0) {
        close(descriptors[0]); hl_activation_process_destroy(process); return 9;
    }
    if (kill((pid_t)process_id, signal_number) != 0) {
        close(descriptors[0]); hl_activation_process_destroy(process); return 11;
    }
    while (used < sizeof(output)) {
        ssize_t count = read(descriptors[0], output + used, sizeof(output) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
    }
    status = hl_activation_wait(process, &result);
    hl_activation_process_destroy(process);
    close(descriptors[0]);
    return status == HL_STATUS_OK && result.kind == HL_ENGINE_EXIT_CODE && result.guest_status == 0 &&
                   used == sizeof(output) && memcmp(output, expected, sizeof(output)) == 0
               ? 0
               : 10;
}

int main(int argc, char **argv) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    uint32_t ready = 0;
    uint64_t process_id = 0;
    int32_t master = 99;
    hl_terminal_size empty = {0};

    if (hl_activation_start(NULL, HL_GUEST_ISA_AARCH64, NULL, &process) != HL_STATUS_INVALID_ARGUMENT ||
        process != NULL || hl_activation_process_id(NULL, &process_id) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_wait(NULL, &result) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_try_wait(NULL, &ready, &result) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_kill(NULL) != HL_STATUS_INVALID_ARGUMENT || sizeof(hl_terminal_size) != 4 ||
        hl_activation_start_terminal(argv[0], HL_GUEST_ISA_AARCH64, argv[0], empty, &master, &process) !=
            HL_STATUS_INVALID_ARGUMENT || master != -1 || process != NULL ||
        hl_terminal_resize(-1, (hl_terminal_size){.rows = 24, .columns = 80}) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_spawn(NULL, HL_GUEST_ISA_AARCH64, NULL, NULL) != HL_STATUS_INVALID_ARGUMENT)
        return 1;
    hl_activation_process_destroy(NULL);
    if (argc >= 2) {
        int stopped = force_stop_descendants(argv[0], argv[1]);
        if (stopped != 0) return stopped;
    }
    if (argc >= 3) {
        int forwarded = forward_signal(argv[0], argv[2], SIGTERM, "READYGOT_TERM");
        if (forwarded != 0) return forwarded;
        forwarded = forward_signal(argv[0], argv[2], SIGQUIT, "READYGOT_QUIT");
        if (forwarded != 0) return forwarded;
    }
    puts("installed hl-engine activation package: ok");
    return 0;
}
