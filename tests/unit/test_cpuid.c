#include "test.h"

#include <stdint.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/cpuid.h"

#include <string.h>

static void query(struct cpu *cpu, uint32_t leaf, uint32_t subleaf) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[RAX] = leaf;
    cpu->r[RCX] = subleaf;
    hl_x86_cpuid(cpu);
}

int main(void) {
    struct cpu cpu;
    char vendor[13] = {0};
    char brand[49] = {0};

    query(&cpu, 0, 0);
    HL_CHECK(cpu.r[RAX] == 7);
    memcpy(vendor, &cpu.r[RBX], 4);
    memcpy(vendor + 4, &cpu.r[RDX], 4);
    memcpy(vendor + 8, &cpu.r[RCX], 4);
    HL_CHECK(strcmp(vendor, "GenuineIntel") == 0);

    query(&cpu, 1, 0);
    HL_CHECK(cpu.r[RAX] == 0x000206c2);
    HL_CHECK((cpu.r[RCX] & ((UINT64_C(1) << 19) | (UINT64_C(1) << 20) | (UINT64_C(1) << 25))) != 0);
    HL_CHECK((cpu.r[RCX] & (UINT64_C(1) << 28)) == 0); /* AVX is not advertised. */

    query(&cpu, 7, 0);
    HL_CHECK((cpu.r[RBX] & ((UINT64_C(1) << 3) | (UINT64_C(1) << 8) | (UINT64_C(1) << 9) |
                            (UINT64_C(1) << 29))) != 0);
    HL_CHECK(cpu.r[RDX] == (UINT64_C(1) << 4));
    query(&cpu, 7, 1);
    HL_CHECK(cpu.r[RAX] == 0 && cpu.r[RBX] == 0 && cpu.r[RCX] == 0 && cpu.r[RDX] == 0);

    for (uint32_t leaf = 0; leaf < 3; ++leaf) {
        query(&cpu, 0x80000002u + leaf, 0);
        memcpy(brand + leaf * 16, &cpu.r[RAX], 4);
        memcpy(brand + leaf * 16 + 4, &cpu.r[RBX], 4);
        memcpy(brand + leaf * 16 + 8, &cpu.r[RCX], 4);
        memcpy(brand + leaf * 16 + 12, &cpu.r[RDX], 4);
    }
    HL_CHECK(strcmp(brand, "hl JIT x86-64 processor") == 0);

    query(&cpu, 0x12345678u, 0);
    HL_CHECK(cpu.r[RAX] == 0 && cpu.r[RBX] == 0 && cpu.r[RCX] == 0 && cpu.r[RDX] == 0);
    return 0;
}
