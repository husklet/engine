#include "test.h"

#include "../../src/translator/guest/x86_64/x87state.h"
#include "../../src/translator/guest/x86_64/cpu.h"

#include <math.h>
#include <string.h>

static uint64_t bits(double value) {
    uint64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

int main(void) {
    const uint64_t values[] = {UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000),
                               UINT64_C(0xc004000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                               UINT64_C(0x7ff8000000000001)};
    uint8_t image[10];
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        double value;
        double restored;
        memcpy(&value, &values[index], sizeof(value));
        hl_x86_ext80_store(value, image);
        restored = hl_x86_ext80_load(image);
        if (isnan(value))
            HL_CHECK(isnan(restored));
        else
            HL_CHECK(bits(restored) == values[index]);
    }
    {
        struct cpu cpu = {0};
        uint8_t state_image[512] = {0};
        cpu.x87_ea = (uint64_t)(uintptr_t)state_image;
        cpu.fpcw = 0x027f;
        cpu.fpsw = 0x4500;
        cpu.fptop = 5;
        for (size_t index = 0; index < 8; ++index)
            cpu.st[index] = (double)index + 0.25;
        for (size_t index = 0; index < sizeof(cpu.v); ++index)
            ((uint8_t *)cpu.v)[index] = (uint8_t)index;
        hl_x86_fxsave(&cpu);
        memset(cpu.st, 0, sizeof(cpu.st));
        memset(cpu.v, 0, sizeof(cpu.v));
        cpu.fpcw = 0;
        cpu.fpsw = 0;
        cpu.fptop = 0;
        hl_x86_fxrstor(&cpu);
        HL_CHECK(cpu.fpcw == 0x027f && cpu.fpsw == 0x4500 && cpu.fptop == 5);
        for (size_t index = 0; index < 8; ++index)
            HL_CHECK(cpu.st[index] == (double)index + 0.25);
        for (size_t index = 0; index < sizeof(cpu.v); ++index)
            HL_CHECK(((const uint8_t *)cpu.v)[index] == (uint8_t)index);
        cpu.fptop = 3;
        cpu.st[3] = -2.5;
        hl_x86_x87_store_ext80_pop(&cpu);
        HL_CHECK(cpu.fptop == 4);
        cpu.fptop = 4;
        hl_x86_x87_load_ext80(&cpu);
        HL_CHECK(cpu.fptop == 3 && cpu.st[3] == -2.5);
    }
    return 0;
}
