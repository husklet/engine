#include "test.h"

#include "../../src/translator/guest/x86_64/x87state.h"

#include <math.h>
#include <string.h>

static uint64_t bits(double value) {
    uint64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

int main(void) {
    const uint64_t values[] = {
        UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000), UINT64_C(0xc004000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000001)};
    uint8_t image[10];
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        double value;
        double restored;
        memcpy(&value, &values[index], sizeof(value));
        hl_x86_ext80_store(value, image);
        restored = hl_x86_ext80_load(image);
        if (isnan(value))
            HL_CHECK(isnan(restored));
        else
            HL_CHECK(bits(restored) == values[index]);
    }
    return 0;
}
