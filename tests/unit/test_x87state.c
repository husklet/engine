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
        // TOP now shares cpu.fptop with the tag bits (x87state.h), so compare the low three only.
        HL_CHECK(cpu.fpcw == 0x027f && cpu.fpsw == 0x4500 && (cpu.fptop & 7) == 5);
        for (size_t index = 0; index < 8; ++index)
            HL_CHECK(cpu.st[index] == (double)index + 0.25);
        for (size_t index = 0; index < sizeof(cpu.v); ++index)
            HL_CHECK(((const uint8_t *)cpu.v)[index] == (uint8_t)index);
        cpu.fptop = 3;
        cpu.st[3] = -2.5;
        hl_x86_x87_store_ext80_pop(&cpu);
        HL_CHECK((cpu.fptop & 7) == 4);
        cpu.fptop = 4;
        hl_x86_x87_load_ext80(&cpu);
        HL_CHECK((cpu.fptop & 7) == 3 && cpu.st[3] == -2.5);
        HL_CHECK((cpu.fptop & HL_X87_ARMED) != 0);
        HL_CHECK(!hl_x87_phys_empty(cpu.fptop, 3));
        HL_CHECK(hl_x87_phys_empty(cpu.fptop, 4));
    }
    {
        // The tag bits: FXSAVE's abridged byte is PHYSICAL (bit i = st[i] live) and FXRSTOR brings it back,
        // while the register AREA is TOP-relative (slot i is ST(i)) -- both measured on hardware.
        struct cpu cpu = {0};
        uint8_t state_image[512] = {0};
        cpu.x87_ea = (uint64_t)(uintptr_t)state_image;
        cpu.fpcw = 0x037f;
        cpu.fptop = 5 | HL_X87_ARMED | HL_X87_EMPTY_ALL;
        for (size_t index = 0; index < 8; ++index)
            cpu.st[index] = (double)index + 1.0;
        hl_x87_phys_mark(&cpu.fptop, 5, 0); // ST(0)
        hl_x87_phys_mark(&cpu.fptop, 7, 0); // ST(2), with a hole at ST(1)
        hl_x86_fxsave(&cpu);
        HL_CHECK(state_image[4] == (hl_x87_tags_modelled() ? 0xa0 : 0xff));
        HL_CHECK(hl_x87_tag_word(cpu.fptop, cpu.st) == (hl_x87_tags_modelled() ? 0x33ff : 0x0000));
        {
            double slot0 = hl_x86_ext80_load(state_image + 32);
            double slot2 = hl_x86_ext80_load(state_image + 32 + 32);
            HL_CHECK(slot0 == cpu.st[5] && slot2 == cpu.st[7]);
        }
        cpu.fptop = 0;
        hl_x86_fxrstor(&cpu);
        HL_CHECK((cpu.fptop & 7) == 5);
        HL_CHECK(hl_x87_phys_empty(cpu.fptop, 6) == hl_x87_tags_modelled());
        HL_CHECK(!hl_x87_phys_empty(cpu.fptop, 5) && !hl_x87_phys_empty(cpu.fptop, 7));
        for (size_t index = 0; index < 8; ++index)
            HL_CHECK(cpu.st[index] == (double)index + 1.0);
    }
    return 0;
}
