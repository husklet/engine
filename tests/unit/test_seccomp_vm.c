#include "test.h"

#include "seccomp_vm.h"

#include <stddef.h>
#include <stdlib.h>

#define STMT(code_, value_) ((struct hl_linux_sock_filter){(code_), 0, 0, (value_)})
#define JUMP(code_, value_, yes_, no_) ((struct hl_linux_sock_filter){(code_), (yes_), (no_), (value_)})

int main(void) {
    struct hl_linux_seccomp_data data = {
        .nr = 257, .arch = 0xc000003e, .instruction_pointer = UINT64_C(0x123456789abcdef0), .args = {3, 4, 5, 6, 7, 8}};
    const struct hl_linux_sock_filter syscall_filter[] = {
        STMT(0x20, (uint32_t)offsetof(struct hl_linux_seccomp_data, nr)),
        JUMP(0x15, 257, 0, 1),
        STMT(0x06, HL_LINUX_SECCOMP_RET_ERRNO | 13),
        STMT(0x06, HL_LINUX_SECCOMP_RET_ALLOW),
    };
    const struct hl_linux_sock_filter arithmetic[] = {
        STMT(0x00, 6), STMT(0x02, 0), STMT(0x01, 7),  STMT(0x60, 0),
        STMT(0xac, 0), STMT(0x24, 3), STMT(0x94, 10), STMT(0x16, 0),
    };
    const struct hl_linux_sock_filter invalid_load[] = {STMT(0x20, (uint32_t)sizeof data),
                                                        STMT(0x06, HL_LINUX_SECCOMP_RET_ALLOW)};
    const struct hl_linux_sock_filter invalid_jump[] = {STMT(0x05, 8), STMT(0x06, HL_LINUX_SECCOMP_RET_ALLOW)};

    HL_CHECK(hl_seccomp_run(syscall_filter, 4, &data) == (HL_LINUX_SECCOMP_RET_ERRNO | 13));
    data.nr = 1;
    HL_CHECK(hl_seccomp_run(syscall_filter, 4, &data) == HL_LINUX_SECCOMP_RET_ALLOW);
    HL_CHECK(hl_seccomp_run(arithmetic, 8, &data) == 3);
    HL_CHECK(hl_seccomp_run(invalid_load, 2, &data) == HL_LINUX_SECCOMP_RET_KILL_THREAD);
    HL_CHECK(hl_seccomp_run(invalid_jump, 2, &data) == HL_LINUX_SECCOMP_RET_KILL_THREAD);
    HL_CHECK(hl_seccomp_run(NULL, 1, &data) == HL_LINUX_SECCOMP_RET_KILL_THREAD);
    HL_CHECK(hl_seccomp_run(syscall_filter, 0, &data) == HL_LINUX_SECCOMP_RET_KILL_THREAD);

    HL_CHECK(hl_seccomp_precedence(HL_LINUX_SECCOMP_RET_KILL_PROCESS) == 0);
    HL_CHECK(hl_seccomp_precedence(HL_LINUX_SECCOMP_RET_ERRNO | 1) < hl_seccomp_precedence(HL_LINUX_SECCOMP_RET_ALLOW));
    HL_CHECK(hl_seccomp_precedence(UINT32_C(0x12340000)) == 1);
    return EXIT_SUCCESS;
}
