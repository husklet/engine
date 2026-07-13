#include "hl/host_services.h"

#include <stddef.h>

static int hl_has_field(uint32_t size, size_t offset, size_t field_size) {
    return size >= offset && (size_t)size - offset >= field_size;
}

static int hl_valid_group(const void *group, uint32_t abi, size_t size) {
    const uint32_t *header = group;
    return group != NULL && header[0] == abi && header[1] >= size;
}

hl_status hl_host_services_validate(const hl_host_services *services, uint64_t required_capabilities) {
    if (services == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (services->abi != HL_HOST_SERVICES_ABI) return HL_STATUS_ABI_MISMATCH;
    if (!hl_has_field(services->size, offsetof(hl_host_services, log), sizeof(services->log)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & required_capabilities) != required_capabilities) return HL_STATUS_NOT_SUPPORTED;
    if ((services->capabilities & HL_HOST_CAP_MEMORY) != 0) {
        const hl_host_memory_services *memory = services->memory;
        if (memory == NULL || memory->abi != HL_HOST_MEMORY_ABI || memory->size < sizeof(*memory) ||
            memory->reserve == NULL || memory->protect == NULL || memory->release == NULL ||
            memory->publish_code == NULL)
            return HL_STATUS_ABI_MISMATCH;
    }
    if ((services->capabilities & HL_HOST_CAP_CLOCK) != 0) {
        const hl_host_clock_services *clock = services->clock;
        if (clock == NULL || clock->abi != HL_HOST_CLOCK_ABI || clock->size < sizeof(*clock) ||
            clock->monotonic_ns == NULL || clock->realtime_ns == NULL)
            return HL_STATUS_ABI_MISMATCH;
    }
    if ((services->capabilities & HL_HOST_CAP_FILE) != 0 &&
        !hl_valid_group(services->file, HL_HOST_FILE_ABI, sizeof(*services->file)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_PROCESS) != 0 &&
        !hl_valid_group(services->process, HL_HOST_PROCESS_ABI, sizeof(*services->process)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_EVENT) != 0 &&
        !hl_valid_group(services->event, HL_HOST_EVENT_ABI, sizeof(*services->event)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_NETWORK) != 0 &&
        !hl_valid_group(services->network, HL_HOST_NETWORK_ABI, sizeof(*services->network)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_SHARED_MEMORY) != 0 &&
        !hl_valid_group(services->shared_memory, HL_HOST_SHARED_MEMORY_ABI, sizeof(*services->shared_memory)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_GPU) != 0 &&
        !hl_valid_group(services->gpu, HL_HOST_GPU_ABI, sizeof(*services->gpu)))
        return HL_STATUS_ABI_MISMATCH;
    return HL_STATUS_OK;
}
