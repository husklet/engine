#include "test.h"

#include "../../src/translator/guest/x86_64/avx.h"
#include "../../src/translator/guest/x86_64/cpu.h"

static int check_legacy_palignr_large_immediate(uint8_t immediate) {
    uint8_t code[] = {0x66, 0x0f, 0x3a, 0x0f, 0xc1, immediate};
    struct cpu cpu = {0};
    cpu.rip = (uintptr_t)code;
    cpu.v[0] = UINT64_MAX;
    cpu.v[1] = UINT64_MAX;
    cpu.v[2] = UINT64_C(0x0123456789abcdef);
    cpu.v[3] = UINT64_C(0xfedcba9876543210);
    hl_x86_sse_run(NULL, &cpu);
    HL_CHECK(cpu.v[0] == 0);
    HL_CHECK(cpu.v[1] == 0);
    return EXIT_SUCCESS;
}

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
    HL_CHECK(check_legacy_palignr_large_immediate(0x80) == EXIT_SUCCESS);
    HL_CHECK(check_legacy_palignr_large_immediate(0xff) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
