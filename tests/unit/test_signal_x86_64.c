#include "test.h"

#include <stdint.h>
#include <string.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/frame.h"

static uint64_t load_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static uint64_t handler(void *context, int signal_number) {
    (void)signal_number;
    return *(const uint64_t *)context;
}

int main(void) {
    _Alignas(16) uint8_t stack[16384];
    struct cpu cpu;
    struct cpu saved;
    int code = -6, pid = 123, uid = 456;
    uint64_t value = UINT64_C(0xabcdef0123456789), address = UINT64_C(0x99887766);
    hl_x86_signal_state state = {
        .handler = UINT64_C(0x70001000), .mask = UINT64_C(0x200), .code = &code, .value = &value,
        .address = &address, .pid = &pid, .uid = &uid, .sigreturn_pc = UINT64_C(0xfffffffffff0),
    };

    memset(&cpu, 0, sizeof(cpu));
    for (size_t i = 0; i < 16; ++i) cpu.r[i] = UINT64_C(0x1000) + i;
    for (size_t i = 0; i < 32; ++i) cpu.v[i] = UINT64_C(0x2000) + i;
    for (size_t i = 0; i < 32; ++i) cpu.vhi[i] = UINT64_C(0x3000) + i;
    for (size_t i = 0; i < 8; ++i) cpu.kreg[i] = UINT64_C(0x4000) + i;
    for (size_t i = 0; i < 8; ++i) cpu.st[i] = (double)i + 0.25;
    cpu.r[4] = (uint64_t)(uintptr_t)(stack + 7000);
    cpu.rip = UINT64_C(0x401234);
    cpu.nzcv = UINT64_C(0xb0000000);
    cpu.df = 1;
    cpu.sigmask = UINT64_C(0x40);
    cpu.fptop = 3; cpu.fpsw = 4; cpu.fpcw = 5;
    saved = cpu;

    hl_x86_signal_build(&cpu, 12, &state);
    uint64_t uc = cpu.r[2], mc = uc + 40;
    HL_CHECK(uc == ((saved.r[4] - 2048) & ~UINT64_C(15)));
    HL_CHECK(cpu.r[4] == uc - 8 && load_u64(cpu.r[4]) == state.sigreturn_pc);
    HL_CHECK(cpu.r[7] == 12 && cpu.r[6] == uc + 512 && cpu.rip == state.handler);
    HL_CHECK(load_u64(mc + 16 * 8) == saved.rip);
    HL_CHECK(load_u64(uc + 296) == saved.sigmask);
    HL_CHECK(code == 0 && value == 0 && address == 0 && pid == 0 && uid == 0);
    HL_CHECK(cpu.sigmask == (saved.sigmask | state.mask | (UINT64_C(1) << 11)));

    /* Sigreturn restores the frame, including edits made by the handler. */
    uint64_t edited = UINT64_C(0xfeedface);
    memcpy((void *)(uintptr_t)(mc + 13 * 8), &edited, sizeof(edited)); /* gregs[13] = rax */
    cpu.r[4] = uc;
    hl_x86_signal_restore(&cpu);
    HL_CHECK(cpu.r[0] == edited && cpu.r[4] == saved.r[4] && cpu.rip == saved.rip);
    HL_CHECK(cpu.nzcv == saved.nzcv && cpu.df == saved.df && cpu.sigmask == saved.sigmask);
    HL_CHECK(memcmp(cpu.v, saved.v, sizeof(cpu.v)) == 0);
    HL_CHECK(memcmp(cpu.vhi, saved.vhi, sizeof(cpu.vhi)) == 0);
    HL_CHECK(memcmp(cpu.kreg, saved.kreg, sizeof(cpu.kreg)) == 0);
    HL_CHECK(memcmp(cpu.st, saved.st, sizeof(cpu.st)) == 0);
    HL_CHECK(cpu.fptop == saved.fptop && cpu.fpsw == saved.fpsw && cpu.fpcw == saved.fpcw);

    /* SA_ONSTACK selects the alternate stack; SA_NODEFER leaves the signal unblocked. */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = (uint64_t)(uintptr_t)(stack + 7000);
    cpu.alt_sp = (uint64_t)(uintptr_t)(stack + 8192);
    cpu.alt_size = 8192;
    state.flags = UINT64_C(0x08000000) | UINT64_C(0x40000000);
    state.mask = UINT64_C(0x80);
    hl_x86_signal_build(&cpu, 5, &state);
    uc = cpu.r[2];
    HL_CHECK(uc == ((cpu.alt_sp + cpu.alt_size - 2048) & ~UINT64_C(15)));
    HL_CHECK(*(const int *)(uintptr_t)(uc + 24) == 1);
    HL_CHECK(cpu.sigmask == state.mask);

    uint64_t installed = 2, pending = 0, addresses[65] = {0};
    int codes[65] = {0};
    hl_x86_signal_queue queue = {handler, &installed, codes, addresses, &pending};
    cpu.rip = UINT64_C(0x1234); cpu.sigmask = UINT64_MAX;
    HL_CHECK(hl_x86_signal_raise_divide(&cpu, &queue, 1 /* FPE_INTDIV */) == 1);
    HL_CHECK(codes[8] == 1 && addresses[8] == cpu.rip && (pending & (UINT64_C(1) << 8)));
    cpu.divop = 4 | (2 << 8); pending = 0;
    HL_CHECK(hl_x86_signal_raise_trap(&cpu, &queue) == 1);
    HL_CHECK(codes[4] == 2 && addresses[4] == cpu.rip && (pending & (UINT64_C(1) << 4)));
    installed = 1;
    HL_CHECK(hl_x86_signal_raise_divide(&cpu, &queue, 1 /* FPE_INTDIV */) == 0);

    uint64_t flags = hl_x86_signal_nzcv_to_eflags(UINT64_C(0xb0000000));
    HL_CHECK(hl_x86_signal_eflags_to_nzcv(flags) == UINT64_C(0xb0000000));
    return 0;
}
