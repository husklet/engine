#include "test.h"
#include "../../src/translator/guest/x86_64/operand.h"
#include <stdint.h>

int main(void) {
    uint64_t value = UINT64_C(0x8877665544332211);
    uint64_t address = (uint64_t)&value;
    HL_CHECK(hl_x86_operand_read(address, 1) == UINT64_C(0x11));
    HL_CHECK(hl_x86_operand_read(address, 2) == UINT64_C(0x2211));
    HL_CHECK(hl_x86_operand_read(address, 4) == UINT64_C(0x44332211));
    HL_CHECK(hl_x86_operand_read(address, 8) == value);
    return 0;
}
