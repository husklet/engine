#include "test.h"

#include "../../src/linux_abi/errno.h"

int main(void) {
    HL_CHECK(hl_linux_errno_from_macos(0) == 0);
    HL_CHECK(hl_linux_errno_from_macos(1) == 1);
#if defined(__linux__)
    HL_CHECK(hl_linux_errno_from_macos(11) == 11);
    HL_CHECK(hl_linux_errno_from_macos(35) == 35);
    HL_CHECK(hl_linux_errno_from_macos(62) == 62);
    HL_CHECK(hl_linux_errno_from_macos(4095) == 4095);
#else
    HL_CHECK(hl_linux_errno_from_macos(11) == 35);
    HL_CHECK(hl_linux_errno_from_macos(35) == 11);
    HL_CHECK(hl_linux_errno_from_macos(62) == 40);
    HL_CHECK(hl_linux_errno_from_macos(78) == 38);
    HL_CHECK(hl_linux_errno_from_macos(84) == 75);
    HL_CHECK(hl_linux_errno_from_macos(91) == 42);
    HL_CHECK(hl_linux_errno_from_macos(93) == 61);
    HL_CHECK(hl_linux_errno_from_macos(96) == 61);
    HL_CHECK(hl_linux_errno_from_macos(102) == 95);
    HL_CHECK(hl_linux_errno_from_macos(104) == 131);
    HL_CHECK(hl_linux_errno_from_macos(105) == 130);
    HL_CHECK(hl_linux_errno_from_macos(4095) == 4095);
#endif
    HL_CHECK(hl_linux_errno_from_macos(-1) == -1);
    return EXIT_SUCCESS;
}
