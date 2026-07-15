#define _GNU_SOURCE
#include "test.h"

#include "../../src/core/target/native.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    hl_native_host *host = NULL;
    hl_host_services services = {0};
    hl_host_services injected;
    HL_CHECK(HL_NATIVE_HOST_NAME[0] != '\0');
    HL_CHECK(strcmp(HL_NATIVE_HOST_NAME, "macos") == 0 || strcmp(HL_NATIVE_HOST_NAME, "linux") == 0);
    HL_CHECK(hl_native_host_bind(&host, &services, NULL) == 0);
    HL_CHECK(host != NULL);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_CODE_MAPPING) ==
             HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_WATCH | HL_HOST_CAP_EVENT | HL_HOST_CAP_FILE) ==
             HL_STATUS_OK);
    {
        char directory[] = "/tmp/hl-private-store-XXXXXX";
        char path[160];
        const char first[] = "complete generation one";
        const char second[] = "two";
        HL_CHECK(mkdtemp(directory) != NULL);
        snprintf(path, sizeof path, "%s/cache", directory);
        HL_CHECK(services.file
                     ->store_private_atomic(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                            (hl_host_const_bytes){first, sizeof(first) - 1}, 0600)
                     .status == HL_STATUS_OK);
        hl_host_result file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                                           HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
        char bytes[32] = {0};
        HL_CHECK(file.status == HL_STATUS_OK);
        HL_CHECK(services.file->validate_private_regular(services.context, file.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->read_at(services.context, file.value, 0, (hl_host_bytes){bytes, sizeof bytes}).value ==
                 sizeof(first) - 1);
        HL_CHECK(memcmp(bytes, first, sizeof(first) - 1) == 0);
        HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
        HL_CHECK(services.file
                     ->store_private_atomic(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                            (hl_host_const_bytes){second, sizeof(second) - 1}, 0600)
                     .status == HL_STATUS_OK);
        file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                            HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
        memset(bytes, 0, sizeof bytes);
        HL_CHECK(file.status == HL_STATUS_OK &&
                 services.file->read_at(services.context, file.value, 0, (hl_host_bytes){bytes, sizeof bytes}).value ==
                     sizeof(second) - 1);
        HL_CHECK(memcmp(bytes, second, sizeof(second) - 1) == 0);
        HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
        HL_CHECK(chmod(path, 0666) == 0);
        file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                            HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
        HL_CHECK(file.status == HL_STATUS_OK &&
                 services.file->validate_private_regular(services.context, file.value).status ==
                     HL_STATUS_PERMISSION_DENIED);
        HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
        HL_CHECK(unlink(path) == 0 && rmdir(directory) == 0);
    }
    {
        char path[] = "/tmp/hl-native-watch-XXXXXX", moved[128];
        int native = mkstemp(path);
        HL_CHECK(native >= 0 && close(native) == 0);
        hl_host_result file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                                           HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, 0, 0);
        hl_host_result watch = services.watch->open(services.context, file.value);
        hl_host_result pollset = services.event->create(services.context);
        hl_host_watch_record record = {0};
        hl_host_event_record event = {0};
        HL_CHECK(file.status == HL_STATUS_OK && watch.status == HL_STATUS_OK && pollset.status == HL_STATUS_OK);
        HL_CHECK(services.event
                     ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, watch.value, 909,
                               HL_HOST_READY_READ)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){"a", 1}).value == 1);
        uint64_t deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
        HL_CHECK(services.event->wait(services.context, pollset.value, &event, 1, deadline).value == 1);
        HL_CHECK(services.watch->drain(services.context, watch.value, &record, 1).value == 1 && record.size == 1 &&
                 (record.changes & (HL_HOST_WATCH_SIZE | HL_HOST_WATCH_DATA)) != 0);
        HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){"b", 1}).value == 1);
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
        HL_CHECK(services.event->wait(services.context, pollset.value, &event, 1, deadline).value == 1);
        HL_CHECK(services.watch->drain(services.context, watch.value, &record, 1).value == 1 && record.size == 1 &&
                 (record.changes & HL_HOST_WATCH_DATA) != 0);
        pid_t child = fork();
        HL_CHECK(child >= 0);
        if (child == 0) {
            hl_host_watch_record inherited = {0};
            _exit(services.watch->query(services.context, watch.value, &inherited).status == HL_STATUS_OK &&
                          inherited.size == 1
                      ? 0
                      : 40);
        }
        int child_status = 0;
        HL_CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
        snprintf(moved, sizeof moved, "%s.moved", path);
        HL_CHECK(rename(path, moved) == 0);
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
        HL_CHECK(services.event->wait(services.context, pollset.value, &event, 1, deadline).value == 1);
        HL_CHECK(services.watch->drain(services.context, watch.value, &record, 1).value == 1 &&
                 (record.changes & HL_HOST_WATCH_IDENTITY) != 0);
        HL_CHECK(unlink(moved) == 0);
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
        HL_CHECK(services.event->wait(services.context, pollset.value, &event, 1, deadline).value == 1);
        HL_CHECK(services.watch->drain(services.context, watch.value, &record, 1).value == 1 &&
                 (record.changes & HL_HOST_WATCH_DELETED) != 0);
        HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(services.watch->close(services.context, watch.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    }
    injected = services;
    HL_CHECK(hl_native_host_bind(&host, &services, &injected) == 0);
    injected.context = NULL;
    HL_CHECK(hl_native_host_bind(&host, &services, &injected) == -1);
    HL_CHECK(hl_native_host_bind(&host, &services, NULL) == 0);
    hl_native_host_destroy(host);
    return 0;
}
