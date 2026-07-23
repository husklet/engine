#define _GNU_SOURCE

#include "test.h"

#ifdef HL_TEST_MACOS
#include "hl/macos.h"
typedef hl_host_macos hl_test_host;
#define hl_test_create hl_host_macos_create
#define hl_test_destroy hl_host_macos_destroy
#else
#include "hl/linux.h"
typedef hl_host_linux hl_test_host;
#define hl_test_create hl_host_linux_create
#define hl_test_destroy hl_host_linux_destroy
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int create_file(const char *directory, const char *name) {
    char path[256];
    int descriptor;
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    descriptor = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor < 0) return -1;
    return close(descriptor);
}

static int wait_ready(const hl_host_services *services, hl_host_handle pollset, uint64_t token) {
    hl_host_event_record event;
    hl_host_result result =
        services->event->wait(services->context, pollset, &event, 1,
                              services->clock->monotonic_ns(services->context).value + UINT64_C(2000000000));
    return result.status == HL_STATUS_OK && result.value == 1 && event.token == token &&
           (event.readiness & HL_HOST_READY_READ) != 0;
}

static int has_change(const hl_host_directory_record *records, uint32_t count, uint64_t token, uint32_t change) {
    uint32_t index;
    for (index = 0; index < count; ++index)
        if (records[index].token == token && (records[index].changes & change) != 0) return 1;
    return 0;
}

int main(void) {
    char path[] = "/tmp/hl-directory-services-XXXXXX";
    char watched_paths[3][256];
    hl_test_host *host = NULL;
    hl_host_services services = {0};
    hl_host_result directory_files[3];
    hl_host_result instance;
    hl_host_result duplicate;
    hl_host_result pollset;
    hl_host_result result;
    hl_host_directory_record records[16];
    uint32_t count;
    uint32_t seen = 0;

    HL_CHECK(mkdtemp(path) != NULL);
    for (count = 0; count < 3; ++count) {
        snprintf(watched_paths[count], sizeof(watched_paths[count]), "%s/watch-%u", path, count);
        HL_CHECK(mkdir(watched_paths[count], 0700) == 0);
    }
    HL_CHECK(hl_test_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_DIRECTORY | HL_HOST_CAP_EVENT) == HL_STATUS_OK);
    {
        hl_host_file_metadata metadata;
        hl_host_result root = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                                           HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY, 0, 0);
        HL_CHECK(root.status == HL_STATUS_OK);
        HL_CHECK(services.file->make_fifo(services.context, root.value, "probe-fifo", 10, 0600).status == HL_STATUS_OK);
        hl_host_result fifo =
            services.file->open_relative(services.context, root.value, "probe-fifo", 10, HL_HOST_FILE_PATH_ONLY, 0, 0);
        HL_CHECK(fifo.status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, fifo.value, &metadata).status == HL_STATUS_OK);
        HL_CHECK(metadata.type == HL_HOST_FILE_TYPE_FIFO);
        HL_CHECK(services.file->close(services.context, fifo.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->unlink_relative(services.context, root.value, "probe-fifo", 10).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, root.value).status == HL_STATUS_OK);
    }
    for (count = 0; count < 3; ++count) {
        directory_files[count] = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD,
                                                              watched_paths[count], strlen(watched_paths[count]),
                                                              HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY, 0, 0);
        HL_CHECK(directory_files[count].status == HL_STATUS_OK);
    }
    instance = services.directory->create(services.context);
    HL_CHECK(instance.status == HL_STATUS_OK);
    duplicate = services.directory->duplicate(services.context, instance.value);
    HL_CHECK(duplicate.status == HL_STATUS_OK && duplicate.value != instance.value);
    pollset = services.event->create(services.context);
    HL_CHECK(pollset.status == HL_STATUS_OK);
    HL_CHECK(services.event
                 ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, duplicate.value, 91, HL_HOST_READY_READ)
                 .status == HL_STATUS_OK);
    for (count = 0; count < 3; ++count)
        HL_CHECK(services.directory
                     ->add(services.context, instance.value, directory_files[count].value, 11 + count,
                           HL_HOST_DIRECTORY_CREATE | HL_HOST_DIRECTORY_RENAME)
                     .status == HL_STATUS_OK);
    HL_CHECK(services.directory->close(services.context, instance.value).status == HL_STATUS_OK);

    HL_CHECK(create_file(watched_paths[0], "first") == 0);
    HL_CHECK(create_file(watched_paths[1], "second") == 0);
    HL_CHECK(create_file(watched_paths[2], "third") == 0);
    HL_CHECK(wait_ready(&services, pollset.value, 91));
    for (count = 0; count < 3; ++count) {
        result = services.directory->read(services.context, duplicate.value, records, 1);
        HL_CHECK(result.status == HL_STATUS_OK && result.value == 1 &&
                 (records[0].changes & HL_HOST_DIRECTORY_CREATE) != 0 && records[0].token >= 11 &&
                 records[0].token <= 13);
        seen |= 1u << (uint32_t)(records[0].token - 11);
    }
    HL_CHECK(seen == 7);

    HL_CHECK(services.directory
                 ->modify(services.context, duplicate.value, 11, HL_HOST_DIRECTORY_CREATE | HL_HOST_DIRECTORY_DELETE)
                 .status == HL_STATUS_OK);
    HL_CHECK(unlinkat(AT_FDCWD, "/tmp/nonexistent-hl-directory-test", 0) == -1);
    {
        char first[512];
        snprintf(first, sizeof(first), "%s/first", watched_paths[0]);
        HL_CHECK(unlink(first) == 0);
    }
    HL_CHECK(wait_ready(&services, pollset.value, 91));
    result = services.directory->read(services.context, duplicate.value, records, 16);
    HL_CHECK(result.status == HL_STATUS_OK &&
             has_change(records, (uint32_t)result.value, 11, HL_HOST_DIRECTORY_DELETE));
    HL_CHECK(services.directory->remove(services.context, duplicate.value, 11).status == HL_STATUS_OK);
    HL_CHECK(services.directory->remove(services.context, duplicate.value, 11).status == HL_STATUS_NOT_FOUND);
    result = services.directory->read(services.context, duplicate.value, records, 16);
    if (result.status == HL_STATUS_OK)
        HL_CHECK(has_change(records, (uint32_t)result.value, 11, HL_HOST_DIRECTORY_IGNORED));
    HL_CHECK(services.directory->remove(services.context, duplicate.value, 12).status == HL_STATUS_OK);
    HL_CHECK(services.directory->remove(services.context, duplicate.value, 13).status == HL_STATUS_OK);

    HL_CHECK(services.directory
                 ->add(services.context, duplicate.value, directory_files[0].value, 22,
                       HL_HOST_DIRECTORY_CREATE | HL_HOST_DIRECTORY_ONESHOT)
                 .status == HL_STATUS_OK);
    HL_CHECK(create_file(watched_paths[0], "oneshot-a") == 0);
    HL_CHECK(wait_ready(&services, pollset.value, 91));
    result = services.directory->read(services.context, duplicate.value, records, 16);
    HL_CHECK(result.status == HL_STATUS_OK &&
             has_change(records, (uint32_t)result.value, 22, HL_HOST_DIRECTORY_CREATE));
    HL_CHECK(create_file(watched_paths[0], "oneshot-b") == 0);
    usleep(20000);
    result = services.directory->read(services.context, duplicate.value, records, 16);
    HL_CHECK(result.status == HL_STATUS_WOULD_BLOCK ||
             !has_change(records, (uint32_t)result.value, 22, HL_HOST_DIRECTORY_CREATE));

    for (count = 0; count < 3; ++count)
        HL_CHECK(services.file->close(services.context, directory_files[count].value).status == HL_STATUS_OK);
    HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
    HL_CHECK(services.directory->close(services.context, duplicate.value).status == HL_STATUS_OK);
    hl_test_destroy(host);
    {
        static const char *names[] = {"oneshot-a", "oneshot-b", "second", "third"};
        static const uint32_t directories[] = {0, 0, 1, 2};
        for (count = 0; count < 4; ++count) {
            char file[512];
            snprintf(file, sizeof(file), "%s/%s", watched_paths[directories[count]], names[count]);
            HL_CHECK(unlink(file) == 0);
        }
    }
    for (count = 0; count < 3; ++count)
        HL_CHECK(rmdir(watched_paths[count]) == 0);
    HL_CHECK(rmdir(path) == 0);
    return 0;
}
