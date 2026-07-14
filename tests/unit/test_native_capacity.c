#define _GNU_SOURCE
#include "test.h"

#include "../../src/core/target/native.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    WATCH_COUNT = 257,
    EVENT_COUNT = 129
};

int main(void) {
    hl_native_host *host = NULL;
    hl_host_services services = {0};
    hl_host_handle watches[WATCH_COUNT] = {0};
    hl_host_handle events[EVENT_COUNT] = {0};
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

    for (size_t index = EVENT_COUNT; index != 0; --index)
        HL_CHECK(services.event->close(services.context, events[index - 1]).status == HL_STATUS_OK);
    for (size_t index = WATCH_COUNT; index != 0; --index)
        HL_CHECK(services.watch->close(services.context, watches[index - 1]).status == HL_STATUS_OK);
    HL_CHECK(services.watch->query(services.context, watches[0], &(hl_host_watch_record){0}).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    hl_native_host_destroy(host);
    HL_CHECK(unlink(path) == 0);
    return 0;
}
