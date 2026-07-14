#include "test.h"

#include "../../src/translator/window.h"

int main(void) {
    static const struct {
        uint64_t extent, offset, width, alignment;
        int expected;
    } cases[] = {
        {64, 0, 16, 1, 1},
        {64, 48, 16, 8, 1},
        {64, 49, 16, 1, 0},
        {64, 64, 0, 4, 1},
        {64, 64, 1, 1, 0},
        {64, 3, 4, 4, 0},
        {0, 0, 0, 1, 1},
        {0, 0, 1, 1, 0},
        {UINT64_MAX, UINT64_MAX - 15, 16, 1, 0},
        {UINT64_MAX, UINT64_MAX, 1, 1, 0},
        {UINT64_MAX, UINT64_MAX, 0, 1, 1},
        {UINT64_MAX, UINT64_MAX - 7, UINT64_MAX, 1, 0},
        {64, 0, 4, 0, 0},
    };

    size_t index;
    for (index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        HL_CHECK(hl_window_contains(cases[index].extent, cases[index].offset, cases[index].width,
                                    cases[index].alignment) == cases[index].expected);
    }
    return EXIT_SUCCESS;
}
