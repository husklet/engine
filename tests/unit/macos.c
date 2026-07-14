#include "test.h"

#include "hl/macos.h"
#include "../../src/host/clock.h"
#include "../../src/host/file.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int32_t child_exit_37(void *context) {
    HL_CHECK(context == (void *)(uintptr_t)37);
    return 37;
}

static int32_t child_sleep(void *context) {
    struct timespec duration = {0, (long)(intptr_t)context};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {}
    return 41;
}

static int32_t child_pause(void *context) {
    (void)context;
    for (;;)
        pause();
    return 0;
}

typedef struct process_wait_context {
    const hl_host_services *services;
    hl_host_handle process;
    hl_host_result result;
} process_wait_context;

static void *wait_for_process(void *opaque) {
    process_wait_context *waiter = opaque;
    waiter->result =
        waiter->services->process->wait(waiter->services->context, waiter->process, HL_HOST_DEADLINE_INFINITE);
    return NULL;
}

int main(void) {
    hl_host_macos *host;
    hl_host_services services;
    hl_host_code_mapping code;
    hl_host_result process;
    hl_host_result process_exit;
    hl_host_result file;
    char path[128];
    char contents[3] = {0};
    HL_CHECK(hl_host_macos_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_PROCESS |
                                                      HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_SYNC) == HL_STATUS_OK);
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
    {
        struct timespec realtime;
        struct timespec monotonic;
        HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_REALTIME, &realtime) == 0);
        HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &monotonic) == 0);
        HL_CHECK(realtime.tv_sec > 0 && monotonic.tv_sec > 0);
    }

    process = services.process->spawn_cloned(services.context, child_exit_37, (void *)(uintptr_t)37);
    HL_CHECK(process.status == HL_STATUS_OK && process.value != HL_HOST_HANDLE_INVALID);
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_INVALID_ARGUMENT);

    process = services.process->spawn_cloned(services.context, child_sleep, (void *)(intptr_t)150000000);
    HL_CHECK(process.status == HL_STATUS_OK);
    {
        uint64_t deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(20000000);
        HL_CHECK(services.process->wait(services.context, process.value, deadline).status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline);
    }
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 41);
    HL_CHECK(services.process->wait(services.context, process.value, 0).value == 41);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);

    {
        process_wait_context first = {&services, 0, {0}};
        process_wait_context second = {&services, 0, {0}};
        pthread_t first_thread;
        pthread_t second_thread;
        process = services.process->spawn_cloned(services.context, child_pause, NULL);
        HL_CHECK(process.status == HL_STATUS_OK);
        first.process = process.value;
        second.process = process.value;
        HL_CHECK(pthread_create(&first_thread, NULL, wait_for_process, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, wait_for_process, &second) == 0);
        HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_BUSY);
        HL_CHECK(services.process->terminate(services.context, process.value, HL_HOST_PROCESS_TERMINATE_FORCE).status ==
                 HL_STATUS_OK);
        HL_CHECK(pthread_join(first_thread, NULL) == 0 && pthread_join(second_thread, NULL) == 0);
        HL_CHECK(first.result.status == HL_STATUS_OK && second.result.status == HL_STATUS_OK &&
                 first.result.detail == HL_HOST_PROCESS_EXIT_SIGNAL && second.result.detail == first.result.detail &&
                 first.result.value == SIGKILL && second.result.value == first.result.value);
        HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    {
        hl_host_result mapping =
            services.memory->reserve(services.context, 16384, 16384, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
        HL_CHECK(mapping.status == HL_STATUS_OK && mapping.value != HL_HOST_HANDLE_INVALID);
        HL_CHECK(services.memory->protect(services.context, mapping.value, 0, 16384, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->protect(services.context, mapping.value, 16384, 1, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_INVALID_ARGUMENT);
    }
    snprintf(path, sizeof(path), "/tmp/hl_host_macos_%ld", (long)getpid());
    file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_WRITE | HL_HOST_FILE_APPEND,
                                        HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
    HL_CHECK(file.status == HL_STATUS_OK);
    HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){"a", 1}).value == 1);
    {
        const hl_host_iovec positioned[] = {{(uint64_t)(uintptr_t)"x", 1}};
        const hl_host_iovec appended[] = {{(uint64_t)(uintptr_t)"b", 1}, {(uint64_t)(uintptr_t)"c", 1}};
        HL_CHECK(services.file->writev_at(services.context, file.value, positioned, 1, 0).value == 1);
        HL_CHECK(services.file->appendv(services.context, file.value, appended, 2).value == 2);
    }
    HL_CHECK(
        services.file->read_at(services.context, file.value, 0, (hl_host_bytes){contents, sizeof(contents)}).value ==
        sizeof(contents));
    HL_CHECK(memcmp(contents, "xbc", sizeof(contents)) == 0);
    {
        hl_host_file_metadata metadata;
        hl_host_result clone;
        char first = 0;
        char second = 0;
        char vector_contents[3] = {0};
        hl_host_iovec vectors[] = {{(uint64_t)(uintptr_t)&vector_contents[0], 1},
                                   {(uint64_t)(uintptr_t)&vector_contents[1], 2}};
        HL_CHECK(services.file->metadata(services.context, file.value, &metadata).status == HL_STATUS_OK);
        HL_CHECK(metadata.type == HL_HOST_FILE_TYPE_REGULAR && metadata.size == 3 &&
                 (metadata.permissions & 0600u) == 0600u);
        HL_CHECK(services.file->seek(services.context, file.value, 0, SEEK_SET).value == 0);
        clone = services.file->clone_for_fork(services.context, file.value);
        HL_CHECK(clone.status == HL_STATUS_OK && clone.value != file.value);
        HL_CHECK(services.file->read(services.context, clone.value, &first, 1).value == 1 && first == 'x');
        HL_CHECK(services.file->read(services.context, file.value, &second, 1).value == 1 && second == 'b');
        HL_CHECK(services.file->seek(services.context, clone.value, 0, SEEK_SET).value == 0);
        HL_CHECK(services.file->readv(services.context, clone.value, vectors, 2).value == 3);
        HL_CHECK(memcmp(vector_contents, "xbc", 3) == 0);
        HL_CHECK(services.file->sync(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->data_sync(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, clone.value, 2).status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, clone.value, 3).status == HL_STATUS_OK);
        HL_CHECK(services.file->write_at(services.context, clone.value, 2, (hl_host_const_bytes){"c", 1}).value == 1);
        HL_CHECK(services.file->close(services.context, clone.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    {
        struct stat status;
        errno = 0;
        HL_CHECK(hl_host_file_exclusive(&services, path, 0600) == -1 && errno == EIO);
        HL_CHECK(stat(path, &status) == 0 && status.st_size == 3);
        HL_CHECK(hl_host_file_reset(&services, path, 0600) == 0);
        HL_CHECK(stat(path, &status) == 0 && status.st_size == 0);
    }
    unlink(path);
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, HL_HOST_CODE_DUAL_ALIAS, &code).status ==
             HL_STATUS_OK);
    memcpy((void *)(uintptr_t)code.writable_address, "code", 5);
    HL_CHECK(services.memory->publish_code(services.context, code.handle, 0, 5).status == HL_STATUS_OK);
    HL_CHECK(memcmp((const void *)(uintptr_t)code.executable_address, "code", 5) == 0);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        if (services.memory->repair_code_after_fork(services.context, &code, 1).status != HL_STATUS_OK) _exit(10);
        memcpy((void *)(uintptr_t)code.writable_address, "fork", 5);
        if (services.memory->publish_code(services.context, code.handle, 0, 5).status != HL_STATUS_OK) _exit(11);
        _exit(memcmp((const void *)(uintptr_t)code.executable_address, "fork", 5) == 0 ? 0 : 12);
    }
    int status = 0;
    HL_CHECK(waitpid(child, &status, 0) == child);
    HL_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    HL_CHECK(services.memory->release(services.context, code.handle).status == HL_STATUS_OK);
    {
        process_wait_context cleanup_waiter = {&services, 0, {0}};
        pthread_t cleanup_thread;
        struct timespec settle = {0, 10000000};
        process = services.process->spawn_cloned(services.context, child_pause, NULL);
        HL_CHECK(process.status == HL_STATUS_OK);
        cleanup_waiter.process = process.value;
        HL_CHECK(pthread_create(&cleanup_thread, NULL, wait_for_process, &cleanup_waiter) == 0);
        nanosleep(&settle, NULL);
        hl_host_macos_destroy(host);
        HL_CHECK(pthread_join(cleanup_thread, NULL) == 0);
        HL_CHECK(cleanup_waiter.result.status == HL_STATUS_OK &&
                 cleanup_waiter.result.detail == HL_HOST_PROCESS_EXIT_SIGNAL && cleanup_waiter.result.value == SIGKILL);
    }
    return EXIT_SUCCESS;
}
