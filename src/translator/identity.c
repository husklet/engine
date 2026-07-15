#include "identity.h"

#include <stddef.h>

#define HL_IDENTITY_SEED 1469598103934665603ull
#define HL_IDENTITY_PRIME 1099511628211ull

static uint64_t identity_bytes(uint64_t value, const char *bytes) {
    while (*bytes != '\0') {
        value ^= (uint8_t)*bytes++;
        value *= HL_IDENTITY_PRIME;
    }
    return value;
}

uint64_t hl_identity_name(const char *name) {
    const char *base;
    const char *cursor;

    if (name == NULL) return 0x1357ull;
    base = name;
    for (cursor = name; *cursor != '\0'; ++cursor)
        if (*cursor == '/') base = cursor + 1;
    return identity_bytes(HL_IDENTITY_SEED, base);
}

uint64_t hl_identity_file(const hl_host_file_metadata *metadata) {
    uint64_t value = HL_IDENTITY_SEED;
    uint64_t fields[5];
    size_t index;
    if (metadata == NULL || metadata->type != HL_HOST_FILE_TYPE_REGULAR) return 0;
    fields[0] = metadata->stable_device;
    fields[1] = metadata->stable_object;
    fields[2] = metadata->size;
    fields[3] = metadata->modified_ns / UINT64_C(1000000000);
    fields[4] = metadata->modified_ns % UINT64_C(1000000000);
    for (index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        value ^= fields[index];
        value *= HL_IDENTITY_PRIME;
    }
    return value;
}

uint64_t hl_identity_mix(uint64_t program, uint64_t interpreter, uint64_t engine, uint64_t name) {
    return (program ^ (interpreter * HL_IDENTITY_PRIME)) ^ engine ^ (name * HL_IDENTITY_PRIME);
}
