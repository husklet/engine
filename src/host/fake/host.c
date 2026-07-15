#include "hl/fake.h"

#include <string.h>
#include <sched.h>

static hl_host_result hl_fake_result(hl_fake_host *fake, uint64_t value) {
    hl_host_result result = {HL_STATUS_OK, 0, value, 0};
    if (fake->next_failure != HL_STATUS_OK) {
        result.status = fake->next_failure;
        fake->next_failure = HL_STATUS_OK;
    }
    return result;
}

static hl_host_result hl_fake_reserve(void *context, uint64_t size, uint64_t alignment, uint32_t flags) {
    hl_fake_host *fake = context;
    hl_host_result result;
    (void)alignment;
    (void)flags;
    if (size == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) fake->live_mappings++;
    return result;
}

static hl_host_result hl_fake_protect(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size,
                                      uint32_t flags) {
    (void)offset;
    (void)flags;
    if (mapping == 0 || size == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    return hl_fake_result(context, 0);
}

static hl_host_result hl_fake_release(void *context, hl_host_handle mapping) {
    hl_fake_host *fake = context;
    hl_host_result result;
    if (mapping == 0 || fake->live_mappings == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) fake->live_mappings--;
    return result;
}

static hl_host_result hl_fake_discard(void *context, hl_host_handle mapping) {
    hl_fake_host *fake = context;
    if (mapping == 0 || fake->live_mappings == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->live_mappings--;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static int hl_fake_repair_signal_page(void *context, uint64_t address, uint64_t size, uint32_t protection) {
    (void)context;
    return address != 0 && size == UINT64_C(4096) && (address & UINT64_C(4095)) == 0 &&
           (protection & ~(uint32_t)(HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE | HL_HOST_MEMORY_EXECUTE)) == 0;
}

static hl_host_result hl_fake_publish(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size) {
    return hl_fake_protect(context, mapping, offset, size, 0);
}

static hl_host_result hl_fake_map_file(void *context, hl_host_handle file, uint64_t address, uint64_t offset,
                                       uint64_t size, uint32_t protection, uint32_t flags,
                                       hl_host_file_mapping *output) {
    hl_fake_host *fake = context;
    hl_host_result result;
    (void)offset;
    (void)protection;
    if (file == 0 || size == 0 || output == NULL || output->abi != HL_HOST_FILE_MAPPING_ABI ||
        output->size < sizeof(*output) ||
        ((flags & (HL_HOST_MEMORY_SHARED | HL_HOST_MEMORY_PRIVATE)) != HL_HOST_MEMORY_SHARED &&
         (flags & (HL_HOST_MEMORY_SHARED | HL_HOST_MEMORY_PRIVATE)) != HL_HOST_MEMORY_PRIVATE))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status != HL_STATUS_OK) return result;
    fake->live_mappings++;
    output->handle = result.value;
    output->address = address != 0 ? address : UINT64_C(0x10000000) + result.value * UINT64_C(0x10000);
    output->mapped_size = size;
    output->reserved = 0;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result hl_fake_map_anonymous(void *context, uint64_t address, uint64_t size, uint32_t protection,
                                            uint32_t flags, hl_host_memory_mapping *output) {
    hl_fake_host *fake = context;
    hl_host_result result;
    (void)protection;
    if (size == 0 || output == NULL || output->abi != HL_HOST_MEMORY_MAPPING_ABI || output->size < sizeof(*output) ||
        ((flags & (HL_HOST_MEMORY_SHARED | HL_HOST_MEMORY_PRIVATE)) != HL_HOST_MEMORY_SHARED &&
         (flags & (HL_HOST_MEMORY_SHARED | HL_HOST_MEMORY_PRIVATE)) != HL_HOST_MEMORY_PRIVATE))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status != HL_STATUS_OK) return result;
    fake->live_mappings++;
    *output = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(*output), result.value,
                                       address != 0 ? address : UINT64_C(0x20000000) + result.value * UINT64_C(0x10000),
                                       size, 0};
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result hl_fake_mapping_sync(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size) {
    return hl_fake_protect(context, mapping, offset, size, 0);
}

static hl_host_result hl_fake_unmap_range(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size) {
    hl_fake_host *fake = context;
    (void)offset;
    if (mapping == 0 || size == 0 || fake->live_mappings == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    hl_host_result result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) fake->live_mappings--;
    return result;
}

static hl_host_result hl_fake_monotonic(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->monotonic_ns++);
}

static hl_host_result hl_fake_realtime(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->realtime_ns++);
}

static hl_host_result hl_fake_raw_monotonic(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->raw_monotonic_ns++);
}

static hl_host_result hl_fake_process_cpu(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->process_cpu_ns++);
}

static hl_host_result hl_fake_thread_cpu(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->thread_cpu_ns++);
}

static hl_host_result hl_fake_architectural_counter(void *context) {
    hl_fake_host *fake = context;
    if (fake->architectural_counter_hz == 0) return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, 0, 0};
    return hl_fake_result(fake, fake->architectural_counter_hz);
}

static hl_host_result hl_fake_backoff(void *context, uint64_t interval_ns) {
    hl_fake_host *fake = context;
    hl_host_result result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) {
        if (UINT64_MAX - fake->monotonic_ns < interval_ns)
            fake->monotonic_ns = UINT64_MAX;
        else
            fake->monotonic_ns += interval_ns;
    }
    return result;
}

static hl_host_result hl_fake_sleep_until(void *context, uint32_t clock_kind, uint64_t deadline_ns) {
    hl_fake_host *fake = context;
    uint64_t *clock;
    switch (clock_kind) {
    case HL_HOST_CLOCK_MONOTONIC: clock = &fake->monotonic_ns; break;
    case HL_HOST_CLOCK_REALTIME: clock = &fake->realtime_ns; break;
    case HL_HOST_CLOCK_RAW_MONOTONIC: clock = &fake->raw_monotonic_ns; break;
    default: return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, 0, 0};
    }
    hl_host_result result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK && *clock < deadline_ns) *clock = deadline_ns;
    return result;
}

static hl_host_result hl_fake_spawn_cloned(void *context, hl_host_process_entry entry, void *entry_context) {
    hl_fake_host *fake = context;
    hl_host_result result;
    if (entry == NULL) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    (void)entry_context;
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) __atomic_add_fetch(&fake->live_processes, 1, __ATOMIC_RELEASE);
    return result;
}

static hl_host_result hl_fake_process_wait(void *context, hl_host_handle process, uint64_t deadline_ns) {
    hl_fake_host *fake = context;
    hl_host_result result;
    (void)deadline_ns;
    if (process == 0 || __atomic_load_n(&fake->live_processes, __ATOMIC_ACQUIRE) == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    while (__atomic_load_n(&fake->process_block_wait, __ATOMIC_ACQUIRE) != 0)
        sched_yield();
    result = hl_fake_result(fake, (uint64_t)(uint32_t)fake->process_exit_value);
    result.detail = fake->process_exit_kind;
    if (result.status == HL_STATUS_OK) fake->process_waited = 1;
    return result;
}

static hl_host_result hl_fake_process_terminate(void *context, hl_host_handle process, uint32_t reason) {
    hl_fake_host *fake = context;
    if (process == 0 || __atomic_load_n(&fake->live_processes, __ATOMIC_ACQUIRE) == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (reason != HL_HOST_PROCESS_TERMINATE_INTERRUPT && reason != HL_HOST_PROCESS_TERMINATE_FORCE)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->process_exit_kind = HL_HOST_PROCESS_EXIT_SIGNAL;
    fake->process_exit_value = reason == HL_HOST_PROCESS_TERMINATE_INTERRUPT ? 2 : 9;
    __atomic_store_n(&fake->process_block_wait, 0, __ATOMIC_RELEASE);
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_process_close(void *context, hl_host_handle process) {
    hl_fake_host *fake = context;
    hl_host_result result;
    if (process == 0 || fake->live_processes == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (!fake->process_waited) return (hl_host_result){HL_STATUS_BUSY, 0, 0, 0};
    result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) {
        __atomic_sub_fetch(&fake->live_processes, 1, __ATOMIC_RELEASE);
        fake->process_waited = 0;
    }
    return result;
}

static hl_host_result hl_fake_mutex_create(void *context) {
    hl_fake_host *fake = context;
    uint32_t index;
    for (index = 0; index < 64 && fake->mutex_handles[index] != 0; ++index) {}
    if (index == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    hl_host_result result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) {
        fake->mutex_handles[index] = result.value;
        fake->live_mutexes++;
    }
    return result;
}

static int hl_fake_mutex_index(const hl_fake_host *fake, hl_host_handle mutex) {
    uint32_t index;
    if (mutex == HL_HOST_HANDLE_INVALID) return -1;
    for (index = 0; index < 64; ++index)
        if (fake->mutex_handles[index] == mutex) return (int)index;
    return -1;
}

static hl_host_result hl_fake_mutex_lock(void *context, hl_host_handle mutex) {
    hl_fake_host *fake = context;
    int index = hl_fake_mutex_index(fake, mutex);
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    hl_host_result result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) {
        uint8_t expected;
        do {
            expected = 0;
            if (__atomic_compare_exchange_n(&fake->mutex_locked[index], &expected, 1, 0, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                break;
            sched_yield();
        } while (1);
    }
    return result;
}

static hl_host_result hl_fake_mutex_unlock(void *context, hl_host_handle mutex) {
    hl_fake_host *fake = context;
    int index = hl_fake_mutex_index(fake, mutex);
    if (index < 0 || __atomic_load_n(&fake->mutex_locked[index], __ATOMIC_ACQUIRE) == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    hl_host_result result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) __atomic_store_n(&fake->mutex_locked[index], 0, __ATOMIC_RELEASE);
    return result;
}

static hl_host_result hl_fake_mutex_close(void *context, hl_host_handle mutex) {
    hl_fake_host *fake = context;
    int index = hl_fake_mutex_index(fake, mutex);
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (__atomic_load_n(&fake->mutex_locked[index], __ATOMIC_ACQUIRE) != 0)
        return (hl_host_result){HL_STATUS_BUSY, 0, 0, 0};
    hl_host_result result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) {
        fake->mutex_handles[index] = 0;
        fake->live_mutexes--;
    }
    return result;
}

static hl_host_result hl_fake_fork_lifecycle(void *context) {
    (void)context;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static int hl_fake_counter_handle_index(const hl_fake_host *fake, hl_host_handle handle) {
    uint32_t index;
    for (index = 0; index < 64; ++index)
        if (fake->counter_handles[index] == handle) return (int)index;
    return -1;
}

static hl_host_result hl_fake_counter_create(void *context, uint64_t initial, uint32_t flags) {
    hl_fake_host *fake = context;
    uint32_t handle_index;
    uint32_t object_index;
    hl_host_result result;
    if (initial == UINT64_MAX || (flags & ~(uint32_t)(HL_HOST_COUNTER_SEMAPHORE | HL_HOST_COUNTER_NONBLOCK)) != 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (handle_index = 0; handle_index < 64 && fake->counter_handles[handle_index] != 0; ++handle_index) {}
    for (object_index = 0; object_index < 64 && fake->counter_references[object_index] != 0; ++object_index) {}
    if (handle_index == 64 || object_index == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) {
        fake->counter_handles[handle_index] = result.value;
        fake->counter_objects[handle_index] = (uint8_t)object_index;
        fake->counter_rights[handle_index] =
            HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WRITE | HL_HOST_TRANSFER_WAIT | HL_HOST_TRANSFER_CONTROL;
        fake->counter_values[object_index] = initial;
        fake->counter_flags[object_index] = flags;
        fake->counter_references[object_index] = 1;
        fake->live_counters++;
    }
    return result;
}

static hl_host_result hl_fake_counter_read(void *context, hl_host_handle handle) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    uint32_t object;
    uint64_t value;
    hl_host_result result;
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if ((fake->counter_rights[index] & HL_HOST_TRANSFER_READ) == 0)
        return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    object = fake->counter_objects[index];
    value = fake->counter_values[object];
    if (value == 0) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    result = hl_fake_result(fake, value);
    if (result.status != HL_STATUS_OK) return result;
    if ((fake->counter_flags[object] & HL_HOST_COUNTER_SEMAPHORE) != 0) {
        fake->counter_values[object]--;
        result.value = 1;
    } else {
        fake->counter_values[object] = 0;
    }
    return result;
}

static hl_host_result hl_fake_counter_write(void *context, hl_host_handle handle, uint64_t value) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    uint32_t object;
    hl_host_result result;
    if (index < 0 || value == UINT64_MAX) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if ((fake->counter_rights[index] & HL_HOST_TRANSFER_WRITE) == 0)
        return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    object = fake->counter_objects[index];
    if (value > UINT64_MAX - 1 - fake->counter_values[object]) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    result = hl_fake_result(fake, 0);
    if (result.status != HL_STATUS_OK) return result;
    fake->counter_values[object] += value;
    for (uint32_t subscription = 0; subscription < 64; ++subscription)
        if (fake->counter_subscription_handles[subscription] != 0 &&
            fake->counter_subscription_counters[subscription] == handle)
            fake->counter_subscription_notify[subscription](fake->counter_subscription_observers[subscription],
                                                            fake->counter_subscription_tokens[subscription]);
    return result;
}

static hl_host_result hl_fake_counter_get_flags(void *context, hl_host_handle handle) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if ((fake->counter_rights[index] & HL_HOST_TRANSFER_CONTROL) == 0)
        return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    return hl_fake_result(fake, fake->counter_flags[fake->counter_objects[index]]);
}

static hl_host_result hl_fake_counter_set_flags(void *context, hl_host_handle handle, uint32_t flags) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    uint32_t object;
    if (index < 0 || (flags & ~(uint32_t)(HL_HOST_COUNTER_SEMAPHORE | HL_HOST_COUNTER_NONBLOCK)) != 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if ((fake->counter_rights[index] & HL_HOST_TRANSFER_CONTROL) == 0)
        return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    object = fake->counter_objects[index];
    if ((flags & HL_HOST_COUNTER_SEMAPHORE) != (fake->counter_flags[object] & HL_HOST_COUNTER_SEMAPHORE))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->counter_flags[object] = flags;
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_counter_duplicate(void *context, hl_host_handle handle) {
    hl_fake_host *fake = context;
    int source = hl_fake_counter_handle_index(fake, handle);
    uint32_t index;
    uint32_t object;
    hl_host_result result;
    if (source < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (index = 0; index < 64 && fake->counter_handles[index] != 0; ++index) {}
    if (index == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) {
        object = fake->counter_objects[source];
        fake->counter_handles[index] = result.value;
        fake->counter_objects[index] = (uint8_t)object;
        fake->counter_rights[index] = fake->counter_rights[source];
        fake->counter_references[object]++;
    }
    return result;
}

static hl_host_result hl_fake_counter_close(void *context, hl_host_handle handle) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    uint32_t object;
    hl_host_result result;
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, 0);
    if (result.status != HL_STATUS_OK) return result;
    for (uint32_t subscription = 0; subscription < 64; ++subscription)
        if (fake->counter_subscription_counters[subscription] == handle) {
            fake->counter_subscription_handles[subscription] = 0;
            fake->counter_subscription_counters[subscription] = 0;
            fake->counter_subscription_notify[subscription] = NULL;
            fake->counter_subscription_observers[subscription] = NULL;
            fake->counter_subscription_tokens[subscription] = 0;
        }
    object = fake->counter_objects[index];
    fake->counter_handles[index] = 0;
    fake->counter_objects[index] = 0;
    fake->counter_rights[index] = 0;
    if (--fake->counter_references[object] == 0) {
        fake->counter_values[object] = 0;
        fake->counter_flags[object] = 0;
        fake->live_counters--;
    }
    return result;
}

static hl_host_result hl_fake_counter_readiness(void *context, hl_host_handle handle, uint32_t interests) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    uint32_t readiness = 0;
    if (index < 0 || (interests & ~(uint32_t)HL_HOST_READY_READ) != 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if ((fake->counter_rights[index] & HL_HOST_TRANSFER_WAIT) == 0)
        return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    if (fake->counter_values[fake->counter_objects[index]] != 0) readiness = HL_HOST_READY_READ;
    return hl_fake_result(fake, readiness & interests);
}

static hl_host_result hl_fake_counter_subscribe(void *context, hl_host_handle handle, void (*notify)(void *, uint64_t),
                                                void *observer, uint64_t token) {
    hl_fake_host *fake = context;
    int index = hl_fake_counter_handle_index(fake, handle);
    uint32_t slot;
    hl_host_result result;
    if (index < 0 || notify == NULL || token == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if ((fake->counter_rights[index] & HL_HOST_TRANSFER_WAIT) == 0)
        return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    for (slot = 0; slot < 64 && fake->counter_subscription_handles[slot] != 0; ++slot) {}
    if (slot == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) {
        fake->counter_subscription_handles[slot] = result.value;
        fake->counter_subscription_counters[slot] = handle;
        fake->counter_subscription_notify[slot] = notify;
        fake->counter_subscription_observers[slot] = observer;
        fake->counter_subscription_tokens[slot] = token;
    }
    return result;
}

static hl_host_result hl_fake_counter_unsubscribe(void *context, hl_host_handle handle) {
    hl_fake_host *fake = context;
    uint32_t slot;
    for (slot = 0; slot < 64 && fake->counter_subscription_handles[slot] != handle; ++slot) {}
    if (slot == 64) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->counter_subscription_handles[slot] = 0;
    fake->counter_subscription_counters[slot] = 0;
    fake->counter_subscription_notify[slot] = NULL;
    fake->counter_subscription_observers[slot] = NULL;
    fake->counter_subscription_tokens[slot] = 0;
    return hl_fake_result(fake, 0);
}

static int hl_fake_transfer_channel_index(const hl_fake_host *fake, hl_host_handle channel) {
    uint32_t index;
    for (index = 0; index < 64; ++index)
        if (fake->transfer_channels[index] == channel) return (int)index;
    return -1;
}

static void hl_fake_transfer_drop_message(hl_fake_host *fake, uint32_t index) {
    uint32_t attachment;
    for (attachment = 0; attachment < fake->transfer_attachment_counts[index]; ++attachment) {
        uint32_t object = fake->transfer_objects[index][attachment];
        if (--fake->counter_references[object] == 0) {
            fake->counter_values[object] = 0;
            fake->counter_flags[object] = 0;
            fake->live_counters--;
        }
    }
    fake->transfer_message_pending[index] = 0;
    fake->transfer_data_sizes[index] = 0;
    fake->transfer_attachment_counts[index] = 0;
}

static hl_host_result hl_fake_transfer_channel_pair(void *context) {
    hl_fake_host *fake = context;
    uint32_t first;
    uint32_t second;
    hl_host_result result;
    for (first = 0; first < 64 && fake->transfer_channels[first] != 0; ++first) {}
    for (second = first + 1; second < 64 && fake->transfer_channels[second] != 0; ++second) {}
    if (first == 64 || second == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status != HL_STATUS_OK) return result;
    fake->transfer_channels[first] = result.value;
    fake->transfer_endpoints[first] = (uint8_t)first;
    result.detail = ++fake->next_handle;
    fake->transfer_channels[second] = result.detail;
    fake->transfer_endpoints[second] = (uint8_t)second;
    fake->transfer_peers[first] = (uint8_t)second;
    fake->transfer_peers[second] = (uint8_t)first;
    fake->transfer_references[first] = 1;
    fake->transfer_references[second] = 1;
    fake->live_transfer_channels += 2;
    return result;
}

static hl_host_result hl_fake_transfer_send(void *context, hl_host_handle channel, hl_host_const_bytes data,
                                            const hl_host_transfer_attachment *attachments, uint32_t attachment_count) {
    hl_fake_host *fake = context;
    int source = hl_fake_transfer_channel_index(fake, channel);
    uint32_t destination;
    uint32_t index;
    hl_host_result result;
    const uint32_t all_rights =
        HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WRITE | HL_HOST_TRANSFER_WAIT | HL_HOST_TRANSFER_CONTROL;
    if (source < 0 || data.size > HL_HOST_TRANSFER_MAX_DATA || (data.size != 0 && data.data == NULL) ||
        attachment_count > HL_HOST_TRANSFER_MAX_ATTACHMENTS || (attachment_count != 0 && attachments == NULL))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    source = fake->transfer_endpoints[source];
    destination = fake->transfer_peers[source];
    if (fake->transfer_references[destination] == 0) return (hl_host_result){HL_STATUS_NOT_FOUND, 0, 0, 0};
    if (fake->transfer_message_pending[destination]) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    for (index = 0; index < attachment_count; ++index) {
        int handle = hl_fake_counter_handle_index(fake, attachments[index].object);
        if (handle < 0 || attachments[index].kind != HL_HOST_TRANSFER_KIND_COUNTER || attachments[index].rights == 0 ||
            (attachments[index].rights & ~all_rights) != 0 ||
            (attachments[index].rights & ~fake->counter_rights[handle]) != 0)
            return (hl_host_result){HL_STATUS_PERMISSION_DENIED, 0, 0, 0};
    }
    result = hl_fake_result(fake, data.size);
    if (result.status != HL_STATUS_OK) return result;
    if (data.size != 0) memcpy(fake->transfer_data[destination], data.data, data.size);
    fake->transfer_data_sizes[destination] = (uint16_t)data.size;
    fake->transfer_attachment_counts[destination] = (uint8_t)attachment_count;
    for (index = 0; index < attachment_count; ++index) {
        int handle = hl_fake_counter_handle_index(fake, attachments[index].object);
        uint32_t object = fake->counter_objects[handle];
        fake->transfer_objects[destination][index] = (uint8_t)object;
        fake->transfer_rights[destination][index] = attachments[index].rights;
        fake->counter_references[object]++;
    }
    fake->transfer_message_pending[destination] = 1;
    result.detail = attachment_count;
    return result;
}

static hl_host_result hl_fake_transfer_receive(void *context, hl_host_handle channel, hl_host_bytes data,
                                               hl_host_transfer_attachment *attachments, uint32_t attachment_capacity) {
    hl_fake_host *fake = context;
    int channel_index = hl_fake_transfer_channel_index(fake, channel);
    uint32_t count;
    uint32_t slots[HL_HOST_TRANSFER_MAX_ATTACHMENTS];
    uint32_t index;
    hl_host_result result;
    if (channel_index < 0 || (data.size != 0 && data.data == NULL) || (attachment_capacity != 0 && attachments == NULL))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    channel_index = fake->transfer_endpoints[channel_index];
    if (!fake->transfer_message_pending[channel_index]) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    count = fake->transfer_attachment_counts[channel_index];
    if (data.size < fake->transfer_data_sizes[channel_index] || attachment_capacity < count)
        return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, fake->transfer_data_sizes[channel_index], count};
    for (index = 0; index < count; ++index) {
        uint32_t slot;
        for (slot = index == 0 ? 0 : slots[index - 1] + 1; slot < 64 && fake->counter_handles[slot] != 0; ++slot) {}
        if (slot == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
        slots[index] = slot;
    }
    result = hl_fake_result(fake, fake->transfer_data_sizes[channel_index]);
    if (result.status != HL_STATUS_OK) return result;
    if (result.value != 0) memcpy(data.data, fake->transfer_data[channel_index], result.value);
    for (index = 0; index < count; ++index) {
        uint32_t slot = slots[index];
        fake->counter_handles[slot] = ++fake->next_handle;
        fake->counter_objects[slot] = fake->transfer_objects[channel_index][index];
        fake->counter_rights[slot] = fake->transfer_rights[channel_index][index];
        attachments[index].object = fake->counter_handles[slot];
        attachments[index].kind = HL_HOST_TRANSFER_KIND_COUNTER;
        attachments[index].rights = fake->counter_rights[slot];
    }
    fake->transfer_message_pending[channel_index] = 0;
    fake->transfer_data_sizes[channel_index] = 0;
    fake->transfer_attachment_counts[channel_index] = 0;
    result.detail = count;
    return result;
}

static hl_host_result hl_fake_transfer_close(void *context, hl_host_handle channel) {
    hl_fake_host *fake = context;
    int index = hl_fake_transfer_channel_index(fake, channel);
    hl_host_result result;
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, 0);
    if (result.status != HL_STATUS_OK) return result;
    uint32_t endpoint = fake->transfer_endpoints[index];
    fake->transfer_channels[index] = 0;
    fake->transfer_endpoints[index] = 0;
    if (--fake->transfer_references[endpoint] == 0) {
        if (fake->transfer_message_pending[endpoint]) hl_fake_transfer_drop_message(fake, endpoint);
        fake->transfer_peers[endpoint] = 0;
    }
    fake->live_transfer_channels--;
    return result;
}

static hl_host_result hl_fake_transfer_duplicate(void *context, hl_host_handle channel) {
    hl_fake_host *fake = context;
    int source = hl_fake_transfer_channel_index(fake, channel);
    uint32_t slot;
    hl_host_result result;
    if (source < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (slot = 0; slot < 64 && fake->transfer_channels[slot] != 0; ++slot) {}
    if (slot == 64) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) {
        uint32_t endpoint = fake->transfer_endpoints[source];
        fake->transfer_channels[slot] = result.value;
        fake->transfer_endpoints[slot] = (uint8_t)endpoint;
        fake->transfer_references[endpoint]++;
        fake->live_transfer_channels++;
    }
    return result;
}

static hl_host_result hl_fake_begin_code_write(void *context) {
    ((hl_fake_host *)context)->code_write_begins++;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result hl_fake_end_code_write(void *context) {
    ((hl_fake_host *)context)->code_write_ends++;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static int hl_fake_directory_handle(const hl_fake_host *fake, hl_host_handle handle) {
    uint32_t index;
    for (index = 0; index < 16; ++index)
        if (fake->directory_handles[index] == handle) return (int)index;
    return -1;
}

static hl_host_result hl_fake_directory_create(void *context) {
    hl_fake_host *fake = context;
    uint32_t handle;
    uint32_t object;
    for (handle = 0; handle < 16 && fake->directory_handles[handle] != 0; ++handle) {}
    for (object = 0; object < 16 && fake->directory_references[object] != 0; ++object) {}
    if (handle == 16 || object == 16) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    fake->directory_handles[handle] = ++fake->next_handle;
    fake->directory_objects[handle] = (uint8_t)object;
    fake->directory_references[object] = 1;
    return hl_fake_result(fake, fake->directory_handles[handle]);
}

static hl_host_result hl_fake_directory_add(void *context, hl_host_handle instance, hl_host_handle file, uint64_t token,
                                            uint32_t interests) {
    hl_fake_host *fake = context;
    int handle = hl_fake_directory_handle(fake, instance);
    uint32_t watch;
    uint32_t object;
    if (handle < 0 || file == 0 || token == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    object = fake->directory_objects[handle];
    for (watch = 0; watch < 16 && fake->directory_tokens[object][watch] != 0; ++watch) {
        if (fake->directory_tokens[object][watch] == token) return (hl_host_result){HL_STATUS_ALREADY_EXISTS, 0, 0, 0};
    }
    if (watch == 16) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    fake->directory_tokens[object][watch] = token;
    fake->directory_interests[object][watch] = interests;
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_directory_modify(void *context, hl_host_handle instance, uint64_t token,
                                               uint32_t interests) {
    hl_fake_host *fake = context;
    int handle = hl_fake_directory_handle(fake, instance);
    uint32_t watch;
    uint32_t object;
    if (handle < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    object = fake->directory_objects[handle];
    for (watch = 0; watch < 16; ++watch)
        if (fake->directory_tokens[object][watch] == token) {
            fake->directory_interests[object][watch] = interests;
            return hl_fake_result(fake, 0);
        }
    return (hl_host_result){HL_STATUS_NOT_FOUND, 0, 0, 0};
}

static hl_host_result hl_fake_directory_remove(void *context, hl_host_handle instance, uint64_t token) {
    hl_fake_host *fake = context;
    int handle = hl_fake_directory_handle(fake, instance);
    uint32_t watch;
    uint32_t object;
    if (handle < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    object = fake->directory_objects[handle];
    for (watch = 0; watch < 16; ++watch)
        if (fake->directory_tokens[object][watch] == token) {
            fake->directory_tokens[object][watch] = 0;
            fake->directory_interests[object][watch] = 0;
            return hl_fake_result(fake, 0);
        }
    return (hl_host_result){HL_STATUS_NOT_FOUND, 0, 0, 0};
}

static hl_host_result hl_fake_directory_read(void *context, hl_host_handle instance, hl_host_directory_record *records,
                                             uint32_t capacity) {
    hl_fake_host *fake = context;
    int handle = hl_fake_directory_handle(fake, instance);
    uint32_t object;
    uint32_t count;
    if (handle < 0 || records == NULL || capacity == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    object = fake->directory_objects[handle];
    count = capacity < fake->directory_record_counts[object] ? capacity : fake->directory_record_counts[object];
    if (count == 0) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    memcpy(records, fake->directory_records[object], count * sizeof(*records));
    fake->directory_record_counts[object] = (uint8_t)(fake->directory_record_counts[object] - count);
    memmove(fake->directory_records[object], fake->directory_records[object] + count,
            fake->directory_record_counts[object] * sizeof(*records));
    return hl_fake_result(fake, count);
}

static hl_host_result hl_fake_directory_duplicate(void *context, hl_host_handle instance) {
    hl_fake_host *fake = context;
    int source = hl_fake_directory_handle(fake, instance);
    uint32_t handle;
    uint32_t object;
    if (source < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (handle = 0; handle < 16 && fake->directory_handles[handle] != 0; ++handle) {}
    if (handle == 16) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    object = fake->directory_objects[source];
    fake->directory_handles[handle] = ++fake->next_handle;
    fake->directory_objects[handle] = (uint8_t)object;
    fake->directory_references[object]++;
    return hl_fake_result(fake, fake->directory_handles[handle]);
}

static hl_host_result hl_fake_directory_close(void *context, hl_host_handle instance) {
    hl_fake_host *fake = context;
    int handle = hl_fake_directory_handle(fake, instance);
    uint32_t object;
    if (handle < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    object = fake->directory_objects[handle];
    fake->directory_handles[handle] = 0;
    if (--fake->directory_references[object] == 0) {
        memset(fake->directory_tokens[object], 0, sizeof(fake->directory_tokens[object]));
        memset(fake->directory_interests[object], 0, sizeof(fake->directory_interests[object]));
        fake->directory_record_counts[object] = 0;
    }
    return hl_fake_result(fake, 0);
}

void hl_fake_host_directory_emit(hl_fake_host *fake, uint64_t token, uint32_t changes) {
    uint32_t object;
    for (object = 0; object < 16; ++object) {
        uint32_t watch;
        for (watch = 0; watch < 16; ++watch) {
            uint8_t count;
            uint32_t delivered;
            if (fake->directory_tokens[object][watch] != token) continue;
            delivered = changes & fake->directory_interests[object][watch] & ~HL_HOST_DIRECTORY_ONESHOT;
            count = fake->directory_record_counts[object];
            if (delivered != 0 && count < 64)
                fake->directory_records[object][fake->directory_record_counts[object]++] =
                    (hl_host_directory_record){token, delivered, 0};
            if ((fake->directory_interests[object][watch] & HL_HOST_DIRECTORY_ONESHOT) != 0) {
                count = fake->directory_record_counts[object];
                if (count < 64)
                    fake->directory_records[object][fake->directory_record_counts[object]++] =
                        (hl_host_directory_record){token, HL_HOST_DIRECTORY_IGNORED, 0};
                fake->directory_tokens[object][watch] = 0;
                fake->directory_interests[object][watch] = 0;
            }
            break;
        }
    }
}

static int hl_fake_watch_index(const hl_fake_host *fake, hl_host_handle handle) {
    for (uint32_t index = 0; index < 16; ++index)
        if (fake->watch_handles[index] == handle) return (int)index;
    return -1;
}

static hl_host_result hl_fake_watch_open(void *context, hl_host_handle file) {
    hl_fake_host *fake = context;
    uint32_t index;
    if (file == HL_HOST_HANDLE_INVALID) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (index = 0; index < 16 && fake->watch_handles[index] != 0; ++index) {}
    if (index == 16) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    fake->watch_handles[index] = ++fake->next_handle;
    fake->watch_files[index] = file;
    fake->watch_records[index].generation = 1;
    fake->watch_delivered[index] = 1;
    return hl_fake_result(fake, fake->watch_handles[index]);
}

static hl_host_result hl_fake_watch_query(void *context, hl_host_handle handle, hl_host_watch_record *record) {
    hl_fake_host *fake = context;
    int index = hl_fake_watch_index(fake, handle);
    if (index < 0 || record == NULL) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    *record = fake->watch_records[index];
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_watch_drain(void *context, hl_host_handle handle, hl_host_watch_record *records,
                                          size_t capacity) {
    hl_fake_host *fake = context;
    int index = hl_fake_watch_index(fake, handle);
    if (index < 0 || records == NULL || capacity == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (fake->watch_delivered[index] == fake->watch_records[index].generation)
        return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    records[0] = fake->watch_records[index];
    fake->watch_delivered[index] = fake->watch_records[index].generation;
    return hl_fake_result(fake, 1);
}

static hl_host_result hl_fake_watch_close(void *context, hl_host_handle handle) {
    hl_fake_host *fake = context;
    int index = hl_fake_watch_index(fake, handle);
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->watch_handles[index] = 0;
    fake->watch_files[index] = 0;
    memset(&fake->watch_records[index], 0, sizeof(fake->watch_records[index]));
    return hl_fake_result(fake, 0);
}

static int hl_fake_stream_index(const hl_fake_host *fake, hl_host_handle handle) {
    uint32_t index;
    for (index = 0; index < 16; ++index)
        if (fake->stream_handles[index] == handle) return (int)index;
    return -1;
}

static hl_host_result hl_fake_stream_pipe_pair(void *context, uint32_t flags) {
    hl_fake_host *fake = context;
    uint32_t first, second, object;
    if ((flags & ~(uint32_t)HL_HOST_STREAM_NONBLOCK) != 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (first = 0; first < 16 && fake->stream_handles[first] != 0; ++first) {}
    for (second = first + 1; second < 16 && fake->stream_handles[second] != 0; ++second) {}
    for (object = 0; object < 8 && fake->stream_sizes[object] != 0; ++object) {}
    if (first == 16 || second == 16 || object == 8)
        return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    fake->stream_handles[first] = ++fake->next_handle;
    fake->stream_handles[second] = ++fake->next_handle;
    fake->stream_objects[first] = (uint8_t)object;
    fake->stream_objects[second] = (uint8_t)object;
    fake->stream_write_ends[second] = 1;
    fake->stream_flags[first] = flags;
    fake->stream_flags[second] = flags;
    /* UINT16_MAX marks an allocated empty stream. */
    fake->stream_sizes[object] = UINT16_MAX;
    return (hl_host_result){HL_STATUS_OK, 0, fake->stream_handles[first], fake->stream_handles[second]};
}

static hl_host_result hl_fake_stream_set_status_flags(void *context, hl_host_handle stream, uint32_t flags) {
    hl_fake_host *fake = context;
    int index = hl_fake_stream_index(fake, stream);
    if (index < 0 || (flags & ~(uint32_t)HL_HOST_STREAM_NONBLOCK) != 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->stream_flags[index] = flags;
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_stream_read(void *context, hl_host_handle stream, hl_host_bytes output) {
    hl_fake_host *fake = context;
    int index = hl_fake_stream_index(fake, stream);
    uint32_t size, count;
    if (index < 0 || fake->stream_write_ends[index] || (output.size != 0 && output.data == NULL))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    size = fake->stream_sizes[fake->stream_objects[index]];
    size = size == UINT16_MAX ? 0 : size;
    if (size == 0) {
        uint32_t peer;
        for (peer = 0; peer < 16; ++peer)
            if (fake->stream_handles[peer] != 0 &&
                fake->stream_objects[peer] == fake->stream_objects[index] && fake->stream_write_ends[peer])
                break;
        return peer == 16 ? hl_fake_result(fake, 0) : (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    }
    count = output.size < size ? (uint32_t)output.size : size;
    memcpy(output.data, fake->stream_data[fake->stream_objects[index]], count);
    memmove(fake->stream_data[fake->stream_objects[index]], fake->stream_data[fake->stream_objects[index]] + count,
            size - count);
    fake->stream_sizes[fake->stream_objects[index]] = size == count ? UINT16_MAX : (uint16_t)(size - count);
    return hl_fake_result(fake, count);
}

static hl_host_result hl_fake_stream_write(void *context, hl_host_handle stream, hl_host_const_bytes input) {
    hl_fake_host *fake = context;
    int index = hl_fake_stream_index(fake, stream);
    uint32_t size, count;
    if (index < 0 || !fake->stream_write_ends[index] || (input.size != 0 && input.data == NULL))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    {
        uint32_t peer;
        for (peer = 0; peer < 16; ++peer)
            if (fake->stream_handles[peer] != 0 &&
                fake->stream_objects[peer] == fake->stream_objects[index] && !fake->stream_write_ends[peer])
                break;
        if (peer == 16) return (hl_host_result){HL_STATUS_DISCONNECTED, 0, 0, 0};
    }
    size = fake->stream_sizes[fake->stream_objects[index]];
    size = size == UINT16_MAX ? 0 : size;
    count = input.size < 1024u - size ? (uint32_t)input.size : 1024u - size;
    if (count == 0 && input.size != 0) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    memcpy(fake->stream_data[fake->stream_objects[index]] + size, input.data, count);
    fake->stream_sizes[fake->stream_objects[index]] = (uint16_t)(size + count);
    return hl_fake_result(fake, count);
}

static hl_host_result hl_fake_stream_duplicate(void *context, hl_host_handle stream) {
    hl_fake_host *fake = context;
    int source = hl_fake_stream_index(fake, stream);
    uint32_t slot;
    if (source < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    for (slot = 0; slot < 16 && fake->stream_handles[slot] != 0; ++slot) {}
    if (slot == 16) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    fake->stream_handles[slot] = ++fake->next_handle;
    fake->stream_objects[slot] = fake->stream_objects[source];
    fake->stream_write_ends[slot] = fake->stream_write_ends[source];
    fake->stream_flags[slot] = fake->stream_flags[source];
    return hl_fake_result(fake, fake->stream_handles[slot]);
}

static hl_host_result hl_fake_stream_close(void *context, hl_host_handle stream) {
    hl_fake_host *fake = context;
    int index = hl_fake_stream_index(fake, stream);
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    uint8_t object = fake->stream_objects[index];
    fake->stream_handles[index] = 0;
    fake->stream_objects[index] = 0;
    fake->stream_write_ends[index] = 0;
    for (uint32_t rest = 0; rest < 16; ++rest)
        if (fake->stream_handles[rest] != 0 && fake->stream_objects[rest] == object) return hl_fake_result(fake, 0);
    fake->stream_sizes[object] = 0;
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_stream_readiness(void *context, hl_host_handle stream, uint32_t interests) {
    hl_fake_host *fake = context;
    int index = hl_fake_stream_index(fake, stream);
    uint32_t ready = 0, size;
    if (index < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    size = fake->stream_sizes[fake->stream_objects[index]];
    size = size == UINT16_MAX ? 0 : size;
    if (!fake->stream_write_ends[index] && size != 0) ready |= HL_HOST_READY_READ;
    if (fake->stream_write_ends[index] && size < 1024) ready |= HL_HOST_READY_WRITE;
    return hl_fake_result(fake, ready & interests);
}

static hl_host_result hl_fake_stream_move(void *context, hl_host_handle source, uint64_t source_offset,
                                          hl_host_handle destination, uint64_t destination_offset, uint64_t size,
                                          uint32_t flags) {
    hl_fake_host *fake = context;
    int input = hl_fake_stream_index(fake, source), output = hl_fake_stream_index(fake, destination);
    uint32_t input_size, output_size, count;
    (void)source_offset;
    (void)destination_offset;
    if (input < 0 || output < 0 || fake->stream_write_ends[input] || !fake->stream_write_ends[output] || flags != 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    input_size = fake->stream_sizes[fake->stream_objects[input]];
    output_size = fake->stream_sizes[fake->stream_objects[output]];
    input_size = input_size == UINT16_MAX ? 0 : input_size;
    output_size = output_size == UINT16_MAX ? 0 : output_size;
    count = size < input_size ? (uint32_t)size : input_size;
    if (count > 1024u - output_size) count = 1024u - output_size;
    if (count == 0) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    memcpy(fake->stream_data[fake->stream_objects[output]] + output_size,
           fake->stream_data[fake->stream_objects[input]], count);
    memmove(fake->stream_data[fake->stream_objects[input]], fake->stream_data[fake->stream_objects[input]] + count,
            input_size - count);
    fake->stream_sizes[fake->stream_objects[input]] = input_size == count ? UINT16_MAX : (uint16_t)(input_size - count);
    fake->stream_sizes[fake->stream_objects[output]] = (uint16_t)(output_size + count);
    return hl_fake_result(fake, count);
}

void hl_fake_host_watch_emit(hl_fake_host *fake, hl_host_handle file, uint64_t device, uint64_t object,
                             uint64_t size, uint32_t changes) {
    for (uint32_t index = 0; index < 16; ++index) {
        if (fake->watch_handles[index] == 0 || fake->watch_files[index] != file) continue;
        hl_host_watch_record *record = &fake->watch_records[index];
        record->generation++;
        record->stable_device = device;
        record->stable_object = object;
        record->size = size;
        record->changes = changes;
    }
}

static int hl_fake_event_index(const hl_fake_host *fake, hl_host_handle handle) {
    uint32_t index;
    for (index = 0; index < 16; ++index)
        if (fake->event_handles[index] == handle) return (int)index;
    return -1;
}

static hl_host_result hl_fake_event_create(void *context) {
    hl_fake_host *fake = context;
    uint32_t index;
    for (index = 0; index < 16 && fake->event_handles[index] != 0; ++index) {}
    if (index == 16) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    fake->event_handles[index] = ++fake->next_handle;
    return hl_fake_result(fake, fake->event_handles[index]);
}

static hl_host_result hl_fake_event_control(void *context, hl_host_handle pollset, uint32_t operation,
                                            hl_host_handle object, uint64_t token, uint32_t interests) {
    hl_fake_host *fake = context;
    int event = hl_fake_event_index(fake, pollset);
    int directory = hl_fake_directory_handle(fake, object);
    int watch = hl_fake_watch_index(fake, object);
    if (event < 0 || token == 0 || (interests & HL_HOST_READY_READ) == 0)
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (operation == HL_HOST_EVENT_DELETE) {
        fake->event_directories[event] = 0;
        fake->event_watches[event] = 0;
        fake->event_tokens[event] = 0;
        return hl_fake_result(fake, 0);
    }
    if ((operation != HL_HOST_EVENT_ADD && operation != HL_HOST_EVENT_MODIFY) || (directory < 0 && watch < 0))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->event_directories[event] = directory < 0 ? 0 : (uint8_t)(fake->directory_objects[directory] + 1u);
    fake->event_watches[event] = watch < 0 ? 0 : (uint8_t)(watch + 1);
    fake->event_tokens[event] = token;
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_event_wait(void *context, hl_host_handle pollset, hl_host_event_record *events,
                                         size_t capacity, uint64_t deadline) {
    hl_fake_host *fake = context;
    int event = hl_fake_event_index(fake, pollset);
    uint32_t object;
    (void)deadline;
    if (event < 0 || events == NULL || capacity == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (fake->event_directories[event] != 0) {
        object = fake->event_directories[event] - 1u;
        if (fake->directory_record_counts[object] == 0) return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    } else if (fake->event_watches[event] != 0) {
        object = fake->event_watches[event] - 1u;
        if (fake->watch_records[object].generation == fake->watch_delivered[object])
            return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    } else {
        return (hl_host_result){HL_STATUS_WOULD_BLOCK, 0, 0, 0};
    }
    events[0] = (hl_host_event_record){fake->event_tokens[event], HL_HOST_READY_READ, 0};
    return hl_fake_result(fake, 1);
}

static hl_host_result hl_fake_event_wake(void *context, hl_host_handle pollset) {
    hl_fake_host *fake = context;
    return hl_fake_event_index(fake, pollset) < 0 ? (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0}
                                                  : hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_event_close(void *context, hl_host_handle pollset) {
    hl_fake_host *fake = context;
    int event = hl_fake_event_index(fake, pollset);
    if (event < 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->event_handles[event] = 0;
    fake->event_directories[event] = 0;
    fake->event_watches[event] = 0;
    fake->event_tokens[event] = 0;
    return hl_fake_result(fake, 0);
}

void hl_fake_host_init(hl_fake_host *fake, hl_host_services *services) {
    static const hl_host_memory_services memory = {HL_HOST_MEMORY_ABI,
                                                   sizeof(memory),
                                                   hl_fake_reserve,
                                                   hl_fake_protect,
                                                   hl_fake_release,
                                                   hl_fake_publish,
                                                   NULL,
                                                   NULL,
                                                   hl_fake_begin_code_write,
                                                   hl_fake_end_code_write,
                                                   hl_fake_map_file,
                                                   hl_fake_mapping_sync,
                                                   hl_fake_unmap_range,
                                                   hl_fake_map_anonymous,
                                                   hl_fake_discard,
                                                   hl_fake_repair_signal_page};
    static const hl_host_clock_services clock = {.abi = HL_HOST_CLOCK_ABI,
                                                  .size = sizeof(clock),
                                                  .monotonic_ns = hl_fake_monotonic,
                                                  .realtime_ns = hl_fake_realtime,
                                                  .raw_monotonic_ns = hl_fake_raw_monotonic,
                                                  .process_cpu_ns = hl_fake_process_cpu,
                                                  .thread_cpu_ns = hl_fake_thread_cpu,
                                                  .sleep_until = hl_fake_sleep_until,
                                                  .architectural_counter_hz = hl_fake_architectural_counter,
                                                  .backoff_ns = hl_fake_backoff};
    static const hl_host_process_services process = {
        HL_HOST_PROCESS_ABI,       sizeof(process),       hl_fake_spawn_cloned, hl_fake_process_wait,
        hl_fake_process_terminate, hl_fake_process_close, hl_fake_spawn_cloned};
    static const hl_host_sync_services sync = {HL_HOST_SYNC_ABI,       sizeof(sync),           hl_fake_mutex_create,
                                               hl_fake_mutex_lock,     hl_fake_mutex_unlock,   hl_fake_mutex_close,
                                               hl_fake_fork_lifecycle, hl_fake_fork_lifecycle, hl_fake_fork_lifecycle};
    static const hl_host_counter_services counter = {
        HL_HOST_COUNTER_ABI,       sizeof(counter),           hl_fake_counter_create,      hl_fake_counter_read,
        hl_fake_counter_write,     hl_fake_counter_get_flags, hl_fake_counter_set_flags,   hl_fake_counter_duplicate,
        hl_fake_counter_readiness, hl_fake_counter_subscribe, hl_fake_counter_unsubscribe, hl_fake_counter_close,
    };
    static const hl_host_transfer_services transfer = {
        HL_HOST_TRANSFER_ABI,     sizeof(transfer),           hl_fake_transfer_channel_pair, hl_fake_transfer_send,
        hl_fake_transfer_receive, hl_fake_transfer_duplicate, hl_fake_transfer_close,
    };
    static const hl_host_directory_services directory = {
        HL_HOST_DIRECTORY_ABI,  sizeof(directory),           hl_fake_directory_create,
        hl_fake_directory_add,  hl_fake_directory_modify,    hl_fake_directory_remove,
        hl_fake_directory_read, hl_fake_directory_duplicate, hl_fake_directory_close};
    static const hl_host_event_services event = {HL_HOST_EVENT_ABI,
                                                 sizeof(event),
                                                 hl_fake_event_create,
                                                 hl_fake_event_control,
                                                 hl_fake_event_wait,
                                                 hl_fake_event_wake,
                                                 hl_fake_event_close,
                                                 NULL,
                                                 NULL};
    static const hl_host_watch_services watch = {HL_HOST_WATCH_ABI, sizeof(watch), hl_fake_watch_open,
                                                  hl_fake_watch_query, hl_fake_watch_drain, hl_fake_watch_close};
    static const hl_host_stream_services stream = {
        HL_HOST_STREAM_ABI, sizeof(stream), hl_fake_stream_pipe_pair, hl_fake_stream_read,
        hl_fake_stream_write, hl_fake_stream_duplicate, hl_fake_stream_close,
        hl_fake_stream_set_status_flags, hl_fake_stream_readiness, hl_fake_stream_move};
    memset(fake, 0, sizeof(*fake));
    memset(services, 0, sizeof(*services));
    fake->monotonic_ns = 1000;
    fake->realtime_ns = 2000;
    fake->raw_monotonic_ns = 3000;
    fake->architectural_counter_hz = UINT64_C(24000000);
    fake->process_cpu_ns = 4000;
    fake->thread_cpu_ns = 5000;
    fake->process_exit_kind = HL_HOST_PROCESS_EXIT_CODE;
    services->abi = HL_HOST_SERVICES_ABI;
    services->size = sizeof(*services);
    services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_PROCESS | HL_HOST_CAP_SYNC |
                             HL_HOST_CAP_COUNTER | HL_HOST_CAP_TRANSFER | HL_HOST_CAP_DIRECTORY | HL_HOST_CAP_EVENT |
                             HL_HOST_CAP_WATCH | HL_HOST_CAP_STREAM;
    services->context = fake;
    services->memory = &memory;
    services->clock = &clock;
    services->process = &process;
    services->sync = &sync;
    services->counter = &counter;
    services->transfer = &transfer;
    services->directory = &directory;
    services->event = &event;
    services->watch = &watch;
    services->stream = &stream;
}

void hl_fake_host_fail_next(hl_fake_host *fake, hl_status status) {
    fake->next_failure = status;
}

void hl_fake_host_block_process_wait(hl_fake_host *fake, uint32_t block) {
    __atomic_store_n(&fake->process_block_wait, block != 0, __ATOMIC_RELEASE);
}
