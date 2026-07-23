#include "test.h"

#include "hl/fake.h"
#include "../../src/linux_abi/pipe.h"

#include <string.h>

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[32] = {0};
    hl_linux_ofd_entry ofds[32] = {0};
    hl_linux_fd pipe[2];
    hl_linux_fd unchanged[2] = {17, 19};
    char bytes[16] = {0};
    int64_t copy;
    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, 32, ofds, 32) == HL_STATUS_OK);
    HL_CHECK(hl_linux_pipe_create(&linux_abi, UINT32_MAX, 0, unchanged) == -HL_LINUX_EINVAL && unchanged[0] == 17 &&
             unchanged[1] == 19);
    HL_CHECK(hl_linux_pipe_create(&linux_abi, HL_LINUX_O_NONBLOCK, HL_LINUX_FD_CLOEXEC, pipe) == 0);
    HL_CHECK(hl_linux_fcntl(&linux_abi, pipe[0], HL_LINUX_F_GETFD, 0) == HL_LINUX_FD_CLOEXEC);
    HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == -HL_LINUX_EAGAIN);
    HL_CHECK(hl_linux_write(&linux_abi, pipe[1], "typed", 5) == 5);
    {
        hl_linux_poll_entry poll = {pipe[0], HL_LINUX_READY_READ, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &poll, 1, 0) == 1 && poll.readiness == HL_LINUX_READY_READ);
    }
    HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 5 && memcmp(bytes, "typed", 5) == 0);
    copy = hl_linux_dup(&linux_abi, pipe[1]);
    HL_CHECK(copy >= 0);
    HL_CHECK(hl_linux_fcntl(&linux_abi, pipe[1], HL_LINUX_F_SETFL, 0) == 0);
    HL_CHECK((hl_linux_fcntl(&linux_abi, (hl_linux_fd)copy, HL_LINUX_F_GETFL, 0) & HL_LINUX_O_NONBLOCK) == 0);
    HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)copy, "dup", 3) == 3);
    HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 3 && memcmp(bytes, "dup", 3) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[0]) == 0);
    HL_CHECK(hl_linux_write(&linux_abi, pipe[1], "x", 1) == -HL_LINUX_EPIPE);
    HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)copy, "x", 1) == -HL_LINUX_EPIPE);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[1]) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)copy) == 0);
    HL_CHECK(hl_linux_pipe_create(&linux_abi, 0, 0, pipe) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[1]) == 0);
    HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[0]) == 0);
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    {
        hl_linux_abi small;
        hl_linux_fd_entry small_fds[2] = {0};
        hl_linux_ofd_entry small_ofds[3] = {0};
        hl_linux_fd first[2], untouched[2] = {23, 29};
        HL_CHECK(hl_linux_abi_init(&small, &services, small_fds, 2, small_ofds, 3) == HL_STATUS_OK);
        HL_CHECK(hl_linux_pipe_create(&small, 0, 0, first) == 0);
        HL_CHECK(hl_linux_pipe_create(&small, 0, 0, untouched) == -HL_LINUX_EMFILE && untouched[0] == 23 &&
                 untouched[1] == 29);
        HL_CHECK(hl_linux_close(&small, first[0]) == 0 && hl_linux_close(&small, first[1]) == 0);
        HL_CHECK(hl_linux_abi_validate_fds(&small) == HL_STATUS_OK);
        HL_CHECK(hl_linux_abi_destroy(&small) == HL_STATUS_OK);
    }
    return 0;
}
