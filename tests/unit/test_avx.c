#include "test.h"

#include "../../src/translator/guest/x86_64/avx.h"

int main(void) {
    uint64_t low = UINT64_C(0x1000);
    uint64_t high = UINT64_C(0x2000);
    uint64_t bias = UINT64_C(0x100000000);
    hl_x86_avx_state state = {&low, &high, &bias};

    HL_CHECK(hl_x86_avx_address(NULL, UINT64_C(0x1000)) == UINT64_C(0x1000));
    HL_CHECK(hl_x86_avx_address(&state, UINT64_C(0xfff)) == UINT64_C(0xfff));
    HL_CHECK(hl_x86_avx_address(&state, UINT64_C(0x1000)) == UINT64_C(0x100001000));
    HL_CHECK(hl_x86_avx_address(&state, UINT64_C(0x1fff)) == UINT64_C(0x100001fff));
    HL_CHECK(hl_x86_avx_address(&state, UINT64_C(0x2000)) == UINT64_C(0x2000));
    low = 0;
    HL_CHECK(hl_x86_avx_address(&state, UINT64_C(0x1000)) == UINT64_C(0x1000));
    return EXIT_SUCCESS;
}
