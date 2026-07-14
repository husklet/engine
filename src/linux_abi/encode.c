#include "encode.h"

#include <limits.h>
#include <string.h>

enum { HL_STAT_EINVAL = 22, HL_STAT_EOVERFLOW = 75 };

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

static int hl_linux_stat_record_validate(const hl_linux_stat_record *record, void *output, size_t output_size,
                                         size_t required_size) {
    if (record == NULL || output == NULL || output_size < required_size) return -HL_STAT_EINVAL;
    if (record->size > INT64_MAX || record->blocks_512 > INT64_MAX || record->accessed_nanoseconds >= 1000000000 ||
        record->modified_nanoseconds >= 1000000000 || record->changed_nanoseconds >= 1000000000)
        return -HL_STAT_EOVERFLOW;
    return 0;
}

int hl_linux_stat_encode_aarch64(const hl_linux_stat_record *record, void *output, size_t output_size) {
    uint8_t *bytes = output;
    int result = hl_linux_stat_record_validate(record, output, output_size, HL_LINUX_STAT_RECORD_AARCH64_SIZE);
    if (result != 0) return result;
    memset(bytes, 0, HL_LINUX_STAT_RECORD_AARCH64_SIZE);
    hl_stat_u64(bytes, 0, record->device);
    hl_stat_u64(bytes, 8, record->object);
    hl_stat_u32(bytes, 16, record->mode);
    hl_stat_u32(bytes, 20, (uint32_t)record->links);
    hl_stat_u32(bytes, 24, record->user);
    hl_stat_u32(bytes, 28, record->group);
    hl_stat_u64(bytes, 32, record->special_device);
    hl_stat_u64(bytes, 48, record->size);
    hl_stat_u32(bytes, 56, 4096);
    hl_stat_u64(bytes, 64, record->blocks_512);
    hl_stat_u64(bytes, 72, (uint64_t)record->accessed_seconds);
    hl_stat_u64(bytes, 80, record->accessed_nanoseconds);
    hl_stat_u64(bytes, 88, (uint64_t)record->modified_seconds);
    hl_stat_u64(bytes, 96, record->modified_nanoseconds);
    hl_stat_u64(bytes, 104, (uint64_t)record->changed_seconds);
    hl_stat_u64(bytes, 112, record->changed_nanoseconds);
    return 0;
}

int hl_linux_stat_encode_x86_64(const hl_linux_stat_record *record, void *output, size_t output_size) {
    uint8_t *bytes = output;
    int result = hl_linux_stat_record_validate(record, output, output_size, HL_LINUX_STAT_RECORD_X86_64_SIZE);
    if (result != 0) return result;
    memset(bytes, 0, HL_LINUX_STAT_RECORD_X86_64_SIZE);
    hl_stat_u64(bytes, 0, record->device);
    hl_stat_u64(bytes, 8, record->object);
    hl_stat_u64(bytes, 16, record->links);
    hl_stat_u32(bytes, 24, record->mode);
    hl_stat_u32(bytes, 28, record->user);
    hl_stat_u32(bytes, 32, record->group);
    hl_stat_u64(bytes, 40, record->special_device);
    hl_stat_u64(bytes, 48, record->size);
    hl_stat_u64(bytes, 56, 4096);
    hl_stat_u64(bytes, 64, record->blocks_512);
    hl_stat_u64(bytes, 72, (uint64_t)record->accessed_seconds);
    hl_stat_u64(bytes, 80, record->accessed_nanoseconds);
    hl_stat_u64(bytes, 88, (uint64_t)record->modified_seconds);
    hl_stat_u64(bytes, 96, record->modified_nanoseconds);
    hl_stat_u64(bytes, 104, (uint64_t)record->changed_seconds);
    hl_stat_u64(bytes, 112, record->changed_nanoseconds);
    return 0;
}
