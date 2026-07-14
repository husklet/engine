#include "test.h"

#include "hl/linux.h"
#include "../../src/linux_abi/epoll.h"
#include "../../src/linux_abi/pipe.h"

#include <pthread.h>
#include <string.h>

typedef struct read_context {
    hl_linux_abi *linux_abi;
    hl_linux_fd fd;
    char byte;
    int64_t result;
} read_context;

typedef struct child_context {
    hl_linux_abi *linux_abi;
    hl_linux_fd fd;
} child_context;

static int32_t child_write(void *opaque) {
    child_context *child = opaque;
    return hl_linux_write(child->linux_abi, child->fd, "f", 1) == 1 ? 0 : 21;
}

static void *blocking_read(void *opaque) {
    read_context *reader = opaque;
    reader->result = hl_linux_read(reader->linux_abi, reader->fd, &reader->byte, 1);
    return NULL;
}

int main(void) {
    hl_host_linux *host;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[32];
    hl_linux_ofd_entry ofds[32];
    hl_linux_fd pipe[2];
    char bytes[8] = {0};
    int64_t writer;
    HL_CHECK(hl_host_linux_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, 32, ofds, 32) == HL_STATUS_OK);
    HL_CHECK(hl_linux_pipe_create(&linux_abi, 0, 0, pipe) == 0);
    writer = hl_linux_dup(&linux_abi, pipe[1]);
    HL_CHECK(writer >= 0 && hl_linux_close(&linux_abi, pipe[1]) == 0);
    {
        int64_t epoll = hl_linux_epoll_create(&linux_abi, HL_LINUX_FD_CLOEXEC);
        hl_linux_epoll_event event = {0};
        HL_CHECK(epoll >= 0);
        HL_CHECK(hl_linux_epoll_control(&linux_abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, pipe[0],
                                        HL_LINUX_READY_READ, UINT64_C(0x70697065)) == 0);
        HL_CHECK(hl_linux_epoll_wait(&linux_abi, (hl_linux_fd)epoll, &event, 1, 0) == 0);
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)writer, "e", 1) == 1);
        HL_CHECK(hl_linux_epoll_wait(&linux_abi, (hl_linux_fd)epoll, &event, 1,
                                     HL_HOST_DEADLINE_INFINITE) == 1);
        HL_CHECK(event.data == UINT64_C(0x70697065) && (event.readiness & HL_LINUX_READY_READ) != 0);
        HL_CHECK(hl_linux_epoll_control(&linux_abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_DELETE, pipe[0], 0, 0) == 0);
        HL_CHECK(hl_linux_epoll_wait(&linux_abi, (hl_linux_fd)epoll, &event, 1, 0) == 0);
        HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)epoll) == 0);
        HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 1 && bytes[0] == 'e');
    }
    HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)writer, "alive", 5) == 5);
    {
        hl_linux_poll_entry poll = {pipe[0], HL_LINUX_READY_READ, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &poll, 1, HL_HOST_DEADLINE_INFINITE) == 1 &&
                 (poll.readiness & HL_LINUX_READY_READ) != 0);
    }
    HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 5 && memcmp(bytes, "alive", 5) == 0);
    {
        child_context child = {&linux_abi, (hl_linux_fd)writer};
        hl_host_handle process;
        hl_host_result waited;
        HL_CHECK(hl_linux_abi_spawn(&linux_abi, child_write, &child, &process) == HL_STATUS_OK);
        waited = services.process->wait(services.context, process, HL_HOST_DEADLINE_INFINITE);
        HL_CHECK(waited.status == HL_STATUS_OK && waited.value == 0);
        HL_CHECK(services.process->close(services.context, process).status == HL_STATUS_OK);
        HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 1 && bytes[0] == 'f');
    }
    {
        read_context reader = {&linux_abi, pipe[0], 0, 0};
        pthread_t thread;
        HL_CHECK(pthread_create(&thread, NULL, blocking_read, &reader) == 0);
        HL_CHECK(hl_linux_write(&linux_abi, (hl_linux_fd)writer, "w", 1) == 1);
        HL_CHECK(pthread_join(thread, NULL) == 0 && reader.result == 1 && reader.byte == 'w');
    }
    HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)writer) == 0);
    HL_CHECK(hl_linux_read(&linux_abi, pipe[0], bytes, sizeof bytes) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[0]) == 0);

    HL_CHECK(hl_linux_pipe_create(&linux_abi, 0, 0, pipe) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[0]) == 0);
    HL_CHECK(hl_linux_write(&linux_abi, pipe[1], "x", 1) == -HL_LINUX_EPIPE);
    HL_CHECK(hl_linux_close(&linux_abi, pipe[1]) == 0);
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    hl_host_linux_destroy(host);
    return 0;
}
