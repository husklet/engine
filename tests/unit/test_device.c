#include "test.h"

#include "../../src/linux_abi/device.h"

int main(void) {
    HL_CHECK(hl_linux_device_major(0) == 0);
    HL_CHECK(hl_linux_device_minor(0) == 0);
    HL_CHECK(hl_linux_device_major(UINT64_C(0x0801)) == 8);
    HL_CHECK(hl_linux_device_minor(UINT64_C(0x0801)) == 1);
    HL_CHECK(hl_linux_device_major(UINT64_C(0x1200006734589)) == UINT32_C(0x12345));
    HL_CHECK(hl_linux_device_minor(UINT64_C(0x1200006734589)) == UINT32_C(0x6789));
    HL_CHECK(hl_linux_device_major(UINT64_MAX) == UINT32_MAX);
    HL_CHECK(hl_linux_device_minor(UINT64_MAX) == UINT32_MAX);
    return EXIT_SUCCESS;
}
