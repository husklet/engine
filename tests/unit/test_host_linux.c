#include "test.h"

#include "hl/host_linux.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    hl_host_linux *linux_host;
    hl_host_services services;
    hl_host_result mapping;
    hl_host_result file;
    hl_host_result pollset;
    hl_host_result shared;
    hl_host_event_record event;
    hl_host_file_metadata metadata;
    const char contents[] = "portable-host";
    char readback[sizeof(contents)] = {0};
    char path[128];

    HL_CHECK(hl_host_linux_create(&linux_host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_FILE |
                                                      HL_HOST_CAP_EVENT | HL_HOST_CAP_NETWORK |
                                                      HL_HOST_CAP_SHARED_MEMORY) == HL_STATUS_OK);
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.clock->realtime_ns(services.context).status == HL_STATUS_OK);

    mapping = services.memory->reserve(services.context, 4096, 4096, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
    HL_CHECK(mapping.status == HL_STATUS_OK);
    HL_CHECK(services.memory->protect(services.context, mapping.value, 0, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_OK);
    HL_CHECK(services.memory->publish_code(services.context, mapping.value, 0, 4096).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_INVALID_ARGUMENT);

    snprintf(path, sizeof(path), "/tmp/hl_host_linux_%ld", (long)getpid());
    file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                        HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE);
    HL_CHECK(file.status == HL_STATUS_OK);
    HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){contents, sizeof(contents)})
                 .value == sizeof(contents));
    HL_CHECK(
        services.file->read_at(services.context, file.value, 0, (hl_host_bytes){readback, sizeof(readback)}).value ==
        sizeof(readback));
    HL_CHECK(memcmp(contents, readback, sizeof(contents)) == 0);
    HL_CHECK(services.file->metadata(services.context, file.value, &metadata).status == HL_STATUS_OK);
    HL_CHECK(metadata.size == sizeof(contents));
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

    hl_host_linux_destroy(linux_host);
    return EXIT_SUCCESS;
}
