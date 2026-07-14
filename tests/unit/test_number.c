#include "test.h"

#include "../../src/linux_abi/number.h"

int main(void) {
    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_AARCH64, 63) == 63);
    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_AARCH64, 999) == 999);

    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_X86_64, 0) == 63);
    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_X86_64, 60) == 93);
    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_X86_64, 257) == 56);
    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_X86_64, 158) == (HL_LINUX_SYSCALL_X86_ONLY | 158));
    HL_CHECK(hl_linux_syscall_number(HL_LINUX_GUEST_X86_64, 999) == (HL_LINUX_SYSCALL_X86_ONLY | 999));
    return EXIT_SUCCESS;
}
