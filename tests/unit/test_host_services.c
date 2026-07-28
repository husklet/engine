#include "test.h"

#include "hl/fake.h"

#include <stddef.h>
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

static void fake_counter_notify(void *observer, uint64_t token) {
    uint64_t *value = observer;
    *value = token;
}

static hl_host_result fake_watch_open(void *context, hl_host_handle file) {
    (void)context;
    (void)file;
    return (hl_host_result){HL_STATUS_OK, 0, 1, 0};
}

static hl_host_result fake_watch_query(void *context, hl_host_handle watch, hl_host_watch_record *record) {
    (void)context;
    (void)watch;
    (void)record;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result fake_watch_drain(void *context, hl_host_handle watch, hl_host_watch_record *records,
                                       size_t capacity) {
    (void)context;
    (void)watch;
    (void)records;
    (void)capacity;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result fake_watch_close(void *context, hl_host_handle watch) {
    (void)context;
    (void)watch;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
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
    hl_host_network_services malformed_network;
    hl_host_transfer_services malformed_transfer;
    hl_host_file_services malformed_file;
    hl_host_result directory;
    hl_host_result directory_copy;
    hl_host_result pollset;
    hl_host_directory_record directory_record;
    hl_host_event_record ready;
    hl_host_watch_services watch = {HL_HOST_WATCH_ABI, sizeof(watch),    fake_watch_open,
                                    fake_watch_query,  fake_watch_drain, fake_watch_close};

    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_PROCESS) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_SYNC) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_COUNTER) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_TRANSFER) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_STREAM) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_FILE) == HL_STATUS_OK);
    truncated = services;
    truncated.size = sizeof(truncated) - 1;
    HL_CHECK(hl_host_services_validate(&truncated, 0) == HL_STATUS_ABI_MISMATCH);
    malformed_memory = *services.memory;
    --malformed_memory.abi;
    truncated = services;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_network, 0xff, sizeof(malformed_network));
    malformed_network.abi = 0;
    malformed_network.size = sizeof(malformed_network);
    truncated = services;
    truncated.capabilities |= HL_HOST_CAP_NETWORK;
    truncated.network = &malformed_network;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_NETWORK) == HL_STATUS_ABI_MISMATCH);
    malformed_sync = *services.sync;
    --malformed_sync.abi;
    truncated = services;
    truncated.sync = &malformed_sync;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
    {
        hl_host_result source = hl_fake_host_file_create(&fake);
        hl_host_result clone;
        HL_CHECK(source.status == HL_STATUS_OK && source.value != HL_HOST_HANDLE_INVALID);
        HL_CHECK(fake.live_files == 1 && fake.live_file_clones == 0 && fake.file_close_count == 0);
        clone = services.file->clone_for_fork(services.context, source.value);
        HL_CHECK(clone.status == HL_STATUS_OK && clone.value != source.value);
        HL_CHECK(fake.live_files == 2 && fake.live_file_clones == 1);
        hl_fake_host_fail_next(&fake, HL_STATUS_OUT_OF_MEMORY);
        HL_CHECK(services.file->clone_for_fork(services.context, source.value).status == HL_STATUS_OUT_OF_MEMORY);
        HL_CHECK(fake.live_files == 2 && fake.live_file_clones == 1);
        HL_CHECK(services.file->close(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, source.value).status == HL_STATUS_OK);
        HL_CHECK(fake.live_files == 0 && fake.live_file_clones == 0 && fake.file_close_count == 2);
    }
    {
        hl_host_services standalone = services;
        hl_host_result pipe;
        char bytes[8] = {0};
        standalone.file = NULL;
        standalone.capabilities &= ~(uint64_t)HL_HOST_CAP_FILE;
        HL_CHECK(hl_host_services_validate(&standalone, HL_HOST_CAP_STREAM) == HL_STATUS_OK);
        pipe = standalone.stream->pipe_pair(standalone.context, HL_HOST_STREAM_NONBLOCK);
        HL_CHECK(pipe.status == HL_STATUS_OK);
        HL_CHECK(standalone.stream->write(standalone.context, pipe.detail, (hl_host_const_bytes){"fake", 4}).value ==
                 4);
        HL_CHECK(standalone.stream->read(standalone.context, pipe.value, (hl_host_bytes){bytes, sizeof bytes}).value ==
                 4);
        HL_CHECK(memcmp(bytes, "fake", 4) == 0);
        HL_CHECK(standalone.stream->close(standalone.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(standalone.stream->close(standalone.context, pipe.detail).status == HL_STATUS_OK);
    }
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_DIRECTORY | HL_HOST_CAP_EVENT) == HL_STATUS_OK);
    truncated = services;
    truncated.watch = NULL;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_WATCH) == HL_STATUS_ABI_MISMATCH);
    truncated = services;
    truncated.stream = NULL;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_STREAM) == HL_STATUS_ABI_MISMATCH);
    truncated = services;
    truncated.watch = &watch;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_WATCH) == HL_STATUS_OK);
    truncated.size = (uint32_t)offsetof(hl_host_services, watch);
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_WATCH) == HL_STATUS_ABI_MISMATCH);
    truncated.size = sizeof(truncated);
    watch.size = sizeof(watch) - 1;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_WATCH) == HL_STATUS_ABI_MISMATCH);
    watch.size = sizeof(watch);
    {
        hl_host_result watched = services.watch->open(services.context, 77);
        hl_host_result watch_pollset = services.event->create(services.context);
        hl_host_watch_record record = {0};
        HL_CHECK(watched.status == HL_STATUS_OK && watch_pollset.status == HL_STATUS_OK);
        HL_CHECK(services.event
                     ->control(services.context, watch_pollset.value, HL_HOST_EVENT_ADD, watched.value, 88,
                               HL_HOST_READY_READ)
                     .status == HL_STATUS_OK);
        hl_fake_host_watch_emit(&fake, 77, 3, 4, 99, HL_HOST_WATCH_SIZE | HL_HOST_WATCH_DATA);
        HL_CHECK(services.event->wait(services.context, watch_pollset.value, &ready, 1, 0).value == 1 &&
                 ready.token == 88);
        HL_CHECK(services.watch->drain(services.context, watched.value, &record, 1).value == 1 &&
                 record.stable_device == 3 && record.stable_object == 4 && record.size == 99 &&
                 record.changes == (HL_HOST_WATCH_SIZE | HL_HOST_WATCH_DATA));
        HL_CHECK(services.watch->drain(services.context, watched.value, &record, 1).status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.watch->close(services.context, watched.value).status == HL_STATUS_OK);
        HL_CHECK(services.event->close(services.context, watch_pollset.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->repair_signal_page(services.context, 0x400000, 4096,
                                                 HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE));
    HL_CHECK(!services.memory->repair_signal_page(services.context, 0x400001, 4096, HL_HOST_MEMORY_READ));
    HL_CHECK(!services.memory->repair_signal_page(services.context, 0x400000, 8192, HL_HOST_MEMORY_READ));
    HL_CHECK(!services.memory->repair_signal_page(services.context, 0x400000, 4096, UINT32_MAX));
    HL_CHECK(fake.code_write_begins == 1 && fake.code_write_ends == 1);
    directory = services.directory->create(services.context);
    HL_CHECK(directory.status == HL_STATUS_OK);
    directory_copy = services.directory->duplicate(services.context, directory.value);
    HL_CHECK(directory_copy.status == HL_STATUS_OK);
    HL_CHECK(services.directory
                 ->add(services.context, directory.value, 999, 41, HL_HOST_DIRECTORY_CREATE | HL_HOST_DIRECTORY_ONESHOT)
                 .status == HL_STATUS_OK);
    pollset = services.event->create(services.context);
    HL_CHECK(pollset.status == HL_STATUS_OK);
    HL_CHECK(
        services.event
            ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, directory_copy.value, 77, HL_HOST_READY_READ)
            .status == HL_STATUS_OK);
    HL_CHECK(services.directory->close(services.context, directory.value).status == HL_STATUS_OK);
    hl_fake_host_directory_emit(&fake, 41, HL_HOST_DIRECTORY_CREATE);
    HL_CHECK(services.event->wait(services.context, pollset.value, &ready, 1, 0).status == HL_STATUS_OK &&
             ready.token == 77);
    HL_CHECK(services.directory->read(services.context, directory_copy.value, &directory_record, 1).status ==
                 HL_STATUS_OK &&
             directory_record.token == 41 && directory_record.changes == HL_HOST_DIRECTORY_CREATE);
    HL_CHECK(services.directory->read(services.context, directory_copy.value, &directory_record, 1).status ==
                 HL_STATUS_OK &&
             directory_record.changes == HL_HOST_DIRECTORY_IGNORED);
    hl_fake_host_directory_emit(&fake, 41, HL_HOST_DIRECTORY_CREATE);
    HL_CHECK(services.directory->read(services.context, directory_copy.value, &directory_record, 1).status ==
             HL_STATUS_WOULD_BLOCK);
    HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
    HL_CHECK(services.directory->close(services.context, directory_copy.value).status == HL_STATUS_OK);
    /* HL_HOST_MEMORY_ABI 7 behaviour on the fake provider. It owns no address space, so the honest answer
     * to a wiring request is the typed unsupported status, not a success that pinned nothing. */
    HL_CHECK(services.memory->abi == HL_HOST_MEMORY_ABI && services.memory->unmap_address != NULL &&
             services.memory->wire_range != NULL && services.memory->unwire_range != NULL);
    HL_CHECK(services.memory->unmap_address(services.context, 0x40000000, 4096).status == HL_STATUS_OK);
    HL_CHECK(services.memory->unmap_address(services.context, 0, 4096).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->unmap_address(services.context, 0x40000000, 0).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->unmap_address(services.context, 0x40000001, 4096).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->unmap_address(services.context, 0x40000000, 4095).status == HL_STATUS_INVALID_ARGUMENT);
    hl_fake_host_fail_next(&fake, HL_STATUS_PLATFORM_FAILURE);
    HL_CHECK(services.memory->unmap_address(services.context, 0x40000000, 4096).status == HL_STATUS_PLATFORM_FAILURE);
    HL_CHECK(services.memory->unmap_address(services.context, 0x40000000, 4096).status == HL_STATUS_OK);
    {
        hl_host_result unsupported = services.memory->wire_range(services.context, 0x40000000, 4096, 0);
        HL_CHECK(unsupported.status == HL_STATUS_NOT_SUPPORTED && unsupported.detail == (uint64_t)HL_HOST_WIRE_NONE);
        unsupported = services.memory->unwire_range(services.context, 0x40000000, 4096);
        HL_CHECK(unsupported.status == HL_STATUS_NOT_SUPPORTED && unsupported.detail == (uint64_t)HL_HOST_WIRE_NONE);
    }
    HL_CHECK(services.memory->wire_range(services.context, 0x40000000, 4096, 1).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->wire_range(services.context, 0, 4096, 0).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->wire_range(services.context, 0x40000001, 4096, 0).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->unwire_range(services.context, 0, 4096).status == HL_STATUS_INVALID_ARGUMENT);

    /* An ABI 7 group must carry every appended callback. */
    malformed_memory = *services.memory;
    malformed_memory.unmap_address = NULL;
    truncated = services;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    malformed_memory = *services.memory;
    malformed_memory.wire_range = NULL;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    malformed_memory = *services.memory;
    malformed_memory.unwire_range = NULL;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    /* An ABI 6 group stops before them and stays valid: two shipping hosts are still on it. */
    malformed_memory = *services.memory;
    malformed_memory.abi = HL_HOST_MEMORY_ABI_MIN;
    malformed_memory.size = (uint32_t)offsetof(hl_host_memory_services, unmap_address);
    malformed_memory.unmap_address = NULL;
    malformed_memory.wire_range = NULL;
    malformed_memory.unwire_range = NULL;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_OK);
    /* The code-mapping prefix is inside the ABI 6 group, so it stays reachable there too. */
    malformed_memory.reserve_code = fake_reserve_code;
    malformed_memory.repair_code_after_fork = fake_repair_code;
    truncated.capabilities |= HL_HOST_CAP_CODE_MAPPING;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CODE_MAPPING) == HL_STATUS_OK);
    truncated.capabilities = services.capabilities;
    /* One byte short of the ABI 6 prefix is not a prefix. */
    malformed_memory.size = (uint32_t)offsetof(hl_host_memory_services, unmap_address) - 1u;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    /* Neither an unreleased future group nor a retired older one is accepted. */
    malformed_memory = *services.memory;
    malformed_memory.abi = HL_HOST_MEMORY_ABI + 1u;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    malformed_memory.abi = HL_HOST_MEMORY_ABI_MIN - 1u;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);

    /* HL_HOST_MEMORY_ABI 8: the address-keyed protection and flush the handle-keyed pair could not
     * reach. The fake owns no address space, so what it can be held to is argument validation and
     * the absence of any surprise -- the behaviour that needs pages is proven on the real hosts. */
    HL_CHECK(services.memory->protect_address != NULL && services.memory->sync_address != NULL);
    HL_CHECK(services.memory->protect_address(services.context, 0x40000000, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_OK);
    HL_CHECK(services.memory->protect_address(services.context, 0, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->protect_address(services.context, 0x40000000, 0, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->protect_address(services.context, 0x40000001, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->protect_address(services.context, 0x40000000, 4096, UINT32_MAX).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory
                 ->sync_address(services.context, 0x40000000, 4096,
                                HL_HOST_MEMORY_SYNC_ASYNC | HL_HOST_MEMORY_SYNC_INVALIDATE)
                 .status == HL_STATUS_OK);
    HL_CHECK(services.memory->sync_address(services.context, 0x40000001, 4096, 0).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.memory->sync_address(services.context, 0x40000000, 4096, UINT32_MAX).status ==
             HL_STATUS_INVALID_ARGUMENT);
    /* Rollback: an injected failure leaves the operation refused and nothing half-applied. */
    hl_fake_host_fail_next(&fake, HL_STATUS_PLATFORM_FAILURE);
    HL_CHECK(services.memory->protect_address(services.context, 0x40000000, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_PLATFORM_FAILURE);
    HL_CHECK(services.memory->protect_address(services.context, 0x40000000, 4096, HL_HOST_MEMORY_READ).status ==
             HL_STATUS_OK);

    /* An ABI 8 group must carry both appended callbacks; an ABI 7 group stops before them and is
     * still accepted, which is the whole point of keeping the prefix valid. */
    malformed_memory = *services.memory;
    malformed_memory.protect_address = NULL;
    truncated = services;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    malformed_memory = *services.memory;
    malformed_memory.sync_address = NULL;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    malformed_memory = *services.memory;
    malformed_memory.abi = HL_HOST_MEMORY_ABI - 1u;
    malformed_memory.size = (uint32_t)offsetof(hl_host_memory_services, protect_address);
    malformed_memory.protect_address = NULL;
    malformed_memory.sync_address = NULL;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_OK);
    /* One byte short of the ABI 7 prefix is not a prefix. */
    malformed_memory.size = (uint32_t)offsetof(hl_host_memory_services, protect_address) - 1u;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
    {
        /* HL_HOST_SYNC_ABI 3: the parking trio, and the ABI 2 prefix that must keep validating. */
        uint64_t word = 0;
        uint64_t key = (uint64_t)(uintptr_t)&word;
        HL_CHECK(services.sync->abi == HL_HOST_SYNC_ABI && services.sync->park != NULL &&
                 services.sync->unpark != NULL && services.sync->interrupt_park != NULL);
        truncated = services;
        malformed_sync = *services.sync;
        malformed_sync.park = NULL;
        truncated.sync = &malformed_sync;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
        malformed_sync = *services.sync;
        malformed_sync.unpark = NULL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
        malformed_sync = *services.sync;
        malformed_sync.interrupt_park = NULL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
        malformed_sync = *services.sync;
        malformed_sync.abi = HL_HOST_SYNC_ABI_MIN;
        malformed_sync.size = (uint32_t)offsetof(hl_host_sync_services, park);
        malformed_sync.park = NULL;
        malformed_sync.unpark = NULL;
        malformed_sync.interrupt_park = NULL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_OK);
        malformed_sync.size = (uint32_t)offsetof(hl_host_sync_services, park) - 1u;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
        malformed_sync = *services.sync;
        malformed_sync.abi = HL_HOST_SYNC_ABI + 1u;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);
        malformed_sync.abi = HL_HOST_SYNC_ABI_MIN - 1u;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_SYNC) == HL_STATUS_ABI_MISMATCH);

        /* Behaviour. This provider never blocks, so what it can answer is the set decided without
         * waiting: the compare, the interruption record, and a deadline already reached. */
        HL_CHECK(services.sync->park(services.context, 0, HL_HOST_PARK_PRIVATE, key, &word, 0, 4, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, NULL, 0, 4, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, 5u, key, &word, 0, 4, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, &word, 0, 2, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, (const char *)&word + 1, 0, 4, 0)
                     .status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, &word, 7, 4, 0).status ==
                 HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_SHARED, key, &word, 0, 8, 0).status ==
                 HL_STATUS_TIMED_OUT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, &word, 0, 4,
                                     HL_HOST_DEADLINE_INFINITE)
                     .status == HL_STATUS_NOT_SUPPORTED);
        HL_CHECK(services.sync->unpark(services.context, HL_HOST_PARK_PRIVATE, key, NULL, 1).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->unpark(services.context, 5u, key, &word, 1).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->unpark(services.context, HL_HOST_PARK_PRIVATE, key, &word, UINT32_MAX).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.sync->interrupt_park(services.context, 0).status == HL_STATUS_INVALID_ARGUMENT);
        /* Recorded against the waiter, so it survives until a block consumes it -- and exactly one
         * block does. An infinite deadline proves the record is consulted before any waiting. */
        HL_CHECK(services.sync->interrupt_park(services.context, 55).status == HL_STATUS_OK);
        HL_CHECK(services.sync->interrupt_park(services.context, 55).status == HL_STATUS_OK);
        HL_CHECK(services.sync->park(services.context, 55, HL_HOST_PARK_PRIVATE, key, &word, 0, 4,
                                     HL_HOST_DEADLINE_INFINITE)
                     .status == HL_STATUS_INTERRUPTED);
        HL_CHECK(services.sync->park(services.context, 55, HL_HOST_PARK_PRIVATE, key, &word, 0, 4, 0).status ==
                 HL_STATUS_TIMED_OUT);
    }
    {
        /* The terminal group. It exists because an object-type field cannot answer whether a live,
         * valid object is a terminal, so the fake models exactly that: one console and one entirely
         * ordinary file, both perfectly good handles. */
        hl_host_terminal_services malformed_terminal;
        hl_host_result console = hl_fake_host_file_create(&fake);
        hl_host_result ordinary = hl_fake_host_file_create(&fake);
        hl_host_terminal_size window = {0, 0, 0, 0};
        hl_host_terminal_size wanted = {100, 40, 0, 0};
        uint32_t mode = 0;
        char bytes[8] = {0};

        HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_TERMINAL) == HL_STATUS_OK);
        HL_CHECK(services.terminal->abi == HL_HOST_TERMINAL_ABI && console.status == HL_STATUS_OK &&
                 ordinary.status == HL_STATUS_OK);

        HL_CHECK(services.terminal->probe(services.context, console.value).value == 1);
        HL_CHECK(services.terminal->probe(services.context, ordinary.value).status == HL_STATUS_OK &&
                 services.terminal->probe(services.context, ordinary.value).value == 0);
        HL_CHECK(services.terminal->probe(services.context, HL_HOST_HANDLE_INVALID).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        /* Wrong kind: a counter handle is a live host object and still not a terminal. */
        {
            hl_host_result counter_handle = services.counter->create(services.context, 0, 0);
            HL_CHECK(counter_handle.status == HL_STATUS_OK);
            HL_CHECK(services.terminal->probe(services.context, counter_handle.value).status ==
                     HL_STATUS_INVALID_ARGUMENT);
            HL_CHECK(services.counter->close(services.context, counter_handle.value).status == HL_STATUS_OK);
        }

        HL_CHECK(services.terminal->get_mode(services.context, console.value, &mode).status == HL_STATUS_OK &&
                 mode == 0);
        HL_CHECK(services.terminal->get_mode(services.context, console.value, NULL).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->get_mode(services.context, ordinary.value, &mode).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal
                     ->set_mode(services.context, console.value,
                                HL_HOST_TERMINAL_RAW_INPUT | HL_HOST_TERMINAL_OUTPUT_PROCESSING)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.terminal->get_mode(services.context, console.value, &mode).status == HL_STATUS_OK &&
                 mode == (HL_HOST_TERMINAL_RAW_INPUT | HL_HOST_TERMINAL_OUTPUT_PROCESSING));
        HL_CHECK(services.terminal->set_mode(services.context, console.value, UINT32_MAX).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->get_mode(services.context, console.value, &mode).status == HL_STATUS_OK &&
                 mode == (HL_HOST_TERMINAL_RAW_INPUT | HL_HOST_TERMINAL_OUTPUT_PROCESSING));

        HL_CHECK(services.terminal->get_size(services.context, console.value, &window).status == HL_STATUS_OK &&
                 window.columns == 80 && window.rows == 24);
        HL_CHECK(services.terminal->get_size(services.context, console.value, NULL).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->set_size(services.context, console.value, &wanted).status == HL_STATUS_OK);
        HL_CHECK(services.terminal->get_size(services.context, console.value, &window).status == HL_STATUS_OK &&
                 window.columns == 100 && window.rows == 40);
        wanted.rows = 0;
        HL_CHECK(services.terminal->set_size(services.context, console.value, &wanted).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->set_size(services.context, console.value, NULL).status ==
                 HL_STATUS_INVALID_ARGUMENT);

        HL_CHECK(services.terminal->write(services.context, console.value, (hl_host_const_bytes){"tty", 3}).value ==
                 3);
        HL_CHECK(services.terminal->read(services.context, console.value, (hl_host_bytes){bytes, sizeof bytes}).value ==
                 3);
        HL_CHECK(memcmp(bytes, "tty", 3) == 0);
        HL_CHECK(services.terminal->read(services.context, console.value, (hl_host_bytes){bytes, sizeof bytes}).value ==
                 0);
        HL_CHECK(services.terminal->write(services.context, ordinary.value, (hl_host_const_bytes){"x", 1}).status ==
                 HL_STATUS_INVALID_ARGUMENT);

        /* The size-change object is independently closeable, and destroying it returns the provider
         * to the count it started from. */
        {
            uint32_t before = fake.live_files;
            hl_host_result notifier = services.terminal->size_change_event(services.context, console.value);
            HL_CHECK(notifier.status == HL_STATUS_OK && fake.live_files == before + 1u);
            HL_CHECK(services.file->close(services.context, notifier.value).status == HL_STATUS_OK &&
                     fake.live_files == before);
        }
        HL_CHECK(services.terminal->size_change_event(services.context, ordinary.value).status ==
                 HL_STATUS_INVALID_ARGUMENT);

        /* Every callback is required, the group ABI is exact, and the pointer must be inside the
         * caller's declared size. */
        malformed_terminal = *services.terminal;
        truncated = services;
        truncated.terminal = &malformed_terminal;
        malformed_terminal.probe = NULL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_ABI_MISMATCH);
        malformed_terminal = *services.terminal;
        malformed_terminal.size_change_event = NULL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_ABI_MISMATCH);
        malformed_terminal = *services.terminal;
        malformed_terminal.abi = HL_HOST_TERMINAL_ABI + 1u;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_ABI_MISMATCH);
        malformed_terminal = *services.terminal;
        malformed_terminal.size = sizeof(malformed_terminal) - 1u;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_ABI_MISMATCH);
        truncated = services;
        truncated.terminal = NULL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_ABI_MISMATCH);
        truncated = services;
        truncated.size = (uint32_t)offsetof(hl_host_services, terminal);
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_ABI_MISMATCH);
        /* A provider that does not claim the capability is not asked for the group at all. */
        truncated = services;
        truncated.terminal = NULL;
        truncated.capabilities &= ~(uint64_t)HL_HOST_CAP_TERMINAL;
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_OK);
        HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_TERMINAL) == HL_STATUS_NOT_SUPPORTED);

        HL_CHECK(services.file->close(services.context, ordinary.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, console.value).status == HL_STATUS_OK);
        HL_CHECK(fake.live_files == 0);
    }
    truncated = services;

    malformed_memory = *services.memory;
    malformed_memory.reserve_code = fake_reserve_code;
    malformed_memory.repair_code_after_fork = fake_repair_code;
    malformed_memory.begin_code_write = NULL;
    truncated = services;
    truncated.memory = &malformed_memory;
    truncated.capabilities |= HL_HOST_CAP_CODE_MAPPING;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CODE_MAPPING) == HL_STATUS_ABI_MISMATCH);
    malformed_memory = *services.memory;
    malformed_memory.repair_signal_page = NULL;
    truncated = services;
    truncated.memory = &malformed_memory;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_MEMORY) == HL_STATUS_ABI_MISMATCH);
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
    malformed_clock = *services.clock;
    malformed_clock.architectural_counter_hz = NULL;
    truncated.clock = &malformed_clock;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_CLOCK) == HL_STATUS_ABI_MISMATCH);
    malformed_clock = *services.clock;
    malformed_clock.backoff_ns = NULL;
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
    /* Validator-only mock: every callback slot starts as a non-null sentinel;
     * no callback is invoked. This isolates each mandatory file ABI tail field. */
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.sync_range = NULL;
    truncated = services;
    truncated.capabilities |= HL_HOST_CAP_FILE;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.sync_filesystem = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.open_beneath = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.allocate_range = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.filesystem_metadata = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.make_directory = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.make_symlink = NULL;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.make_link = NULL;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.make_fifo = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.validate_private_regular = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.store_private_atomic = NULL;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);
    memset(&malformed_file, 0xff, sizeof(malformed_file));
    malformed_file.abi = HL_HOST_FILE_ABI;
    malformed_file.size = sizeof(malformed_file);
    malformed_file.validate_private_directory = NULL;
    truncated.file = &malformed_file;
    HL_CHECK(hl_host_services_validate(&truncated, HL_HOST_CAP_FILE) == HL_STATUS_ABI_MISMATCH);

    truncated = services;
    truncated.size = 8;
    HL_CHECK(hl_host_services_validate(&truncated, 0) == HL_STATUS_ABI_MISMATCH);

    mapping = services.memory->reserve(services.context, 4096, 4096, 0);
    HL_CHECK(mapping.status == HL_STATUS_OK && mapping.value != 0 && fake.live_mappings == 1);
    HL_CHECK(services.memory->publish_code(services.context, mapping.value, 0, 4096).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_mappings == 0);

    {
        hl_host_memory_mapping anonymous = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, 8192, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &anonymous)
                     .status == HL_STATUS_OK);
        HL_CHECK(anonymous.handle != HL_HOST_HANDLE_INVALID && anonymous.address != 0 &&
                 anonymous.mapped_size == 8192 && fake.live_mappings == 1);
        HL_CHECK(services.memory->protect(services.context, anonymous.handle, 0, 4096, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, anonymous.handle).status == HL_STATUS_OK &&
                 fake.live_mappings == 0);
        anonymous = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, 8192, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_SHARED, &anonymous)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, anonymous.handle).status == HL_STATUS_OK &&
                 fake.live_mappings == 0);
        anonymous = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, 8192, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &anonymous)
                     .status == HL_STATUS_OK);
        hl_fake_host_fail_next(&fake, HL_STATUS_OUT_OF_MEMORY);
        HL_CHECK(services.memory->discard(services.context, anonymous.handle).status == HL_STATUS_OK &&
                 fake.live_mappings == 0);
        /* discard is an infallible ownership transition and must not consume
         * fault injection intended for the next allocating operation. */
        anonymous = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, 8192, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &anonymous)
                     .status == HL_STATUS_OUT_OF_MEMORY);
        hl_fake_host_fail_next(&fake, HL_STATUS_OUT_OF_MEMORY);
        anonymous = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, 8192, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &anonymous)
                     .status == HL_STATUS_OUT_OF_MEMORY);
        HL_CHECK(anonymous.handle == HL_HOST_HANDLE_INVALID && fake.live_mappings == 0);
        /* A partial unmap_range keeps its handle -- only a full-range unmap consumes one. Retiring
         * on a partial unmap is what strands a handle over a hole it no longer has. */
        anonymous = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, 8192, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &anonymous)
                     .status == HL_STATUS_OK &&
                 fake.live_mappings == 1);
        HL_CHECK(services.memory->unmap_range(services.context, anonymous.handle, 4096, 4096).status ==
                     HL_STATUS_OK &&
                 fake.live_mappings == 1);
        HL_CHECK(services.memory->unmap_range(services.context, anonymous.handle, 0, 4096).status == HL_STATUS_OK &&
                 fake.live_mappings == 0);
    }

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
    {
        uint64_t notified = 0;
        hl_host_result subscription =
            services.counter->subscribe(services.context, counter.value, fake_counter_notify, &notified, 41);
        HL_CHECK(subscription.status == HL_STATUS_OK);
        HL_CHECK(services.counter->readiness(services.context, counter.value, HL_HOST_READY_READ).value ==
                 HL_HOST_READY_READ);
        HL_CHECK(services.counter->write(services.context, counter.value, 1).status == HL_STATUS_OK && notified == 41);
        HL_CHECK(services.counter->unsubscribe(services.context, subscription.value).status == HL_STATUS_OK);
        notified = 0;
        HL_CHECK(services.counter->write(services.context, counter.value, 1).status == HL_STATUS_OK && notified == 0);
        HL_CHECK(services.counter->close(services.context, counter.value).status == HL_STATUS_OK);
        counter = services.counter->create(services.context, 7, 0);
        HL_CHECK(counter.status == HL_STATUS_OK);
    }
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

    channels = services.transfer->channel_pair(services.context);
    {
        hl_host_result alias = services.transfer->duplicate(services.context, channels.detail);
        HL_CHECK(alias.status == HL_STATUS_OK && fake.live_transfer_channels == 3);
        HL_CHECK(services.transfer->close(services.context, channels.detail).status == HL_STATUS_OK);
        HL_CHECK(
            services.transfer->send(services.context, channels.value, (hl_host_const_bytes){"d", 1}, NULL, 0).status ==
            HL_STATUS_OK);
        HL_CHECK(services.transfer
                         ->receive(services.context, alias.value, (hl_host_bytes){received_data, sizeof(received_data)},
                                   NULL, 0)
                         .status == HL_STATUS_OK &&
                 received_data[0] == 'd');
        HL_CHECK(services.transfer->close(services.context, alias.value).status == HL_STATUS_OK);
        HL_CHECK(services.transfer->close(services.context, channels.value).status == HL_STATUS_OK &&
                 fake.live_transfer_channels == 0);
    }

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
