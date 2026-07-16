#include "hl/activation.h"
#include "engine_backend.h"
#include "launch.h"
#if defined(__APPLE__)
#include "hl/macos.h"
typedef hl_host_macos hl_activation_host;
#elif defined(__linux__)
#include "hl/linux.h"
typedef hl_host_linux hl_activation_host;
#else
#error unsupported activation host
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static hl_status activation_host_create(hl_activation_host **host, hl_host_services *services) {
#if defined(__APPLE__)
    return hl_host_macos_create(host, services);
#else
    return hl_host_linux_create(host, services);
#endif
}

static void activation_host_destroy(hl_activation_host *host) {
#if defined(__APPLE__)
    hl_host_macos_destroy(host);
#else
    hl_host_linux_destroy(host);
#endif
}

extern char **environ;
void hl_activation_test_mode(uint32_t mode);

enum { HL_ACTIVATION_FD = 198, HL_ACTIVATION_ABI = 1, HL_ACTIVATION_PATH_MAX = 4096 };
#define HL_ACTIVATION_MAGIC UINT64_C(0x484c414354495631)

typedef struct hl_activation_request {
    uint64_t magic;
    uint32_t abi;
    uint32_t size;
    uint64_t nonce[2];
    uint32_t guest_isa;
    uint32_t path_size;
    uint32_t test_flags;
    uint32_t reserved;
    char path[HL_ACTIVATION_PATH_MAX];
} hl_activation_request;

typedef struct hl_activation_reply {
    uint64_t magic;
    uint32_t abi;
    uint32_t size;
    uint64_t nonce[2];
    int32_t status;
    uint32_t reserved;
    hl_engine_exit result;
} hl_activation_reply;

static int transfer(int fd, void *data, size_t size, int writing) {
    unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        if (writing) flags = MSG_NOSIGNAL;
#endif
        ssize_t count = writing ? send(fd, bytes + offset, size - offset, flags)
                                : read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

void hl_aarch64_target_register_backend(void);
void hl_x86_64_target_register_backend(void);
void hl_aarch64_target_runtime_init(void);
void hl_x86_64_target_runtime_init(void);
void hl_host_private_init(void);
void hl_fdcache_runtime_init(void);

static void hl_embedded_runtime_init(uint32_t guest_isa) {
    hl_host_private_init();
    hl_fdcache_runtime_init();
    hl_aarch64_target_register_backend();
    hl_x86_64_target_register_backend();
    if (guest_isa == HL_GUEST_ISA_AARCH64)
        hl_aarch64_target_runtime_init();
    else
        hl_x86_64_target_runtime_init();
}

static hl_host_services *activation_services;
static uint32_t activation_guest_isa;
static hl_engine_exit *activation_result;
static hl_status activation_status;

static int activation_run_config(const char *rootfs, uint32_t argc, char *const argv[],
                                 const hl_options *options, const char *result_path) {
    hl_engine_fd_binding bindings[3] = {0};
    hl_engine_config config = {.abi = HL_ENGINE_ABI, .size = sizeof(config),
                               .guest_isa = activation_guest_isa, .rootfs = rootfs};
    hl_engine *engine = NULL;
    uint32_t count = 0;
    uint32_t stream;
    (void)result_path;
    for (stream = 0; stream < 3; ++stream) {
        hl_host_result adopted = activation_services->file->standard_stream(activation_services->context, stream);
        uint32_t access;
        if (adopted.status == HL_STATUS_NOT_FOUND) continue;
        if (adopted.status != HL_STATUS_OK) { activation_status = (hl_status)adopted.status; return 78; }
        access = (uint32_t)adopted.detail & (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
        bindings[count] = (hl_engine_fd_binding){.abi = HL_ENGINE_ABI, .size = sizeof(bindings[count]),
                                                 .guest_fd = stream,
                                                 .status_flags = access == (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE)
                                                                     ? HL_LINUX_O_RDWR
                                                                 : access == HL_HOST_FILE_WRITE ? HL_LINUX_O_WRONLY
                                                                                                  : HL_LINUX_O_RDONLY,
                                                 .ownership = HL_ENGINE_FD_TRANSFER,
                                                 .host_handle = adopted.value};
        if (((uint32_t)adopted.detail & HL_HOST_FILE_APPEND) != 0)
            bindings[count].status_flags |= HL_LINUX_O_APPEND;
        if (((uint32_t)adopted.detail & HL_HOST_FILE_NONBLOCK) != 0)
            bindings[count].status_flags |= HL_LINUX_O_NONBLOCK;
        ++count;
    }
    config.fd_bindings = bindings;
    config.fd_binding_count = count;
    activation_status = hl_engine_create_with_options(&config, activation_services, options, &engine);
    if (activation_status == HL_STATUS_OK)
        activation_status = hl_engine_run(engine, (int)argc, (const char *const *)argv, activation_result);
    hl_engine_destroy(engine);
    return activation_status == HL_STATUS_OK ? 0 : 78;
}

static void hl_activation_child(void) {
    const char *value = getenv("HL_ACTIVATION_FD");
    hl_activation_request request;
    hl_activation_reply reply = {0};
    hl_activation_host *host = NULL;
    hl_host_services services;
    char *end = NULL;
    long descriptor;
    hl_status status = HL_STATUS_CORRUPT;
    unsigned char commit;
    if (value == NULL) return;
    descriptor = strtol(value, &end, 10);
    (void)unsetenv("HL_ACTIVATION_FD");
    if (end == value || *end != 0 || descriptor != HL_ACTIVATION_FD) _exit(125);
    if (transfer((int)descriptor, &request, sizeof(request), 0) != 0) _exit(126);
    reply.magic = HL_ACTIVATION_MAGIC;
    reply.abi = HL_ACTIVATION_ABI;
    reply.size = sizeof(reply);
    reply.nonce[0] = request.nonce[0];
    reply.nonce[1] = request.nonce[1];
    reply.result.abi = HL_ENGINE_ABI;
    reply.result.size = sizeof(reply.result);
    if (request.test_flags == 1) reply.nonce[0] ^= UINT64_C(1);
    if (request.magic == HL_ACTIVATION_MAGIC && request.abi == HL_ACTIVATION_ABI &&
        request.size == sizeof(request) && request.path_size > 1 && request.path_size <= sizeof(request.path) &&
        request.path[0] == '/' && request.path[request.path_size - 1] == 0 &&
        (request.guest_isa == HL_GUEST_ISA_AARCH64 || request.guest_isa == HL_GUEST_ISA_X86_64)) {
        reply.status = HL_STATUS_OK;
        if (transfer((int)descriptor, &reply, sizeof(reply), 1) != 0) _exit(124);
        if (request.test_flags == 4) _exit(123);
        if (transfer((int)descriptor, &commit, 1, 0) != 0 || commit != 0xa5u) _exit(124);
        /* Explicit setup is idempotent and independent of constructor order. */
        hl_embedded_runtime_init(request.guest_isa);
        hl_embedded_runtime_init(request.guest_isa);
        status = activation_host_create(&host, &services);
        activation_services = &services;
        activation_guest_isa = request.guest_isa;
        activation_result = &reply.result;
        activation_status = status;
        if (status == HL_STATUS_OK && hl_run_config_file_with(request.path, activation_run_config) != 0 &&
            activation_status == HL_STATUS_OK) activation_status = HL_STATUS_CORRUPT;
        status = activation_status;
    } else {
        (void)transfer((int)descriptor, &reply, sizeof(reply), 1);
        _exit(127);
    }
    reply.status = (int32_t)status;
    if (request.test_flags == 5) reply.nonce[0] ^= UINT64_C(1);
    (void)transfer((int)descriptor, &reply, sizeof(reply), 1);
    activation_host_destroy(host);
    (void)close((int)descriptor);
    _exit(status == HL_STATUS_OK ? 0 : 127);
}

static uint32_t activation_test_mode;

void hl_activation_test_mode(uint32_t mode) {
    activation_test_mode = mode;
}

__attribute__((constructor)) static void hl_activation_constructor(void) {
    hl_activation_child();
}

struct hl_activation_process {
    int descriptor;
    pid_t pid;
    uint64_t nonce[2];
    uint32_t finished;
    hl_status final_status;
    hl_engine_exit final_exit;
};

static int wait_child(pid_t child, int *waited) {
    pid_t result;
    do { result = waitpid(child, waited, 0); } while (result < 0 && errno == EINTR);
    return result == child ? 0 : -1;
}

static int reserve_control_descriptors(int pair[2]) {
    size_t index;
    for (index = 0; index < 2; ++index) {
        int replacement;
        if (pair[index] > STDERR_FILENO) continue;
        replacement = fcntl(pair[index], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) return -1;
        (void)close(pair[index]);
        pair[index] = replacement;
    }
    return 0;
}

static void cache_failure(hl_activation_process *process, hl_status status) {
    process->finished = 1;
    process->final_status = status;
    process->final_exit = (hl_engine_exit){.abi = HL_ENGINE_ABI, .size = sizeof(process->final_exit),
                                           .kind = HL_ENGINE_EXIT_ENGINE_ERROR, .detail = (uint64_t)status};
}

hl_status hl_activation_start_with_stdio(const char *executable, uint32_t guest_isa, const char *guest,
                                         const hl_activation_stdio *stdio, hl_activation_process **out_process) {
    int pair[2];
    pid_t child;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    hl_activation_request request = {0};
    hl_activation_reply reply;
    char activation[] = "HL_ACTIVATION_FD=198";
    char *child_argv[2];
    char **child_env;
    size_t env_count = 0;
    size_t env_output = 0;
    size_t path_size;
    int waited = 0;
    uint32_t test_mode;
    hl_activation_process *process;
    if (out_process == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_process = NULL;
    if (executable == NULL || guest == NULL || executable[0] != '/' || guest[0] != '/' ||
        (guest_isa != HL_GUEST_ISA_AARCH64 && guest_isa != HL_GUEST_ISA_X86_64))
        return HL_STATUS_INVALID_ARGUMENT;
    if (stdio != NULL && (stdio->input < -1 || stdio->output < -1 || stdio->error < -1))
        return HL_STATUS_INVALID_ARGUMENT;
    path_size = strlen(guest) + 1;
    if (path_size > sizeof(request.path)) return HL_STATUS_INVALID_ARGUMENT;
    while (environ[env_count] != NULL) ++env_count;
    child_env = calloc(env_count + 2, sizeof(*child_env));
    if (child_env == NULL) return HL_STATUS_OUT_OF_MEMORY;
    for (env_count = 0; environ[env_count] != NULL; ++env_count)
        if (strncmp(environ[env_count], "HL_ACTIVATION_FD=", 17) != 0) child_env[env_output++] = environ[env_count];
    child_env[env_output] = activation;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { free(child_env); return HL_STATUS_PLATFORM_FAILURE; }
    if (reserve_control_descriptors(pair) != 0) {
        close(pair[0]); close(pair[1]); free(child_env); return HL_STATUS_PLATFORM_FAILURE;
    }
#if defined(__APPLE__)
    { int enabled = 1; if (setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0 ||
                           setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        close(pair[0]); close(pair[1]); free(child_env); return HL_STATUS_PLATFORM_FAILURE;
    } }
#endif
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(pair[0]); close(pair[1]); free(child_env); return HL_STATUS_PLATFORM_FAILURE;
    }
    request.magic = HL_ACTIVATION_MAGIC;
    request.abi = HL_ACTIVATION_ABI;
    request.size = sizeof(request);
    request.guest_isa = guest_isa;
    request.path_size = (uint32_t)path_size;
    test_mode = activation_test_mode;
    request.test_flags = test_mode == 1 ? 1u : test_mode == 4 ? 4u : test_mode == 5 ? 5u : 0u;
    if (test_mode == 2) request.magic ^= UINT64_C(1);
    activation_test_mode = 0;
    memcpy(request.path, guest, path_size);
    arc4random_buf(request.nonce, sizeof(request.nonce));
    child_argv[0] = (char *)(uintptr_t)executable;
    child_argv[1] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pair[0]); close(pair[1]); free(child_env); return HL_STATUS_PLATFORM_FAILURE;
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions); close(pair[0]); close(pair[1]); free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0 ||
        posix_spawnattr_setpgroup(&attributes, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions); close(pair[0]); close(pair[1]); free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (stdio != NULL &&
        ((stdio->input >= 0 && posix_spawn_file_actions_adddup2(&actions, stdio->input, 0) != 0) ||
         (stdio->output >= 0 && posix_spawn_file_actions_adddup2(&actions, stdio->output, 1) != 0) ||
         (stdio->error >= 0 && posix_spawn_file_actions_adddup2(&actions, stdio->error, 2) != 0))) {
        posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
        close(pair[0]); close(pair[1]); free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (posix_spawn_file_actions_adddup2(&actions, pair[1], HL_ACTIVATION_FD) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pair[0]) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pair[1]) != 0) {
        posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
        close(pair[0]); close(pair[1]); free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (posix_spawn(&child, executable, &actions, &attributes, child_argv, child_env) != 0) {
        posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
        close(pair[0]); close(pair[1]); free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(pair[1]);
    free(child_env);
    if (transfer(pair[0], &request, test_mode == 3 ? sizeof(request) / 2u : sizeof(request), 1) != 0) {
        close(pair[0]); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    }
    if (test_mode == 3) (void)shutdown(pair[0], SHUT_WR);
    if (transfer(pair[0], &reply, sizeof(reply), 0) != 0) {
        close(pair[0]); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    }
    if (reply.magic != request.magic || reply.abi != request.abi || reply.size != sizeof(reply) ||
        memcmp(reply.nonce, request.nonce, sizeof(request.nonce)) != 0 || reply.status != HL_STATUS_OK) {
        close(pair[0]); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    }
    { unsigned char commit = 0xa5u; if (transfer(pair[0], &commit, 1, 1) != 0) {
        close(pair[0]); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    } }
    process = calloc(1, sizeof(*process));
    if (process == NULL) { close(pair[0]); (void)kill(-child, SIGKILL); wait_child(child, &waited); return HL_STATUS_OUT_OF_MEMORY; }
    process->descriptor = pair[0];
    process->pid = child;
    memcpy(process->nonce, request.nonce, sizeof(process->nonce));
    *out_process = process;
    return HL_STATUS_OK;
}

hl_status hl_activation_start(const char *executable, uint32_t guest_isa, const char *config_path,
                              hl_activation_process **out_process) {
    return hl_activation_start_with_stdio(executable, guest_isa, config_path, NULL, out_process);
}

hl_status hl_activation_process_id(const hl_activation_process *process, uint64_t *out_process_id) {
    if (process == NULL || out_process_id == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_process_id = (uint64_t)process->pid;
    return HL_STATUS_OK;
}

hl_status hl_activation_wait(hl_activation_process *process, hl_engine_exit *out_exit) {
    hl_activation_reply reply;
    int waited = 0;
    if (process == NULL || out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (process->finished) { *out_exit = process->final_exit; return process->final_status; }
    if (transfer(process->descriptor, &reply, sizeof(reply), 0) != 0) {
        (void)close(process->descriptor);
        if (wait_child(process->pid, &waited) != 0) {
            cache_failure(process, HL_STATUS_PLATFORM_FAILURE);
            *out_exit = process->final_exit;
            return process->final_status;
        }
        if (WIFSIGNALED(waited)) {
            process->finished = 1;
            process->final_status = HL_STATUS_OK;
            process->final_exit = (hl_engine_exit){.abi = HL_ENGINE_ABI, .size = sizeof(process->final_exit),
                                                   .kind = HL_ENGINE_EXIT_SIGNAL,
                                                   .guest_status = WTERMSIG(waited)};
            *out_exit = process->final_exit;
            return HL_STATUS_OK;
        }
        cache_failure(process, HL_STATUS_CORRUPT);
        *out_exit = process->final_exit;
        return process->final_status;
    }
    (void)close(process->descriptor);
    if (wait_child(process->pid, &waited) != 0) {
        cache_failure(process, HL_STATUS_PLATFORM_FAILURE);
        *out_exit = process->final_exit;
        return process->final_status;
    }
    if (reply.magic != HL_ACTIVATION_MAGIC || reply.abi != HL_ACTIVATION_ABI || reply.size != sizeof(reply) ||
        memcmp(reply.nonce, process->nonce, sizeof(process->nonce)) != 0 || reply.status < HL_STATUS_OK ||
        reply.status > HL_STATUS_ADDRESS_IN_USE || !WIFEXITED(waited) ||
        (reply.status == HL_STATUS_OK ? WEXITSTATUS(waited) != 0 : WEXITSTATUS(waited) != 127)) {
        cache_failure(process, HL_STATUS_CORRUPT);
        *out_exit = process->final_exit;
        return process->final_status;
    }
    process->finished = 1;
    process->final_status = (hl_status)reply.status;
    process->final_exit = reply.result;
    *out_exit = process->final_exit;
    return process->final_status;
}

hl_status hl_activation_try_wait(hl_activation_process *process, uint32_t *out_ready,
                                 hl_engine_exit *out_exit) {
    struct pollfd descriptor;
    int ready;
    if (process == NULL || out_ready == NULL || out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (process->finished) { *out_ready = 1; *out_exit = process->final_exit; return process->final_status; }
    descriptor = (struct pollfd){.fd = process->descriptor, .events = POLLIN | POLLHUP};
    do { ready = poll(&descriptor, 1, 0); } while (ready < 0 && errno == EINTR);
    if (ready < 0) return HL_STATUS_PLATFORM_FAILURE;
    if (ready == 0) { *out_ready = 0; return HL_STATUS_OK; }
    *out_ready = 1;
    return hl_activation_wait(process, out_exit);
}

hl_status hl_activation_kill(hl_activation_process *process) {
    if (process == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (process->finished) return HL_STATUS_BUSY;
    return kill(-process->pid, SIGKILL) == 0 ? HL_STATUS_OK : HL_STATUS_PLATFORM_FAILURE;
}

void hl_activation_process_destroy(hl_activation_process *process) {
    hl_engine_exit ignored;
    if (process == NULL) return;
    if (!process->finished) { (void)hl_activation_kill(process); (void)hl_activation_wait(process, &ignored); }
    free(process);
}

hl_status hl_activation_spawn(const char *executable, uint32_t guest_isa, const char *config_path,
                              hl_engine_exit *out_exit) {
    hl_activation_process *process = NULL;
    hl_status status;
    if (out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    status = hl_activation_start(executable, guest_isa, config_path, &process);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, out_exit);
    hl_activation_process_destroy(process);
    return status;
}
