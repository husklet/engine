#include "hl/linux_abi.h"

#include <string.h>

static void hl_stat_u32(uint8_t *output, size_t offset, uint32_t value) {
    output[offset + 0] = (uint8_t)value;
    output[offset + 1] = (uint8_t)(value >> 8);
    output[offset + 2] = (uint8_t)(value >> 16);
    output[offset + 3] = (uint8_t)(value >> 24);
}

static void hl_stat_u64(uint8_t *output, size_t offset, uint64_t value) {
    size_t byte;
    for (byte = 0; byte < 8; byte++)
        output[offset + byte] = (uint8_t)(value >> (byte * 8));
}

static int64_t hl_stat_validate(const hl_linux_file_status *status, void *output, size_t output_size,
                                size_t required_size) {
    uint64_t modified_seconds;
    if (status == NULL || output == NULL || output_size < required_size) return -HL_LINUX_EINVAL;
    modified_seconds = status->modified_ns / UINT64_C(1000000000);
    if (status->size > INT64_MAX || status->blocks_512 > INT64_MAX || modified_seconds > INT64_MAX)
        return -HL_LINUX_EOVERFLOW;
    return 0;
}

int64_t hl_linux_stat_aarch64(const hl_linux_file_status *status, void *output, size_t output_size) {
    uint8_t *bytes = output;
    uint64_t seconds;
    int64_t result = hl_stat_validate(status, output, output_size, HL_LINUX_STAT_AARCH64_SIZE);
    if (result != 0) return result;
    seconds = status->modified_ns / UINT64_C(1000000000);
    memset(bytes, 0, HL_LINUX_STAT_AARCH64_SIZE);
    hl_stat_u64(bytes, 0, status->device);
    hl_stat_u64(bytes, 8, status->object);
    hl_stat_u32(bytes, 16, status->mode);
    hl_stat_u64(bytes, 48, status->size);
    hl_stat_u64(bytes, 64, status->blocks_512);
    hl_stat_u64(bytes, 88, seconds);
    hl_stat_u64(bytes, 96, status->modified_ns % UINT64_C(1000000000));
    return 0;
}

int64_t hl_linux_stat_x86_64(const hl_linux_file_status *status, void *output, size_t output_size) {
    uint8_t *bytes = output;
    uint64_t seconds;
    int64_t result = hl_stat_validate(status, output, output_size, HL_LINUX_STAT_X86_64_SIZE);
    if (result != 0) return result;
    seconds = status->modified_ns / UINT64_C(1000000000);
    memset(bytes, 0, HL_LINUX_STAT_X86_64_SIZE);
    hl_stat_u64(bytes, 0, status->device);
    hl_stat_u64(bytes, 8, status->object);
    hl_stat_u32(bytes, 24, status->mode);
    hl_stat_u64(bytes, 48, status->size);
    hl_stat_u64(bytes, 64, status->blocks_512);
    hl_stat_u64(bytes, 88, seconds);
    hl_stat_u64(bytes, 96, status->modified_ns % UINT64_C(1000000000));
    return 0;
}
