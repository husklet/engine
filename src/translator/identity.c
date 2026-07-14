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

uint64_t hl_identity_mix(uint64_t program, uint64_t interpreter, uint64_t engine, uint64_t name) {
    return (program ^ (interpreter * HL_IDENTITY_PRIME)) ^ engine ^ (name * HL_IDENTITY_PRIME);
}
