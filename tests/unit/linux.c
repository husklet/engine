#include "test.h"

#include "hl/linux.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
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
    hl_host_result shared;
    hl_host_code_mapping code;
    hl_host_event_record event;
    hl_host_file_metadata metadata;
    hl_host_result process;
    hl_host_result process_result;
    const char contents[] = "portable-host";
    const char suffix[] = "append";
    char readback[sizeof(contents) + sizeof(suffix)] = {0};
    char path[128];

    HL_CHECK(hl_host_linux_create(&linux_host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_FILE |
                                                      HL_HOST_CAP_EVENT | HL_HOST_CAP_NETWORK |
                                                      HL_HOST_CAP_SHARED_MEMORY | HL_HOST_CAP_PROCESS |
                                                      HL_HOST_CAP_CODE_MAPPING) == HL_STATUS_OK);
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.clock->realtime_ns(services.context).status == HL_STATUS_OK);

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
    HL_CHECK(services.memory->release(services.context, code.handle).status == HL_STATUS_OK);

    snprintf(path, sizeof(path), "/tmp/hl_host_linux_%ld", (long)getpid());
    file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_WRITE | HL_HOST_FILE_APPEND,
                                        HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
    HL_CHECK(file.status == HL_STATUS_OK);
    HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){contents, sizeof(contents)})
                 .value == sizeof(contents));
    HL_CHECK(services.file->append(services.context, file.value, (hl_host_const_bytes){suffix, sizeof(suffix)}).value ==
             sizeof(suffix));
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
    unlink(path);

    pollset = services.event->create(services.context);
    HL_CHECK(pollset.status == HL_STATUS_OK);
    HL_CHECK(services.event->wake(services.context, pollset.value).status == HL_STATUS_OK);
    HL_CHECK(services.event->wait(services.context, pollset.value, &event, 1, 100000000).value == 0);
    HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);

    shared = services.shared_memory->create(services.context, 4096, 0);
    HL_CHECK(shared.status == HL_STATUS_OK);
    HL_CHECK(services.shared_memory->resize(services.context, shared.value, 8192).status == HL_STATUS_OK);
    HL_CHECK(services.shared_memory->close(services.context, shared.value).status == HL_STATUS_OK);

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
