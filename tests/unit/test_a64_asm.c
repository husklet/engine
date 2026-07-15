#include "test.h"

#include <stdalign.h>
#include <string.h>

#include "../../src/translator/host/aarch64/asm.h"

static void reset(hl_emit_state *state, uint8_t *bytes) {
    memset(bytes, 0, 128);
    state->base = bytes;
    state->cursor = bytes;
    state->start = bytes;
    state->rx_delta = 0;
}

int main(void) {
    alignas(uint32_t) uint8_t bytes[128];
    hl_emit_state state = {0};
    uint32_t *words = (uint32_t *)bytes;

    reset(&state, bytes);
    hl_a64_str(&state, 1, 2, 24);
    hl_a64_ldr(&state, 3, 4, 40);
    hl_a64_stur(&state, 5, 31, -16);
    hl_a64_ldur(&state, 6, 31, -24);
    HL_CHECK(state.cursor == bytes + 16);
    HL_CHECK(words[0] == UINT32_C(0xF9000C41));
    HL_CHECK(words[1] == UINT32_C(0xF9401483));
    HL_CHECK(words[2] == UINT32_C(0xF81F03E5));
    HL_CHECK(words[3] == UINT32_C(0xF85E83E6));

    reset(&state, bytes);
    hl_a64_mov_sp_from(&state, 9);
    hl_a64_mov_from_sp(&state, 10);
    hl_a64_movr(&state, 11, 12);
    hl_a64_br(&state, 13);
    hl_a64_ret(&state);
    HL_CHECK(words[0] == UINT32_C(0x9100013F));
    HL_CHECK(words[1] == UINT32_C(0x910003EA));
    HL_CHECK(words[2] == UINT32_C(0xAA0C03EB));
    HL_CHECK(words[3] == UINT32_C(0xD61F01A0));
    HL_CHECK(words[4] == UINT32_C(0xD65F03C0));

    reset(&state, bytes);
    hl_a64_stp(&state, 1, 2, 3, -16);
    hl_a64_ldp(&state, 4, 5, 6, 16);
    hl_a64_stp_q(&state, 7, 8, 9, -32);
    hl_a64_ldp_q(&state, 10, 11, 12, 32);
    HL_CHECK(words[0] == UINT32_C(0xA93F0861));
    HL_CHECK(words[1] == UINT32_C(0xA94114C4));
    HL_CHECK(words[2] == UINT32_C(0xAD3F2127));
    HL_CHECK(words[3] == UINT32_C(0xAD412D8A));

    reset(&state, bytes);
    hl_a64_addi(&state, 1, 2, 0x123);
    hl_a64_subi(&state, 3, 4, 0x456);
    hl_a64_addlsl3(&state, 5, 6, 7);
    hl_a64_addlsl4(&state, 8, 9, 10);
    HL_CHECK(words[0] == UINT32_C(0x91048C41));
    HL_CHECK(words[1] == UINT32_C(0xD1115883));
    HL_CHECK(words[2] == UINT32_C(0x8B070CC5));
    HL_CHECK(words[3] == UINT32_C(0x8B0A1128));

    reset(&state, bytes);
    hl_a64_movconst(&state, 3, UINT64_C(0x1234000056789ABC));
    HL_CHECK(state.cursor == bytes + 12);
    HL_CHECK(words[0] == UINT32_C(0xD2935783));
    HL_CHECK(words[1] == UINT32_C(0xF2AACF03));
    HL_CHECK(words[2] == UINT32_C(0xF2E24683));

    reset(&state, bytes);
    state.rx_delta = 0x1000;
    uintptr_t pc = (uintptr_t)hl_emit_rx(&state, state.cursor);
    uint64_t nearby = ((uint64_t)pc & ~UINT64_C(0xFFF)) + UINT64_C(0x2345);
    hl_a64_adrp_add(&state, 7, nearby);
    HL_CHECK(state.cursor == bytes + 8);
    HL_CHECK(words[1] == (UINT32_C(0x91000000) | ((uint32_t)(nearby & 0xFFF) << 10) | (7u << 5) | 7u));

    reset(&state, bytes);
    pc = (uintptr_t)hl_emit_rx(&state, state.cursor);
    uint64_t far = ((uint64_t)pc & ~UINT64_C(0xFFF)) + (UINT64_C(1) << 32) + UINT64_C(0xABC);
    unsigned far_words = 1;
    for (unsigned shift = 16; shift < 64; shift += 16)
        if ((far >> shift) & UINT64_C(0xFFFF)) far_words++;
    hl_a64_adrp_add(&state, 7, far);
    HL_CHECK(state.cursor == bytes + far_words * 4);
    HL_CHECK((words[0] & UINT32_C(0xFF800000)) == UINT32_C(0xD2800000));

    reset(&state, bytes);
    hl_a64_load_cpu(&state, 8, 5);
    HL_CHECK(state.cursor == bytes + 12);
    HL_CHECK(words[0] == UINT32_C(0xD53BD068));
    HL_CHECK(words[1] == UINT32_C(0x9243F108));
    HL_CHECK(words[2] == UINT32_C(0xF9401508));
    return EXIT_SUCCESS;
}
