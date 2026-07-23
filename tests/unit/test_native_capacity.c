#define _GNU_SOURCE
#include "test.h"

#include "../../src/core/target/native.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int32_t capacity_child(void *context) {
    return (int32_t)(uintptr_t)context;
}

enum {
    WATCH_COUNT = 257,
    EVENT_COUNT = 129,
    DIRECTORY_COUNT = 129,
    COUNTER_COUNT = 129,
    SUBSCRIPTION_COUNT = 129,
    TRANSFER_PAIR_COUNT = 33,
    TIMER_COUNT = 257,
    PROCESS_COUNT = 1025,
    DIRECTORY_WATCH_COUNT = 257,
    MAPPING_COUNT = 4097,
    FILE_COUNT = 1025
};

static void capacity_notify(void *observer, uint64_t token) {
    (void)observer;
    (void)token;
}

int main(void) {
    hl_native_host *host = NULL;
    hl_host_services services = {0};
    hl_host_handle watches[WATCH_COUNT] = {0};
    hl_host_handle events[EVENT_COUNT] = {0};
    hl_host_handle directories[DIRECTORY_COUNT] = {0};
    hl_host_handle counters[COUNTER_COUNT] = {0};
    hl_host_handle subscriptions[SUBSCRIPTION_COUNT] = {0};
    hl_host_handle transfers[TRANSFER_PAIR_COUNT * 2] = {0};
    hl_host_handle *mappings = NULL;
    hl_host_handle *files = NULL;
    hl_host_handle *processes = NULL;
    hl_host_result cross_mapping;
    hl_host_handle grown_directory_copy = HL_HOST_HANDLE_INVALID;
    char path[] = "/tmp/hl-native-capacity-XXXXXX";
    int native = mkstemp(path);
    HL_CHECK(native >= 0);
    HL_CHECK(close(native) == 0);
    HL_CHECK(hl_native_host_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_FILE | HL_HOST_CAP_WATCH | HL_HOST_CAP_EVENT |
                                                      HL_HOST_CAP_DIRECTORY | HL_HOST_CAP_COUNTER |
                                                      HL_HOST_CAP_TRANSFER) == HL_STATUS_OK);

    hl_host_result file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                                       HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, 0, 0);
    HL_CHECK(file.status == HL_STATUS_OK);
    cross_mapping = services.memory->reserve(services.context, 4096, 4096, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
    HL_CHECK(cross_mapping.status == HL_STATUS_OK);
    for (size_t index = 0; index < WATCH_COUNT; ++index) {
        hl_host_result opened = services.watch->open(services.context, file.value);
        HL_CHECK(opened.status == HL_STATUS_OK);
        watches[index] = opened.value;
        hl_host_watch_record record = {0};
        HL_CHECK(services.watch->query(services.context, watches[index], &record).status == HL_STATUS_OK);
    }
    for (size_t index = 0; index < EVENT_COUNT; ++index) {
        hl_host_result created = services.event->create(services.context);
        HL_CHECK(created.status == HL_STATUS_OK);
        events[index] = created.value;
    }
    for (size_t index = 0; index < DIRECTORY_COUNT; ++index) {
        hl_host_result created = services.directory->create(services.context);
        HL_CHECK(created.status == HL_STATUS_OK);
        directories[index] = created.value;
    }
    for (size_t index = 0; index < DIRECTORY_WATCH_COUNT; ++index)
        HL_CHECK(
            services.directory->add(services.context, directories[0], file.value, index + 1, HL_HOST_DIRECTORY_MODIFY)
                .status == HL_STATUS_OK);
    {
        hl_host_result duplicated = services.directory->duplicate(services.context, directories[0]);
        HL_CHECK(duplicated.status == HL_STATUS_OK);
        grown_directory_copy = duplicated.value;
        HL_CHECK(services.directory->remove(services.context, grown_directory_copy, DIRECTORY_WATCH_COUNT).status ==
                 HL_STATUS_OK);
        HL_CHECK(
            services.directory
                ->add(services.context, directories[0], file.value, DIRECTORY_WATCH_COUNT, HL_HOST_DIRECTORY_MODIFY)
                .status == HL_STATUS_OK);
        HL_CHECK(services.directory->remove(services.context, directories[0], DIRECTORY_WATCH_COUNT).status ==
                 HL_STATUS_OK);
    }
    for (size_t index = 0; index < COUNTER_COUNT; ++index) {
        hl_host_result created = services.counter->create(services.context, index, 0);
        HL_CHECK(created.status == HL_STATUS_OK);
        counters[index] = created.value;
    }
    for (size_t index = 0; index < SUBSCRIPTION_COUNT; ++index) {
        hl_host_result subscribed =
            services.counter->subscribe(services.context, counters[0], capacity_notify, NULL, index + 1u);
        HL_CHECK(subscribed.status == HL_STATUS_OK);
        subscriptions[index] = subscribed.value;
    }
    HL_CHECK(services.counter->unsubscribe(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    for (size_t index = 0; index < TRANSFER_PAIR_COUNT; ++index) {
        hl_host_result pair = services.transfer->channel_pair(services.context);
        HL_CHECK(pair.status == HL_STATUS_OK);
        transfers[index * 2] = pair.value;
        transfers[index * 2 + 1] = pair.detail;
    }
    uint64_t timer_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(60000000000);
    for (size_t index = 0; index < TIMER_COUNT; ++index)
        HL_CHECK(services.event->arm_timer(services.context, events[0], index + 1, timer_deadline, 0).status ==
                 HL_STATUS_OK);
    HL_CHECK(services.watch->query(services.context, events[0], &(hl_host_watch_record){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.event->wake(services.context, watches[0]).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->protect(services.context, file.value, 0, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->metadata(services.context, events[0], &(hl_host_file_metadata){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.counter->get_flags(services.context, directories[0]).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.directory->close(services.context, counters[0]).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->close(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    if (services.network != NULL)
        HL_CHECK(services.network->close(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    if (services.shared_memory != NULL)
        HL_CHECK(services.shared_memory->close(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.counter->close(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.transfer->close(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    {
        mappings = calloc(MAPPING_COUNT, sizeof(*mappings));
        files = calloc(FILE_COUNT, sizeof(*files));
        HL_CHECK(mappings != NULL && files != NULL);
        for (size_t index = 0; index < MAPPING_COUNT; ++index) {
            hl_host_result reserved =
                services.memory->reserve(services.context, 4096, 4096, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
            HL_CHECK(reserved.status == HL_STATUS_OK);
            mappings[index] = reserved.value;
        }
        for (size_t index = 0; index < FILE_COUNT; ++index) {
            hl_host_result opened = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path,
                                                                 strlen(path), HL_HOST_FILE_READ, 0, 0);
            HL_CHECK(opened.status == HL_STATUS_OK);
            files[index] = opened.value;
        }
        hl_host_watch_record retained = {0};
        HL_CHECK(services.watch->query(services.context, watches[0], &retained).status == HL_STATUS_OK);
        pid_t child = fork();
        HL_CHECK(child >= 0);
        if (child == 0) {
            hl_host_watch_record inherited = {0};
            int valid =
                services.watch->query(services.context, watches[WATCH_COUNT - 1], &inherited).status == HL_STATUS_OK &&
                services.counter->get_flags(services.context, counters[COUNTER_COUNT - 1]).status == HL_STATUS_OK &&
                services.memory->protect(services.context, mappings[MAPPING_COUNT - 1], 0, 4096, HL_HOST_MEMORY_READ)
                        .status == HL_STATUS_OK;
            _exit(valid ? 0 : 41);
        }
        int status = 0;
        HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    for (size_t index = FILE_COUNT; files != NULL && index != 0; --index)
        HL_CHECK(services.file->close(services.context, files[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = files[0];
        hl_host_result replacement = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path,
                                                                  strlen(path), HL_HOST_FILE_READ, 0, 0);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.file->metadata(services.context, stale, &(hl_host_file_metadata){0}).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.file->metadata(services.context, replacement.value, &(hl_host_file_metadata){0}).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, replacement.value).status == HL_STATUS_OK);
    }
    for (size_t index = MAPPING_COUNT; mappings != NULL && index != 0; --index)
        HL_CHECK(services.memory->release(services.context, mappings[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = mappings[0];
        hl_host_result replacement =
            services.memory->reserve(services.context, 4096, 4096, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.memory->protect(services.context, stale, 0, 4096, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->protect(services.context, replacement.value, 0, 4096, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, replacement.value).status == HL_STATUS_OK);
    }
    for (size_t index = TIMER_COUNT; index != 0; --index)
        HL_CHECK(services.event->disarm_timer(services.context, events[0], index).status == HL_STATUS_OK);
    HL_CHECK(services.event->arm_timer(services.context, events[0], 1, timer_deadline, 0).status == HL_STATUS_OK);
    HL_CHECK(services.event->disarm_timer(services.context, events[0], 1).status == HL_STATUS_OK);
    for (size_t index = TRANSFER_PAIR_COUNT * 2; index != 0; --index)
        HL_CHECK(services.transfer->close(services.context, transfers[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = transfers[0];
        hl_host_result replacement = services.transfer->channel_pair(services.context);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale && replacement.detail != stale);
        HL_CHECK(services.transfer->close(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.transfer->close(services.context, replacement.value).status == HL_STATUS_OK);
        HL_CHECK(services.transfer->close(services.context, replacement.detail).status == HL_STATUS_OK);
    }
    for (size_t index = SUBSCRIPTION_COUNT; index != 0; --index)
        HL_CHECK(services.counter->unsubscribe(services.context, subscriptions[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = subscriptions[0];
        hl_host_result replacement =
            services.counter->subscribe(services.context, counters[0], capacity_notify, NULL, UINT64_C(9001));
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.counter->unsubscribe(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.counter->unsubscribe(services.context, replacement.value).status == HL_STATUS_OK);
    }
    for (size_t index = COUNTER_COUNT; index != 0; --index)
        HL_CHECK(services.counter->close(services.context, counters[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = counters[0];
        hl_host_result replacement = services.counter->create(services.context, 0, 0);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.counter->get_flags(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.counter->get_flags(services.context, replacement.value).status == HL_STATUS_OK);
        HL_CHECK(services.counter->close(services.context, replacement.value).status == HL_STATUS_OK);
    }
    for (size_t index = DIRECTORY_COUNT; index != 0; --index)
        HL_CHECK(services.directory->close(services.context, directories[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = directories[0];
        hl_host_result replacement = services.directory->create(services.context);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.directory->close(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.directory->close(services.context, replacement.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.directory->close(services.context, grown_directory_copy).status == HL_STATUS_OK);
    HL_CHECK(services.directory->close(services.context, grown_directory_copy).status == HL_STATUS_INVALID_ARGUMENT);
    if (strcmp(HL_NATIVE_HOST_NAME, "macos") == 0) {
        processes = calloc(PROCESS_COUNT, sizeof(*processes));
        HL_CHECK(processes != NULL);
        for (size_t index = 0; index < PROCESS_COUNT; ++index) {
            hl_host_result spawned =
                services.process->spawn_cloned(services.context, capacity_child, (void *)(index & 7u));
            HL_CHECK(spawned.status == HL_STATUS_OK);
            processes[index] = spawned.value;
            hl_host_result waited = services.process->wait(services.context, spawned.value, HL_HOST_DEADLINE_INFINITE);
            HL_CHECK(waited.status == HL_STATUS_OK && waited.value == (index & 7u));
        }
        HL_CHECK(services.process->close(services.context, file.value).status == HL_STATUS_INVALID_ARGUMENT);
        for (size_t index = PROCESS_COUNT; index != 0; --index)
            HL_CHECK(services.process->close(services.context, processes[index - 1]).status == HL_STATUS_OK);
        {
            hl_host_handle stale = processes[0];
            hl_host_result replacement = services.process->spawn_cloned(services.context, capacity_child, NULL);
            HL_CHECK(replacement.status == HL_STATUS_OK);
            HL_CHECK(replacement.value != stale);
            HL_CHECK(services.process->close(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
            HL_CHECK(services.process->wait(services.context, replacement.value, HL_HOST_DEADLINE_INFINITE).status ==
                     HL_STATUS_OK);
            HL_CHECK(services.process->close(services.context, replacement.value).status == HL_STATUS_OK);
        }
    }
    for (size_t index = EVENT_COUNT; index != 0; --index)
        HL_CHECK(services.event->close(services.context, events[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = events[0];
        hl_host_result replacement = services.event->create(services.context);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.event->wake(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.event->wake(services.context, replacement.value).status == HL_STATUS_OK);
        HL_CHECK(services.event->close(services.context, replacement.value).status == HL_STATUS_OK);
    }
    for (size_t index = WATCH_COUNT; index != 0; --index)
        HL_CHECK(services.watch->close(services.context, watches[index - 1]).status == HL_STATUS_OK);
    {
        hl_host_handle stale = watches[0];
        hl_host_result replacement = services.watch->open(services.context, file.value);
        HL_CHECK(replacement.status == HL_STATUS_OK);
        HL_CHECK(replacement.value != stale);
        HL_CHECK(services.watch->query(services.context, stale, &(hl_host_watch_record){0}).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.watch->query(services.context, replacement.value, &(hl_host_watch_record){0}).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.watch->close(services.context, replacement.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    HL_CHECK(services.file->metadata(services.context, file.value, &(hl_host_file_metadata){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->release(services.context, cross_mapping.value).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, cross_mapping.value).status == HL_STATUS_INVALID_ARGUMENT);
    free(files);
    free(mappings);
    free(processes);
    hl_native_host_destroy(host);
    HL_CHECK(unlink(path) == 0);
    return 0;
}
