#include "test.h"

#include "hl/fake.h"

#include <string.h>

static int32_t fake_process_entry(void *context) {
    return context == NULL ? 23 : 24;
}

static hl_host_result fake_reserve_code(void *context, uint64_t size, uint64_t alignment, uint32_t flags,
                                        hl_host_code_mapping *mapping) {
    (void)context;
    (void)size;
    (void)alignment;
    (void)flags;
    (void)mapping;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, 0, 0};
}

static hl_host_result fake_repair_code(void *context, hl_host_code_mapping *mapping, uint32_t preserve) {
    (void)context;
    (void)mapping;
    (void)preserve;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, 0, 0};
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
    hl_host_result counter;
    hl_host_result duplicate;
    hl_host_result channels;
    hl_host_result transfer_result;
    hl_host_transfer_attachment sent_attachment;
    hl_host_transfer_attachment received_attachment;
    char received_data[8] = {0};
    hl_host_clock_services malformed_clock;
    hl_host_sync_services malformed_sync;
    hl_host_memory_services malformed_memory;
    hl_host_transfer_services malformed_transfer;

    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_PROCESS) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_SYNC) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_COUNTER) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_TRANSFER) == HL_STATUS_OK);
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(fake.code_write_begins == 1 && fake.code_write_ends == 1);
    malformed_memory = *services.memory;
    malformed_memory.reserve_code = fake_reserve_code;
    malformed_memory.repair_code_after_fork = fake_repair_code;
    malformed_memory.begin_code_write = NULL;
    truncated = services;
    truncated.memory = &malformed_memory;
    truncated.capabilities |= HL_HOST_CAP_CODE_MAPPING;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CODE_MAPPING) == HL_STATUS_ABI_MISMATCH);
    malformed_clock = *services.clock;
    malformed_clock.raw_monotonic_ns = NULL;
    truncated = services;
    truncated.clock = &malformed_clock;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CLOCK) == HL_STATUS_ABI_MISMATCH);
    malformed_clock = *services.clock;
    malformed_clock.process_cpu_ns = NULL;
    truncated.clock = &malformed_clock;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CLOCK) == HL_STATUS_ABI_MISMATCH);
    malformed_clock = *services.clock;
    malformed_clock.thread_cpu_ns = NULL;
    truncated.clock = &malformed_clock;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CLOCK) == HL_STATUS_ABI_MISMATCH);
    malformed_clock = *services.clock;
    malformed_clock.sleep_until = NULL;
    truncated.clock = &malformed_clock;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CLOCK) == HL_STATUS_ABI_MISMATCH);
    malformed_sync = *services.sync;
    malformed_sync.mutex_close = NULL;
    truncated = services;
    truncated.sync = &malformed_sync;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
    malformed_transfer = *services.transfer;
    malformed_transfer.receive = NULL;
    truncated = services;
    truncated.transfer = &malformed_transfer;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TRANSFER) == HL_STATUS_ABI_MISMATCH);

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

    counter = services.counter->create(services.context, 2, HL_HOST_COUNTER_NONBLOCK);
    HL_CHECK(counter.status == HL_STATUS_OK && counter.value != 0 && fake.live_counters == 1);
    duplicate = services.counter->duplicate(services.context, counter.value);
    HL_CHECK(duplicate.status == HL_STATUS_OK && duplicate.value != counter.value && fake.live_counters == 1);
    HL_CHECK(services.counter->write(services.context, duplicate.value, 3).status == HL_STATUS_OK);
    HL_CHECK(services.counter->read(services.context, counter.value).value == 5);
    HL_CHECK(services.counter->read(services.context, duplicate.value).status == HL_STATUS_WOULD_BLOCK);
    HL_CHECK(services.counter->get_flags(services.context, duplicate.value).value == HL_HOST_COUNTER_NONBLOCK);
    HL_CHECK(services.counter->set_flags(services.context, duplicate.value, 0).status == HL_STATUS_OK);
    HL_CHECK(services.counter->get_flags(services.context, counter.value).value == 0);
    HL_CHECK(services.counter->write(services.context, counter.value, UINT64_MAX).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.counter->write(services.context, counter.value, UINT64_MAX - 1).status == HL_STATUS_OK);
    HL_CHECK(services.counter->write(services.context, counter.value, 1).status == HL_STATUS_WOULD_BLOCK);
    HL_CHECK(services.counter->close(services.context, counter.value).status == HL_STATUS_OK &&
             fake.live_counters == 1);
    HL_CHECK(services.counter->read(services.context, duplicate.value).value == UINT64_MAX - 1);
    HL_CHECK(services.counter->close(services.context, duplicate.value).status == HL_STATUS_OK &&
             fake.live_counters == 0);

    counter = services.counter->create(services.context, 2, HL_HOST_COUNTER_SEMAPHORE);
    HL_CHECK(counter.status == HL_STATUS_OK);
    HL_CHECK(services.counter->read(services.context, counter.value).value == 1);
    HL_CHECK(services.counter->read(services.context, counter.value).value == 1);
    HL_CHECK(services.counter->read(services.context, counter.value).status == HL_STATUS_WOULD_BLOCK);
    HL_CHECK(services.counter->set_flags(services.context, counter.value, 0).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.counter->close(services.context, counter.value).status == HL_STATUS_OK);

    counter = services.counter->create(services.context, 7, 0);
    HL_CHECK(counter.status == HL_STATUS_OK);
    channels = services.transfer->channel_pair(services.context);
    HL_CHECK(channels.status == HL_STATUS_OK && channels.value != 0 && channels.detail != 0 &&
             fake.live_transfer_channels == 2);
    sent_attachment = (hl_host_transfer_attachment){counter.value, HL_HOST_TRANSFER_KIND_COUNTER,
                                                    HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WAIT};
    transfer_result = services.transfer->send(services.context, channels.value, (hl_host_const_bytes){"hello", 5},
                                              &sent_attachment, 1);
    HL_CHECK(transfer_result.status == HL_STATUS_OK && transfer_result.value == 5 && transfer_result.detail == 1);
    HL_CHECK(services.counter->close(services.context, counter.value).status == HL_STATUS_OK &&
             fake.live_counters == 1);
    transfer_result = services.transfer->receive(services.context, channels.detail, (hl_host_bytes){received_data, 4},
                                                 &received_attachment, 1);
    HL_CHECK(transfer_result.status == HL_STATUS_RESOURCE_LIMIT);
    transfer_result =
        services.transfer->receive(services.context, channels.detail,
                                   (hl_host_bytes){received_data, sizeof(received_data)}, &received_attachment, 0);
    HL_CHECK(transfer_result.status == HL_STATUS_RESOURCE_LIMIT);
    transfer_result =
        services.transfer->receive(services.context, channels.detail,
                                   (hl_host_bytes){received_data, sizeof(received_data)}, &received_attachment, 1);
    HL_CHECK(transfer_result.status == HL_STATUS_OK && transfer_result.value == 5 && transfer_result.detail == 1 &&
             memcmp(received_data, "hello", 5) == 0);
    HL_CHECK(received_attachment.kind == HL_HOST_TRANSFER_KIND_COUNTER &&
             received_attachment.rights == (HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WAIT));
    HL_CHECK(services.counter->read(services.context, received_attachment.object).value == 7);
    HL_CHECK(services.counter->write(services.context, received_attachment.object, 1).status ==
             HL_STATUS_PERMISSION_DENIED);
    HL_CHECK(services.counter->get_flags(services.context, received_attachment.object).status ==
             HL_STATUS_PERMISSION_DENIED);
    HL_CHECK(services.counter->close(services.context, received_attachment.object).status == HL_STATUS_OK &&
             fake.live_counters == 0);
    HL_CHECK(services.transfer->close(services.context, channels.value).status == HL_STATUS_OK);
    HL_CHECK(services.transfer->close(services.context, channels.detail).status == HL_STATUS_OK &&
             fake.live_transfer_channels == 0);

    counter = services.counter->create(services.context, 1, 0);
    channels = services.transfer->channel_pair(services.context);
    sent_attachment =
        (hl_host_transfer_attachment){counter.value, HL_HOST_TRANSFER_KIND_COUNTER, HL_HOST_TRANSFER_READ};
    HL_CHECK(services.transfer
                 ->send(services.context, channels.value, (hl_host_const_bytes){NULL, 0}, &sent_attachment,
                        HL_HOST_TRANSFER_MAX_ATTACHMENTS + 1)
                 .status == HL_STATUS_INVALID_ARGUMENT);
    sent_attachment.rights = UINT32_MAX;
    HL_CHECK(
        services.transfer->send(services.context, channels.value, (hl_host_const_bytes){NULL, 0}, &sent_attachment, 1)
            .status == HL_STATUS_PERMISSION_DENIED);
    sent_attachment.rights = HL_HOST_TRANSFER_READ;
    HL_CHECK(
        services.transfer->send(services.context, channels.value, (hl_host_const_bytes){NULL, 0}, &sent_attachment, 1)
            .status == HL_STATUS_OK);
    HL_CHECK(services.counter->close(services.context, counter.value).status == HL_STATUS_OK &&
             fake.live_counters == 1);
    HL_CHECK(services.transfer->close(services.context, channels.detail).status == HL_STATUS_OK &&
             fake.live_counters == 0);
    HL_CHECK(services.transfer->close(services.context, channels.value).status == HL_STATUS_OK);

    hl_fake_host_fail_next(&fake, HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(services.memory->reserve(services.context, 4096, 4096, 0).status == HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(fake.live_mappings == 0);
    return EXIT_SUCCESS;
}
