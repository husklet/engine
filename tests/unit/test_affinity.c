#include "test.h"

#include "../../src/linux_abi/affinity.h"

#include <string.h>

int main(void) {
    struct hl_linux_affinity affinity = {0};
    const uint8_t *mask = hl_linux_affinity_get(&affinity, 10);
    HL_CHECK(mask[0] == UINT8_C(0xff));
    HL_CHECK(mask[1] == UINT8_C(0x03));
    HL_CHECK(mask[2] == 0);
    HL_CHECK(hl_linux_affinity_first(&affinity, 10) == 0);

    uint8_t wanted[HL_LINUX_AFFINITY_BYTES] = {0};
    wanted[0] = UINT8_C(0x08);
    wanted[1] = UINT8_C(0x80);
    HL_CHECK(hl_linux_affinity_set(&affinity, wanted, sizeof wanted, 10) == 1);
    mask = hl_linux_affinity_get(&affinity, 10);
    HL_CHECK(mask[0] == UINT8_C(0x08));
    HL_CHECK(mask[1] == 0);
    HL_CHECK(hl_linux_affinity_first(&affinity, 10) == 3);

    memset(wanted, 0, sizeof wanted);
    wanted[4] = 1;
    HL_CHECK(hl_linux_affinity_set(&affinity, wanted, sizeof wanted, 10) == 0);
    HL_CHECK(hl_linux_affinity_first(&affinity, 10) == 3);

    char range[16];
    hl_linux_affinity_range(range, sizeof range, 1);
    HL_CHECK(strcmp(range, "0\n") == 0);
    hl_linux_affinity_range(range, sizeof range, 10);
    HL_CHECK(strcmp(range, "0-9\n") == 0);
    return EXIT_SUCCESS;
}
