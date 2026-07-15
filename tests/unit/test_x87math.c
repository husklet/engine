#include "test.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/x87math.h"

int main(void) {
    struct cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.x87_ea = X87_FSIN;
    cpu.st[0] = 0.5;
    hl_x86_x87_math(&cpu);
    HL_CHECK(fabs(cpu.st[0] - sin(0.5)) < 1e-15);

    cpu.x87_ea = X87_FPTAN;
    cpu.st[0] = 0.0;
    hl_x86_x87_math(&cpu);
    HL_CHECK(cpu.fptop == 7 && cpu.st[7] == 1.0 && cpu.st[0] == 0.0);

    memset(&cpu, 0, sizeof(cpu));
    cpu.x87_ea = X87_FCOS;
    cpu.st[0] = 0x1p63;
    hl_x86_x87_math(&cpu);
    HL_CHECK(cpu.st[0] == 0x1p63 && (cpu.fpsw & UINT64_C(0x400)) != 0);
    return 0;
}
