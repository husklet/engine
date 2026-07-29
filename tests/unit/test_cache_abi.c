#include "test.h"

#include "../../src/translator/cache_abi.h"

int main(void) {
    const uint64_t format = 10;

    HL_CHECK(hl_pcache_compatible(format, HL_PCACHE_ABI_AARCH64, format, HL_PCACHE_ABI_AARCH64));
    HL_CHECK(!hl_pcache_compatible(format - 1, HL_PCACHE_ABI_AARCH64, format, HL_PCACHE_ABI_AARCH64));
    HL_CHECK(!hl_pcache_compatible(format, HL_PCACHE_ABI_AARCH64 - 1, format, HL_PCACHE_ABI_AARCH64));
    HL_CHECK(!hl_pcache_compatible(format, HL_PCACHE_ABI_X86_64, format, HL_PCACHE_ABI_AARCH64));
    return EXIT_SUCCESS;
}
