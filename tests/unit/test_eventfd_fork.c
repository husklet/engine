#include "test.h"

#include "hl/linux.h"
#include "../../src/linux_abi/eventfd.h"

typedef struct child_context {
    hl_linux_abi *linux_abi;
    hl_linux_fd fd;
} child_context;

static int32_t child_read(void *opaque) {
    child_context *context = opaque;
    uint64_t value = 0;
    return hl_linux_read(context->linux_abi, context->fd, &value, sizeof(value)) == 8 && value == 17 ? 0 : 19;
}

int main(void) {
    hl_host_linux *host;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[32];
    hl_linux_ofd_entry ofds[32];
    child_context context;
    hl_host_handle process;
    hl_host_result waited;
    int64_t fd;
    HL_CHECK(hl_host_linux_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, 32, ofds, 32) == HL_STATUS_OK);
    fd = hl_linux_eventfd_create(&linux_abi, 17, HL_LINUX_EVENTFD_NONBLOCK, 0);
    HL_CHECK(fd >= 0);
    context = (child_context){&linux_abi, (hl_linux_fd)fd};
    HL_CHECK(hl_linux_abi_spawn(&linux_abi, child_read, &context, &process) == HL_STATUS_OK);
    waited = services.process->wait(services.context, process, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(waited.status == HL_STATUS_OK && waited.detail == HL_HOST_PROCESS_EXIT_CODE && waited.value == 0);
    HL_CHECK(services.process->close(services.context, process).status == HL_STATUS_OK);
    HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)fd) == 0);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    hl_host_linux_destroy(host);
    return 0;
}
