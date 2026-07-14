#include "test.h"

#include "hl/linux_abi.h"

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
    hl_linux_file_status status = {UINT64_C(0x1020304050607080), UINT64_C(0x1122334455667788), 123456789, 241,
                                   UINT64_C(987654321012345678), HL_LINUX_S_IFREG | 0754u};
    uint8_t aarch64[HL_LINUX_STAT_AARCH64_SIZE];
    uint8_t x86_64[HL_LINUX_STAT_X86_64_SIZE];
    size_t index;

    memset(aarch64, 0xa5, sizeof(aarch64));
    HL_CHECK(hl_linux_stat_aarch64(&status, aarch64, sizeof(aarch64)) == 0);
    HL_CHECK(load_u64(aarch64, 0) == status.device && load_u64(aarch64, 8) == status.object);
    HL_CHECK(load_u32(aarch64, 16) == status.mode);
    HL_CHECK(load_u64(aarch64, 48) == status.size);
    HL_CHECK(load_u64(aarch64, 64) == status.blocks_512);
    HL_CHECK(load_u64(aarch64, 88) == 987654321 && load_u64(aarch64, 96) == 12345678);
    HL_CHECK(all_zero(aarch64, 20, 48) && all_zero(aarch64, 56, 64));
    HL_CHECK(all_zero(aarch64, 72, 88) && all_zero(aarch64, 104, sizeof(aarch64)));

    memset(x86_64, 0xa5, sizeof(x86_64));
    HL_CHECK(hl_linux_stat_x86_64(&status, x86_64, sizeof(x86_64)) == 0);
    HL_CHECK(load_u64(x86_64, 0) == status.device && load_u64(x86_64, 8) == status.object);
    HL_CHECK(load_u32(x86_64, 24) == status.mode);
    HL_CHECK(load_u64(x86_64, 48) == status.size);
    HL_CHECK(load_u64(x86_64, 64) == status.blocks_512);
    HL_CHECK(load_u64(x86_64, 88) == 987654321 && load_u64(x86_64, 96) == 12345678);
    HL_CHECK(all_zero(x86_64, 16, 24) && all_zero(x86_64, 28, 48));
    HL_CHECK(all_zero(x86_64, 56, 64) && all_zero(x86_64, 72, 88));
    HL_CHECK(all_zero(x86_64, 104, sizeof(x86_64)));

    HL_CHECK(hl_linux_stat_aarch64(&status, aarch64, sizeof(aarch64) - 1) == -HL_LINUX_EINVAL);
    status.size = UINT64_MAX;
    memset(x86_64, 0x5a, sizeof(x86_64));
    HL_CHECK(hl_linux_stat_x86_64(&status, x86_64, sizeof(x86_64)) == -HL_LINUX_EOVERFLOW);
    for (index = 0; index < sizeof(x86_64); index++)
        HL_CHECK(x86_64[index] == 0x5a);
    return EXIT_SUCCESS;
}
