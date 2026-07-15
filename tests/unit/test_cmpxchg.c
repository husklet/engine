#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cmpxchg.h"
#include "../../src/translator/guest/x86_64/cpu.h"

int main(void) {
    uint64_t memory[2] = {UINT64_C(0x1111222233334444), UINT64_C(0x5555666677778888)};
    struct cpu cpu;

    memset(&cpu, 0, sizeof(cpu));
    cpu.x87_ea = (uint64_t)memory;
    cpu.r[RAX] = memory[0];
    cpu.r[RDX] = memory[1];
    cpu.r[RBX] = UINT64_C(0x9999aaaabbbbcccc);
    cpu.r[RCX] = UINT64_C(0xddddeeeeffff0000);
    cpu.nzcv = UINT64_C(0xa0000000);
    hl_x86_cmpxchg16(&cpu);
    HL_CHECK(memory[0] == UINT64_C(0x9999aaaabbbbcccc));
    HL_CHECK(memory[1] == UINT64_C(0xddddeeeeffff0000));
    HL_CHECK((cpu.nzcv & (UINT64_C(1) << 30)) != 0);
    HL_CHECK((cpu.nzcv & ~(UINT64_C(1) << 30)) == (UINT64_C(0xa0000000) & ~(UINT64_C(1) << 30)));

    cpu.r[RAX] = 1;
    cpu.r[RDX] = 2;
    cpu.r[RBX] = 3;
    cpu.r[RCX] = 4;
    cpu.nzcv = UINT64_C(0xf0000000);
    hl_x86_cmpxchg16(&cpu);
    HL_CHECK(memory[0] == UINT64_C(0x9999aaaabbbbcccc));
    HL_CHECK(memory[1] == UINT64_C(0xddddeeeeffff0000));
    HL_CHECK(cpu.r[RAX] == memory[0]);
    HL_CHECK(cpu.r[RDX] == memory[1]);
    HL_CHECK((cpu.nzcv & (UINT64_C(1) << 30)) == 0);
    HL_CHECK((cpu.nzcv & ~(UINT64_C(1) << 30)) == (UINT64_C(0xf0000000) & ~(UINT64_C(1) << 30)));
    return 0;
}
