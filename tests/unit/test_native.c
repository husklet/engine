#define _GNU_SOURCE
#include "test.h"

#include "../../src/core/target/native.h"
#include "../../src/core/target/services.h"
#include "../../src/translator/persist.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

typedef struct persist_stress {
    hl_persist_directory *directory;
    unsigned char value;
    atomic_int *failed;
} persist_stress;

static void *persist_writer(void *opaque) {
    persist_stress *stress = opaque;
    unsigned char payload[257];
    memset(payload, stress->value, sizeof payload);
    for (int iteration = 0; iteration < 100; iteration++)
        if (!hl_persist_store_at(stress->directory, "cache", payload, sizeof payload)) atomic_store(stress->failed, 1);
    return NULL;
}

static void *persist_reader(void *opaque) {
    persist_stress *stress = opaque;
    for (int iteration = 0; iteration < 500; iteration++) {
        void *data = NULL;
        size_t size = 0;
        if (!hl_persist_load_at(stress->directory, "cache", 1024, &data, &size) || size != 257) {
            atomic_store(stress->failed, 1);
            free(data);
            continue;
        }
        unsigned char *bytes = data;
        for (size_t index = 1; index < size; index++)
            if (bytes[index] != bytes[0]) atomic_store(stress->failed, 1);
        free(data);
    }
    return NULL;
}

int main(void) {
    hl_native_host *host = NULL;
    hl_host_services services = {0};
    hl_host_services injected;
    HL_CHECK(HL_NATIVE_HOST_NAME[0] != '\0');
    HL_CHECK(strcmp(HL_NATIVE_HOST_NAME, "macos") == 0 || strcmp(HL_NATIVE_HOST_NAME, "linux") == 0);
    HL_CHECK(hl_native_host_bind(&host, &services, NULL) == 0);
    {
        hl_target_services target = {0};
        hl_host_services mismatch;
        hl_target_services_inject(&target, &services);
        HL_CHECK(hl_target_services_effective(&target) == &services);
        HL_CHECK(hl_target_services_bind(&target) == 0);
        HL_CHECK(hl_target_services_bound(&target)->context == services.context);
        mismatch = services;
        mismatch.context = NULL;
        hl_target_services_inject(&target, &mismatch);
        HL_CHECK(hl_target_services_bind(&target) == -1);
        hl_target_services_destroy(&target);
        HL_CHECK(hl_target_services_effective(&target) == &target.bound);
    }
    {
        hl_target_services target = {0};
        hl_host_services missing = services;
        char directory[] = "/tmp/hl-target-directory-XXXXXX";
        HL_CHECK(mkdtemp(directory) != NULL);
        HL_CHECK(rmdir(directory) == 0);
        hl_target_services_inject(&target, &services);
        HL_CHECK(hl_target_services_make_directory(&target, directory, 0700) == 0);
        HL_CHECK(hl_target_services_make_directory(&target, directory, 0700) == 0);
        HL_CHECK(rmdir(directory) == 0);
        missing.file = NULL;
        hl_target_services_inject(&target, &missing);
        HL_CHECK(hl_target_services_make_directory(&target, directory, 0700) == -1);
        hl_target_services_inject(&target, &services);
        HL_CHECK(hl_target_services_make_directory(&target, "/missing/hl-target-directory", 0700) == -1);
    }
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
        hl_host_result parent =
            services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, directory, strlen(directory),
                                         HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY | HL_HOST_FILE_NOFOLLOW, 0, 0);
        HL_CHECK(parent.status == HL_STATUS_OK &&
                 services.file->validate_private_directory(services.context, parent.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, parent.value).status == HL_STATUS_OK);
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
        HL_CHECK(chmod(directory, 0777) == 0);
        parent = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, directory, strlen(directory),
                                              HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY | HL_HOST_FILE_NOFOLLOW, 0, 0);
        HL_CHECK(parent.status == HL_STATUS_OK &&
                 services.file->validate_private_directory(services.context, parent.value).status ==
                     HL_STATUS_PERMISSION_DENIED);
        HL_CHECK(services.file->close(services.context, parent.value).status == HL_STATUS_OK);
        HL_CHECK(chmod(directory, 0700) == 0);
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
        char path[] = "/tmp/hl-persist-stress-XXXXXX";
        hl_persist_directory directory;
        pthread_t writers[4], reader;
        atomic_int failed = 0;
        persist_stress stress[4];
        unsigned char initial[257];
        memset(initial, 0x5a, sizeof initial);
        HL_CHECK(mkdtemp(path) != NULL);
        HL_CHECK(hl_persist_directory_open(&directory, &services, path, 0) == 1);
        HL_CHECK(hl_persist_store_at(&directory, "cache", initial, sizeof initial) == 1);
        for (int index = 0; index < 4; index++) {
            stress[index] = (persist_stress){&directory, (unsigned char)(0x20 + index), &failed};
            HL_CHECK(pthread_create(&writers[index], NULL, persist_writer, &stress[index]) == 0);
        }
        HL_CHECK(pthread_create(&reader, NULL, persist_reader, &stress[0]) == 0);
        for (int index = 0; index < 4; index++)
            HL_CHECK(pthread_join(writers[index], NULL) == 0);
        HL_CHECK(pthread_join(reader, NULL) == 0);
        HL_CHECK(atomic_load(&failed) == 0);
        HL_CHECK(hl_persist_remove_at(&directory, "cache") == 1);
        HL_CHECK(hl_persist_directory_close(&directory) == 1);
        HL_CHECK(rmdir(path) == 0); /* no temporary publication file leaked */
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
                     ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, watch.value, 909, HL_HOST_READY_READ)
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
        HL_CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
                 WEXITSTATUS(child_status) == 0);
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
