#include "test.h"

#include "hl/fake_host.h"
#include "hl/linux_abi.h"

#include <string.h>
#if !defined(__STDC_NO_THREADS__)
#include <threads.h>
#endif

typedef struct test_file_host {
    char bytes[32];
    size_t size;
    _Atomic uint32_t reads;
    _Atomic uint32_t closes;
    _Atomic uint32_t bad_handles;
    hl_status next_status;
    uint64_t next_value;
    _Atomic uint32_t barrier_enabled;
    _Atomic uint32_t concurrent_calls;
} test_file_host;

static hl_host_result file_result(hl_status status, uint64_t value) {
    hl_host_result result = {(int32_t)status, 0, value, 0};
    return result;
}

static hl_host_result test_read_at(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output) {
    test_file_host *host = context;
    size_t count;
    if (file != 55 && file != 56) {
        host->bad_handles++;
        return file_result(HL_STATUS_PLATFORM_FAILURE, 0);
    }
    host->reads++;
    if (atomic_load_explicit(&host->barrier_enabled, memory_order_acquire) != 0) {
        atomic_fetch_add_explicit(&host->concurrent_calls, 1, memory_order_release);
        while (atomic_load_explicit(&host->concurrent_calls, memory_order_acquire) < 2) {}
    }
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    if (host->next_value != 0) {
        uint64_t value = host->next_value;
        host->next_value = 0;
        return file_result(HL_STATUS_OK, value);
    }
    if (offset >= host->size) return file_result(HL_STATUS_OK, 0);
    count = host->size - (size_t)offset;
    if (count > output.size) count = output.size;
    if (count != 0) memcpy(output.data, host->bytes + offset, count);
    return file_result(HL_STATUS_OK, count);
}

static hl_host_result test_close(void *context, hl_host_handle file) {
    test_file_host *host = context;
    if (file != 55 && file != 56) {
        host->bad_handles++;
        return file_result(HL_STATUS_PLATFORM_FAILURE, 0);
    }
    host->closes++;
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result unsupported_open(void *c, hl_host_handle d, const char *p, size_t n, uint32_t a, uint32_t x) {
    (void)c;
    (void)d;
    (void)p;
    (void)n;
    (void)a;
    (void)x;
    return file_result(HL_STATUS_NOT_SUPPORTED, 0);
}

static hl_host_result unsupported_write(void *c, hl_host_handle f, uint64_t o, hl_host_const_bytes b) {
    (void)c;
    (void)f;
    (void)o;
    (void)b;
    return file_result(HL_STATUS_NOT_SUPPORTED, 0);
}

static hl_host_result unsupported_metadata(void *c, hl_host_handle f, hl_host_file_metadata *m) {
    (void)c;
    (void)f;
    (void)m;
    return file_result(HL_STATUS_NOT_SUPPORTED, 0);
}

#if !defined(__STDC_NO_THREADS__)
typedef struct read_thread_args {
    hl_linux_abi *linux_abi;
    hl_linux_fd fd;
    char bytes[3];
} read_thread_args;

static int read_three_bytes(void *opaque) {
    read_thread_args *args = opaque;
    size_t i;
    for (i = 0; i < sizeof(args->bytes); ++i) {
        if (hl_linux_read(args->linux_abi, args->fd, &args->bytes[i], 1) != 1) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int read_one_byte(void *opaque) {
    read_thread_args *args = opaque;
    return hl_linux_read(args->linux_abi, args->fd, &args->bytes[0], 1) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[8];
    hl_linux_ofd_entry ofds[8];
    hl_linux_fd original;
    hl_linux_fd duplicate;
    hl_linux_fd_snapshot snapshot;
    hl_host_handle closed;
    test_file_host file_host = {{0}, 6, 0, 0, 0, HL_STATUS_OK, 0, 0, 0};
    hl_host_file_services files = {HL_HOST_FILE_ABI,  sizeof(files),        unsupported_open, test_read_at,
                                   unsupported_write, unsupported_metadata, test_close};
    char buffer[8] = {0};

    hl_fake_host_init(&fake, &services);
    memcpy(file_host.bytes, "abcdef", 6);
    services.capabilities |= HL_HOST_CAP_FILE;
    services.context = &file_host;
    services.file = &files;
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) ==
             HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0123, 1, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(original != duplicate);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, duplicate, &snapshot) == HL_STATUS_OK);
    HL_CHECK(snapshot.fd == duplicate && snapshot.ofd == fds[original].ofd);
    HL_CHECK(snapshot.descriptor_references == 2 && snapshot.status_flags == 0123);
    HL_CHECK(snapshot.host_handle == 55 && snapshot.descriptor_flags == 0);

    /* read advances the OFD shared by both descriptors. */
    HL_CHECK(hl_linux_read(&linux_abi, original, buffer, 2) == 2);
    HL_CHECK(memcmp(buffer, "ab", 2) == 0);
    memset(buffer, 0, sizeof(buffer));
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, buffer, 2) == 2);
    HL_CHECK(memcmp(buffer, "cd", 2) == 0);
    HL_CHECK(ofds[fds[original].ofd].offset == 4);

    /* pread reads at an explicit offset and never changes the shared OFD offset. */
    memset(buffer, 0, sizeof(buffer));
    HL_CHECK(hl_linux_pread64(&linux_abi, duplicate, buffer, 2, 1) == 2);
    HL_CHECK(memcmp(buffer, "bc", 2) == 0);
    HL_CHECK(ofds[fds[original].ofd].offset == 4);

    /* A failed host read becomes Linux errno and does not advance the offset. */
    file_host.next_status = HL_STATUS_WOULD_BLOCK;
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, buffer, 1) == -HL_LINUX_EAGAIN);
    HL_CHECK(ofds[fds[original].ofd].offset == 4);
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, NULL, 1) == -HL_LINUX_EINVAL);
    file_host.next_value = 9;
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, buffer, 1) == -HL_LINUX_EIO);
    HL_CHECK(ofds[fds[original].ofd].offset == 4);

    closed = HL_HOST_HANDLE_INVALID;
    HL_CHECK(hl_linux_fd_close(&linux_abi, original, &closed) == HL_STATUS_OK);
    HL_CHECK(closed == HL_HOST_HANDLE_INVALID);
    HL_CHECK(hl_linux_fd_close(&linux_abi, duplicate, &closed) == HL_STATUS_OK);
    HL_CHECK(closed == 55);
    HL_CHECK(hl_linux_fd_close(&linux_abi, duplicate, &closed) == HL_STATUS_NOT_FOUND);

    /* Syscall close closes the host handle exactly once, at the final OFD reference. */
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_HOST_FILE_READ, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(hl_linux_close(&linux_abi, original) == 0 && file_host.closes == 0);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == 0 && file_host.closes == 1);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == -HL_LINUX_EBADF);
    HL_CHECK(file_host.bad_handles == 0);

#if !defined(__STDC_NO_THREADS__)
    {
        read_thread_args first = {&linux_abi, 0, {0}};
        read_thread_args second = {&linux_abi, 0, {0}};
        uint32_t seen[6] = {0};
        thrd_t first_thread;
        thrd_t second_thread;
        int first_result;
        int second_result;
        size_t i;

        /* Concurrent reads through duplicated fds consume one shared OFD offset atomically. */
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_HOST_FILE_READ, 0, &first.fd) == HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_dup(&linux_abi, first.fd, 0, &second.fd) == HL_STATUS_OK);
        HL_CHECK(thrd_create(&first_thread, read_three_bytes, &first) == thrd_success);
        HL_CHECK(thrd_create(&second_thread, read_three_bytes, &second) == thrd_success);
        HL_CHECK(thrd_join(first_thread, &first_result) == thrd_success);
        HL_CHECK(thrd_join(second_thread, &second_result) == thrd_success);
        HL_CHECK(first_result == EXIT_SUCCESS && second_result == EXIT_SUCCESS);
        for (i = 0; i < sizeof(first.bytes); ++i) {
            HL_CHECK(first.bytes[i] >= 'a' && first.bytes[i] <= 'f');
            HL_CHECK(second.bytes[i] >= 'a' && second.bytes[i] <= 'f');
            seen[(size_t)(first.bytes[i] - 'a')]++;
            seen[(size_t)(second.bytes[i] - 'a')]++;
        }
        for (i = 0; i < HL_ARRAY_COUNT(seen); ++i)
            HL_CHECK(seen[i] == 1);
        HL_CHECK(hl_linux_close(&linux_abi, first.fd) == 0);
        HL_CHECK(hl_linux_close(&linux_abi, second.fd) == 0);

        /* Host calls on unrelated OFDs overlap; table ownership is not held across I/O. */
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_HOST_FILE_READ, 0, &first.fd) == HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_install(&linux_abi, 56, HL_HOST_FILE_READ, 0, &second.fd) == HL_STATUS_OK);
        atomic_store_explicit(&file_host.concurrent_calls, 0, memory_order_relaxed);
        atomic_store_explicit(&file_host.barrier_enabled, 1, memory_order_release);
        HL_CHECK(thrd_create(&first_thread, read_one_byte, &first) == thrd_success);
        HL_CHECK(thrd_create(&second_thread, read_one_byte, &second) == thrd_success);
        HL_CHECK(thrd_join(first_thread, &first_result) == thrd_success);
        HL_CHECK(thrd_join(second_thread, &second_result) == thrd_success);
        atomic_store_explicit(&file_host.barrier_enabled, 0, memory_order_release);
        HL_CHECK(first_result == EXIT_SUCCESS && second_result == EXIT_SUCCESS);
        HL_CHECK(atomic_load_explicit(&file_host.concurrent_calls, memory_order_acquire) == 2);
        HL_CHECK(hl_linux_close(&linux_abi, first.fd) == 0);
        HL_CHECK(hl_linux_close(&linux_abi, second.fd) == 0);
    }
#endif
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    return EXIT_SUCCESS;
}
