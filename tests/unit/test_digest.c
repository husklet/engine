#include "test.h"

#include "../../src/production/translate/digest.h"

#include <string.h>

static uint64_t legacy(uint64_t value, const void *bytes, size_t size) {
    const uint8_t *cursor = bytes;
    for (; size >= 8; cursor += 8, size -= 8) {
        uint64_t word;
        memcpy(&word, cursor, sizeof(word));
        value ^= word;
        value *= UINT64_C(1099511628211);
    }
    for (; size != 0; ++cursor, --size) {
        value ^= *cursor;
        value *= UINT64_C(1099511628211);
    }
    return value;
}

int main(void) {
    static const uint8_t header[] = {0x32, 0x11, 0xaa, 0x87, 0x42};
    static const uint8_t arena[] = {0x00, 0xff, 0x83, 0x19, 0x25, 0x76, 0x44, 0x90, 0x10, 0x37, 0xbc, 0xde, 0xef};
    hl_digest digest;
    uint8_t serialized[28];
    uint64_t expected = legacy(legacy(HL_DIGEST_SEED, header, sizeof(header)), arena, sizeof(arena));

    hl_digest_init(&digest, HL_DIGEST_SEED);
    hl_digest_update(&digest, header, sizeof(header));
    hl_digest_update(&digest, arena, sizeof(arena));
    HL_CHECK(hl_digest_value(&digest) == expected);
    HL_CHECK(hl_digest_bytes(HL_DIGEST_SEED, arena, sizeof(arena)) == legacy(HL_DIGEST_SEED, arena, sizeof(arena)));

    /* Empty serialized sections leave the running cache checksum unchanged. */
    hl_digest_update(&digest, NULL, 0);
    HL_CHECK(hl_digest_value(&digest) == expected);

    /* Cache record sections are eight-byte aligned, so load-side section updates equal save-side payload hashing. */
    for (size_t i = 0; i < sizeof(serialized); ++i)
        serialized[i] = (uint8_t)(i * 13u + 7u);
    hl_digest_init(&digest, HL_DIGEST_SEED);
    hl_digest_update(&digest, serialized, 8);
    hl_digest_update(&digest, serialized + 8, 8);
    hl_digest_update(&digest, serialized + 16, 12);
    HL_CHECK(hl_digest_value(&digest) == hl_digest_bytes(HL_DIGEST_SEED, serialized, sizeof(serialized)));
    return EXIT_SUCCESS;
}
