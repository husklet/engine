#include "test.h"

#include "../../src/translator/emit.h"

int main(void) {
    uint8_t bytes[32];
    hl_emit_state state = {bytes, bytes + 8, bytes + 4, 16};

    HL_CHECK(hl_emit_rx(&state, state.base) == bytes + 16);
    HL_CHECK(hl_emit_rw(&state, hl_emit_rx(&state, state.cursor)) == state.cursor);
    HL_CHECK(state.start == bytes + 4);
    return EXIT_SUCCESS;
}
