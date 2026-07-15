#include "hl/activation.h"
#include "hl/config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct sigaction initial_handlers[3];
void hl_activation_test_mode(uint32_t mode);

__attribute__((constructor(101))) static void capture_handlers(void) {
    (void)sigaction(SIGILL, NULL, &initial_handlers[0]);
    (void)sigaction(SIGFPE, NULL, &initial_handlers[1]);
    (void)sigaction(SIGTRAP, NULL, &initial_handlers[2]);
}

static int handlers_unchanged(void) {
    const int signals[3] = {SIGILL, SIGFPE, SIGTRAP};
    struct sigaction current;
    size_t index;
    for (index = 0; index < 3; ++index) {
        if (sigaction(signals[index], NULL, &current) != 0 ||
            current.sa_sigaction != initial_handlers[index].sa_sigaction ||
            current.sa_flags != initial_handlers[index].sa_flags)
            return 0;
    }
    return 1;
}

static int write_full(int fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, bytes + offset, size - offset);
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static int make_config(const char *guest, char path[64]) {
    hl_launch_config config = {0};
    char pool[4096] = {0};
    size_t guest_size = strlen(guest) + 1;
    int fd;
    if (guest_size + 2 > sizeof(pool)) return -1;
    strcpy(path, "/tmp/hl-activation.XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    memcpy(pool + 1, guest, guest_size);
    config.magic = HL_CONFIG_MAGIC;
    config.abi = HL_CONFIG_ABI;
    config.header_size = sizeof(config);
    config.pool_size = (uint32_t)(guest_size + 2);
    config.uid = -1;
    config.gid = -1;
    config.arguments_offset = 1;
    if (write_full(fd, &config, sizeof(config)) != 0 || write_full(fd, pool, config.pool_size) != 0 ||
        close(fd) != 0) { close(fd); unlink(path); return -1; }
    return 0;
}

static int run_one(const char *executable, uint32_t guest_isa, const char *guest, int32_t expected) {
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    hl_engine_exit repeated = {.abi = HL_ENGINE_ABI, .size = sizeof(repeated)};
    hl_activation_process *process = NULL;
    uint64_t process_id = 0;
    uint32_t ready = 0;
    char config_path[64];
    hl_status status;
    if (make_config(guest, config_path) != 0) return 1;
    status = hl_activation_start(executable, guest_isa, config_path, &process);
    if (status == HL_STATUS_OK) status = hl_activation_process_id(process, &process_id);
    if (status == HL_STATUS_OK) status = hl_activation_try_wait(process, &ready, &result);
    if (status == HL_STATUS_OK && !ready) status = hl_activation_wait(process, &result);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, &repeated);
    if (status == HL_STATUS_OK) status = hl_activation_try_wait(process, &ready, &repeated);
    if (status == HL_STATUS_OK && hl_activation_kill(process) != HL_STATUS_BUSY) status = HL_STATUS_CORRUPT;
    hl_activation_process_destroy(process);
    if (status != HL_STATUS_OK || process_id == 0 || ready != 1 || memcmp(&result, &repeated, sizeof(result)) != 0 ||
        result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != expected) {
        fprintf(stderr, "dual backend: isa=%u status=%d kind=%u guest=%d detail=%llu\n", guest_isa,
                status, result.kind, result.guest_status, (unsigned long long)result.detail);
        return 1;
    }
    return 0;
}

static int kill_one(const char *executable, const char *guest) {
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    hl_engine_exit repeated = {.abi = HL_ENGINE_ABI, .size = sizeof(repeated)};
    hl_activation_process *process = NULL;
    char config_path[64];
    hl_status status;
    if (make_config(guest, config_path) != 0) return 1;
    status = hl_activation_start(executable, HL_GUEST_ISA_AARCH64, config_path, &process);
    if (status == HL_STATUS_OK) status = hl_activation_kill(process);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, &result);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, &repeated);
    hl_activation_process_destroy(process);
    return status != HL_STATUS_OK || memcmp(&result, &repeated, sizeof(result)) != 0 ||
           result.kind != HL_ENGINE_EXIT_SIGNAL || result.guest_status != SIGKILL;
}

static int pipe_one(const char *executable, const char *guest) {
    static const char expected[] = "activation-stdio\n";
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    hl_activation_process *process = NULL;
    hl_activation_stdio stdio = {.input = -1, .output = -1, .error = -1};
    char config_path[64];
    char output[sizeof(expected)] = {0};
    int descriptors[2];
    hl_status status;
    ssize_t count;
    if (make_config(guest, config_path) != 0 || pipe(descriptors) != 0) return 1;
    stdio.output = descriptors[1];
    status = hl_activation_start_with_stdio(executable, HL_GUEST_ISA_AARCH64, config_path, &stdio, &process);
    close(descriptors[1]);
    count = read(descriptors[0], output, sizeof(output));
    close(descriptors[0]);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, &result);
    hl_activation_process_destroy(process);
    return status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 42 ||
           count != (ssize_t)(sizeof(expected) - 1) || memcmp(output, expected, sizeof(expected) - 1) != 0;
}

static int reject_without_launch(const char *executable, const char *guest, uint32_t mode) {
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    char config_path[64];
    hl_status status;
    if (make_config(guest, config_path) != 0) return 1;
    hl_activation_test_mode(mode);
    status = hl_activation_spawn(executable, HL_GUEST_ISA_AARCH64, config_path, &result);
    if (status != HL_STATUS_CORRUPT || access(config_path, F_OK) != 0) return 1;
    return unlink(config_path) != 0;
}

static int closed_stdio_one(const char *executable, const char *guest) {
    char config_path[64];
    pid_t child;
    int waited = 0;
    if (make_config(guest, config_path) != 0) return 1;
    child = fork();
    if (child < 0) { unlink(config_path); return 1; }
    if (child == 0) {
        hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
        hl_status status;
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        status = hl_activation_spawn(executable, HL_GUEST_ISA_AARCH64, config_path, &result);
        _exit(status == HL_STATUS_OK && result.kind == HL_ENGINE_EXIT_CODE && result.guest_status == 42 ? 0 : 1);
    }
    while (waitpid(child, &waited, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    return !WIFEXITED(waited) || WEXITSTATUS(waited) != 0;
}

static int corrupt_wait_one(const char *executable, const char *guest) {
    hl_engine_exit first = {.abi = HL_ENGINE_ABI, .size = sizeof(first)};
    hl_engine_exit repeated = {.abi = HL_ENGINE_ABI, .size = sizeof(repeated)};
    hl_activation_process *process = NULL;
    char config_path[64];
    hl_status first_status, repeated_status;
    if (make_config(guest, config_path) != 0) return 1;
    hl_activation_test_mode(5);
    first_status = hl_activation_start(executable, HL_GUEST_ISA_AARCH64, config_path, &process);
    if (first_status == HL_STATUS_OK) first_status = hl_activation_wait(process, &first);
    repeated_status = process == NULL ? first_status : hl_activation_wait(process, &repeated);
    hl_activation_process_destroy(process);
    return first_status != HL_STATUS_CORRUPT || repeated_status != first_status ||
           first.kind != HL_ENGINE_EXIT_ENGINE_ERROR || first.detail != HL_STATUS_CORRUPT ||
           memcmp(&first, &repeated, sizeof(first)) != 0;
}

int main(int argc, char **argv) {
    struct sigaction pipe_default = {.sa_handler = SIG_DFL};
    if (argc != 7) {
        fprintf(stderr, "usage: dual-backend-e2e A64_EXIT42 X86_EXIT42 A64_EXIT70 X86_EXIT70 A64_SPIN A64_OUTPUT\n");
        return 64;
    }
    if (!handlers_unchanged()) return 65;
    if (sigemptyset(&pipe_default.sa_mask) != 0 || sigaction(SIGPIPE, &pipe_default, NULL) != 0) return 65;
    if (reject_without_launch(argv[0], argv[1], 1) || reject_without_launch(argv[0], argv[1], 2) ||
        reject_without_launch(argv[0], argv[1], 3) || reject_without_launch(argv[0], argv[1], 4) ||
        hl_activation_spawn(argv[0], HL_GUEST_ISA_AARCH64, argv[1], NULL) != HL_STATUS_INVALID_ARGUMENT)
        return 66;
    if (run_one(argv[0], HL_GUEST_ISA_AARCH64, argv[1], 42) ||
        run_one(argv[0], HL_GUEST_ISA_X86_64, argv[2], 42) ||
        run_one(argv[0], HL_GUEST_ISA_AARCH64, argv[3], 70) ||
        run_one(argv[0], HL_GUEST_ISA_X86_64, argv[4], 70) || kill_one(argv[0], argv[5]) ||
        pipe_one(argv[0], argv[6]) || closed_stdio_one(argv[0], argv[1]) || corrupt_wait_one(argv[0], argv[1]))
        return 71;
    return 0;
}
