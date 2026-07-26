#include "guest_fetch.h"

#include "guest_memory.h"

#include <errno.h>
#include <string.h>

static hl_guest_fetch_direct_validator g_direct_validator;

void hl_guest_fetch_set_direct_validator(hl_guest_fetch_direct_validator validator) {
    g_direct_validator = validator;
}

int hl_guest_fetch_exec(uint64_t guest, void *destination, size_t length) {
    /*
     * Validate the complete fetch before copying anything.  A logical VMA
     * followed by an unmapped logical hole is reported as "ordinary" by a
     * lookup beginning in the hole; treating that second chunk as a direct
     * host VA would both crash the translator and partially modify the decode
     * buffer.  A direct/logical transition inside one instruction is likewise
     * not one executable mapping.
     */
    uint64_t probe = guest;
    size_t remaining = length;
    while (remaining != 0) {
        size_t page_left = 4096u - (size_t)(probe & UINT64_C(4095));
        size_t chunk = remaining < page_left ? remaining : page_left;
        const void *host = NULL;
        size_t contiguous = 0;
        int resolution = hl_guest_memory_resolve_exec(probe, chunk, &host, &contiguous);
        if (resolution < 0) return -1;
        int current = resolution > 0;
        if (!current && g_direct_validator != NULL && !g_direct_validator(probe, chunk)) {
            errno = EFAULT;
            return -1;
        }
        if (current && contiguous < chunk) {
            errno = EFAULT;
            return -1;
        }
        probe += chunk;
        remaining -= chunk;
    }

    unsigned char *output = destination;
    while (length != 0) {
        size_t page_left = 4096u - (size_t)(guest & UINT64_C(4095));
        size_t chunk = length < page_left ? length : page_left;
        const void *host = NULL;
        size_t contiguous = 0;
        int resolution = hl_guest_memory_resolve_exec(guest, chunk, &host, &contiguous);
        if (resolution < 0) return -1; /* snapshot changed only at STW */
        if (resolution == 0) host = (const void *)(uintptr_t)guest;
        if (resolution > 0 && contiguous < chunk) {
            errno = EFAULT;
            return -1;
        }
        memcpy(output, host, chunk);
        output += chunk;
        guest += chunk;
        length -= chunk;
    }
    return 0;
}

int hl_guest_fetch_u32(uint64_t guest, uint32_t *instruction) {
    return hl_guest_fetch_exec(guest, instruction, sizeof(*instruction));
}

/*
 * decode.c supplies a weak direct-address default so its standalone unit
 * tests remain independent of the Linux ABI.  Engine links include this
 * strong definition and therefore route every decoder (translator, trace
 * lookahead, and C AVX fallback) through the same executable-byte reader.
 */
