#include "test.h"

#include "hl/fake.h"

#include <string.h>

static int32_t fake_process_entry(void *context) {
    return context == NULL ? 23 : 24;
}

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_host_result mapping;
    hl_host_services truncated;
    hl_host_result process;
    hl_host_result process_exit;
    hl_host_result mutex;
    hl_host_result other_mutex;
    hl_host_sync_services malformed_sync;

    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_PROCESS) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_SYNC) == HL_STATUS_OK);
    malformed_sync = *services.sync;
    malformed_sync.mutex_close = NULL;
    truncated = services;
    truncated.sync = &malformed_sync;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);

    truncated = services;
    truncated.size = 8;
    HL_CHECK(hl_host_services_validate(&truncated, 0) == HL_STATUS_ABI_MISMATCH);

    mapping = services.memory->reserve(services.context, 4096, 4096, 0);
    HL_CHECK(mapping.status == HL_STATUS_OK && mapping.value != 0 && fake.live_mappings == 1);
    HL_CHECK(services.memory->publish_code(services.context, mapping.value, 0, 4096).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_mappings == 0);

    process = services.process->spawn_cloned(services.context, fake_process_entry, NULL);
    HL_CHECK(process.status == HL_STATUS_OK && process.value != 0 && fake.live_processes == 1);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_BUSY);
    fake.process_exit_value = 37;
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_processes == 0);

    mutex = services.sync->mutex_create(services.context);
    HL_CHECK(mutex.status == HL_STATUS_OK && mutex.value != 0 && fake.live_mutexes == 1);
    HL_CHECK(services.sync->mutex_lock(services.context, mutex.value).status == HL_STATUS_OK);
    other_mutex = services.sync->mutex_create(services.context);
    HL_CHECK(other_mutex.status == HL_STATUS_OK && other_mutex.value != mutex.value && fake.live_mutexes == 2);
    HL_CHECK(services.sync->mutex_lock(services.context, other_mutex.value).status == HL_STATUS_OK);
    HL_CHECK(services.sync->mutex_close(services.context, mutex.value).status == HL_STATUS_BUSY);
    HL_CHECK(services.sync->mutex_unlock(services.context, mutex.value).status == HL_STATUS_OK);
    HL_CHECK(services.sync->mutex_close(services.context, mutex.value).status == HL_STATUS_OK);
    HL_CHECK(services.sync->mutex_unlock(services.context, other_mutex.value).status == HL_STATUS_OK);
    HL_CHECK(services.sync->mutex_close(services.context, other_mutex.value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_mutexes == 0);
    HL_CHECK(services.sync->mutex_lock(services.context, HL_HOST_HANDLE_INVALID).status == HL_STATUS_INVALID_ARGUMENT);

    hl_fake_host_fail_next(&fake, HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(services.memory->reserve(services.context, 4096, 4096, 0).status == HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(fake.live_mappings == 0);
    return EXIT_SUCCESS;
}
