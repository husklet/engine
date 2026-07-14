#include "test.h"

#include "hl/fake.h"
#include "../../src/linux_abi/eventfd.h"

#include <string.h>

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[32];
    hl_linux_ofd_entry ofds[32];
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
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    return 0;
}
