#include "test.h"
#include "../../src/translator/guest/x86_64/flags.h"
#include <stdint.h>

int main(void) {
    HL_CHECK(hl_x86_sub_nzcv(5, 5, 1) == ((UINT64_C(1) << 30) | (UINT64_C(1) << 29)));
    HL_CHECK(hl_x86_sub_nzcv(0, 1, 1) == (UINT64_C(1) << 31));
    HL_CHECK(hl_x86_sub_nzcv(2, 1, 4) == (UINT64_C(1) << 29));
    HL_CHECK(hl_x86_sub_nzcv(UINT64_C(0x80), 1, 1) == ((UINT64_C(1) << 29) | (UINT64_C(1) << 28)));
    HL_CHECK(hl_x86_sub_nzcv(UINT64_C(0x7fffffffffffffff), UINT64_MAX, 8) ==
             ((UINT64_C(1) << 31) | (UINT64_C(1) << 28)));
    return 0;
}
