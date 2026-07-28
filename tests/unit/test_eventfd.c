#include "test.h"

#include "hl/fake.h"
#include "../../src/linux_abi/eventfd.h"

#include <string.h>

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[32] = {0};
    hl_linux_ofd_entry ofds[32] = {0};
    uint64_t value;
    int64_t fd;
    int64_t copy;
    hl_host_result endpoints;
    hl_linux_fd imported;
    char payload[8] = {0};
    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, 32, ofds, 32) == HL_STATUS_OK);

    fd = hl_linux_eventfd_create(&linux_abi, 2, HL_LINUX_EVENTFD_SEMAPHORE | HL_LINUX_EVENTFD_NONBLOCK,
                                 HL_LINUX_FD_CLOEXEC);
    HL_CHECK(fd >= 0);
    copy = hl_linux_dup(&linux_abi, (hl_linux_fd)fd);
    HL_CHECK(copy >= 0);
    HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)fd, &value, sizeof(value)) == 8 && value == 1);
    HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)copy, &value, sizeof(value)) == 8 && value == 1);
    HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)fd, &value, sizeof(value)) == -HL_LINUX_EAGAIN);
    HL_CHECK(hl_linux_fcntl(&linux_abi, (hl_linux_fd)fd, HL_LINUX_F_SETFL, 0) == 0);
    HL_CHECK((hl_linux_fcntl(&linux_abi, (hl_linux_fd)copy, HL_LINUX_F_GETFL, 0) & HL_LINUX_O_NONBLOCK) == 0);
    HL_CHECK(hl_linux_fcntl(&linux_abi, (hl_linux_fd)copy, HL_LINUX_F_SETFL, HL_LINUX_O_NONBLOCK) == 0);
    value = 9;
    HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)copy, &value, sizeof(value)) == 8);
    {
        hl_linux_poll_entry poll = {(hl_linux_fd)fd, HL_LINUX_READY_READ, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &poll, 1, 0) == 1 && poll.readiness == HL_LINUX_READY_READ);
    }

    endpoints = services.transfer->channel_pair(services.context);
    HL_CHECK(endpoints.status == HL_STATUS_OK);
    HL_CHECK(hl_linux_eventfd_send(&linux_abi, endpoints.value, (hl_linux_fd)fd, (hl_host_const_bytes){"object", 6},
                                   HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WRITE | HL_HOST_TRANSFER_WAIT |
                                       HL_HOST_TRANSFER_CONTROL) == 6);
    HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)fd) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)copy) == 0);
    HL_CHECK(hl_linux_eventfd_receive(&linux_abi, endpoints.detail, (hl_host_bytes){payload, sizeof(payload)}, 0,
                                      &imported) == 6 &&
             memcmp(payload, "object", 6) == 0);
    HL_CHECK(hl_linux_read(&linux_abi, imported, &value, sizeof(value)) == 8 && value == 1);
    HL_CHECK(hl_linux_close(&linux_abi, imported) == 0);
    HL_CHECK(services.transfer->close(services.context, endpoints.value).status == HL_STATUS_OK);
    HL_CHECK(services.transfer->close(services.context, endpoints.detail).status == HL_STATUS_OK);

    fd = hl_linux_eventfd_create(&linux_abi, UINT64_MAX - 1u, HL_LINUX_EVENTFD_NONBLOCK, 0);
    HL_CHECK(fd >= 0);
    value = 1;
    HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, sizeof(value)) == -HL_LINUX_EAGAIN);
    HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)fd) == 0);

    /* The kernel's read/write size asymmetry (fs/eventfd.c). read takes any
     * buffer of eight bytes or more and transfers exactly one counter into it;
     * write takes eight and nothing else. An exact-8 rule on the read side is
     * the gap this covers: it rejects the ordinary "read into a field of a
     * wider struct" idiom that every eventfd user is entitled to write. */
    {
        unsigned char wide[16];
        uint32_t half = 0;
        fd = hl_linux_eventfd_create(&linux_abi, 0, HL_LINUX_EVENTFD_NONBLOCK, 0);
        HL_CHECK(fd >= 0);
        value = 5;
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, 8) == 8);
        /* write rejects every size but 8, and leaves the counter alone. */
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, 16) == -HL_LINUX_EINVAL);
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, 9) == -HL_LINUX_EINVAL);
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, 4) == -HL_LINUX_EINVAL);
        /* read below 8 is EINVAL and consumes nothing. */
        HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)fd, &half, 4) == -HL_LINUX_EINVAL);
        /* an oversized buffer transfers ONE counter, returns 8, and leaves the
         * bytes past it untouched. */
        memset(wide, 0xAA, sizeof wide);
        HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)fd, wide, sizeof wide) == 8);
        memcpy(&value, wide, sizeof(value));
        HL_CHECK(value == 5 && wide[8] == 0xAA && wide[15] == 0xAA);
        /* and the counter really was drained by that one read. */
        HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)fd, wide, sizeof wide) == -HL_LINUX_EAGAIN);
        /* a write of zero is admitted (returns 8) but is a pure no-op. */
        value = 0;
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, 8) == 8);
        HL_CHECK(hl_linux_read(&linux_abi, (hl_linux_fd)fd, wide, 8) == -HL_LINUX_EAGAIN);
        /* UINT64_MAX is the reserved value and never reaches the counter. */
        value = UINT64_MAX;
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)fd, &value, 8) == -HL_LINUX_EINVAL);
        /* fstat reports the anonymous inode the kernel builds: a zero-length
         * regular file, not a FIFO and not a typeless mode. */
        {
            hl_linux_file_status status;
            HL_CHECK(hl_linux_fstat(&linux_abi, (hl_linux_fd)fd, &status) == 0);
            HL_CHECK(status.mode == (HL_LINUX_S_IFREG | 0600u) && status.size == 0 && status.link_count == 1);
        }
        HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)fd) == 0);
    }
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    return 0;
}
