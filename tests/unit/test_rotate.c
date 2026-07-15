#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/rotate.h"

int main(void) {
    struct cpu cpu;

    memset(&cpu, 0, sizeof(cpu));
    cpu.divop = 1 | ((uint64_t)RAX << 16); /* rcl al,cl */
    cpu.r[RAX] = UINT64_C(0x123480);
    cpu.r[RCX] = 1;
    cpu.nzcv = UINT64_C(1) << 29; /* stored ARM C=1 means x86 CF=0 */
    hl_x86_rotate_carry(&cpu);
    HL_CHECK(cpu.r[RAX] == UINT64_C(0x123400));
    HL_CHECK((cpu.nzcv & (UINT64_C(1) << 29)) == 0); /* outgoing x86 CF=1 */
    HL_CHECK((cpu.nzcv & (UINT64_C(1) << 28)) != 0); /* OF=MSB(result)^CF */

    uint16_t memory = UINT16_C(0x0001);
    memset(&cpu, 0, sizeof(cpu));
    cpu.divop = 2 | (UINT64_C(1) << 8) | (UINT64_C(1) << 9); /* rcr word [mem],cl */
    cpu.x87_ea = (uint64_t)&memory;
    cpu.r[RCX] = 1;
    cpu.nzcv = 0; /* stored ARM C=0 means x86 CF=1 */
    hl_x86_rotate_carry(&cpu);
    HL_CHECK(memory == UINT16_C(0x8000));
    HL_CHECK((cpu.nzcv & (UINT64_C(1) << 29)) == 0); /* outgoing x86 CF=1 */

    memset(&cpu, 0, sizeof(cpu));
    cpu.divop = 1 | ((uint64_t)RBX << 16);
    cpu.r[RBX] = UINT64_C(0xa5);
    cpu.r[RCX] = 9; /* byte width+carry: effective count zero */
    cpu.nzcv = UINT64_C(0xf0000000);
    hl_x86_rotate_carry(&cpu);
    HL_CHECK(cpu.r[RBX] == UINT64_C(0xa5));
    HL_CHECK(cpu.nzcv == UINT64_C(0xf0000000));
    return 0;
}
