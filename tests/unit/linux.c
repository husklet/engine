#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "hl/linux.h"
#include "counter.h"
#include "transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int32_t process_exit(void *context) {
    return (int32_t)(intptr_t)context;
}

static int32_t process_signal(void *context) {
    (void)context;
    raise(SIGTERM);
    return 99;
}

static int32_t process_sleep(void *context) {
    struct timespec duration = {0, (long)(intptr_t)context};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {}
    return 9;
}

static int32_t process_pause(void *context) {
    (void)context;
    for (;;)
        pause();
    return 0;
}

typedef struct cleanup_probe {
    int descriptor;
} cleanup_probe;

typedef struct process_wait_context {
    const hl_host_services *services;
    hl_host_handle process;
    hl_host_result result;
} process_wait_context;

typedef struct clock_interrupt_context {
    pthread_t target;
} clock_interrupt_context;

static void clock_interrupt_handler(int signal_number) {
    (void)signal_number;
}

static void *interrupt_clock_sleep(void *opaque) {
    const clock_interrupt_context *interrupt = opaque;
    const struct timespec delay = {0, 20 * 1000 * 1000};
    (void)nanosleep(&delay, NULL);
    (void)pthread_kill(interrupt->target, SIGUSR1);
    return NULL;
}

static void *wait_for_process(void *opaque) {
    process_wait_context *waiter = opaque;
    waiter->result =
        waiter->services->process->wait(waiter->services->context, waiter->process, HL_HOST_DEADLINE_INFINITE);
    return NULL;
}

static int32_t process_cleanup_probe(void *opaque) {
    cleanup_probe *probe = opaque;
    pid_t pid = getpid();
    if (write(probe->descriptor, &pid, sizeof(pid)) != (ssize_t)sizeof(pid)) return 98;
    for (;;)
        pause();
}

int main(void) {
    hl_host_linux *linux_host;
    hl_host_services services;
    hl_host_result mapping;
    hl_host_result file;
    hl_host_result pollset;
    hl_host_result receiver;
    hl_host_result sender;
    hl_host_result shared;
    hl_host_result shared_copy;
    hl_host_code_mapping code;
    hl_host_event_record event;
    hl_host_file_metadata metadata;
    hl_host_result process;
    hl_host_result process_result;
    const char contents[] = "portable-host";
    const char suffix[] = "append";
    char readback[sizeof(contents) + sizeof(suffix)] = {0};
    char path[128];
    char moved_path[160];
    char socket_path[108];

    HL_CHECK(hl_host_linux_create(&linux_host, &services) == HL_STATUS_OK);
    {
        long page = sysconf(_SC_PAGESIZE);
        char map_path[128];
        hl_host_file_mapping shared_map = {HL_HOST_FILE_MAPPING_ABI, sizeof(shared_map), 0, 0, 0, 0};
        hl_host_file_mapping private_map = {HL_HOST_FILE_MAPPING_ABI, sizeof(private_map), 0, 0, 0, 0};
        hl_host_result mapped_file;
        char disk = 0;
        snprintf(map_path, sizeof(map_path), "/tmp/hl_file_map_linux_%ld", (long)getpid());
        mapped_file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, map_path, strlen(map_path),
                                                   HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                                   HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
        HL_CHECK(page > 0 && mapped_file.status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, mapped_file.value, (uint64_t)page).status == HL_STATUS_OK);
        HL_CHECK(services.file
                     ->sync_range(services.context, mapped_file.value, 0, (uint64_t)page,
                                  HL_HOST_FILE_SYNC_WAIT_BEFORE | HL_HOST_FILE_SYNC_WRITE |
                                      HL_HOST_FILE_SYNC_WAIT_AFTER)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.file->sync_range(services.context, mapped_file.value, 0, 1, 8).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.file->sync_filesystem(services.context, mapped_file.value).status == HL_STATUS_OK);
        HL_CHECK(services.memory->map_file(services.context, mapped_file.value, 0, 0, (uint64_t)page,
                                           HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE, HL_HOST_MEMORY_SHARED,
                                           &shared_map).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, mapped_file.value).status == HL_STATUS_OK);
        ((char *)(uintptr_t)shared_map.address)[0] = 'S';
        HL_CHECK(services.memory->sync(services.context, shared_map.handle, 0, (uint64_t)page).status == HL_STATUS_OK);
        mapped_file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, map_path, strlen(map_path),
                                                   HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, 0, 0);
        HL_CHECK(mapped_file.status == HL_STATUS_OK);
        HL_CHECK(services.file->read_at(services.context, mapped_file.value, 0, (hl_host_bytes){&disk, 1}).status ==
                     HL_STATUS_OK &&
                 disk == 'S');
        HL_CHECK(services.memory->map_file(services.context, mapped_file.value, 0, 0, (uint64_t)page,
                                           HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE, HL_HOST_MEMORY_PRIVATE,
                                           &private_map).status == HL_STATUS_OK);
        ((char *)(uintptr_t)private_map.address)[0] = 'P';
        HL_CHECK(services.memory->sync(services.context, private_map.handle, 0, (uint64_t)page).status == HL_STATUS_OK);
        disk = 0;
        HL_CHECK(services.file->read_at(services.context, mapped_file.value, 0, (hl_host_bytes){&disk, 1}).status ==
                     HL_STATUS_OK &&
                 disk == 'S');
        HL_CHECK(services.memory->protect(services.context, private_map.handle, 0, (uint64_t)page,
                                          HL_HOST_MEMORY_READ).status == HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, private_map.handle, 0, (uint64_t)page).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, shared_map.handle, 0, (uint64_t)page).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, mapped_file.value).status == HL_STATUS_OK);
        HL_CHECK(unlink(map_path) == 0);
    }
    {
        long page = sysconf(_SC_PAGESIZE);
        char map_path[128];
        hl_host_file_mapping first = {HL_HOST_FILE_MAPPING_ABI, sizeof(first), 0, 0, 0, 0};
        hl_host_file_mapping fork_shared = {HL_HOST_FILE_MAPPING_ABI, sizeof(fork_shared), 0, 0, 0, 0};
        hl_host_file_mapping replacement = {HL_HOST_FILE_MAPPING_ABI, sizeof(replacement), 0, 0, 0, 0};
        hl_host_file_mapping collision = {HL_HOST_FILE_MAPPING_ABI, sizeof(collision), 0, 0, 0, 0};
        hl_host_result mapped_file;
        snprintf(map_path, sizeof(map_path), "/tmp/hl_file_fixed_linux_%ld", (long)getpid());
        mapped_file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, map_path, strlen(map_path),
                                                   HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                                   HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
        HL_CHECK(page > 0 && mapped_file.status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, mapped_file.value, (uint64_t)page * 3).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->map_file(services.context, mapped_file.value, 0, 0, (uint64_t)page * 3,
                                           HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE, HL_HOST_MEMORY_PRIVATE,
                                           &first).status == HL_STATUS_OK);
        memset((void *)(uintptr_t)first.address, 'A', (size_t)page * 3);
        HL_CHECK(services.memory->map_file(services.context, mapped_file.value, 0, 0, (uint64_t)page,
                                           HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE, HL_HOST_MEMORY_SHARED,
                                           &fork_shared).status == HL_STATUS_OK);
        {
            pid_t child = fork();
            int child_status = 0;
            HL_CHECK(child >= 0);
            if (child == 0) {
                ((char *)(uintptr_t)first.address)[0] = 'P';
                ((char *)(uintptr_t)fork_shared.address)[0] = 'S';
                _exit(0);
            }
            HL_CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
                     WEXITSTATUS(child_status) == 0);
            HL_CHECK(((char *)(uintptr_t)first.address)[0] == 'A');
            HL_CHECK(((char *)(uintptr_t)fork_shared.address)[0] == 'S');
        }
        HL_CHECK(services.memory->map_file(services.context, mapped_file.value, first.address, (uint64_t)page,
                                           (uint64_t)page, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                           HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED_NOREPLACE,
                                           &collision).status == HL_STATUS_ALREADY_EXISTS);
        HL_CHECK(((char *)(uintptr_t)first.address)[0] == 'A');
        HL_CHECK(services.memory->map_file(services.context, mapped_file.value, first.address, (uint64_t)page,
                                           (uint64_t)page * 3, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                           HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED,
                                           &replacement).status == HL_STATUS_OK &&
                 replacement.address == first.address);
        HL_CHECK(services.memory->protect(services.context, replacement.handle, (uint64_t)page, (uint64_t)page,
                                          HL_HOST_MEMORY_READ).status == HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, replacement.handle, (uint64_t)page,
                                              (uint64_t)page).status == HL_STATUS_OK);
        HL_CHECK(services.memory->protect(services.context, replacement.handle, 0, (uint64_t)page,
                                          HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE).status == HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, replacement.handle, 0, (uint64_t)page).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, replacement.handle, (uint64_t)page * 2,
                                              (uint64_t)page).status == HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, fork_shared.handle, 0, (uint64_t)page).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, mapped_file.value).status == HL_STATUS_OK);
        HL_CHECK(unlink(map_path) == 0);
    }
    {
        hl_host_result stream = services.file->standard_stream(services.context, HL_HOST_STANDARD_OUTPUT);
        HL_CHECK(stream.status == HL_STATUS_OK && (stream.detail & HL_HOST_FILE_WRITE) != 0);
        HL_CHECK(services.file->close(services.context, stream.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->standard_stream(services.context, 3).status == HL_STATUS_INVALID_ARGUMENT);
    }
    {
        char probe_path[128];
        char first = 0;
        char second = 0;
        int saved = dup(STDIN_FILENO);
        int descriptor;
        hl_host_result stream;
        snprintf(probe_path, sizeof(probe_path), "/tmp/hl_stdio_linux_%ld", (long)getpid());
        descriptor = open(probe_path, O_CREAT | O_EXCL | O_RDWR | O_APPEND | O_NONBLOCK, 0600);
        HL_CHECK(saved >= 0 && descriptor >= 0 && write(descriptor, "ab", 2) == 2 &&
                 lseek(descriptor, 0, SEEK_SET) == 0 && dup2(descriptor, STDIN_FILENO) == STDIN_FILENO);
        stream = services.file->standard_stream(services.context, HL_HOST_STANDARD_INPUT);
        HL_CHECK(stream.status == HL_STATUS_OK && (stream.detail & HL_HOST_FILE_READ) != 0 &&
                 (stream.detail & (HL_HOST_FILE_APPEND | HL_HOST_FILE_NONBLOCK)) ==
                     (HL_HOST_FILE_APPEND | HL_HOST_FILE_NONBLOCK));
        HL_CHECK(services.file->read(services.context, stream.value, &first, 1).value == 1 && first == 'a');
        HL_CHECK(read(STDIN_FILENO, &second, 1) == 1 && second == 'b');
        HL_CHECK(services.file->close(services.context, stream.value).status == HL_STATUS_OK);
        HL_CHECK(dup2(saved, STDIN_FILENO) == STDIN_FILENO && close(saved) == 0 && close(descriptor) == 0 &&
                 unlink(probe_path) == 0);
    }
    HL_CHECK(check_counter(&services) == 0);
    HL_CHECK(check_transfer_fork(&services) == 0);
    HL_CHECK(hl_host_services_validate(
                 &services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_FILE | HL_HOST_CAP_EVENT |
                                HL_HOST_CAP_NETWORK | HL_HOST_CAP_EVENT_TIMER | HL_HOST_CAP_SHARED_MEMORY |
                                HL_HOST_CAP_PROCESS | HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_SYNC) == HL_STATUS_OK);
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.clock->realtime_ns(services.context).status == HL_STATUS_OK);
    {
        hl_host_result raw_before = services.clock->raw_monotonic_ns(services.context);
        hl_host_result process_before = services.clock->process_cpu_ns(services.context);
        hl_host_result thread_before = services.clock->thread_cpu_ns(services.context);
        volatile uint64_t work = 0;
        uint64_t index;
        hl_host_result raw_after;
        hl_host_result process_after;
        hl_host_result thread_after;
        hl_host_result deadline;
        struct sigaction action = {0};
        struct sigaction previous;
        clock_interrupt_context interrupt = {pthread_self()};
        pthread_t interrupter;

        HL_CHECK(raw_before.status == HL_STATUS_OK && process_before.status == HL_STATUS_OK &&
                 thread_before.status == HL_STATUS_OK);
        for (index = 0; index < UINT64_C(1000000); ++index)
            work += index;
        HL_CHECK(work != 0);
        raw_after = services.clock->raw_monotonic_ns(services.context);
        process_after = services.clock->process_cpu_ns(services.context);
        thread_after = services.clock->thread_cpu_ns(services.context);
        HL_CHECK(raw_after.status == HL_STATUS_OK && raw_after.value >= raw_before.value);
        HL_CHECK(process_after.status == HL_STATUS_OK && process_after.value > process_before.value);
        HL_CHECK(thread_after.status == HL_STATUS_OK && thread_after.value > thread_before.value);

        deadline = services.clock->monotonic_ns(services.context);
        HL_CHECK(deadline.status == HL_STATUS_OK);
        deadline.value += UINT64_C(5000000);
        HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_MONOTONIC, deadline.value).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline.value);
        HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_PROCESS_CPU, 0).status ==
                 HL_STATUS_NOT_SUPPORTED);

        action.sa_handler = clock_interrupt_handler;
        HL_CHECK(sigemptyset(&action.sa_mask) == 0);
        HL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
        deadline = services.clock->monotonic_ns(services.context);
        HL_CHECK(deadline.status == HL_STATUS_OK);
        HL_CHECK(pthread_create(&interrupter, NULL, interrupt_clock_sleep, &interrupt) == 0);
        HL_CHECK(services.clock
                     ->sleep_until(services.context, HL_HOST_CLOCK_MONOTONIC, deadline.value + UINT64_C(2000000000))
                     .status == HL_STATUS_INTERRUPTED);
        HL_CHECK(pthread_join(interrupter, NULL) == 0);
        HL_CHECK(sigaction(SIGUSR1, &previous, NULL) == 0);
    }
    {
        const uint32_t count = 65536;
        hl_host_handle *mutexes = calloc(count, sizeof(*mutexes));
        uint32_t index;
        HL_CHECK(mutexes != NULL);
        for (index = 0; index < count; ++index) {
            hl_host_result created = services.sync->mutex_create(services.context);
            HL_CHECK(created.status == HL_STATUS_OK);
            mutexes[index] = created.value;
        }
        HL_CHECK(services.sync->mutex_create(services.context).status == HL_STATUS_RESOURCE_LIMIT);
        for (index = 0; index < count; ++index) {
            HL_CHECK(services.sync->mutex_lock(services.context, mutexes[index]).status == HL_STATUS_OK);
            HL_CHECK(services.sync->mutex_unlock(services.context, mutexes[index]).status == HL_STATUS_OK);
            HL_CHECK(services.sync->mutex_close(services.context, mutexes[index]).status == HL_STATUS_OK);
        }
        free(mutexes);
    }
    {
        hl_host_result first = services.sync->mutex_create(services.context);
        hl_host_result second = services.sync->mutex_create(services.context);
        HL_CHECK(first.status == HL_STATUS_OK && second.status == HL_STATUS_OK && first.value != second.value);
        HL_CHECK(services.sync->mutex_lock(services.context, first.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_lock(services.context, second.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_close(services.context, first.value).status == HL_STATUS_BUSY);
        HL_CHECK(services.sync->mutex_unlock(services.context, first.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_unlock(services.context, second.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_close(services.context, first.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_close(services.context, second.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_lock(services.context, first.value).status == HL_STATUS_INVALID_ARGUMENT);
    }

    mapping = services.memory->reserve(services.context, 4096, 4096, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
    HL_CHECK(mapping.status == HL_STATUS_OK);
    HL_CHECK(services.memory->protect(services.context, mapping.value, 0, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_OK);
    HL_CHECK(services.memory->publish_code(services.context, mapping.value, 0, 4096).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_INVALID_ARGUMENT);

    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 4096, 4096, HL_HOST_CODE_DUAL_ALIAS, &code).status ==
             HL_STATUS_OK);
    HL_CHECK(code.handle != 0 && code.writable_address != 0 && code.executable_address != 0);
    memcpy((void *)(uintptr_t)code.writable_address, "jit", 4);
    HL_CHECK(services.memory->publish_code(services.context, code.handle, 0, 4).status == HL_STATUS_OK);
    HL_CHECK(memcmp((const void *)(uintptr_t)code.executable_address, "jit", 4) == 0);
    HL_CHECK(services.memory->repair_code_after_fork(services.context, &code, 1).status == HL_STATUS_OK);
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, code.handle).status == HL_STATUS_OK);

    snprintf(path, sizeof(path), "/tmp/hl_host_linux_%ld", (long)getpid());
    file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_WRITE | HL_HOST_FILE_APPEND,
                                        HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
    HL_CHECK(file.status == HL_STATUS_OK);
    {
        char resolved[1024];
        char *expected = realpath(path, NULL);
        hl_host_result result =
            services.file->path(services.context, file.value, (hl_host_bytes){resolved, sizeof resolved});
        HL_CHECK(expected != NULL);
        HL_CHECK(result.status == HL_STATUS_OK && result.value == strlen(expected) &&
                 memcmp(resolved, expected, (size_t)result.value) == 0);
        result = services.file->path(services.context, file.value, (hl_host_bytes){resolved, strlen(expected) - 1});
        HL_CHECK(result.status == HL_STATUS_RESOURCE_LIMIT && result.value == strlen(expected));
        free(expected);
    }
    HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){contents, sizeof(contents)})
                 .value == sizeof(contents));
    snprintf(moved_path, sizeof(moved_path), "%s.moved", path);
    HL_CHECK(services.file
                 ->rename_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path), HL_HOST_HANDLE_CWD,
                                   moved_path, strlen(moved_path))
                 .status == HL_STATUS_OK);
    {
        int replacement = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        const hl_host_iovec append_vectors[] = {{(uint64_t)(uintptr_t)suffix, 3},
                                                {(uint64_t)(uintptr_t)(suffix + 3), sizeof(suffix) - 3}};
        HL_CHECK(replacement >= 0 && write(replacement, "replacement", 11) == 11 && close(replacement) == 0);
        HL_CHECK(services.file->appendv(services.context, file.value, append_vectors, 2).value == sizeof(suffix));
    }
    HL_CHECK(
        services.file->read_at(services.context, file.value, 0, (hl_host_bytes){readback, sizeof(readback)}).value ==
        sizeof(readback));
    HL_CHECK(memcmp(contents, readback, sizeof(contents)) == 0);
    HL_CHECK(memcmp(suffix, readback + sizeof(contents), sizeof(suffix)) == 0);
    HL_CHECK(services.file->metadata(services.context, file.value, &metadata).status == HL_STATUS_OK);
    HL_CHECK(metadata.size == sizeof(readback));
    HL_CHECK(metadata.type == HL_HOST_FILE_TYPE_REGULAR);
    HL_CHECK((metadata.permissions & 0777u) == 0600u);
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    {
        char replacement[11];
        int descriptor = open(path, O_RDONLY);
        HL_CHECK(descriptor >= 0 &&
                 read(descriptor, replacement, sizeof(replacement)) == (ssize_t)sizeof(replacement) &&
                 memcmp(replacement, "replacement", sizeof(replacement)) == 0 && close(descriptor) == 0);
    }
    HL_CHECK(services.file
                 ->rename_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                   HL_HOST_HANDLE_CWD, path, strlen(path))
                 .status == HL_STATUS_OK);
    {
        char published[sizeof(readback)] = {0};
        int descriptor = open(path, O_RDONLY);
        HL_CHECK(descriptor >= 0 && read(descriptor, published, sizeof(published)) == (ssize_t)sizeof(published) &&
                 memcmp(published, readback, sizeof(published)) == 0 && close(descriptor) == 0);
    }
    HL_CHECK(services.file->unlink_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path)).status ==
             HL_STATUS_OK);
    HL_CHECK(
        services.file->unlink_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path)).status ==
        HL_STATUS_NOT_FOUND);
    {
        char target[32] = {0};
        hl_host_result link;
        snprintf(moved_path, sizeof(moved_path), "%s.link", path);
        HL_CHECK(symlink("portable-target", moved_path) == 0);
        HL_CHECK(services.file
                     ->open_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                     HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0)
                     .status != HL_STATUS_OK);
        link = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                            HL_HOST_FILE_PATH_ONLY | HL_HOST_FILE_NOFOLLOW, 0, 0);
        HL_CHECK(link.status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, link.value, &metadata).status == HL_STATUS_OK &&
                 metadata.type == HL_HOST_FILE_TYPE_SYMLINK);
        HL_CHECK(services.file->readlink(services.context, link.value, (hl_host_bytes){target, sizeof target}).value ==
                 strlen("portable-target"));
        HL_CHECK(memcmp(target, "portable-target", strlen("portable-target")) == 0);
        HL_CHECK(
            services.file->set_owner(services.context, link.value, (uint32_t)getuid(), (uint32_t)getgid()).status ==
            HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, link.value).status == HL_STATUS_OK);
        HL_CHECK(unlink(moved_path) == 0);
    }

    HL_CHECK(services.network->socket(services.context, 99, HL_HOST_NETWORK_DATAGRAM, 0).status ==
             HL_STATUS_INVALID_ARGUMENT);
    receiver = services.network->socket(services.context, HL_HOST_NETWORK_LOCAL, HL_HOST_NETWORK_DATAGRAM, 0);
    sender = services.network->socket(services.context, HL_HOST_NETWORK_LOCAL, HL_HOST_NETWORK_DATAGRAM, 0);
    HL_CHECK(receiver.status == HL_STATUS_OK && sender.status == HL_STATUS_OK);
    snprintf(socket_path, sizeof(socket_path), "/tmp/hl-host-linux-socket-%ld", (long)getpid());
    unlink(socket_path);
    {
        hl_host_network_address address = {0};
        const char datagram[] = "ready";
        char received[sizeof(datagram)] = {0};
        address.family = HL_HOST_NETWORK_LOCAL;
        address.size = (uint16_t)strlen(socket_path);
        memcpy(address.local_path, socket_path, address.size);
        HL_CHECK(services.network->bind(services.context, receiver.value, &address).status == HL_STATUS_OK);
        HL_CHECK(services.network->connect(services.context, sender.value, &address).status == HL_STATUS_OK);

        pollset = services.event->create(services.context);
        HL_CHECK(pollset.status == HL_STATUS_OK);
        HL_CHECK(
            services.event
                ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, receiver.value, 0, HL_HOST_READY_READ)
                .status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(
            services.event
                ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, receiver.value, 73, HL_HOST_READY_READ)
                .status == HL_STATUS_OK);
        uint64_t start = services.clock->monotonic_ns(services.context).value;
        uint64_t deadline = start + UINT64_C(10000000);
        HL_CHECK(services.event->wait(services.context, pollset.value, &event, 1, deadline).value == 0);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline);

        HL_CHECK(
            services.network->send(services.context, sender.value, (hl_host_const_bytes){datagram, sizeof(datagram)}, 0)
                .value == sizeof(datagram));
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
        {
            hl_host_result ready = services.event->wait(services.context, pollset.value, &event, 1, deadline);
            HL_CHECK(ready.status == HL_STATUS_OK && ready.value == 1 && event.token == 73 &&
                     (event.readiness & HL_HOST_READY_READ) != 0);
        }
        HL_CHECK(
            services.network->receive(services.context, receiver.value, (hl_host_bytes){received, sizeof(received)}, 0)
                .value == sizeof(received));
        HL_CHECK(memcmp(received, datagram, sizeof(datagram)) == 0);
        HL_CHECK(services.event->control(services.context, pollset.value, HL_HOST_EVENT_DELETE, receiver.value, 73, 0)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.event->control(services.context, pollset.value, 99, receiver.value, 73, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.event->wake(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(services.event
                     ->wait(services.context, pollset.value, &event, 1,
                            services.clock->monotonic_ns(services.context).value + UINT64_C(100000000))
                     .value == 0);
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(5000000);
        HL_CHECK(services.event->arm_timer(services.context, pollset.value, 91, deadline, 0).status == HL_STATUS_OK);
        {
            hl_host_result ready =
                services.event->wait(services.context, pollset.value, &event, 1, deadline + UINT64_C(1000000000));
            HL_CHECK(ready.status == HL_STATUS_OK && ready.value == 1 && event.token == 91 &&
                     (event.readiness & HL_HOST_READY_TIMER) != 0);
        }
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(5000000);
        HL_CHECK(services.event->arm_timer(services.context, pollset.value, 92, deadline, UINT64_C(5000000)).status ==
                 HL_STATUS_OK);
        HL_CHECK(
            services.event->wait(services.context, pollset.value, &event, 1, deadline + UINT64_C(1000000000)).value ==
            1);
        HL_CHECK(services.event->disarm_timer(services.context, pollset.value, 92).status == HL_STATUS_OK);
        HL_CHECK(services.event->disarm_timer(services.context, pollset.value, 92).status == HL_STATUS_NOT_FOUND);
        HL_CHECK(services.event->arm_timer(services.context, pollset.value, 0, deadline, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(services.event->wake(services.context, pollset.value).status == HL_STATUS_INVALID_ARGUMENT);
    }
    HL_CHECK(services.network->close(services.context, receiver.value).status == HL_STATUS_OK);
    HL_CHECK(services.network->receive(services.context, receiver.value, (hl_host_bytes){readback, 1}, 0).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.network->close(services.context, receiver.value).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.network->close(services.context, sender.value).status == HL_STATUS_OK);
    unlink(socket_path);

    shared = services.shared_memory->create(services.context, 4096, 0);
    HL_CHECK(shared.status == HL_STATUS_OK && shared.detail == shared.value);
    HL_CHECK(services.shared_memory->create(services.context, 4096, 1).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.shared_memory->open(services.context, shared.detail, 1).status == HL_STATUS_INVALID_ARGUMENT);
    shared_copy = services.shared_memory->open(services.context, shared.detail, 0);
    HL_CHECK(shared_copy.status == HL_STATUS_OK && shared_copy.value != shared.value &&
             shared_copy.detail == shared.detail);
    HL_CHECK(services.shared_memory->resize(services.context, shared_copy.value, 8192).status == HL_STATUS_OK);
    HL_CHECK(services.file->metadata(services.context, shared.value, &metadata).status == HL_STATUS_OK &&
             metadata.size == 8192);
    HL_CHECK(services.shared_memory->close(services.context, shared.value).status == HL_STATUS_OK);
    HL_CHECK(services.shared_memory->open(services.context, shared.detail, 0).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.shared_memory->resize(services.context, shared_copy.value, 12288).status == HL_STATUS_OK);
    HL_CHECK(services.shared_memory->close(services.context, shared_copy.value).status == HL_STATUS_OK);

    process = services.process->spawn_cloned(services.context, process_exit, (void *)(intptr_t)37);
    HL_CHECK(process.status == HL_STATUS_OK);
    process_result = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_result.status == HL_STATUS_OK && process_result.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_result.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.process->wait(services.context, process.value, 0).status == HL_STATUS_INVALID_ARGUMENT);

    process = services.process->spawn_cloned(services.context, process_signal, NULL);
    HL_CHECK(process.status == HL_STATUS_OK);
    process_result = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_result.status == HL_STATUS_OK && process_result.detail == HL_HOST_PROCESS_EXIT_SIGNAL &&
             process_result.value == SIGTERM);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);

    process = services.process->spawn_cloned(services.context, process_sleep, (void *)(intptr_t)200000000);
    HL_CHECK(process.status == HL_STATUS_OK);
    HL_CHECK(services.process->wait(services.context, process.value, 0).status == HL_STATUS_WOULD_BLOCK);
    {
        uint64_t deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(20000000);
        HL_CHECK(services.process->wait(services.context, process.value, deadline).status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline);
    }
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_BUSY);
    process_result = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_result.status == HL_STATUS_OK && process_result.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_result.value == 9);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);

    {
        process_wait_context first = {&services, 0, {0}};
        process_wait_context second = {&services, 0, {0}};
        pthread_t first_thread;
        pthread_t second_thread;
        process = services.process->spawn_cloned(services.context, process_pause, NULL);
        HL_CHECK(process.status == HL_STATUS_OK);
        first.process = process.value;
        second.process = process.value;
        HL_CHECK(pthread_create(&first_thread, NULL, wait_for_process, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, wait_for_process, &second) == 0);
        HL_CHECK(services.process->terminate(services.context, process.value, HL_HOST_PROCESS_TERMINATE_FORCE).status ==
                 HL_STATUS_OK);
        HL_CHECK(pthread_join(first_thread, NULL) == 0 && pthread_join(second_thread, NULL) == 0);
        HL_CHECK(first.result.status == HL_STATUS_OK && second.result.status == HL_STATUS_OK &&
                 first.result.detail == HL_HOST_PROCESS_EXIT_SIGNAL && second.result.detail == first.result.detail &&
                 first.result.value == SIGKILL && second.result.value == first.result.value);
        HL_CHECK(services.process->wait(services.context, process.value, 0).value == SIGKILL);
        HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    }

    process = services.process->spawn_cloned(services.context, process_pause, NULL);
    HL_CHECK(process.status == HL_STATUS_OK);
    HL_CHECK(services.process->terminate(services.context, process.value, 0).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.process->terminate(services.context, process.value, HL_HOST_PROCESS_TERMINATE_FORCE).status ==
             HL_STATUS_OK);
    process_result = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_result.status == HL_STATUS_OK && process_result.detail == HL_HOST_PROCESS_EXIT_SIGNAL &&
             process_result.value == SIGKILL);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    HL_CHECK(services.process->terminate(services.context, process.value, HL_HOST_PROCESS_TERMINATE_FORCE).status ==
             HL_STATUS_INVALID_ARGUMENT);

    {
        int descriptors[2];
        cleanup_probe probe;
        process_wait_context cleanup_waiter = {&services, 0, {0}};
        pthread_t cleanup_thread;
        struct timespec settle = {0, 10000000};
        pid_t child;
        int status;
        HL_CHECK(pipe(descriptors) == 0);
        probe.descriptor = descriptors[1];
        process = services.process->spawn_cloned(services.context, process_cleanup_probe, &probe);
        HL_CHECK(process.status == HL_STATUS_OK);
        cleanup_waiter.process = process.value;
        HL_CHECK(pthread_create(&cleanup_thread, NULL, wait_for_process, &cleanup_waiter) == 0);
        close(descriptors[1]);
        HL_CHECK(read(descriptors[0], &child, sizeof(child)) == (ssize_t)sizeof(child));
        close(descriptors[0]);
        nanosleep(&settle, NULL);
        hl_host_linux_destroy(linux_host);
        HL_CHECK(pthread_join(cleanup_thread, NULL) == 0);
        HL_CHECK(cleanup_waiter.result.status == HL_STATUS_OK &&
                 cleanup_waiter.result.detail == HL_HOST_PROCESS_EXIT_SIGNAL && cleanup_waiter.result.value == SIGKILL);
        errno = 0;
        HL_CHECK(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    }

    return EXIT_SUCCESS;
}
