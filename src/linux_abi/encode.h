#ifndef HL_LINUX_ABI_ENCODE_H
#define HL_LINUX_ABI_ENCODE_H

#include <stddef.h>
#include <stdint.h>

typedef struct hl_linux_stat_record {
    uint64_t device;
    uint64_t object;
    uint64_t links;
    uint64_t special_device;
    uint64_t size;
    uint64_t blocks_512;
    int64_t accessed_seconds;
    uint64_t accessed_nanoseconds;
    int64_t modified_seconds;
    uint64_t modified_nanoseconds;
    int64_t changed_seconds;
    uint64_t changed_nanoseconds;
    uint32_t mode;
    uint32_t user;
    uint32_t group;
} hl_linux_stat_record;

enum { HL_LINUX_STAT_RECORD_AARCH64_SIZE = 128, HL_LINUX_STAT_RECORD_X86_64_SIZE = 144 };

int hl_linux_stat_encode_aarch64(const hl_linux_stat_record *record, void *output, size_t output_size);
int hl_linux_stat_encode_x86_64(const hl_linux_stat_record *record, void *output, size_t output_size);

#endif
