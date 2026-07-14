#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "hl/macos.h"
#include "counter.h"
#include "transfer.h"
#include "../../src/host/clock.h"
#include "../../src/host/file.h"
#include "../../src/host/system.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON 0x1000
#endif

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

typedef struct stream_read_context {
    const hl_host_services *services;
    hl_host_handle handle;
    unsigned char bytes[256];
    hl_host_result result;
} stream_read_context;

static void *stream_read_thread(void *opaque) {
    stream_read_context *reader = opaque;
    reader->result = reader->services->file->read(reader->services->context, reader->handle, reader->bytes,
                                                   sizeof(reader->bytes));
    return NULL;
}

typedef struct stream_writer_context {
    const hl_host_services *services;
    hl_host_handle handle;
    unsigned char byte;
    uint32_t records;
    uint32_t failed;
} stream_writer_context;

static void *stream_writer_thread(void *opaque) {
    stream_writer_context *writer = opaque;
    unsigned char record[64];
    memset(record, writer->byte, sizeof record);
    for (uint32_t index = 0; index < writer->records; ++index) {
        hl_host_result result = writer->services->stream->write(
            writer->services->context, writer->handle, (hl_host_const_bytes){record, sizeof record});
        if (result.status != HL_STATUS_OK || result.value != sizeof record) {
            writer->failed = 1;
            break;
        }
    }
    return NULL;
}

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

static size_t private_descriptor_count(void) {
    hl_host_process_fd entries[1024];
    size_t count = 0;
    size_t private_count = 0;
    HL_CHECK(hl_host_process_fds(getpid(), entries, 1024, &count));
    HL_CHECK(count <= 1024);
    for (size_t index = 0; index < count; ++index)
        if ((entries[index].flags & HL_HOST_PROCESS_FD_ENGINE_PRIVATE) != 0) private_count++;
    return private_count;
}

int main(void) {
    hl_host_macos *host;
    hl_host_services services;
    hl_host_code_mapping code;
    hl_host_result process;
    hl_host_result process_exit;
    hl_host_result file;
    char path[128];
    char moved_path[160];
    char contents[3] = {0};
    HL_CHECK(hl_host_macos_create(&host, &services) == HL_STATUS_OK);
    {
        char range_path[128];
        hl_host_filesystem_metadata filesystem;
        hl_host_file_metadata range_metadata;
        hl_host_result range_file;
        snprintf(range_path, sizeof(range_path), "/tmp/hl_file_abi14_macos_%ld", (long)getpid());
        range_file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, range_path,
                                                  strlen(range_path), HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                                  HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
        HL_CHECK(range_file.status == HL_STATUS_OK && services.file->abi == HL_HOST_FILE_ABI);
        HL_CHECK(services.file->allocate_range(services.context, range_file.value, 0, 0, 8192).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, range_file.value, &range_metadata).status == HL_STATUS_OK &&
                 range_metadata.size == 8192);
        HL_CHECK(services.file->filesystem_metadata(services.context, range_file.value, &filesystem).status ==
                     HL_STATUS_OK && filesystem.block_size > 0 && filesystem.blocks > 0 &&
                 filesystem.blocks_free <= filesystem.blocks);
        HL_CHECK(services.file->close(services.context, range_file.value).status == HL_STATUS_OK &&
                 unlink(range_path) == 0);
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
        snprintf(probe_path, sizeof(probe_path), "/tmp/hl_stdio_macos_%ld", (long)getpid());
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
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_PROCESS |
                                                      HL_HOST_CAP_EVENT_TIMER | HL_HOST_CAP_SHARED_MEMORY |
                                                      HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_SYNC) == HL_STATUS_OK);
    {
        size_t private_before = private_descriptor_count();
        hl_host_result pollset = services.event->create(services.context);
        HL_CHECK(pollset.status == HL_STATUS_OK);
        HL_CHECK(private_descriptor_count() == private_before + 1);
        hl_host_event_record event;
        uint64_t deadline;
        HL_CHECK(pollset.status == HL_STATUS_OK);
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
        HL_CHECK(services.event->wake(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(services.event
                     ->wait(services.context, pollset.value, &event, 1,
                            services.clock->monotonic_ns(services.context).value + UINT64_C(100000000))
                     .value == 0);
        HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(private_descriptor_count() == private_before);
    }
    {
        static const char payload[] = "shared-memory";
        char readback[sizeof(payload)] = {0};
        hl_host_file_metadata metadata;
        hl_host_result shared = services.shared_memory->create(services.context, 4096, 0);
        hl_host_result copy;
        HL_CHECK(shared.status == HL_STATUS_OK && shared.value != HL_HOST_HANDLE_INVALID &&
                 shared.detail == shared.value);
        HL_CHECK(services.shared_memory->create(services.context, 4096, 1).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.shared_memory->open(services.context, shared.detail, 1).status == HL_STATUS_INVALID_ARGUMENT);
        copy = services.shared_memory->open(services.context, shared.detail, 0);
        HL_CHECK(copy.status == HL_STATUS_OK && copy.value != shared.value && copy.detail == shared.detail);
        HL_CHECK(
            services.file->write_at(services.context, shared.value, 17, (hl_host_const_bytes){payload, sizeof(payload)})
                .value == sizeof(payload));
        HL_CHECK(services.file->read_at(services.context, copy.value, 17, (hl_host_bytes){readback, sizeof(readback)})
                     .value == sizeof(readback));
        HL_CHECK(memcmp(readback, payload, sizeof(payload)) == 0);
        HL_CHECK(services.shared_memory->resize(services.context, copy.value, 8192).status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, shared.value, &metadata).status == HL_STATUS_OK &&
                 metadata.type == HL_HOST_FILE_TYPE_REGULAR && metadata.size == 8192);
        HL_CHECK(services.shared_memory->close(services.context, shared.value).status == HL_STATUS_OK);
        HL_CHECK(services.shared_memory->open(services.context, shared.detail, 0).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.shared_memory->resize(services.context, copy.value, 12288).status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, copy.value, &metadata).status == HL_STATUS_OK &&
                 metadata.size == 12288);
        HL_CHECK(services.shared_memory->close(services.context, copy.value).status == HL_STATUS_OK);
        HL_CHECK(services.shared_memory->resize(services.context, copy.value, 4096).status ==
                 HL_STATUS_INVALID_ARGUMENT);
    }
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
        static const char message[] = {'h', 'o', 's', 't', '\0', 'l', 'o', 'g'};
        char received[sizeof(message)] = {0};
        int descriptors[2];
        int saved_stderr;
        ssize_t count;
        HL_CHECK(pipe(descriptors) == 0);
        saved_stderr = dup(STDERR_FILENO);
        HL_CHECK(saved_stderr >= 0);
        HL_CHECK(dup2(descriptors[1], STDERR_FILENO) == STDERR_FILENO);
        HL_CHECK(close(descriptors[1]) == 0);
        services.log->emit(services.context, 0x8badf00du, message, sizeof(message));
        HL_CHECK(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
        HL_CHECK(close(saved_stderr) == 0);
        count = read(descriptors[0], received, sizeof(received));
        HL_CHECK(count == (ssize_t)sizeof(received));
        HL_CHECK(memcmp(received, message, sizeof(message)) == 0);
        HL_CHECK(close(descriptors[0]) == 0);
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

    HL_CHECK(services.sync->fork_prepare(services.context).status == HL_STATUS_OK);
    process = services.process->spawn_prepared(services.context, child_exit_37, (void *)(uintptr_t)37);
    HL_CHECK(process.status == HL_STATUS_OK && process.value != HL_HOST_HANDLE_INVALID);
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);

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
    {
        hl_host_file_mapping mapped = {HL_HOST_FILE_MAPPING_ABI, sizeof(mapped), 0, 0, 0, 0};
        unsigned char payload[8192];
        unsigned char *reservation;
        memset(payload, 0x5a, sizeof payload);
        HL_CHECK(services.file->write_at(services.context, file.value, 0,
                                         (hl_host_const_bytes){payload, sizeof payload}).value == sizeof payload);
        reservation = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        HL_CHECK(reservation != MAP_FAILED);
        memset(reservation, 0xa5, 4096);
        HL_CHECK(services.memory
                     ->map_file(services.context, file.value, (uint64_t)(uintptr_t)(reservation + 4096), 4096, 4096,
                                HL_HOST_MEMORY_READ, HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED, &mapped)
                     .status == HL_STATUS_OK);
        for (size_t index = 0; index < 4096; ++index)
            HL_CHECK(reservation[index] == 0xa5 && reservation[4096 + index] == 0x5a);
        HL_CHECK(mapped.address == (uint64_t)(uintptr_t)(reservation + 4096) && mapped.reserved == 4096);
        HL_CHECK(services.memory->release(services.context, mapped.handle).status == HL_STATUS_OK);
        HL_CHECK(munmap(reservation + 16384, 16384) == 0);
        reservation = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        HL_CHECK(reservation != MAP_FAILED);
        mapped = (hl_host_file_mapping){HL_HOST_FILE_MAPPING_ABI, sizeof(mapped), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_file(services.context, file.value, (uint64_t)(uintptr_t)(reservation + 4096), 4096, 4096,
                                HL_HOST_MEMORY_READ, HL_HOST_MEMORY_SHARED | HL_HOST_MEMORY_FIXED, &mapped)
                     .status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(munmap(reservation, 32768) == 0);
        HL_CHECK(services.file->truncate(services.context, file.value, 0).status == HL_STATUS_OK);
    }
    HL_CHECK(services.file
                 ->sync_range(services.context, file.value, 0, 0,
                              HL_HOST_FILE_SYNC_WAIT_BEFORE | HL_HOST_FILE_SYNC_WRITE | HL_HOST_FILE_SYNC_WAIT_AFTER)
                 .status == HL_STATUS_OK);
    HL_CHECK(services.file->sync_range(services.context, file.value, 0, 0, 8).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->sync_filesystem(services.context, file.value).status == HL_STATUS_OK);
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
        HL_CHECK(metadata.link_count >= 1);
        HL_CHECK(metadata.user == (uint32_t)getuid());
        HL_CHECK(metadata.group == (uint32_t)getgid());
        HL_CHECK(metadata.modified_ns != 0 && metadata.accessed_ns != 0 && metadata.changed_ns != 0 &&
                 metadata.created_ns != 0);
        HL_CHECK(services.file->seek(services.context, file.value, 0, SEEK_SET).value == 0);
        clone = services.file->clone_for_fork(services.context, file.value);
        HL_CHECK(clone.status == HL_STATUS_OK && clone.value != file.value);
        HL_CHECK(services.file->read(services.context, clone.value, &first, 1).value == 1 && first == 'x');
        HL_CHECK(services.file->read(services.context, file.value, &second, 1).value == 1 && second == 'b');
        HL_CHECK(services.file->seek(services.context, clone.value, 0, SEEK_SET).value == 0);
        HL_CHECK(services.file->readv(services.context, clone.value, vectors, 2).value == 3);
        HL_CHECK(memcmp(vector_contents, "xbc", 3) == 0);
        HL_CHECK(services.file->append(services.context, file.value, (hl_host_const_bytes){"d", 1}).value == 1);
        {
            hl_host_result sequential = services.file->open_relative(
                services.context, HL_HOST_HANDLE_CWD, path, strlen(path), HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, 0, 0);
            char positioned_contents[4] = {0};
            const hl_host_iovec written[] = {{(uint64_t)(uintptr_t)"z", 1}, {(uint64_t)(uintptr_t)"w", 1}};
            hl_host_iovec positioned[] = {{(uint64_t)(uintptr_t)&positioned_contents[0], 2},
                                          {(uint64_t)(uintptr_t)&positioned_contents[2], 2}};
            HL_CHECK(sequential.status == HL_STATUS_OK);
            HL_CHECK(services.file->seek(services.context, sequential.value, 0, SEEK_SET).value == 0);
            HL_CHECK(services.file->write(services.context, sequential.value, "y", 1).value == 1);
            HL_CHECK(services.file->writev(services.context, sequential.value, written, 2).value == 2);
            HL_CHECK(services.file->readv_at(services.context, sequential.value, positioned, 2, 0).value == 4);
            HL_CHECK(memcmp(positioned_contents, "yzwd", 4) == 0);
            HL_CHECK(
                services.file->write_at(services.context, sequential.value, 0, (hl_host_const_bytes){"xbc", 3}).value ==
                3);
            HL_CHECK(services.file->truncate(services.context, sequential.value, 3).status == HL_STATUS_OK);
            HL_CHECK(services.file->close(services.context, sequential.value).status == HL_STATUS_OK);
        }
        HL_CHECK(services.file->sync(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->data_sync(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, clone.value, 2).status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, clone.value, 3).status == HL_STATUS_OK);
        HL_CHECK(services.file->write_at(services.context, clone.value, 2, (hl_host_const_bytes){"c", 1}).value == 1);
        {
            hl_host_result watch = services.watch->open(services.context, clone.value);
            hl_host_result watched_pollset = services.event->create(services.context);
            hl_host_watch_record current = {0}, changed = {0};
            hl_host_event_record notification = {0};
            HL_CHECK(watch.status == HL_STATUS_OK && watched_pollset.status == HL_STATUS_OK);
            HL_CHECK(services.watch->query(services.context, watch.value, &current).status == HL_STATUS_OK &&
                     current.size == 3);
            HL_CHECK(services.event
                         ->control(services.context, watched_pollset.value, HL_HOST_EVENT_ADD, watch.value, 313,
                                   HL_HOST_READY_READ)
                         .status == HL_STATUS_OK);
            HL_CHECK(services.file->truncate(services.context, clone.value, 9).status == HL_STATUS_OK);
            uint64_t watch_deadline =
                services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(services.event
                         ->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline)
                         .value == 1 &&
                     notification.token == 313 && (notification.readiness & HL_HOST_READY_READ) != 0);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     changed.generation > current.generation && changed.size == 9 &&
                     (changed.changes & HL_HOST_WATCH_SIZE) != 0);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).status ==
                     HL_STATUS_WOULD_BLOCK);
            HL_CHECK(services.file->write_at(services.context, clone.value, 0,
                                             (hl_host_const_bytes){"q", 1}).value == 1);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(services.event
                         ->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline)
                         .value == 1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     changed.size == 9 && (changed.changes & HL_HOST_WATCH_DATA) != 0);
            pid_t watch_child = fork();
            HL_CHECK(watch_child >= 0);
            if (watch_child == 0) {
                hl_host_watch_record inherited = {0};
                _exit(services.watch->query(services.context, watch.value, &inherited).status == HL_STATUS_OK &&
                              inherited.size == 9
                          ? 0
                          : 30);
            }
            int watch_status = 0;
            HL_CHECK(waitpid(watch_child, &watch_status, 0) == watch_child && WIFEXITED(watch_status) &&
                     WEXITSTATUS(watch_status) == 0);
            snprintf(moved_path, sizeof(moved_path), "%s.watch", path);
            (void)unlink(moved_path);
            HL_CHECK(rename(path, moved_path) == 0);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(services.event
                         ->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline)
                         .value == 1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     (changed.changes & HL_HOST_WATCH_IDENTITY) != 0);
            HL_CHECK(rename(moved_path, path) == 0);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(services.event
                         ->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline)
                         .value == 1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1);
            HL_CHECK(unlink(path) == 0);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(services.event
                         ->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline)
                         .value == 1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     (changed.changes & HL_HOST_WATCH_DELETED) != 0);
            int replacement = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
            HL_CHECK(replacement >= 0 && ftruncate(replacement, 3) == 0 && close(replacement) == 0);
            HL_CHECK(services.event->close(services.context, watched_pollset.value).status == HL_STATUS_OK);
            HL_CHECK(services.watch->close(services.context, watch.value).status == HL_STATUS_OK);
            HL_CHECK(services.file->truncate(services.context, clone.value, 3).status == HL_STATUS_OK);
        }
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
    snprintf(moved_path, sizeof(moved_path), "%s.moved", path);
    {
        hl_host_result renamed =
            services.file->rename_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path), HL_HOST_HANDLE_CWD,
                                           moved_path, strlen(moved_path));
        HL_CHECK(renamed.status == HL_STATUS_OK);
    }
    HL_CHECK(hl_host_file_store(&services, path, 0600, "replacement", 11) == 0);
    HL_CHECK(services.file
                 ->rename_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                   HL_HOST_HANDLE_CWD, path, strlen(path))
                 .status == HL_STATUS_OK);
    {
        struct stat status;
        HL_CHECK(stat(path, &status) == 0 && status.st_size == 0);
    }
    HL_CHECK(services.file->unlink_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path)).status ==
             HL_STATUS_OK);
    {
        hl_host_services standalone = services;
        standalone.capabilities &= ~(uint64_t)HL_HOST_CAP_FILE;
        standalone.file = NULL;
        HL_CHECK(hl_host_services_validate(&standalone, HL_HOST_CAP_STREAM) == HL_STATUS_OK);
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        char bytes[16] = {0};
        HL_CHECK(pipe.status == HL_STATUS_OK && pipe.value != 0 && pipe.detail != 0);
        HL_CHECK((standalone.stream->readiness(standalone.context, pipe.value, HL_HOST_READY_READ).value &
                  HL_HOST_READY_READ) == 0);
        HL_CHECK((standalone.stream->readiness(standalone.context, pipe.detail, HL_HOST_READY_WRITE).value &
                  HL_HOST_READY_WRITE) != 0);
        HL_CHECK(standalone.stream->write(standalone.context, pipe.detail, (hl_host_const_bytes){"stream", 6}).value ==
                 6);
        {
            hl_host_result pollset = services.event->create(services.context);
            hl_host_event_record event_record = {0};
            HL_CHECK(pollset.status == HL_STATUS_OK);
            HL_CHECK(services.event
                         ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, pipe.value, 73,
                                   HL_HOST_READY_READ)
                         .status == HL_STATUS_OK);
            HL_CHECK(services.event
                         ->wait(services.context, pollset.value, &event_record, 1, HL_HOST_DEADLINE_INFINITE)
                         .value == 1);
            HL_CHECK(event_record.token == 73 && (event_record.readiness & HL_HOST_READY_READ) != 0);
            HL_CHECK(services.event
                         ->control(services.context, pollset.value, HL_HOST_EVENT_DELETE, pipe.value, 73,
                                   HL_HOST_READY_READ)
                         .status == HL_STATUS_OK);
            HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
        }
        HL_CHECK((standalone.stream->readiness(standalone.context, pipe.value, HL_HOST_READY_READ).value &
                  HL_HOST_READY_READ) != 0);
        HL_CHECK(standalone.stream->read(standalone.context, pipe.value, (hl_host_bytes){bytes, sizeof bytes}).value ==
                 6);
        HL_CHECK(memcmp(bytes, "stream", 6) == 0);
        HL_CHECK(services.stream->set_status_flags(services.context, pipe.value, HL_HOST_STREAM_NONBLOCK).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->read(services.context, pipe.value, bytes, sizeof bytes).status ==
                 HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.stream->set_status_flags(services.context, pipe.value, 0).status == HL_STATUS_OK);
        {
            pid_t blocked = fork();
            int blocked_status = 0;
            struct timespec settle = {0, 10000000};
            HL_CHECK(blocked >= 0);
            if (blocked == 0) {
                char byte;
                (void)services.file->read(services.context, pipe.value, &byte, 1);
                _exit(20);
            }
            nanosleep(&settle, NULL);
            HL_CHECK(kill(blocked, SIGKILL) == 0);
            HL_CHECK(waitpid(blocked, &blocked_status, 0) == blocked && WIFSIGNALED(blocked_status));
            HL_CHECK(services.file->write(services.context, pipe.detail, "r", 1).value == 1);
            HL_CHECK(services.file->read(services.context, pipe.value, bytes, 1).value == 1 && bytes[0] == 'r');
        }
        HL_CHECK(services.file->close(services.context, pipe.value).status == HL_STATUS_OK);
        {
            hl_host_result broken = services.file->write(services.context, pipe.detail, "x", 1);
            HL_CHECK(broken.status == HL_STATUS_DISCONNECTED && broken.detail == EPIPE);
        }
        HL_CHECK(services.file->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        hl_host_result clone = services.file->clone_for_fork(services.context, pipe.value);
        char byte = 0;
        HL_CHECK(pipe.status == HL_STATUS_OK && clone.status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->write(services.context, pipe.detail, "d", 1).value == 1);
        HL_CHECK(services.file->read(services.context, clone.value, &byte, 1).value == 1 && byte == 'd');
        HL_CHECK(services.file->close(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        stream_read_context reader = {&services, pipe.value, {0}, {0}};
        pthread_t thread;
        struct timespec settle = {0, 10000000};
        HL_CHECK(pipe.status == HL_STATUS_OK && pthread_create(&thread, NULL, stream_read_thread, &reader) == 0);
        nanosleep(&settle, NULL);
        HL_CHECK(services.file->close(services.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->write(services.context, pipe.detail, "p", 1).value == 1);
        HL_CHECK(pthread_join(thread, NULL) == 0 && reader.result.value == 1 && reader.bytes[0] == 'p');
        HL_CHECK(services.file->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result source = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        hl_host_result destination = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        stream_read_context reader = {&services, source.value, {0}, {0}};
        pthread_t thread;
        hl_host_result moved;
        char delivered[256];
        hl_host_result received;
        unsigned char payload[100];
        memset(payload, 0x5a, sizeof payload);
        HL_CHECK(source.status == HL_STATUS_OK && destination.status == HL_STATUS_OK);
        HL_CHECK(services.file->write(services.context, source.detail, payload, sizeof payload).value == sizeof payload);
        HL_CHECK(pthread_create(&thread, NULL, stream_read_thread, &reader) == 0);
        moved = services.stream->move(services.context, source.value, 0, destination.detail, 0, sizeof payload, 0);
        HL_CHECK(pthread_join(thread, NULL) == 0);
        received = services.file->read(services.context, destination.value, delivered, sizeof delivered);
        HL_CHECK((reader.result.status == HL_STATUS_OK ? reader.result.value : 0) +
                     (received.status == HL_STATUS_OK ? received.value : 0) ==
                 sizeof payload);
        HL_CHECK(moved.status == HL_STATUS_OK || moved.status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.file->close(services.context, source.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, source.detail).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.detail).status == HL_STATUS_OK);
    }
    {
        enum { RECORDS = 100, RECORD_SIZE = 64, TOTAL = 2 * RECORDS * RECORD_SIZE };
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        stream_writer_context first = {&services, pipe.detail, 'A', RECORDS, 0};
        stream_writer_context second = {&services, pipe.detail, 'B', RECORDS, 0};
        pthread_t first_thread, second_thread;
        unsigned char received[TOTAL];
        size_t total = 0;
        HL_CHECK(pipe.status == HL_STATUS_OK);
        HL_CHECK(pthread_create(&first_thread, NULL, stream_writer_thread, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, stream_writer_thread, &second) == 0);
        while (total < sizeof received) {
            hl_host_result result = services.stream->read(
                services.context, pipe.value, (hl_host_bytes){received + total, sizeof received - total});
            HL_CHECK(result.status == HL_STATUS_OK && result.value != 0 && result.value <= sizeof received - total);
            total += (size_t)result.value;
        }
        HL_CHECK(pthread_join(first_thread, NULL) == 0 && pthread_join(second_thread, NULL) == 0);
        HL_CHECK(!first.failed && !second.failed);
        for (size_t offset = 0; offset < sizeof received; offset += RECORD_SIZE) {
            HL_CHECK(received[offset] == 'A' || received[offset] == 'B');
            for (size_t index = 1; index < RECORD_SIZE; ++index)
                HL_CHECK(received[offset + index] == received[offset]);
        }
        HL_CHECK(services.stream->close(services.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(services.stream->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result source = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        hl_host_result destination = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        unsigned char fill[4096];
        char retained[8] = {0};
        hl_host_result written;
        memset(fill, 0xa5, sizeof fill);
        do written = services.file->write(services.context, destination.detail, fill, sizeof fill);
        while (written.status == HL_STATUS_OK && written.value != 0);
        HL_CHECK(written.status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.file->write(services.context, source.detail, "retain", 6).value == 6);
        HL_CHECK(services.stream->move(services.context, source.value, 0, destination.detail, 0, 6, 0).status ==
                 HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.file->read(services.context, source.value, retained, sizeof retained).value == 6);
        HL_CHECK(memcmp(retained, "retain", 6) == 0);
        HL_CHECK(services.file->close(services.context, source.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, source.detail).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.detail).status == HL_STATUS_OK);
    }
    {
        char target[32] = {0};
        hl_host_file_metadata metadata;
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
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, HL_HOST_CODE_DUAL_ALIAS, &code).status ==
             HL_STATUS_OK);
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    memcpy((void *)(uintptr_t)code.writable_address, "code", 5);
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
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
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, 0, &code).status == HL_STATUS_OK);
    HL_CHECK(code.writable_address == code.executable_address);
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    memcpy((void *)(uintptr_t)code.writable_address, "single", 7);
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->publish_code(services.context, code.handle, 0, 7).status == HL_STATUS_OK);
    HL_CHECK(memcmp((const void *)(uintptr_t)code.executable_address, "single", 7) == 0);
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
