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

void hl_fake_host_init(hl_fake_host *fake, hl_host_services *services) {
    static const hl_host_memory_services memory = {HL_HOST_MEMORY_ABI, sizeof(memory),  hl_fake_reserve,
                                                   hl_fake_protect,    hl_fake_release, hl_fake_publish};
    static const hl_host_clock_services clock = {HL_HOST_CLOCK_ABI, sizeof(clock), hl_fake_monotonic, hl_fake_realtime};
    memset(fake, 0, sizeof(*fake));
    memset(services, 0, sizeof(*services));
    fake->monotonic_ns = 1000;
    fake->realtime_ns = 2000;
    services->abi = HL_HOST_SERVICES_ABI;
    services->size = sizeof(*services);
    services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK;
    services->context = fake;
    services->memory = &memory;
    services->clock = &clock;
}

void hl_fake_host_fail_next(hl_fake_host *fake, hl_status status) {
    fake->next_failure = status;
}
