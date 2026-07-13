#include "test.h"

#include "hl/fake_host.h"
#include "hl/linux_abi.h"

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[8];
    hl_linux_ofd_entry ofds[8];
    hl_linux_fd original;
    hl_linux_fd duplicate;
    const hl_linux_fd_entry *fd_entry;
    const hl_linux_ofd_entry *ofd_entry;
    hl_host_handle closed;

    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) ==
             HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0123, 1, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(original != duplicate);
    HL_CHECK(hl_linux_fd_get(&linux_abi, duplicate, &fd_entry, &ofd_entry) == HL_STATUS_OK);
    HL_CHECK(fd_entry->ofd == fds[original].ofd);
    HL_CHECK(ofd_entry->references == 2 && ofd_entry->status_flags == 0123);

    closed = HL_HOST_HANDLE_INVALID;
    HL_CHECK(hl_linux_fd_close(&linux_abi, original, &closed) == HL_STATUS_OK);
    HL_CHECK(closed == HL_HOST_HANDLE_INVALID);
    HL_CHECK(hl_linux_fd_close(&linux_abi, duplicate, &closed) == HL_STATUS_OK);
    HL_CHECK(closed == 55);
    HL_CHECK(hl_linux_fd_close(&linux_abi, duplicate, &closed) == HL_STATUS_NOT_FOUND);
    return EXIT_SUCCESS;
}
