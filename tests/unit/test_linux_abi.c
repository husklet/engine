#include "test.h"

#include "hl/fake.h"
#include "hl/linux_abi.h"

#include <pthread.h>
#include <string.h>

typedef struct test_file_host {
    /* First so the shared host context is also a valid fake-host context. */
    hl_fake_host fake;
    char bytes[32];
    size_t size;
    _Atomic uint32_t reads;
    _Atomic uint32_t closes;
    _Atomic uint32_t bad_handles;
    hl_status next_status;
    uint64_t next_value;
    _Atomic uint32_t barrier_enabled;
    _Atomic uint32_t concurrent_calls;
    uint32_t writes;
    uint32_t appends;
    uint32_t opens;
    uint32_t last_access;
    uint32_t last_creation;
    uint32_t last_permissions;
    uint32_t metadata_type;
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
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result test_open(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                uint32_t access, uint32_t creation, uint32_t permissions) {
    test_file_host *host = context;
    if (directory != HL_HOST_HANDLE_CWD || path_size != 4 || memcmp(path, "file", 4) != 0)
        return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    host->opens++;
    host->last_access = access;
    host->last_creation = creation;
    host->last_permissions = permissions;
    return file_result(HL_STATUS_OK, 55);
}

static hl_host_result test_write_at(void *context, hl_host_handle file, uint64_t offset, hl_host_const_bytes input) {
    test_file_host *host = context;
    if ((file != 55 && file != 56) || offset > sizeof(host->bytes) || input.size > sizeof(host->bytes) - offset)
        return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    if (input.size != 0) memcpy(host->bytes + (size_t)offset, input.data, input.size);
    if (offset + input.size > host->size) host->size = (size_t)offset + input.size;
    host->writes++;
    return file_result(HL_STATUS_OK, input.size);
}

static hl_host_result test_append(void *context, hl_host_handle file, hl_host_const_bytes input) {
    test_file_host *host = context;
    hl_host_result result = test_write_at(context, file, host->size, input);
    if (result.status == HL_STATUS_OK) {
        host->appends++;
        result.detail = host->size;
    }
    return result;
}

static hl_host_result test_metadata(void *context, hl_host_handle file, hl_host_file_metadata *metadata) {
    test_file_host *host = context;
    if ((file != 55 && file != 56) || metadata == NULL) return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    memset(metadata, 0, sizeof(*metadata));
    metadata->stable_device = 7;
    metadata->stable_object = 11;
    metadata->size = host->size;
    metadata->allocated_size = 512;
    metadata->modified_ns = 123456789;
    metadata->type = host->metadata_type == 0 ? HL_HOST_FILE_TYPE_REGULAR : host->metadata_type;
    metadata->permissions = 0640;
    return file_result(HL_STATUS_OK, 0);
}

typedef struct read_thread_args {
    hl_linux_abi *linux_abi;
    hl_linux_fd fd;
    char bytes[3];
} read_thread_args;

static void *read_three_bytes(void *opaque) {
    read_thread_args *args = opaque;
    size_t i;
    for (i = 0; i < sizeof(args->bytes); ++i) {
        if (hl_linux_read(args->linux_abi, args->fd, &args->bytes[i], 1) != 1) return (void *)(uintptr_t)EXIT_FAILURE;
    }
    return (void *)(uintptr_t)EXIT_SUCCESS;
}

static void *read_one_byte(void *opaque) {
    read_thread_args *args = opaque;
    return (void *)(uintptr_t)(hl_linux_read(args->linux_abi, args->fd, &args->bytes[0], 1) == 1 ? EXIT_SUCCESS
                                                                                                 : EXIT_FAILURE);
}

int main(void) {
    hl_host_services services;
    hl_linux_abi linux_abi;
    hl_linux_fd_entry fds[8];
    hl_linux_ofd_entry ofds[8];
    hl_linux_fd original;
    hl_linux_fd duplicate;
    hl_linux_fd_snapshot snapshot;
    hl_host_handle closed;
    test_file_host file_host = {0};
    hl_host_file_services files = {.abi = HL_HOST_FILE_ABI,
                                   .size = sizeof(files),
                                   .open_relative = test_open,
                                   .read_at = test_read_at,
                                   .write_at = test_write_at,
                                   .append = test_append,
                                   .metadata = test_metadata,
                                   .close = test_close};
    char buffer[8] = {0};

    file_host.size = 6;
    hl_fake_host_init(&file_host.fake, &services);
    memcpy(file_host.bytes, "abcdef", 6);
    services.capabilities |= HL_HOST_CAP_FILE;
    services.context = &file_host;
    services.file = &files;
    {
        hl_host_services without_sync = services;
        without_sync.capabilities &= ~(uint64_t)HL_HOST_CAP_SYNC;
        HL_CHECK(hl_linux_abi_init(&linux_abi, &without_sync, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) ==
                 HL_STATUS_NOT_SUPPORTED);
        HL_CHECK(file_host.fake.live_mutexes == 0);
    }
    HL_CHECK(hl_linux_abi_init(&linux_abi, &services, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) ==
             HL_STATUS_OK);
    HL_CHECK(file_host.fake.live_mutexes == 0);
    hl_fake_host_fail_next(&file_host.fake, HL_STATUS_IO);
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0, 0, &original) == HL_STATUS_IO);
    HL_CHECK(file_host.fake.live_mutexes == 0);
    {
        hl_linux_fd capacity_fds[HL_ARRAY_COUNT(ofds) - 1];
        size_t i;
        for (i = 0; i < HL_ARRAY_COUNT(capacity_fds); ++i) {
            HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0, 0, &capacity_fds[i]) == HL_STATUS_OK);
            HL_CHECK(file_host.fake.live_mutexes == i + 1);
        }
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0, 0, &original) == HL_STATUS_RESOURCE_LIMIT);
        HL_CHECK(file_host.fake.live_mutexes == HL_ARRAY_COUNT(capacity_fds));
        for (i = 0; i < HL_ARRAY_COUNT(capacity_fds); ++i) {
            HL_CHECK(hl_linux_fd_close(&linux_abi, capacity_fds[i], &closed) == HL_STATUS_OK);
            HL_CHECK(closed == 55);
            HL_CHECK(file_host.fake.live_mutexes == HL_ARRAY_COUNT(capacity_fds) - i - 1);
        }
    }
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0123, 1, &original) == HL_STATUS_OK);
    HL_CHECK(file_host.fake.live_mutexes == 1);
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_BUSY);
    HL_CHECK(file_host.fake.live_mutexes == 1);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(file_host.fake.live_mutexes == 1);
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

    /* write shares its OFD offset across dup; pwrite is positional and leaves it unchanged. */
    file_host.size = 0;
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDWR, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(hl_linux_write(&linux_abi, original, "xy", 2) == 2);
    HL_CHECK(hl_linux_write(&linux_abi, duplicate, "z", 1) == 1);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK && snapshot.offset == 3);
    HL_CHECK(hl_linux_pwrite64(&linux_abi, duplicate, "Q", 1, 0) == 1);
    HL_CHECK(memcmp(file_host.bytes, "Qyz", 3) == 0);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, duplicate, &snapshot) == HL_STATUS_OK && snapshot.offset == 3);
    file_host.next_status = HL_STATUS_WOULD_BLOCK;
    HL_CHECK(hl_linux_write(&linux_abi, original, "!", 1) == -HL_LINUX_EAGAIN);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK && snapshot.offset == 3);
    HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == 0);

    /* lseek changes the shared OFD offset; SEEK_END uses portable host metadata. */
    file_host.size = 6;
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDWR, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_dup(&linux_abi, original) == 1);
    duplicate = 1;
    HL_CHECK(hl_linux_lseek(&linux_abi, original, 2, HL_LINUX_SEEK_SET) == 2);
    HL_CHECK(hl_linux_lseek(&linux_abi, duplicate, 1, HL_LINUX_SEEK_CUR) == 3);
    HL_CHECK(hl_linux_lseek(&linux_abi, original, -1, HL_LINUX_SEEK_END) == 5);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, duplicate, &snapshot) == HL_STATUS_OK && snapshot.offset == 5);
    HL_CHECK(hl_linux_lseek(&linux_abi, original, -7, HL_LINUX_SEEK_SET) == -HL_LINUX_EINVAL);
    HL_CHECK(hl_linux_lseek(&linux_abi, original, 0, 99) == -HL_LINUX_EINVAL);
    file_host.metadata_type = HL_HOST_FILE_TYPE_SOCKET;
    HL_CHECK(hl_linux_lseek(&linux_abi, original, 0, HL_LINUX_SEEK_SET) == -HL_LINUX_ESPIPE);
    file_host.metadata_type = HL_HOST_FILE_TYPE_REGULAR;

    /* fcntl keeps descriptor flags separate from status flags and honors the minimum fd. */
    HL_CHECK(hl_linux_fcntl(&linux_abi, original, HL_LINUX_F_GETFD, 0) == 0);
    HL_CHECK(hl_linux_fcntl(&linux_abi, original, HL_LINUX_F_SETFD, UINT64_MAX) == 0);
    HL_CHECK(hl_linux_fcntl(&linux_abi, original, HL_LINUX_F_GETFD, 0) == HL_LINUX_FD_CLOEXEC);
    HL_CHECK(hl_linux_fcntl(&linux_abi, duplicate, HL_LINUX_F_GETFL, 0) == HL_LINUX_O_RDWR);
    {
        int64_t high = hl_linux_fcntl(&linux_abi, original, HL_LINUX_F_DUPFD_CLOEXEC, 5);
        HL_CHECK(high == 5);
        HL_CHECK(hl_linux_fcntl(&linux_abi, (hl_linux_fd)high, HL_LINUX_F_GETFD, 0) == HL_LINUX_FD_CLOEXEC);
        HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)high) == 0);
    }
    HL_CHECK(hl_linux_fcntl(&linux_abi, original, 999, 0) == -HL_LINUX_EINVAL);
    HL_CHECK(hl_linux_fcntl(&linux_abi, original, HL_LINUX_F_DUPFD, HL_ARRAY_COUNT(fds)) == -HL_LINUX_EINVAL);

    /* dup2/dup3 replace target atomically and discard a displaced close error. */
    {
        hl_linux_fd target;
        uint32_t closes_before = file_host.closes;
        HL_CHECK(hl_linux_fd_install(&linux_abi, 56, HL_LINUX_O_RDONLY, HL_LINUX_FD_CLOEXEC, &target) == HL_STATUS_OK);
        HL_CHECK(target == 2);
        file_host.next_status = HL_STATUS_IO;
        HL_CHECK(hl_linux_dup3(&linux_abi, original, target, HL_LINUX_O_CLOEXEC) == target);
        HL_CHECK(file_host.closes == closes_before + 1);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, target, &snapshot) == HL_STATUS_OK);
        HL_CHECK(snapshot.host_handle == 55 && snapshot.ofd == fds[original].ofd);
        HL_CHECK(snapshot.descriptor_flags == HL_LINUX_FD_CLOEXEC);
        HL_CHECK(hl_linux_dup2(&linux_abi, original, original) == original);
        HL_CHECK(hl_linux_dup3(&linux_abi, original, original, 0) == -HL_LINUX_EINVAL);
        HL_CHECK(hl_linux_dup3(&linux_abi, original, target, HL_LINUX_O_APPEND) == -HL_LINUX_EINVAL);
        HL_CHECK(hl_linux_dup2(&linux_abi, original, HL_ARRAY_COUNT(fds)) == -HL_LINUX_EBADF);
        HL_CHECK(hl_linux_close(&linux_abi, target) == 0);

        /* Replacing another descriptor for the same OFD has a net-zero reference change. */
        closes_before = file_host.closes;
        HL_CHECK(hl_linux_dup2(&linux_abi, original, duplicate) == duplicate);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, duplicate, &snapshot) == HL_STATUS_OK);
        HL_CHECK(snapshot.descriptor_references == 2 && snapshot.descriptor_flags == 0);
        HL_CHECK(file_host.closes == closes_before);

        /* Source validation precedes target replacement, so errors leave target untouched. */
        HL_CHECK(hl_linux_fd_install(&linux_abi, 56, HL_LINUX_O_RDONLY, 0, &target) == HL_STATUS_OK);
        HL_CHECK(hl_linux_dup2(&linux_abi, 7, target) == -HL_LINUX_EBADF);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, target, &snapshot) == HL_STATUS_OK && snapshot.host_handle == 56);
        HL_CHECK(hl_linux_close(&linux_abi, target) == 0);
    }
    {
        hl_linux_file_status file_status;
        HL_CHECK(hl_linux_fstat(&linux_abi, original, &file_status) == 0);
        HL_CHECK(file_status.device == 7 && file_status.object == 11 && file_status.size == 6);
        HL_CHECK(file_status.blocks_512 == 1 && file_status.modified_ns == 123456789);
        HL_CHECK(file_status.mode == (HL_LINUX_S_IFREG | 0640u));
    }
    HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == 0);

    /* O_APPEND delegates one atomic append to the host and adopts its resulting offset. */
    file_host.size = 3;
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_WRONLY | HL_LINUX_O_APPEND, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(hl_linux_write(&linux_abi, original, "A", 1) == 1);
    HL_CHECK(hl_linux_write(&linux_abi, duplicate, "B", 1) == 1);
    HL_CHECK(file_host.appends == 2 && memcmp(file_host.bytes, "QyzAB", 5) == 0);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK && snapshot.offset == 5);
    HL_CHECK(hl_linux_pwrite64(&linux_abi, duplicate, "P", 1, 1) == 1);
    HL_CHECK(file_host.bytes[1] == 'P');
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK && snapshot.offset == 5);
    HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == 0);

    /* openat translates Linux flags/mode and owns the returned host handle on failure or close. */
    {
        int64_t opened =
            hl_linux_openat(&linux_abi, HL_LINUX_AT_FDCWD, "file", 4,
                            HL_LINUX_O_RDWR | HL_LINUX_O_CREAT | HL_LINUX_O_APPEND | HL_LINUX_O_CLOEXEC, 0640);
        HL_CHECK(opened >= 0);
        HL_CHECK(file_host.opens == 1);
        HL_CHECK(file_host.last_access == (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE | HL_HOST_FILE_APPEND));
        HL_CHECK(file_host.last_creation == HL_HOST_FILE_CREATE && file_host.last_permissions == 0640);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, (hl_linux_fd)opened, &snapshot) == HL_STATUS_OK);
        HL_CHECK(snapshot.descriptor_flags == HL_LINUX_FD_CLOEXEC);
        HL_CHECK((snapshot.status_flags & HL_LINUX_O_APPEND) != 0);
        HL_CHECK(hl_linux_close(&linux_abi, (hl_linux_fd)opened) == 0);
    }
    file_host.next_status = HL_STATUS_NOT_FOUND;
    HL_CHECK(hl_linux_openat(&linux_abi, HL_LINUX_AT_FDCWD, "file", 4, HL_LINUX_O_RDONLY, 0) == -HL_LINUX_ENOENT);

    {
        read_thread_args first = {&linux_abi, 0, {0}};
        read_thread_args second = {&linux_abi, 0, {0}};
        uint32_t seen[6] = {0};
        pthread_t first_thread;
        pthread_t second_thread;
        void *first_result;
        void *second_result;
        size_t i;

        /* Concurrent reads through duplicated fds consume one shared OFD offset atomically. */
        memcpy(file_host.bytes, "abcdef", 6);
        file_host.size = 6;
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_HOST_FILE_READ, 0, &first.fd) == HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_dup(&linux_abi, first.fd, 0, &second.fd) == HL_STATUS_OK);
        HL_CHECK(pthread_create(&first_thread, NULL, read_three_bytes, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, read_three_bytes, &second) == 0);
        HL_CHECK(pthread_join(first_thread, &first_result) == 0);
        HL_CHECK(pthread_join(second_thread, &second_result) == 0);
        HL_CHECK((uintptr_t)first_result == EXIT_SUCCESS && (uintptr_t)second_result == EXIT_SUCCESS);
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
        HL_CHECK(pthread_create(&first_thread, NULL, read_one_byte, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, read_one_byte, &second) == 0);
        HL_CHECK(pthread_join(first_thread, &first_result) == 0);
        HL_CHECK(pthread_join(second_thread, &second_result) == 0);
        atomic_store_explicit(&file_host.barrier_enabled, 0, memory_order_release);
        HL_CHECK((uintptr_t)first_result == EXIT_SUCCESS && (uintptr_t)second_result == EXIT_SUCCESS);
        HL_CHECK(atomic_load_explicit(&file_host.concurrent_calls, memory_order_acquire) == 2);
        HL_CHECK(hl_linux_close(&linux_abi, first.fd) == 0);
        HL_CHECK(hl_linux_close(&linux_abi, second.fd) == 0);
    }
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(file_host.fake.live_mutexes == 0);
    return EXIT_SUCCESS;
}
