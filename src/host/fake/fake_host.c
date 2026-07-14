#include "hl/fake_host.h"

#include <string.h>

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

static hl_host_result hl_fake_publish(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size) {
    return hl_fake_protect(context, mapping, offset, size, 0);
}

static hl_host_result hl_fake_monotonic(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->monotonic_ns++);
}

static hl_host_result hl_fake_realtime(void *context) {
    hl_fake_host *fake = context;
    return hl_fake_result(fake, fake->realtime_ns++);
}

static hl_host_result hl_fake_spawn_cloned(void *context, hl_host_process_entry entry, void *entry_context) {
    hl_fake_host *fake = context;
    hl_host_result result;
    if (entry == NULL) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    (void)entry_context;
    result = hl_fake_result(fake, ++fake->next_handle);
    if (result.status == HL_STATUS_OK) fake->live_processes++;
    return result;
}

static hl_host_result hl_fake_process_wait(void *context, hl_host_handle process, uint64_t deadline_ns) {
    hl_fake_host *fake = context;
    hl_host_result result;
    (void)deadline_ns;
    if (process == 0 || fake->live_processes == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    result = hl_fake_result(fake, (uint64_t)(uint32_t)fake->process_exit_value);
    result.detail = fake->process_exit_kind;
    if (result.status == HL_STATUS_OK) fake->process_waited = 1;
    return result;
}

static hl_host_result hl_fake_process_terminate(void *context, hl_host_handle process, uint32_t reason) {
    hl_fake_host *fake = context;
    (void)reason;
    if (process == 0 || fake->live_processes == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->process_exit_kind = HL_HOST_PROCESS_EXIT_SIGNAL;
    fake->process_exit_value = 9;
    return hl_fake_result(fake, 0);
}

static hl_host_result hl_fake_process_close(void *context, hl_host_handle process) {
    hl_fake_host *fake = context;
    hl_host_result result;
    if (process == 0 || fake->live_processes == 0) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (!fake->process_waited) return (hl_host_result){HL_STATUS_BUSY, 0, 0, 0};
    result = hl_fake_result(fake, 0);
    if (result.status == HL_STATUS_OK) {
        fake->live_processes--;
        fake->process_waited = 0;
    }
    return result;
}

void hl_fake_host_init(hl_fake_host *fake, hl_host_services *services) {
    static const hl_host_memory_services memory = {HL_HOST_MEMORY_ABI,
                                                   sizeof(memory),
                                                   hl_fake_reserve,
                                                   hl_fake_protect,
                                                   hl_fake_release,
                                                   hl_fake_publish,
                                                   NULL,
                                                   NULL};
    static const hl_host_clock_services clock = {HL_HOST_CLOCK_ABI, sizeof(clock), hl_fake_monotonic, hl_fake_realtime};
    static const hl_host_process_services process = {HL_HOST_PROCESS_ABI,       sizeof(process),
                                                     hl_fake_spawn_cloned,      hl_fake_process_wait,
                                                     hl_fake_process_terminate, hl_fake_process_close};
    memset(fake, 0, sizeof(*fake));
    memset(services, 0, sizeof(*services));
    fake->monotonic_ns = 1000;
    fake->realtime_ns = 2000;
    fake->process_exit_kind = HL_HOST_PROCESS_EXIT_CODE;
    services->abi = HL_HOST_SERVICES_ABI;
    services->size = sizeof(*services);
    services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_PROCESS;
    services->context = fake;
    services->memory = &memory;
    services->clock = &clock;
    services->process = &process;
}

void hl_fake_host_fail_next(hl_fake_host *fake, hl_status status) {
    fake->next_failure = status;
}
