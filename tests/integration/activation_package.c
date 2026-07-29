#include "hl/activation.h"
#include "hl/config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/*
 * Every failure path names itself.  This consumer is the payload of the
 * `package.consumer-link` CTest case, whose only channel to CI is a log tail:
 * a bare `return 29` there arrived as "package.consumer-link (Failed)" and
 * ctest's generic exit 8, naming neither the leg nor the reason.  The exit code
 * is still the contract (the driver reports it); this only adds the sentence.
 */
static int fail(int code, const char *leg, const char *what) {
    fprintf(stderr, "activation-package: %s: %s (exit %d)\n", leg, what, code);
    return code;
}

/* Reports WHICH argument-validation contract broke, not just that one did. */
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) return fail(1, "api-contract", #expr);                                                             \
    } while (0)

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
    launch.process_domain[0] = 1;
    launch.header_size = sizeof(launch);
    launch.pool_size = (uint32_t)(guest_size + 2);
    launch.uid = -1;
    launch.gid = -1;
    launch.arguments_offset = 1;
    launch.executable_host_offset = 1;
    if (write(descriptor, &launch, sizeof(launch)) != sizeof(launch) ||
        write(descriptor, pool, launch.pool_size) != launch.pool_size || close(descriptor) != 0) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    return 0;
}

static int domain_config(const char *guest, hl_process_domain domain, char path[64]) {
    hl_launch_config launch = {0};
    char pool[4096] = {0};
    const char *base = strrchr(guest, '/');
    size_t root_size;
    size_t argument_size;
    size_t root_offset;
    int descriptor;
    if (base == NULL || base == guest) return -1;
    root_size = (size_t)(base - guest);
    argument_size = strlen(base) + 1;
    root_offset = argument_size + 2;
    if (root_offset + root_size + 1 > sizeof(pool)) return -1;
    strcpy(path, "/tmp/hl-domain.XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    memcpy(pool + 1, base, argument_size);
    memcpy(pool + root_offset, guest, root_size);
    launch.magic = HL_CONFIG_MAGIC;
    launch.abi = HL_CONFIG_ABI;
    launch.header_size = sizeof(launch);
    launch.pool_size = (uint32_t)(root_offset + root_size + 1);
    launch.uid = -1;
    launch.gid = -1;
    launch.arguments_offset = 1;
    launch.rootfs_offset = (uint32_t)root_offset;
    launch.process_domain[0] = domain.identity[0];
    launch.process_domain[1] = domain.identity[1];
    if (write(descriptor, &launch, sizeof(launch)) != sizeof(launch) ||
        write(descriptor, pool, launch.pool_size) != launch.pool_size || close(descriptor) != 0) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    return 0;
}

static int process_domain(const char *self, const char *guest, uint32_t isa) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    hl_process_domain domain = {{(uint64_t)getpid(), ((uint64_t)time(NULL) << 32) ^ isa}};
    hl_activation_stdio stdio;
    char config_path[64], text[64] = {0}, *end;
    char registry[96], network[96];
    int output[2], status;
    long daemon;
    pid_t sibling;
    uint64_t deadline;
    ssize_t size;
    if (domain_config(guest, domain, config_path) != 0 || pipe(output) != 0)
        return fail(27, "process-domain", "could not write the domain launch config or create the output pipe");
    stdio = (hl_activation_stdio){.input = -1, .output = output[1], .error = output[1]};
    status = hl_activation_start_with_stdio(self, isa, config_path, &stdio, &process);
    close(output[1]);
    if (status != HL_STATUS_OK) {
        close(output[0]);
        return fail(28, "process-domain", "hl_activation_start_with_stdio did not return HL_STATUS_OK");
    }
    do { size = read(output[0], text, sizeof text - 1); } while (size < 0 && errno == EINTR);
    close(output[0]);
    status = hl_activation_wait(process, &result);
    if (size <= 0 || status != HL_STATUS_OK ||
        result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 0) {
        fprintf(stderr, "domain guest isa=%u read=%ld status=%d kind=%u guest=%d text=%s\n", isa,
                (long)size, status, result.kind, result.guest_status, text);
        hl_activation_process_destroy(process);
        return fail(29, "process-domain", "the domain guest did not print a daemon pid and exit 0");
    }
    hl_activation_process_destroy(process);
    errno = 0;
    daemon = strtol(text, &end, 10);
    if (errno != 0 || end == text || daemon <= 0 || kill((pid_t)daemon, 0) != 0)
        return fail(30, "process-domain", "the daemon pid the guest printed is unparseable or already gone");
    snprintf(network, sizeof network, "/tmp/.hl-net-%016llx%016llx",
             (unsigned long long)domain.identity[0], (unsigned long long)domain.identity[1]);
    if (access(network, F_OK) != 0)
        return fail(30, "process-domain", "the domain network registry socket was not created");
    sibling = fork();
    if (sibling < 0) return fail(31, "process-domain", "fork of the control sibling failed");
    if (sibling == 0) { for (;;) pause(); }
    status = hl_activation_domain_terminate(domain);
    deadline = (uint64_t)time(NULL) + 4;
    while (kill((pid_t)daemon, 0) == 0 && (uint64_t)time(NULL) < deadline) {
        struct timespec delay = {.tv_nsec = 10000000};
        (void)nanosleep(&delay, NULL);
    }
    if (status != HL_STATUS_OK || kill((pid_t)daemon, 0) == 0 || kill(sibling, 0) != 0) {
        fprintf(stderr, "activation-package: process-domain terminate status=%d daemon_alive=%d sibling_alive=%d\n",
                status, kill((pid_t)daemon, 0) == 0, kill(sibling, 0) == 0);
        kill(sibling, SIGKILL);
        waitpid(sibling, NULL, 0);
        return fail(32, "process-domain",
                    "hl_activation_domain_terminate did not reap the domain daemon within 4s, or it killed a "
                    "process outside the domain");
    }
    kill(sibling, SIGKILL);
    waitpid(sibling, NULL, 0);
    snprintf(registry, sizeof registry, "/tmp/.hl-domain.%016llx%016llx",
             (unsigned long long)domain.identity[0], (unsigned long long)domain.identity[1]);
    if (hl_activation_domain_terminate(domain) != HL_STATUS_OK)
        return fail(33, "process-domain", "terminating an already-terminated domain did not return HL_STATUS_OK");
    errno = 0;
    if (access(registry, F_OK) == 0 || errno != ENOENT)
        return fail(33, "process-domain", "the domain registry file outlived the domain");
    errno = 0;
    if (access(network, F_OK) != 0 && errno == ENOENT) return 0;
    return fail(33, "process-domain", "the domain network registry socket outlived the domain");
}

static int force_stop_descendants(const char *self, const char *guest) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    uint32_t ready = 0;
    struct timespec delay = {.tv_nsec = 10000000};
    int output[2];
    hl_activation_stdio stdio;
    hl_status status;
    char config_path[64];
    struct pollfd drained;
    char byte;
    int attempt;

    if (config(guest, config_path) != 0 || pipe(output) != 0)
        return fail(2, "force-stop-descendants", "could not write the launch config or create the output pipe");
    stdio = (hl_activation_stdio){.input = -1, .output = output[1], .error = output[1]};
    status = hl_activation_start_with_stdio(self, HL_GUEST_ISA_AARCH64, config_path, &stdio, &process);
    close(output[1]);
    if (status != HL_STATUS_OK) {
        close(output[0]);
        return fail(3, "force-stop-descendants", "hl_activation_start_with_stdio did not return HL_STATUS_OK");
    }
    drained = (struct pollfd){.fd = output[0], .events = POLLIN | POLLHUP};
    if (poll(&drained, 1, 5000) <= 0 || (drained.revents & POLLIN) == 0 || read(output[0], &byte, 1) != 1 ||
        byte != 'R') {
        hl_activation_process_destroy(process);
        close(output[0]);
        return fail(4, "force-stop-descendants", "the setsid descendant did not report readiness");
    }
    if (hl_activation_kill(process) != HL_STATUS_OK) {
        close(output[0]);
        return fail(5, "force-stop-descendants", "hl_activation_kill did not return HL_STATUS_OK");
    }
    status = hl_activation_kill(process);
    if (status != HL_STATUS_OK) {
        close(output[0]);
        fprintf(stderr, "activation-package: force-stop-descendants: repeated kill status=%d\n", status);
        return fail(6, "force-stop-descendants", "repeated hl_activation_kill was not idempotent");
    }
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
    if (status == HL_STATUS_OK && ready && attempt == 0 && result.kind == HL_ENGINE_EXIT_SIGNAL &&
        result.guest_status == 9)
        return 0;
    fprintf(stderr, "activation-package: force-stop-descendants: status=%d ready=%u drained=%d kind=%u guest=%d\n",
            status, ready, attempt, result.kind, result.guest_status);
    return fail(7, "force-stop-descendants",
                "the killed guest and its descendants did not all report SIGKILL and close their output");
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
    if (config(guest, config_path) != 0) return fail(6, "forward-signal", "could not write the launch config");
    if (pipe(descriptors) != 0) return fail(6, "forward-signal", "could not create the output pipe");
    stdio = (hl_activation_stdio){.input = -1, .output = descriptors[1], .error = descriptors[1]};
    status = hl_activation_start_with_stdio(self, HL_GUEST_ISA_AARCH64, config_path, &stdio, &process);
    close(descriptors[1]);
    if (status != HL_STATUS_OK || hl_activation_process_id(process, &process_id) != HL_STATUS_OK) {
        close(descriptors[0]);
        hl_activation_process_destroy(process);
        return fail(7, "forward-signal", "hl_activation_start_with_stdio or hl_activation_process_id failed");
    }
    while (used < 5) {
        ssize_t count = read(descriptors[0], output + used, 5 - used);
        if (count <= 0) {
            close(descriptors[0]);
            hl_activation_process_destroy(process);
            return fail(8, "forward-signal", "the guest closed its output before announcing READY");
        }
        used += (size_t)count;
    }
    if (memcmp(output, "READY", 5) != 0) {
        close(descriptors[0]);
        hl_activation_process_destroy(process);
        return fail(9, "forward-signal", "the guest's first five output bytes were not READY");
    }
    if (kill((pid_t)process_id, signal_number) != 0) {
        close(descriptors[0]);
        hl_activation_process_destroy(process);
        return fail(11, "forward-signal", "kill(2) on the engine-reported guest pid failed");
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
    if (status == HL_STATUS_OK && result.kind == HL_ENGINE_EXIT_CODE && result.guest_status == 0 &&
        used == sizeof(output) && memcmp(output, expected, sizeof(output)) == 0)
        return 0;
    fprintf(stderr, "activation-package: forward-signal signal=%d status=%d kind=%u guest=%d read=%zu of %zu\n",
            signal_number, status, result.kind, result.guest_status, used, sizeof(output));
    return fail(10, "forward-signal", "the guest did not observe the forwarded signal and exit cleanly");
}

int main(int argc, char **argv) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    uint32_t ready = 0;
    uint64_t process_id = 0;
    int32_t master = 99;
    hl_terminal_size empty = {0};

    CHECK(hl_activation_start(NULL, HL_GUEST_ISA_AARCH64, NULL, &process) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(process == NULL);
    CHECK(hl_activation_process_id(NULL, &process_id) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(hl_activation_wait(NULL, &result) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(hl_activation_try_wait(NULL, &ready, &result) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(hl_activation_kill(NULL) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(sizeof(hl_terminal_size) == 4);
    CHECK(hl_activation_domain_terminate((hl_process_domain){{0, 0}}) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(hl_activation_domain_terminate((hl_process_domain){{UINT64_MAX, UINT64_MAX}}) == HL_STATUS_OK);
    CHECK(hl_activation_start_terminal(argv[0], HL_GUEST_ISA_AARCH64, argv[0], empty, &master, &process) ==
          HL_STATUS_INVALID_ARGUMENT);
    CHECK(master == -1);
    CHECK(process == NULL);
    CHECK(hl_terminal_resize(-1, (hl_terminal_size){.rows = 24, .columns = 80}) == HL_STATUS_INVALID_ARGUMENT);
    CHECK(hl_activation_spawn(NULL, HL_GUEST_ISA_AARCH64, NULL, NULL) == HL_STATUS_INVALID_ARGUMENT);
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
    if (argc >= 5) {
        int domain = process_domain(argv[0], argv[3], HL_GUEST_ISA_AARCH64);
        if (domain != 0) return domain;
        domain = process_domain(argv[0], argv[4], HL_GUEST_ISA_X86_64);
        if (domain != 0) return domain;
    }
    puts("installed hl-engine activation package: ok");
    return 0;
}
