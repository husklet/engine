#include "test.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../../src/translator/guest/x86_64/cpu.h"
#include "../../src/translator/guest/x86_64/legacy.h"

typedef struct callbacks {
    int64_t now;
    int alarm_error;
    uint64_t alarm_remaining;
    uint64_t alarm_seconds;
} callbacks;

static int64_t now_seconds(void *context) {
    return ((callbacks *)context)->now;
}

static int set_alarm(void *context, uint64_t seconds, uint64_t *remaining) {
    callbacks *state = context;
    state->alarm_seconds = seconds;
    *remaining = state->alarm_remaining;
    return state->alarm_error;
}

static void initialize(struct cpu *cpu, uint64_t number) {
    memset(cpu, 0, sizeof(*cpu));
    for (size_t index = 0; index < 16; ++index)
        cpu->r[index] = UINT64_C(0x1000) + index;
    cpu->r[RAX] = number;
}

typedef struct rewrite {
    uint64_t source;
    uint64_t target;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
} rewrite;

int main(void) {
    callbacks state = {.now = INT64_C(1700000000), .alarm_remaining = 4};
    hl_x86_legacy_context context = {
        .time_seconds = now_seconds,
        .set_alarm = set_alarm,
        .callback_context = &state,
    };
    struct cpu cpu;
    const uint64_t cwd = (uint64_t)(int64_t)-100;
    const rewrite rewrites[] = {
        {2, 257, cwd, 0x1007, 0x1006, 0x1002, 0x1008},  {4, 262, cwd, 0x1007, 0x1006, 0, 0x1008},
        {6, 262, cwd, 0x1007, 0x1006, 0x100, 0x1008},   {21, 269, cwd, 0x1007, 0x1006, 0x100a, 0x1008},
        {83, 258, cwd, 0x1007, 0x1006, 0x100a, 0x1008}, {84, 263, cwd, 0x1007, 0x200, 0x100a, 0x1008},
        {87, 263, cwd, 0x1007, 0, 0x100a, 0x1008},      {85, 257, cwd, 0x1007, 0x241, 0x1006, 0x1008},
        {89, 267, cwd, 0x1007, 0x1006, 0x1002, 0x1008}, {90, 268, cwd, 0x1007, 0x1006, 0x100a, 0x1008},
        {88, 266, 0x1007, cwd, 0x1006, 0x100a, 0x1008}, {92, 260, cwd, 0x1007, 0x1006, 0x1002, 0},
        {94, 260, cwd, 0x1007, 0x1006, 0x1002, 0x100},  {133, 259, cwd, 0x1007, 0x1006, 0x1002, 0x1008},
        {82, 264, cwd, 0x1007, cwd, 0x1006, 0x1008},    {86, 265, cwd, 0x1007, cwd, 0x1006, 0},
    };

    for (size_t index = 0; index < sizeof(rewrites) / sizeof(rewrites[0]); ++index) {
        const rewrite *expected = &rewrites[index];
        initialize(&cpu, expected->source);
        HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0);
        HL_CHECK(cpu.r[RAX] == expected->target && cpu.r[RDI] == expected->rdi && cpu.r[RSI] == expected->rsi);
        HL_CHECK(cpu.r[RDX] == expected->rdx && cpu.r[10] == expected->r10 && cpu.r[8] == expected->r8);
        HL_CHECK(cpu.r[RBX] == 0x1003 && cpu.r[9] == 0x1009 && cpu.r[15] == 0x100f);
    }

    initialize(&cpu, 158);
    cpu.r[RDI] = 0x1002;
    cpu.r[RSI] = UINT64_C(0xabcdef);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && cpu.fs_base == UINT64_C(0xabcdef) && cpu.r[RAX] == 0);
    initialize(&cpu, 158);
    cpu.fs_base = UINT64_C(0x12345678);
    cpu.r[RDI] = 0x1003;
    cpu.r[RSI] = (uint64_t)(uintptr_t)&state.now;
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && state.now == INT64_C(0x12345678));
    initialize(&cpu, 158);
    cpu.r[RDI] = 0xffff;
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && (int64_t)cpu.r[RAX] == -EINVAL);

    initialize(&cpu, 22);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 293 && cpu.r[RSI] == 0);
    initialize(&cpu, 33);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 292 && cpu.r[RDX] == 0x40000000u);
    initialize(&cpu, 284);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 290 && cpu.r[RSI] == 0);
    initialize(&cpu, 282);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 289 && cpu.r[10] == 0);
    initialize(&cpu, 232);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 281 && cpu.r[8] == 0);
    initialize(&cpu, 253);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 294 && cpu.r[RDI] == 0);
    initialize(&cpu, 213);
    cpu.r[RDI] = 0;
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && (int64_t)cpu.r[RAX] == -EINVAL);

    initialize(&cpu, 56);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[10] == 0x1008 && cpu.r[8] == 0x100a);
    initialize(&cpu, 57);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 56 && cpu.r[RDI] == 17);
    cpu.r[RDI] = cpu.r[RSI] = cpu.r[RDX] = cpu.r[10] = cpu.r[8] = 0;
    hl_x86_legacy_restore_fork(&cpu);
    HL_CHECK(cpu.r[RDI] == 0x1007 && cpu.r[RSI] == 0x1006 && cpu.r[RDX] == 0x1002);
    HL_CHECK(cpu.r[10] == 0x100a && cpu.r[8] == 0x1008);
    cpu.r[RDI] = 77;
    hl_x86_legacy_restore_fork(&cpu);
    HL_CHECK(cpu.r[RDI] == 77);

    {
        int64_t legacy_times[4] = {11, 12, 21, 22};
        uint64_t bias = UINT64_C(0x1000);
        uint64_t guest = (uint64_t)(uintptr_t)legacy_times - bias;
        context.nonpie_low = guest;
        context.nonpie_high = guest + sizeof(legacy_times);
        context.nonpie_bias = bias;
        initialize(&cpu, 235);
        cpu.r[RSI] = guest;
        HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 0 && cpu.r[RAX] == 280);
        struct timespec *converted = (struct timespec *)(uintptr_t)cpu.r[RDX];
        HL_CHECK(converted[0].tv_sec == 11 && converted[0].tv_nsec == 12000);
        HL_CHECK(converted[1].tv_sec == 21 && converted[1].tv_nsec == 22000);
        context.nonpie_low = context.nonpie_high = context.nonpie_bias = 0;
    }

    initialize(&cpu, 201);
    cpu.r[RDI] = (uint64_t)(uintptr_t)&state.now;
    state.now = INT64_C(1700000000);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && cpu.r[RAX] == UINT64_C(1700000000));
    HL_CHECK(state.now == INT64_C(1700000000));
    initialize(&cpu, 37);
    cpu.r[RDI] = 9;
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && cpu.r[RAX] == 4 && state.alarm_seconds == 9);
    state.alarm_error = EPERM;
    initialize(&cpu, 37);
    HL_CHECK(hl_x86_legacy_normalize(&cpu, &context) == 1 && (int64_t)cpu.r[RAX] == -EPERM);
    return 0;
}
