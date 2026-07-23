#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/flags.h"
#include "../../src/translator/guest/x86_64/rep.h"

static uint64_t descriptor(int width, int scan, int repne, int repeat, int reverse) {
    return (uint64_t)width | ((uint64_t)scan << 8) | ((uint64_t)repne << 9) | ((uint64_t)repeat << 10) |
           ((uint64_t)reverse << 11);
}

int main(void) {
    struct cpu cpu;
    uint8_t left[] = {1, 2, 3, 4};
    uint8_t right[] = {1, 2, 9, 4};

    memset(&cpu, 0, sizeof(cpu));
    cpu.divop = descriptor(1, 0, 0, 1, 0);
    cpu.r[RCX] = 0;
    cpu.r[RSI] = (uint64_t)left;
    cpu.r[RDI] = (uint64_t)right;
    cpu.nzcv = UINT64_C(0xf0000000);
    hl_x86_rep_compare(&cpu, 0, 0, 0);
    HL_CHECK(cpu.r[RCX] == 0 && cpu.r[RSI] == (uint64_t)left && cpu.r[RDI] == (uint64_t)right);
    HL_CHECK(cpu.nzcv == UINT64_C(0xf0000000));

    memset(&cpu, 0, sizeof(cpu));
    cpu.divop = descriptor(1, 0, 0, 1, 0); /* repe cmpsb */
    cpu.r[RCX] = 4;
    cpu.r[RSI] = (uint64_t)left;
    cpu.r[RDI] = (uint64_t)right;
    hl_x86_rep_compare(&cpu, 0, 0, 0);
    HL_CHECK(cpu.r[RCX] == 1 && cpu.r[RSI] == (uint64_t)(left + 3) && cpu.r[RDI] == (uint64_t)(right + 3));
    HL_CHECK(cpu.nzcv == hl_x86_sub_nzcv(3, 9, 1));

    memset(&cpu, 0, sizeof(cpu));
    cpu.divop = descriptor(1, 0, 0, 1, 1); /* reverse repe cmpsb */
    cpu.r[RCX] = 3;
    cpu.r[RSI] = (uint64_t)(left + 2);
    cpu.r[RDI] = (uint64_t)(right + 2);
    hl_x86_rep_compare(&cpu, 0, 0, 0);
    HL_CHECK(cpu.r[RCX] == 2 && cpu.r[RSI] == (uint64_t)(left + 1) && cpu.r[RDI] == (uint64_t)(right + 1));
    HL_CHECK(cpu.nzcv == hl_x86_sub_nzcv(3, 9, 1));

    {
        uint8_t haystack[] = {4, 5, 7, 8};
        uint64_t bias = UINT64_C(0x1000);
        uint64_t guest = (uint64_t)haystack - bias;
        memset(&cpu, 0, sizeof(cpu));
        cpu.divop = descriptor(1, 1, 1, 1, 0); /* repne scasb */
        cpu.r[RAX] = 7;
        cpu.r[RCX] = 4;
        cpu.r[RDI] = guest;
        hl_x86_rep_compare(&cpu, guest, guest + sizeof(haystack), bias);
        HL_CHECK(cpu.r[RCX] == 1 && cpu.r[RDI] == guest + 3);
        HL_CHECK(cpu.nzcv == hl_x86_sub_nzcv(7, 7, 1));
    }
    return 0;
}
