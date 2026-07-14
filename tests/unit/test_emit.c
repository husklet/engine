#include "test.h"

#include "../../src/translator/emit.h"

int main(void) {
    uint8_t bytes[32];
    hl_emit_state state = {
        .base = bytes,
        .cursor = bytes + 8,
        .start = bytes + 4,
        .rx_delta = 16,
        .dual_alias = 1,
        .wx_toggles = 3,
        .mapping = {.writable_address = (uintptr_t)bytes, .executable_address = (uintptr_t)(bytes + 16)},
    };

    HL_CHECK(hl_emit_rx(&state, state.base) == bytes + 16);
    HL_CHECK(hl_emit_rw(&state, hl_emit_rx(&state, state.cursor)) == state.cursor);
    HL_CHECK(state.start == bytes + 4);
    HL_CHECK(state.dual_alias == (state.mapping.writable_address != state.mapping.executable_address));
    HL_CHECK(state.mapping.executable_address - state.mapping.writable_address == (uintptr_t)state.rx_delta);
    HL_CHECK(state.wx_toggles == 3);
    return EXIT_SUCCESS;
}
