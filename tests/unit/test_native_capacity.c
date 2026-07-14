#define _GNU_SOURCE
#include "test.h"

#include "../../src/core/target/native.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    WATCH_COUNT = 257,
    EVENT_COUNT = 129,
    LINUX_MAPPING_COUNT = 4000,
    LINUX_FILE_COUNT = 64
};

int main(void) {
    hl_native_host *host = NULL;
    hl_host_services services = {0};
    hl_host_handle watches[WATCH_COUNT] = {0};
    hl_host_handle events[EVENT_COUNT] = {0};
    hl_host_handle *mappings = NULL;
    hl_host_handle *files = NULL;
    hl_host_result cross_mapping;
    char path[] = "/tmp/hl-native-capacity-XXXXXX";
    int native = mkstemp(path);
    HL_CHECK(native >= 0);
    HL_CHECK(close(native) == 0);
    HL_CHECK(hl_native_host_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_FILE | HL_HOST_CAP_WATCH | HL_HOST_CAP_EVENT) ==
             HL_STATUS_OK);

    hl_host_result file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                                       HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, 0, 0);
    HL_CHECK(file.status == HL_STATUS_OK);
    cross_mapping = services.memory->reserve(services.context, 4096, 4096,
                                             HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
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
    HL_CHECK(services.watch->query(services.context, events[0], &(hl_host_watch_record){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.event->wake(services.context, watches[0]).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->protect(services.context, file.value, 0, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->metadata(services.context, events[0], &(hl_host_file_metadata){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    if (strcmp(HL_NATIVE_HOST_NAME, "linux") == 0) {
        mappings = calloc(LINUX_MAPPING_COUNT, sizeof(*mappings));
        files = calloc(LINUX_FILE_COUNT, sizeof(*files));
        HL_CHECK(mappings != NULL && files != NULL);
        for (size_t index = 0; index < LINUX_MAPPING_COUNT; ++index) {
            hl_host_result reserved = services.memory->reserve(services.context, 4096, 4096,
                                                               HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
            HL_CHECK(reserved.status == HL_STATUS_OK);
            mappings[index] = reserved.value;
        }
        for (size_t index = 0; index < LINUX_FILE_COUNT; ++index) {
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
            int valid = services.watch->query(services.context, watches[WATCH_COUNT - 1], &inherited).status ==
                            HL_STATUS_OK &&
                        services.memory->protect(services.context, mappings[LINUX_MAPPING_COUNT - 1], 0, 4096,
                                                 HL_HOST_MEMORY_READ)
                                .status == HL_STATUS_OK;
            _exit(valid ? 0 : 41);
        }
        int status = 0;
        HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    for (size_t index = LINUX_FILE_COUNT; files != NULL && index != 0; --index)
        HL_CHECK(services.file->close(services.context, files[index - 1]).status == HL_STATUS_OK);
    for (size_t index = LINUX_MAPPING_COUNT; mappings != NULL && index != 0; --index)
        HL_CHECK(services.memory->release(services.context, mappings[index - 1]).status == HL_STATUS_OK);
    for (size_t index = EVENT_COUNT; index != 0; --index)
        HL_CHECK(services.event->close(services.context, events[index - 1]).status == HL_STATUS_OK);
    HL_CHECK(services.event->wake(services.context, events[0]).status == HL_STATUS_INVALID_ARGUMENT);
    for (size_t index = WATCH_COUNT; index != 0; --index)
        HL_CHECK(services.watch->close(services.context, watches[index - 1]).status == HL_STATUS_OK);
    HL_CHECK(services.watch->query(services.context, watches[0], &(hl_host_watch_record){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    HL_CHECK(services.file->metadata(services.context, file.value, &(hl_host_file_metadata){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->release(services.context, cross_mapping.value).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, cross_mapping.value).status == HL_STATUS_INVALID_ARGUMENT);
    free(files);
    free(mappings);
    hl_native_host_destroy(host);
    HL_CHECK(unlink(path) == 0);
    return 0;
}
