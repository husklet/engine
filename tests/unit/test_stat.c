#include "test.h"

#include "hl/linux_abi.h"
#include "../../src/linux_abi/encode.h"

#include <string.h>

static uint32_t load_u32(const uint8_t *bytes, size_t offset) {
    return (uint32_t)bytes[offset] | (uint32_t)bytes[offset + 1] << 8 | (uint32_t)bytes[offset + 2] << 16 |
           (uint32_t)bytes[offset + 3] << 24;
}

static uint64_t load_u64(const uint8_t *bytes, size_t offset) {
    uint64_t value = 0;
    size_t byte;
    for (byte = 0; byte < 8; byte++)
        value |= (uint64_t)bytes[offset + byte] << (byte * 8);
    return value;
}

static int all_zero(const uint8_t *bytes, size_t begin, size_t end) {
    size_t index;
    for (index = begin; index < end; index++)
        if (bytes[index] != 0) return 0;
    return 1;
}

int main(void) {
    static const uint8_t aarch64_golden[HL_LINUX_STAT_AARCH64_SIZE] = {
        [0] = 0x01,  [1] = 0x01,  [8] = 0x02,  [9] = 0x02,  [16] = 0xa0, [17] = 0x81,
        [20] = 3,    [24] = 0xe9, [25] = 3,    [28] = 0xea, [29] = 3,    [32] = 0x04,
        [33] = 0x04, [48] = 0xf9, [49] = 1,    [57] = 0x10, [64] = 6,    [72] = 7,
        [80] = 8,    [88] = 9,    [96] = 10,   [104] = 11,  [112] = 12,
    };
    static const uint8_t x86_64_golden[HL_LINUX_STAT_X86_64_SIZE] = {
        [0] = 0x01,  [1] = 0x01,  [8] = 0x02,  [9] = 0x02,  [16] = 3,    [24] = 0xa0,
        [25] = 0x81, [28] = 0xe9, [29] = 3,    [32] = 0xea, [33] = 3,    [40] = 0x04,
        [41] = 0x04, [48] = 0xf9, [49] = 1,    [57] = 0x10, [64] = 6,    [72] = 7,
        [80] = 8,    [88] = 9,    [96] = 10,   [104] = 11,  [112] = 12,
    };
    hl_linux_file_status status = {.device = UINT64_C(0x1020304050607080),
                                   .object = UINT64_C(0x1122334455667788),
                                   .size = 123456789,
                                   .blocks_512 = 241,
                                   .modified_ns = UINT64_C(987654321012345678),
                                   .accessed_ns = UINT64_C(123456789000000042),
                                   .changed_ns = UINT64_C(222333444555666777),
                                   .special_device = UINT64_C(0x8877665544332211),
                                   .link_count = 7,
                                   .mode = HL_LINUX_S_IFREG | 0754u,
                                   .user = 501,
                                   .group = 502};
    uint8_t aarch64[HL_LINUX_STAT_AARCH64_SIZE];
    uint8_t x86_64[HL_LINUX_STAT_X86_64_SIZE];
    uint8_t statfs[HL_LINUX_STATFS_RECORD_SIZE + 1];
    size_t index;
    hl_linux_stat_record record = {UINT64_C(0x101),
                                   UINT64_C(0x202),
                                   3,
                                   UINT64_C(0x404),
                                   505,
                                   6,
                                   7,
                                   8,
                                   9,
                                   10,
                                   11,
                                   12,
                                   HL_LINUX_S_IFREG | 0640u,
                                   1001,
                                   1002};
    const hl_linux_statfs_record filesystem = {
        .type = INT64_C(0x1021994),
        .block_size = UINT64_C(0x1112131415161718),
        .blocks = UINT64_C(0x2122232425262728),
        .blocks_free = UINT64_C(0x3132333435363738),
        .blocks_available = UINT64_C(0x4142434445464748),
        .files = UINT64_C(0x5152535455565758),
        .files_free = UINT64_C(0x6162636465666768),
        .filesystem_id = {UINT32_C(0x71727374), UINT32_C(0x81828384)},
        .name_max = 255,
        .fragment_size = 4096,
        .flags = UINT64_C(0x9192939495969798),
    };

    memset(aarch64, 0xa5, sizeof(aarch64));
    HL_CHECK(hl_linux_stat_aarch64(&status, aarch64, sizeof(aarch64)) == 0);
    HL_CHECK(load_u64(aarch64, 0) == status.device && load_u64(aarch64, 8) == status.object);
    HL_CHECK(load_u32(aarch64, 16) == status.mode);
    HL_CHECK(load_u32(aarch64, 20) == status.link_count);
    HL_CHECK(load_u32(aarch64, 24) == status.user && load_u32(aarch64, 28) == status.group);
    HL_CHECK(load_u64(aarch64, 32) == status.special_device);
    HL_CHECK(load_u64(aarch64, 48) == status.size);
    HL_CHECK(load_u64(aarch64, 64) == status.blocks_512);
    HL_CHECK(load_u64(aarch64, 72) == 123456789 && load_u64(aarch64, 80) == 42);
    HL_CHECK(load_u64(aarch64, 88) == 987654321 && load_u64(aarch64, 96) == 12345678);
    HL_CHECK(load_u64(aarch64, 104) == 222333444 && load_u64(aarch64, 112) == 555666777);
    HL_CHECK(all_zero(aarch64, 40, 48) && all_zero(aarch64, 60, 64));
    HL_CHECK(all_zero(aarch64, 120, sizeof(aarch64)));

    memset(x86_64, 0xa5, sizeof(x86_64));
    HL_CHECK(hl_linux_stat_x86_64(&status, x86_64, sizeof(x86_64)) == 0);
    HL_CHECK(load_u64(x86_64, 0) == status.device && load_u64(x86_64, 8) == status.object);
    HL_CHECK(load_u32(x86_64, 24) == status.mode);
    HL_CHECK(load_u64(x86_64, 16) == status.link_count);
    HL_CHECK(load_u32(x86_64, 28) == status.user && load_u32(x86_64, 32) == status.group);
    HL_CHECK(load_u64(x86_64, 40) == status.special_device);
    HL_CHECK(load_u64(x86_64, 48) == status.size);
    HL_CHECK(load_u64(x86_64, 64) == status.blocks_512);
    HL_CHECK(load_u64(x86_64, 72) == 123456789 && load_u64(x86_64, 80) == 42);
    HL_CHECK(load_u64(x86_64, 88) == 987654321 && load_u64(x86_64, 96) == 12345678);
    HL_CHECK(load_u64(x86_64, 104) == 222333444 && load_u64(x86_64, 112) == 555666777);
    HL_CHECK(all_zero(x86_64, 36, 40) && all_zero(x86_64, 120, sizeof(x86_64)));

    HL_CHECK(hl_linux_stat_aarch64(&status, aarch64, sizeof(aarch64) - 1) == -HL_LINUX_EINVAL);
    memset(aarch64, 0x6d, sizeof aarch64);
    HL_CHECK(hl_linux_stat_aarch64(NULL, aarch64, sizeof aarch64) == -HL_LINUX_EINVAL);
    for (index = 0; index < sizeof aarch64; index++)
        HL_CHECK(aarch64[index] == 0x6d);
    HL_CHECK(hl_linux_stat_x86_64(&status, NULL, sizeof x86_64) == -HL_LINUX_EINVAL);
    status.size = UINT64_MAX;
    memset(x86_64, 0x5a, sizeof(x86_64));
    HL_CHECK(hl_linux_stat_x86_64(&status, x86_64, sizeof(x86_64)) == -HL_LINUX_EOVERFLOW);
    for (index = 0; index < sizeof(x86_64); index++)
        HL_CHECK(x86_64[index] == 0x5a);

    HL_CHECK(hl_linux_stat_encode_aarch64(&record, aarch64, sizeof(aarch64)) == 0);
    HL_CHECK(load_u64(aarch64, 8) == 0x202 && load_u32(aarch64, 20) == 3);
    HL_CHECK(load_u32(aarch64, 24) == 1001 && load_u32(aarch64, 28) == 1002);
    HL_CHECK(load_u64(aarch64, 32) == 0x404 && load_u32(aarch64, 56) == 4096);
    HL_CHECK(load_u64(aarch64, 72) == 7 && load_u64(aarch64, 80) == 8);
    HL_CHECK(load_u64(aarch64, 104) == 11 && load_u64(aarch64, 112) == 12);
    HL_CHECK(memcmp(aarch64, aarch64_golden, sizeof aarch64) == 0);

    HL_CHECK(hl_linux_stat_encode_x86_64(&record, x86_64, sizeof(x86_64)) == 0);
    HL_CHECK(load_u64(x86_64, 16) == 3 && load_u32(x86_64, 28) == 1001 && load_u32(x86_64, 32) == 1002);
    HL_CHECK(load_u64(x86_64, 40) == 0x404 && load_u64(x86_64, 56) == 4096);
    HL_CHECK(load_u64(x86_64, 72) == 7 && load_u64(x86_64, 80) == 8);
    HL_CHECK(load_u64(x86_64, 104) == 11 && load_u64(x86_64, 112) == 12);
    HL_CHECK(memcmp(x86_64, x86_64_golden, sizeof x86_64) == 0);

    memset(statfs, 0xa5, sizeof statfs);
    HL_CHECK(hl_linux_statfs_encode(&filesystem, statfs, HL_LINUX_STATFS_RECORD_SIZE) == 0);
    HL_CHECK(load_u64(statfs, 0) == UINT64_C(0x1021994));
    HL_CHECK(load_u64(statfs, 8) == filesystem.block_size);
    HL_CHECK(load_u64(statfs, 16) == filesystem.blocks);
    HL_CHECK(load_u64(statfs, 24) == filesystem.blocks_free);
    HL_CHECK(load_u64(statfs, 32) == filesystem.blocks_available);
    HL_CHECK(load_u64(statfs, 40) == filesystem.files);
    HL_CHECK(load_u64(statfs, 48) == filesystem.files_free);
    HL_CHECK(load_u32(statfs, 56) == filesystem.filesystem_id[0]);
    HL_CHECK(load_u32(statfs, 60) == filesystem.filesystem_id[1]);
    HL_CHECK(load_u64(statfs, 64) == filesystem.name_max);
    HL_CHECK(load_u64(statfs, 72) == filesystem.fragment_size);
    HL_CHECK(load_u64(statfs, 80) == filesystem.flags);
    HL_CHECK(all_zero(statfs, 88, HL_LINUX_STATFS_RECORD_SIZE));
    HL_CHECK(statfs[HL_LINUX_STATFS_RECORD_SIZE] == 0xa5);
    memset(statfs, 0x5a, sizeof statfs);
    HL_CHECK(hl_linux_statfs_encode(&filesystem, statfs, HL_LINUX_STATFS_RECORD_SIZE - 1) == -HL_LINUX_EINVAL);
    for (index = 0; index < sizeof statfs; index++)
        HL_CHECK(statfs[index] == 0x5a);
    HL_CHECK(hl_linux_statfs_encode(NULL, statfs, HL_LINUX_STATFS_RECORD_SIZE) == -HL_LINUX_EINVAL);

    record.modified_nanoseconds = 1000000000;
    memset(aarch64, 0xa5, sizeof(aarch64));
    HL_CHECK(hl_linux_stat_encode_aarch64(&record, aarch64, sizeof(aarch64)) == -HL_LINUX_EOVERFLOW);
    for (index = 0; index < sizeof(aarch64); index++)
        HL_CHECK(aarch64[index] == 0xa5);
    record.modified_nanoseconds = 0;
    record.links = UINT64_C(0x100000000);
    memset(aarch64, 0x3c, sizeof aarch64);
    HL_CHECK(hl_linux_stat_encode_aarch64(&record, aarch64, sizeof aarch64) == -HL_LINUX_EOVERFLOW);
    for (index = 0; index < sizeof aarch64; index++)
        HL_CHECK(aarch64[index] == 0x3c);
    HL_CHECK(hl_linux_stat_encode_x86_64(&record, x86_64, sizeof x86_64) == 0);
    HL_CHECK(load_u64(x86_64, 16) == UINT64_C(0x100000000));
    return EXIT_SUCCESS;
}
