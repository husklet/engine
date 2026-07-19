#include "hl/activation.h"
#include "../host/fork_wire.h"
#include "../host/system.h"
#include "provider_client.h"
#include "provider_files.h"
#include "provider_namespace.h"
#include "engine_backend.h"
#include "environment.h"
#include "launch.h"
#include "../host/system.h"
#include "hl/config.h"
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
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
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

static int activation_provider_handshake(int descriptor) {
    unsigned char hello[32] = {'R', 'P', 'L', 'H', 1, 0, 1, 0};
    unsigned char ready[32], setup[32], acknowledged[32] = {'R', 'P', 'L', 'H', 1, 0, 8, 0};
    unsigned char *payload = NULL;
    struct pollfd pollfd = {.fd = descriptor, .events = POLLIN | POLLHUP};
    int result;
    hello[20] = 1; /* NamespaceInstall v1. */
    if (transfer(descriptor, hello, sizeof(hello), 1) != 0) return -1;
    do { result = poll(&pollfd, 1, 5000); } while (result < 0 && errno == EINTR);
    if (result <= 0 || (pollfd.revents & POLLIN) == 0 || transfer(descriptor, ready, sizeof(ready), 0) != 0)
        return -1;
    /* HLPR, version 1, READY, empty payload, request id zero, reserved zero. */
    if (ready[0] != 'R' || ready[1] != 'P' || ready[2] != 'L' || ready[3] != 'H' ||
        ready[4] != 1 || ready[5] != 0 || ready[6] != 2 || ready[7] != 0)
        return -1;
    for (size_t index = 8; index < 20; ++index)
        if (ready[index] != 0) return -1;
    for (size_t index = 28; index < sizeof(ready); ++index)
        if (ready[index] != 0) return -1;
    if ((ready[20] & 1u) != 0) {
        uint32_t size;
        if (transfer(descriptor, setup, sizeof(setup), 0) != 0 || setup[0] != 'R' || setup[1] != 'P' ||
            setup[2] != 'L' || setup[3] != 'H' || setup[4] != 1 || setup[5] != 0 || setup[6] != 7 ||
            setup[7] != 0)
            return -1;
        size = (uint32_t)setup[8] | (uint32_t)setup[9] << 8 | (uint32_t)setup[10] << 16 |
               (uint32_t)setup[11] << 24;
        if (size > 1024 * 1024 || memcmp(setup + 12, "\0\0\0\0\0\0\0\0", 8) != 0) return -1;
        payload = malloc(size == 0 ? 1 : size);
        if (payload == NULL || (size != 0 && transfer(descriptor, payload, size, 0) != 0) ||
            hl_provider_namespace_launch_install(payload, size) != 0) {
            free(payload);
            return -1;
        }
        free(payload);
        if (transfer(descriptor, acknowledged, sizeof(acknowledged), 1) != 0) return -1;
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
static hl_engine *activation_engine;
static hl_provider_client activation_provider_client;
static pthread_mutex_t activation_engine_lock = PTHREAD_MUTEX_INITIALIZER;
static int activation_signal_pipe[2] = {-1, -1};
static uint32_t activation_pending_signal;

static int activation_guest_signal(int host_signal) {
#if defined(__linux__)
    return host_signal;
#else
    static const unsigned char macos_to_linux[32] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 7, 11, 31, 13, 14, 15,
        23, 19, 20, 18, 17, 21, 22, 29, 24, 25, 26, 27, 28, 29, 10, 12};
    return host_signal > 0 && host_signal < 32 ? macos_to_linux[host_signal] : host_signal;
#endif
}

static void activation_signal_handler(int signal_number) {
    unsigned char guest = (unsigned char)activation_guest_signal(signal_number);
    if (activation_signal_pipe[1] >= 0) {
        ssize_t ignored = write(activation_signal_pipe[1], &guest, sizeof(guest));
        (void)ignored;
    }
}

static void *activation_signal_relay(void *unused) {
    unsigned char signal_number;
    (void)unused;
    while (read(activation_signal_pipe[0], &signal_number, sizeof(signal_number)) == sizeof(signal_number)) {
        uint32_t guest_signal = signal_number;
        if (signal_number == 0) break;
        pthread_mutex_lock(&activation_engine_lock);
        if (activation_engine != NULL)
            (void)hl_engine_request(activation_engine, HL_ENGINE_REQUEST_SIGNAL, &guest_signal,
                                    sizeof(guest_signal));
        else
            activation_pending_signal = signal_number;
        pthread_mutex_unlock(&activation_engine_lock);
    }
    return NULL;
}

static int activation_signal_relay_start(pthread_t *thread) {
    static const int forwarded[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2};
    struct sigaction action;
    size_t index;
    int flags;
    if (pipe(activation_signal_pipe) != 0) return -1;
    flags = fcntl(activation_signal_pipe[1], F_GETFL);
    if (flags < 0 || fcntl(activation_signal_pipe[1], F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = activation_signal_handler;
    action.sa_flags = SA_RESTART;
    sigfillset(&action.sa_mask);
    for (index = 0; index < sizeof(forwarded) / sizeof(forwarded[0]); ++index)
        if (sigaction(forwarded[index], &action, NULL) != 0) return -1;
    return pthread_create(thread, NULL, activation_signal_relay, NULL);
}

static void activation_signal_relay_stop(pthread_t thread) {
    unsigned char stop = 0;
    ssize_t ignored = write(activation_signal_pipe[1], &stop, sizeof(stop));
    (void)ignored;
    (void)pthread_join(thread, NULL);
    close(activation_signal_pipe[0]);
    close(activation_signal_pipe[1]);
    activation_signal_pipe[0] = -1;
    activation_signal_pipe[1] = -1;
}

static int activation_run_config(const char *rootfs, const char *executable_host, uint32_t argc, char *const argv[],
                                 const hl_options *options, const char *result_path) {
    hl_engine_fd_binding bindings[3] = {0};
    hl_engine_executable executable = {0};
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
    if (executable_host != NULL) {
        hl_host_result opened = activation_services->file->open_relative(
            activation_services->context, HL_HOST_HANDLE_CWD, executable_host, strlen(executable_host),
            HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
        if (opened.status != HL_STATUS_OK) {
            activation_status = (hl_status)opened.status;
            return 78;
        }
        executable = (hl_engine_executable){HL_ENGINE_ABI, sizeof(executable), HL_ENGINE_FD_TRANSFER, 0,
                                            opened.value, NULL, 0};
        config.executable = &executable;
    }
    activation_status = hl_engine_create_with_options(&config, activation_services, options, &engine);
    if (activation_status != HL_STATUS_OK && config.executable != NULL)
        (void)activation_services->file->close(activation_services->context, executable.host_handle);
    if (activation_status == HL_STATUS_OK) {
        uint32_t pending;
        pthread_mutex_lock(&activation_engine_lock);
        activation_engine = engine;
        pending = activation_pending_signal;
        activation_pending_signal = 0;
        if (pending != 0)
            (void)hl_engine_request(engine, HL_ENGINE_REQUEST_SIGNAL, &pending, sizeof(pending));
        pthread_mutex_unlock(&activation_engine_lock);
        activation_status = hl_engine_run(engine, (int)argc, (const char *const *)argv, activation_result);
        pthread_mutex_lock(&activation_engine_lock);
        activation_engine = NULL;
        pthread_mutex_unlock(&activation_engine_lock);
    }
    hl_engine_destroy(engine);
    return activation_status == HL_STATUS_OK ? 0 : 78;
}

static void hl_activation_child(void) {
    hl_activation_request request;
    hl_activation_reply reply = {0};
    hl_activation_host *host = NULL;
    hl_host_services services;
    long descriptor;
    hl_status status = HL_STATUS_CORRUPT;
    unsigned char commit;
    pthread_t signal_thread;
    int signal_relay = 0;
    int inherited[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int inherited_count = 0;
    int environment = hl_environment_take_activation_descriptor(&descriptor);
    if (environment == 0) return;
    if (environment < 0 || descriptor != HL_ACTIVATION_FD) _exit(125);
    /* Embedded builds deliberately omit the native constructor.  Transport
     * adoption needs the private descriptor registry before the later backend
     * initialization boundary, otherwise every attached provider fails with
     * ENOSPC before it can send HELLO. */
    hl_host_private_init();
    if (hl_fork_wire_receive_descriptors((int)descriptor, &request, sizeof(request), inherited,
                                         &inherited_count) != (int)sizeof(request)) _exit(126);
    if (request.reserved > 1 || inherited_count != (int)request.reserved) {
        while (inherited_count > 0) (void)close(inherited[--inherited_count]);
        _exit(126);
    }
    if (inherited_count == 1) {
        int adopted = hl_host_process_fd_private_adopt(inherited[0]);
        if (adopted < 0) _exit(126);
        inherited[0] = adopted;
    }
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
        if (inherited[0] >= 0 && activation_provider_handshake(inherited[0]) != 0) _exit(124);
        /* Explicit setup is idempotent and independent of constructor order. */
        hl_embedded_runtime_init(request.guest_isa);
        hl_embedded_runtime_init(request.guest_isa);
        status = activation_host_create(&host, &services);
        if (status == HL_STATUS_OK && inherited[0] >= 0 &&
            (hl_provider_client_init(&activation_provider_client, inherited[0], 1024 * 1024) != 0 ||
             hl_provider_files_install(&services, &activation_provider_client) != 0))
            status = HL_STATUS_PLATFORM_FAILURE;
        activation_services = &services;
        activation_guest_isa = request.guest_isa;
        activation_result = &reply.result;
        activation_status = status;
        if (status == HL_STATUS_OK) {
            if (activation_signal_relay_start(&signal_thread) != 0) {
                status = HL_STATUS_PLATFORM_FAILURE;
                activation_status = status;
            } else
                signal_relay = 1;
        }
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
    if (signal_relay) activation_signal_relay_stop(signal_thread);
    activation_host_destroy(host);
    if (inherited[0] >= 0) {
        hl_provider_files_revoke();
        hl_provider_namespace_launch_revoke();
        hl_provider_client_destroy(&activation_provider_client);
    }
    if (inherited[0] >= 0) {
        hl_host_process_fd_private_remove(inherited[0]);
        (void)close(inherited[0]);
    }
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

static void domain_path(hl_process_domain domain, char *path, size_t capacity) {
    snprintf(path, capacity, "/tmp/.hl-domain.%016llx%016llx",
             (unsigned long long)domain.identity[0], (unsigned long long)domain.identity[1]);
}

static int domain_birth(const char *directory, pid_t pid, uint64_t *birth) {
    char path[160], text[32], *end;
    unsigned long long value;
    ssize_t count;
    int descriptor;
    snprintf(path, sizeof path, "%s/b%d", directory, (int)pid);
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return 0;
    do { count = read(descriptor, text, sizeof text - 1); } while (count < 0 && errno == EINTR);
    (void)close(descriptor);
    if (count <= 0) return 0;
    text[count] = 0;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\n' && *end != 0) || value == 0) return 0;
    *birth = (uint64_t)value;
    return 1;
}

static void domain_record_remove(const char *directory, pid_t pid) {
    static const char prefixes[] = {'\0', 'b', 'x'};
    char path[160];
    size_t index;
    for (index = 0; index < sizeof prefixes; ++index) {
        if (prefixes[index] == 0)
            snprintf(path, sizeof path, "%s/%d", directory, (int)pid);
        else
            snprintf(path, sizeof path, "%s/%c%d", directory, prefixes[index], (int)pid);
        (void)unlink(path);
    }
}

static void domain_directory_remove(const char *directory) {
    DIR *entries = opendir(directory);
    struct dirent *entry;
    if (entries == NULL) return;
    while ((entry = readdir(entries)) != NULL) {
        const char *name = entry->d_name;
        char *end;
        long raw;
        if (name[0] == 'b' || name[0] == 'x') ++name;
        if (name[0] < '1' || name[0] > '9') continue;
        errno = 0;
        raw = strtol(name, &end, 10);
        if (errno == 0 && *end == 0 && raw > 0 && raw <= INT32_MAX)
            domain_record_remove(directory, (pid_t)raw);
    }
    (void)closedir(entries);
    (void)rmdir(directory);
}

static void domain_network_remove(hl_process_domain domain) {
    char directory[96];
    DIR *entries;
    struct dirent *entry;
    snprintf(directory, sizeof directory, "/tmp/.hl-net-%016llx%016llx",
             (unsigned long long)domain.identity[0], (unsigned long long)domain.identity[1]);
    entries = opendir(directory);
    if (entries == NULL) return;
    while ((entry = readdir(entries)) != NULL) {
        const char *name = entry->d_name;
        if (name[0] != 'p' || name[1] < '0' || name[1] > '9') continue;
        for (const char *digit = name + 1; *digit != 0; ++digit)
            if (*digit < '0' || *digit > '9') goto next;
        (void)unlinkat(dirfd(entries), name, 0);
    next:;
    }
    (void)closedir(entries);
    (void)rmdir(directory);
}

static int process_info_compare(const void *left, const void *right) {
    const hl_activation_process_info *a = left;
    const hl_activation_process_info *b = right;
    return a->host_id < b->host_id ? -1 : a->host_id > b->host_id ? 1 : 0;
}

hl_status hl_activation_domain_processes(hl_process_domain domain, uint64_t initial_process_id,
                                         hl_activation_process_info *processes, uint32_t capacity,
                                         uint32_t *out_count) {
    char directory[96];
    DIR *entries;
    struct dirent *entry;
    uint32_t count = 0;
    if ((domain.identity[0] | domain.identity[1]) == 0 || initial_process_id == 0 || out_count == NULL ||
        (capacity != 0 && processes == NULL))
        return HL_STATUS_INVALID_ARGUMENT;
    domain_path(domain, directory, sizeof directory);
    entries = opendir(directory);
    if (entries == NULL) {
        if (errno != ENOENT) return HL_STATUS_PLATFORM_FAILURE;
        *out_count = 0;
        return HL_STATUS_OK;
    }
    while ((entry = readdir(entries)) != NULL) {
        char *end;
        long raw;
        uint64_t expected;
        hl_host_process_info process;
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
        errno = 0;
        raw = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *end != 0 || raw <= 0 || raw > INT32_MAX) continue;
        if (!domain_birth(directory, (pid_t)raw, &expected) || !hl_host_process_read(raw, &process) ||
            process.start_time_ns != expected) {
            domain_record_remove(directory, (pid_t)raw);
            continue;
        }
        if (count < capacity) {
            processes[count].host_id = (uint64_t)raw;
            processes[count].initial = (uint64_t)raw == initial_process_id ? 1u : 0u;
            processes[count].reserved = 0;
        }
        if (count == UINT32_MAX) {
            (void)closedir(entries);
            return HL_STATUS_RESOURCE_LIMIT;
        }
        ++count;
    }
    (void)closedir(entries);
    *out_count = count;
    if (count > capacity) return HL_STATUS_RESOURCE_LIMIT;
    if (count > 1) qsort(processes, count, sizeof(*processes), process_info_compare);
    return HL_STATUS_OK;
}

hl_status hl_activation_domain_terminate(hl_process_domain domain) {
    char directory[96];
    unsigned round;
    unsigned empty = 0;
    if ((domain.identity[0] | domain.identity[1]) == 0) return HL_STATUS_INVALID_ARGUMENT;
    domain_path(domain, directory, sizeof directory);
    for (round = 0; round < 200; ++round) {
        DIR *entries = opendir(directory);
        struct dirent *entry;
        unsigned live = 0;
        if (entries == NULL) {
            if (errno != ENOENT) return HL_STATUS_PLATFORM_FAILURE;
            domain_network_remove(domain);
            return HL_STATUS_OK;
        }
        while ((entry = readdir(entries)) != NULL) {
            char *end;
            long raw;
            uint64_t expected;
            hl_host_process_info process;
            if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
            errno = 0;
            raw = strtol(entry->d_name, &end, 10);
            if (errno != 0 || *end != 0 || raw <= 0 || raw > INT32_MAX) continue;
            if (!domain_birth(directory, (pid_t)raw, &expected) || !hl_host_process_read(raw, &process) ||
                process.start_time_ns != expected) {
                domain_record_remove(directory, (pid_t)raw);
                continue;
            }
            ++live;
            if (kill((pid_t)raw, SIGKILL) != 0 && errno != ESRCH) {
                (void)closedir(entries);
                return HL_STATUS_PLATFORM_FAILURE;
            }
        }
        (void)closedir(entries);
        if (live == 0) {
            if (++empty >= 2) {
                domain_directory_remove(directory);
                domain_network_remove(domain);
                return HL_STATUS_OK;
            }
        } else {
            empty = 0;
        }
        (void)poll(NULL, 0, 10);
    }
    return HL_STATUS_BUSY;
}

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

static hl_status activation_start(const char *executable, uint32_t guest_isa, const char *guest,
                                  const hl_activation_stdio *stdio, const hl_terminal_size *terminal,
                                  int32_t *out_master, int transport, hl_activation_process **out_process) {
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
    int master = -1;
    int slave = -1;
    if (out_process == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_process = NULL;
    if (out_master != NULL) *out_master = -1;
    if (executable == NULL || guest == NULL || executable[0] != '/' || guest[0] != '/' ||
        (guest_isa != HL_GUEST_ISA_AARCH64 && guest_isa != HL_GUEST_ISA_X86_64))
        return HL_STATUS_INVALID_ARGUMENT;
    if (stdio != NULL && (stdio->input < -1 || stdio->output < -1 || stdio->error < -1))
        return HL_STATUS_INVALID_ARGUMENT;
    if (transport < -1) return HL_STATUS_INVALID_ARGUMENT;
    if (terminal != NULL && (stdio != NULL || out_master == NULL || terminal->rows == 0 || terminal->columns == 0))
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
    request.reserved = transport >= 0 ? 1u : 0u;
    test_mode = activation_test_mode;
    request.test_flags = test_mode == 1 ? 1u : test_mode == 4 ? 4u : test_mode == 5 ? 5u : 0u;
    if (test_mode == 2) request.magic ^= UINT64_C(1);
    activation_test_mode = 0;
    memcpy(request.path, guest, path_size);
    arc4random_buf(request.nonce, sizeof(request.nonce));
    child_argv[0] = (char *)(uintptr_t)executable;
    child_argv[1] = NULL;
    if (terminal != NULL) {
        struct winsize size = {.ws_row = terminal->rows, .ws_col = terminal->columns};
        if (openpty(&master, &slave, NULL, NULL, &size) != 0 ||
            fcntl(master, F_SETFD, FD_CLOEXEC) != 0 || fcntl(slave, F_SETFD, FD_CLOEXEC) != 0) {
            if (master >= 0) close(master);
            if (slave >= 0) close(slave);
            close(pair[0]); close(pair[1]); free(child_env); return HL_STATUS_PLATFORM_FAILURE;
        }
        child = fork();
        if (child == 0) {
            (void)close(master);
            (void)close(pair[0]);
            if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 || dup2(slave, 0) < 0 || dup2(slave, 1) < 0 ||
                dup2(slave, 2) < 0 || dup2(pair[1], HL_ACTIVATION_FD) < 0)
                _exit(126);
            if (slave > STDERR_FILENO) (void)close(slave);
            if (pair[1] != HL_ACTIVATION_FD) (void)close(pair[1]);
            execve(executable, child_argv, child_env);
            _exit(127);
        }
        (void)close(slave);
        slave = -1;
        if (child < 0) {
            close(master); close(pair[0]); close(pair[1]); free(child_env); return HL_STATUS_PLATFORM_FAILURE;
        }
        goto spawned;
    }
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
spawned:
    close(pair[1]);
    free(child_env);
    if ((transport >= 0 && test_mode != 3
             ? hl_fork_wire_send_descriptors(pair[0], &request, sizeof(request), &transport, 1)
             : hl_fork_wire_send_descriptors(pair[0], &request,
                                             test_mode == 3 ? sizeof(request) / 2u : sizeof(request), NULL, 0)) != 0) {
        close(pair[0]); if (master >= 0) close(master); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    }
    if (test_mode == 3) (void)shutdown(pair[0], SHUT_WR);
    if (transfer(pair[0], &reply, sizeof(reply), 0) != 0) {
        close(pair[0]); if (master >= 0) close(master); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    }
    if (reply.magic != request.magic || reply.abi != request.abi || reply.size != sizeof(reply) ||
        memcmp(reply.nonce, request.nonce, sizeof(request.nonce)) != 0 || reply.status != HL_STATUS_OK) {
        close(pair[0]); if (master >= 0) close(master); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    }
    { unsigned char commit = 0xa5u; if (transfer(pair[0], &commit, 1, 1) != 0) {
        close(pair[0]); if (master >= 0) close(master); (void)kill(-child, SIGKILL); (void)wait_child(child, &waited); return HL_STATUS_CORRUPT;
    } }
    process = calloc(1, sizeof(*process));
    if (process == NULL) { close(pair[0]); if (master >= 0) close(master); (void)kill(-child, SIGKILL); wait_child(child, &waited); return HL_STATUS_OUT_OF_MEMORY; }
    process->descriptor = pair[0];
    process->pid = child;
    memcpy(process->nonce, request.nonce, sizeof(process->nonce));
    if (out_master != NULL) *out_master = master;
    *out_process = process;
    return HL_STATUS_OK;
}

hl_status hl_activation_start_with_stdio(const char *executable, uint32_t guest_isa, const char *guest,
                                         const hl_activation_stdio *stdio, hl_activation_process **out_process) {
    return activation_start(executable, guest_isa, guest, stdio, NULL, NULL, -1, out_process);
}

hl_status hl_activation_start_with_transport(const char *executable, uint32_t guest_isa, const char *guest,
                                             const hl_activation_stdio *stdio, int32_t transport,
                                             hl_activation_process **out_process) {
    if (transport < 0) return HL_STATUS_INVALID_ARGUMENT;
    return activation_start(executable, guest_isa, guest, stdio, NULL, NULL, transport, out_process);
}

hl_status hl_activation_start_terminal(const char *executable, uint32_t guest_isa, const char *guest,
                                       hl_terminal_size size, int32_t *out_master,
                                       hl_activation_process **out_process) {
    return activation_start(executable, guest_isa, guest, NULL, &size, out_master, -1, out_process);
}

hl_status hl_activation_start_terminal_with_transport(
    const char *executable, uint32_t guest_isa, const char *guest, hl_terminal_size size,
    int32_t transport, int32_t *out_master, hl_activation_process **out_process) {
    if (transport < 0) return HL_STATUS_INVALID_ARGUMENT;
    return activation_start(executable, guest_isa, guest, NULL, &size, out_master, transport,
                            out_process);
}

hl_status hl_terminal_resize(int32_t master, hl_terminal_size size) {
    struct winsize native = {.ws_row = size.rows, .ws_col = size.columns};
    if (master < 0 || size.rows == 0 || size.columns == 0) return HL_STATUS_INVALID_ARGUMENT;
    return ioctl(master, TIOCSWINSZ, &native) == 0 ? HL_STATUS_OK : HL_STATUS_PLATFORM_FAILURE;
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
