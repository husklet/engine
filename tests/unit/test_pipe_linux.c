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
    hl_linux_fd_entry fds[32] = {0};
    hl_linux_ofd_entry ofds[32] = {0};
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
        HL_CHECK(hl_linux_epoll_wait(&linux_abi, (hl_linux_fd)epoll, &event, 1, HL_HOST_DEADLINE_INFINITE) == 1);
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

    /* POLLERR/POLLHUP are reported whether or not they were requested. A caller
     * that polls only for READ on the read end of a pipe with no writers left
     * must still be told HANGUP, and one that polls only for WRITE on a write
     * end with no readers must still be told HANGUP -- otherwise the second case
     * reports nothing ready at all and the caller spins on a descriptor that
     * will never become writable. Both directions are checked because the two
     * ends take different branches in the host's readiness probe. */
    {
        hl_linux_poll_entry entry;
        hl_linux_file_status file_status;
        HL_CHECK(hl_linux_pipe_create(&linux_abi, HL_LINUX_O_NONBLOCK, 0, pipe) == 0);
        /* fstat reports a FIFO with one link, not a typeless zero-link inode. */
        HL_CHECK(hl_linux_fstat(&linux_abi, pipe[0], &file_status) == 0);
        HL_CHECK(file_status.mode == (HL_LINUX_S_IFIFO | 0600u) && file_status.link_count == 1 &&
                 file_status.size == 0);
        /* Both ends open: read end is not ready, write end is writable, and
         * neither reports hangup. */
        entry = (hl_linux_poll_entry){pipe[0], HL_LINUX_READY_READ, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &entry, 1, 0) == 0 && entry.readiness == 0);
        entry = (hl_linux_poll_entry){pipe[1], HL_LINUX_READY_WRITE, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &entry, 1, 0) == 1 && entry.readiness == HL_LINUX_READY_WRITE);
        /* Drop the writers: the read end asked only for READ still learns HANGUP. */
        HL_CHECK(hl_linux_close(&linux_abi, pipe[1]) == 0);
        entry = (hl_linux_poll_entry){pipe[0], HL_LINUX_READY_READ, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &entry, 1, 0) == 1 && (entry.readiness & HL_LINUX_READY_HANGUP) != 0);
        HL_CHECK(hl_linux_close(&linux_abi, pipe[0]) == 0);
        /* Drop the readers: the write end asked only for WRITE still learns
         * ERROR. The kernel's pipe_poll is asymmetric here -- a read end with no
         * writers is EPOLLHUP, a write end with no readers is EPOLLERR -- so
         * this is the other output-only condition, not the same one twice. */
        HL_CHECK(hl_linux_pipe_create(&linux_abi, HL_LINUX_O_NONBLOCK, 0, pipe) == 0);
        HL_CHECK(hl_linux_close(&linux_abi, pipe[0]) == 0);
        entry = (hl_linux_poll_entry){pipe[1], HL_LINUX_READY_WRITE, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, &entry, 1, 0) == 1 && (entry.readiness & HL_LINUX_READY_ERROR) != 0);
        HL_CHECK(hl_linux_close(&linux_abi, pipe[1]) == 0);
    }
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    hl_host_linux_destroy(host);
    return 0;
}
