#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../../src/translator/guest/aarch64/cpu.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "../../src/translator/guest/aarch64/signal.h"

static uint64_t canonicalize(void *context, uint64_t pc) {
    return pc - *(const uint64_t *)context;
}

static uint32_t load_u32(uint64_t address) {
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static uint64_t load_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

int main(void) {
    _Alignas(16) uint8_t stack[16384];
    uint8_t *alternate = stack + 8192;
    const size_t alternate_size = 8192;
    struct cpu cpu;
    uint64_t bias = UINT64_C(0x100000000);
    int code = -6;
    uint64_t value = UINT64_C(0xabcdef0123456789);
    uint64_t address = UINT64_C(0x99887766);
    int pid = 123;
    int uid = 456;
    hl_aarch64_signal_state state = {
        .handler = UINT64_C(0x70001000),
        .flags = 0,
        .mask = UINT64_C(0x200),
        .code = &code,
        .value = &value,
        .address = &address,
        .pid = &pid,
        .uid = &uid,
        .sigreturn_pc = UINT64_C(0xfffffffffff0),
        .canonicalize_pc = canonicalize,
        .callback_context = &bias,
    };
    uint64_t saved_x[31];
    uint64_t saved_v[64];
    uint64_t saved_sp;
    uint64_t saved_pc;
    uint64_t saved_nzcv;
    uint64_t saved_mask;

    memset(&cpu, 0, sizeof(cpu));
    memset(stack, 0xa7, sizeof(stack));
    for (size_t index = 0; index < 31; ++index)
        cpu.x[index] = UINT64_C(0x1000) + index;
    for (size_t index = 0; index < 64; ++index)
        cpu.v[index] = UINT64_C(0x8000) + index;
    cpu.sp = (uint64_t)(uintptr_t)(stack + 7000);
    cpu.pc = bias + UINT64_C(0x401234);
    cpu.nzcv = UINT64_C(0xa0000000);
    cpu.sigmask = UINT64_C(0x40);
    memcpy(saved_x, cpu.x, sizeof(saved_x));
    memcpy(saved_v, cpu.v, sizeof(saved_v));
    saved_sp = cpu.sp;
    saved_pc = cpu.pc - bias;
    saved_nzcv = cpu.nzcv;
    saved_mask = cpu.sigmask;

    hl_aarch64_signal_build(&cpu, 12, &state);
    uint64_t frame = cpu.sp;
    uint64_t uc = frame + 128;
    uint64_t mc = uc + 176;
    HL_CHECK(frame == ((saved_sp - 4688) & ~UINT64_C(15)));
    HL_CHECK(load_u32(frame) == 12 && (int32_t)load_u32(frame + 8) == -6);
    HL_CHECK(load_u32(frame + 16) == 123 && load_u32(frame + 20) == 456);
    HL_CHECK(load_u64(frame + 24) == UINT64_C(0xabcdef0123456789));
    HL_CHECK(code == 0 && value == 0 && address == 0 && pid == 0 && uid == 0);
    HL_CHECK(load_u64(uc + 40) == saved_mask);
    HL_CHECK(load_u64(mc + 256) == saved_sp && load_u64(mc + 264) == saved_pc);
    HL_CHECK(load_u64(mc + 272) == saved_nzcv);
    HL_CHECK(load_u32(mc + 288) == UINT32_C(0x46508001) && load_u32(mc + 292) == 528);
    HL_CHECK(load_u32(mc + 288 + 16 + sizeof(cpu.v)) == 0);
    HL_CHECK(cpu.x[0] == 12 && cpu.x[1] == frame && cpu.x[2] == uc);
    HL_CHECK(cpu.x[30] == state.sigreturn_pc && cpu.pc == state.handler);
    HL_CHECK(cpu.sigmask == (saved_mask | state.mask | (UINT64_C(1) << 11)));

    /* A handler may edit its ucontext; sigreturn must restore those edits, not cached pre-frame state. */
    uint64_t edited = UINT64_C(0xfeedface);
    memcpy((void *)(uintptr_t)(mc + 8 + 5 * 8), &edited, sizeof(edited));
    hl_aarch64_signal_restore(&cpu);
    for (size_t index = 0; index < 31; ++index)
        HL_CHECK(cpu.x[index] == (index == 5 ? edited : saved_x[index]));
    HL_CHECK(cpu.sp == saved_sp && cpu.pc == saved_pc && cpu.nzcv == saved_nzcv && cpu.sigmask == saved_mask);
    HL_CHECK(memcmp(cpu.v, saved_v, sizeof(saved_v)) == 0);

    /* SA_ONSTACK selects the alternate top and publishes SS_ONSTACK in uc_stack. */
    code = 1;
    value = address = 2;
    pid = uid = 0;
    memset(&cpu, 0, sizeof(cpu));
    cpu.sp = (uint64_t)(uintptr_t)(stack + 7000);
    cpu.alt_sp = (uint64_t)(uintptr_t)alternate;
    cpu.alt_size = alternate_size;
    cpu.sigmask = 1;
    state.flags = UINT64_C(0x08000000) | UINT64_C(0x40000000); /* SA_ONSTACK | SA_NODEFER */
    state.mask = UINT64_C(0x80);
    hl_aarch64_signal_build(&cpu, 5, &state);
    frame = cpu.sp;
    uc = frame + 128;
    HL_CHECK(frame == (((uint64_t)(uintptr_t)(alternate + alternate_size) - 4688) & ~UINT64_C(15)));
    HL_CHECK(load_u64(uc + 16) == (uint64_t)(uintptr_t)alternate);
    HL_CHECK(load_u32(uc + 24) == 1 && load_u64(uc + 32) == alternate_size);
    HL_CHECK(cpu.sigmask == (UINT64_C(1) | UINT64_C(0x80))); /* SA_NODEFER omits the signal's own bit */

    /* A nested handler already on the alternate stack grows from its current SP, not from the top again. */
    uint64_t nested_sp = (uint64_t)(uintptr_t)(alternate + 7000);
    cpu.sp = nested_sp;
    code = pid = uid = 0;
    value = address = 0;
    hl_aarch64_signal_build(&cpu, 6, &state);
    HL_CHECK(cpu.sp == ((nested_sp - 4688) & ~UINT64_C(15)));
    return 0;
}
