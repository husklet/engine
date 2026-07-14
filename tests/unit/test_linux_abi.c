#include "test.h"

#include "hl/fake.h"
#include "hl/linux_abi.h"
#include "../../src/linux_abi/object.h"

#include <pthread.h>
#include <string.h>

typedef struct test_file_host {
    /* First so the shared host context is also a valid fake-host context. */
    hl_fake_host fake;
    char bytes[32];
    size_t size;
    uint64_t offset;
    _Atomic uint32_t reads;
    _Atomic uint32_t closes;
    _Atomic uint32_t bad_handles;
    hl_status next_status;
    uint64_t next_value;
    _Atomic uint32_t barrier_enabled;
    _Atomic uint32_t concurrent_calls;
    uint32_t writes;
    uint32_t appends;
    uint32_t truncates;
    uint32_t syncs;
    uint32_t data_syncs;
    uint32_t opens;
    uint32_t last_access;
    uint32_t last_creation;
    uint32_t last_permissions;
    uint32_t metadata_type;
} test_file_host;

typedef struct test_object {
    char byte;
    uint32_t reads;
    uint32_t writes;
    uint32_t closes;
    uint32_t clones;
    hl_status clone_status;
    hl_status close_status;
} test_object;

static int64_t object_read(void *opaque, void *buffer, size_t size) {
    test_object *object = opaque;
    if (size == 0) return 0;
    *(char *)buffer = object->byte;
    object->reads++;
    return 1;
}

static int64_t object_write(void *opaque, const void *buffer, size_t size) {
    test_object *object = opaque;
    if (size == 0) return 0;
    object->byte = *(const char *)buffer;
    object->writes++;
    return 1;
}

static uint32_t object_ready(void *opaque, uint32_t interests) {
    test_object *object = opaque;
    return object->byte != 0 ? interests & HL_LINUX_READY_READ : 0;
}

static hl_status object_clone(void *opaque, void **child) {
    test_object *object = opaque;
    object->clones++;
    if (object->clone_status != HL_STATUS_OK) return object->clone_status;
    *child = object;
    return HL_STATUS_OK;
}

static hl_status object_close(void *opaque) {
    test_object *object = opaque;
    object->closes++;
    return object->close_status;
}

static const hl_linux_object_ops object_ops = {.read = object_read,
                                               .write = object_write,
                                               .readiness = object_ready,
                                               .clone = object_clone,
                                               .close = object_close};

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
        host->offset = host->size;
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

static hl_host_result test_read(void *context, hl_host_handle file, void *output, uint64_t output_size) {
    test_file_host *host = context;
    hl_host_result result = test_read_at(context, file, host->offset, (hl_host_bytes){output, (size_t)output_size});
    if (result.status == HL_STATUS_OK && result.value <= output_size) host->offset += result.value;
    return result;
}

static hl_host_result test_write(void *context, hl_host_handle file, const void *input, uint64_t input_size) {
    test_file_host *host = context;
    hl_host_result result =
        test_write_at(context, file, host->offset, (hl_host_const_bytes){input, (size_t)input_size});
    if (result.status == HL_STATUS_OK && result.value <= input_size) host->offset += result.value;
    return result;
}

static hl_host_result test_vector(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count,
                                  uint64_t offset, int write_operation, int advance) {
    test_file_host *host = context;
    uint64_t total = 0;
    uint32_t index;
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    if (host->next_value != 0) {
        uint64_t value = host->next_value;
        host->next_value = 0;
        if (advance) host->offset += value;
        return file_result(HL_STATUS_OK, value);
    }
    for (index = 0; index < count; ++index) {
        hl_host_result result;
        if (write_operation)
            result = test_write_at(
                context, file, offset + total,
                (hl_host_const_bytes){(const void *)(uintptr_t)vectors[index].address, (size_t)vectors[index].size});
        else
            result =
                test_read_at(context, file, offset + total,
                             (hl_host_bytes){(void *)(uintptr_t)vectors[index].address, (size_t)vectors[index].size});
        if (result.status != HL_STATUS_OK) return result;
        total += result.value;
        if (result.value != vectors[index].size) break;
    }
    if (advance) host->offset = offset + total;
    return file_result(HL_STATUS_OK, total);
}

static hl_host_result test_readv(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count) {
    test_file_host *host = context;
    return test_vector(context, file, vectors, count, host->offset, 0, 1);
}

static hl_host_result test_writev(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count) {
    test_file_host *host = context;
    return test_vector(context, file, vectors, count, host->offset, 1, 1);
}

static hl_host_result test_readv_at(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count,
                                    uint64_t offset) {
    return test_vector(context, file, vectors, count, offset, 0, 0);
}

static hl_host_result test_writev_at(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count,
                                     uint64_t offset) {
    return test_vector(context, file, vectors, count, offset, 1, 0);
}

static hl_host_result test_appendv(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count) {
    test_file_host *host = context;
    host->appends++;
    return test_vector(context, file, vectors, count, host->size, 1, 0);
}

static hl_host_result test_truncate(void *context, hl_host_handle file, uint64_t size) {
    test_file_host *host = context;
    if ((file != 55 && file != 56) || size > sizeof(host->bytes)) return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    if (size > host->size) memset(host->bytes + host->size, 0, (size_t)size - host->size);
    host->size = (size_t)size;
    host->truncates++;
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result test_sync(void *context, hl_host_handle file) {
    test_file_host *host = context;
    if (file != 55 && file != 56) return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    host->syncs++;
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result test_data_sync(void *context, hl_host_handle file) {
    test_file_host *host = context;
    if (file != 55 && file != 56) return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    host->data_syncs++;
    if (host->next_status != HL_STATUS_OK) {
        hl_status status = host->next_status;
        host->next_status = HL_STATUS_OK;
        return file_result(status, 0);
    }
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result test_rename(void *context, hl_host_handle old_directory, const char *old_path,
                                  size_t old_path_size, hl_host_handle new_directory, const char *new_path,
                                  size_t new_path_size) {
    (void)context;
    (void)old_directory;
    (void)old_path;
    (void)old_path_size;
    (void)new_directory;
    (void)new_path;
    (void)new_path_size;
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result test_unlink(void *context, hl_host_handle directory, const char *path, size_t path_size) {
    (void)context;
    (void)directory;
    (void)path;
    (void)path_size;
    return file_result(HL_STATUS_OK, 0);
}

static hl_host_result test_clone(void *context, hl_host_handle file) {
    (void)context;
    return file == 55 ? file_result(HL_STATUS_OK, 56) : file_result(HL_STATUS_INVALID_ARGUMENT, 0);
}

static hl_host_result test_seek(void *context, hl_host_handle file, int64_t offset, uint32_t whence) {
    test_file_host *host = context;
    int64_t base;
    if (file != 55 && file != 56) return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (whence == HL_LINUX_SEEK_SET)
        base = 0;
    else if (whence == HL_LINUX_SEEK_CUR)
        base = (int64_t)host->offset;
    else if (whence == HL_LINUX_SEEK_END)
        base = (int64_t)host->size;
    else
        return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (offset < -base) return file_result(HL_STATUS_INVALID_ARGUMENT, 0);
    host->offset = (uint64_t)(base + offset);
    return file_result(HL_STATUS_OK, host->offset);
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
                                   .close = test_close,
                                   .read = test_read,
                                   .write = test_write,
                                   .clone_for_fork = test_clone,
                                   .seek = test_seek,
                                   .readv = test_readv,
                                   .writev = test_writev,
                                   .readv_at = test_readv_at,
                                   .writev_at = test_writev_at,
                                   .appendv = test_appendv,
                                   .truncate = test_truncate,
                                   .sync = test_sync,
                                   .data_sync = test_data_sync,
                                   .rename_relative = test_rename,
                                   .unlink_relative = test_unlink};
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
    {
        hl_linux_fd_reservation reservation;
        hl_linux_fork_plan plan = {.abi = HL_LINUX_ABI_VERSION, .size = sizeof(plan)};
        HL_CHECK(hl_linux_fd_reserve_at(&linux_abi, 6, &reservation) == HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, 6, &snapshot) == HL_STATUS_NOT_FOUND);
        HL_CHECK(hl_linux_fd_install_at(&linux_abi, 6, 55, 0, 0) == HL_STATUS_ALREADY_EXISTS);
        HL_CHECK(hl_linux_abi_fork_prepare(&linux_abi, &plan) == HL_STATUS_BUSY);
        HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_BUSY);
        file_host.next_status = HL_STATUS_NOT_FOUND;
        HL_CHECK(hl_linux_openat_reserved(&linux_abi, &reservation, HL_LINUX_AT_FDCWD, "file", 4, HL_LINUX_O_RDONLY,
                                          0) == -HL_LINUX_ENOENT);
        HL_CHECK(hl_linux_fd_cancel(&linux_abi, &reservation) == HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_cancel(&linux_abi, &reservation) == HL_STATUS_NOT_FOUND);
        HL_CHECK(hl_linux_fd_reserve_at(&linux_abi, 6, &reservation) == HL_STATUS_OK);
        HL_CHECK(hl_linux_openat_reserved(&linux_abi, &reservation, HL_LINUX_AT_FDCWD, "file", 4, HL_LINUX_O_RDONLY,
                                          0) == 6);
        HL_CHECK(hl_linux_fd_cancel(&linux_abi, &reservation) == HL_STATUS_NOT_FOUND);
        HL_CHECK(hl_linux_close(&linux_abi, 6) == 0);
        file_host.closes = 0;
        file_host.opens = 0;
    }
    hl_fake_host_fail_next(&file_host.fake, HL_STATUS_IO);
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, 0, 0, &original) == HL_STATUS_IO);
    HL_CHECK(file_host.fake.live_mutexes == 0);
    HL_CHECK(hl_linux_fd_install_at(&linux_abi, HL_ARRAY_COUNT(fds), 55, 0, 0) == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(hl_linux_fd_install_at(&linux_abi, 7, 55, 0123, 1) == HL_STATUS_OK);
    HL_CHECK(file_host.fake.live_mutexes == 1);
    HL_CHECK(hl_linux_fd_install_at(&linux_abi, 7, 56, 0, 0) == HL_STATUS_ALREADY_EXISTS);
    HL_CHECK(file_host.fake.live_mutexes == 1);
    HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, 7, &snapshot) == HL_STATUS_OK);
    HL_CHECK(snapshot.fd == 7 && snapshot.host_handle == 55 && snapshot.status_flags == 0123 &&
             snapshot.descriptor_flags == 1);
    HL_CHECK(hl_linux_fd_close(&linux_abi, 7, &closed) == HL_STATUS_OK && closed == 55);
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
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
    ofds[snapshot.ofd].references++;
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_CORRUPT);
    ofds[snapshot.ofd].references--;
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);

    /* read advances the OFD shared by both descriptors. */
    HL_CHECK(hl_linux_read(&linux_abi, original, buffer, 2) == 2);
    HL_CHECK(memcmp(buffer, "ab", 2) == 0);
    memset(buffer, 0, sizeof(buffer));
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, buffer, 2) == 2);
    HL_CHECK(memcmp(buffer, "cd", 2) == 0);
    HL_CHECK(file_host.offset == 4);

    /* pread reads at an explicit offset and never changes the shared OFD offset. */
    memset(buffer, 0, sizeof(buffer));
    HL_CHECK(hl_linux_pread64(&linux_abi, duplicate, buffer, 2, 1) == 2);
    HL_CHECK(memcmp(buffer, "bc", 2) == 0);
    HL_CHECK(file_host.offset == 4);

    /* A failed host read becomes Linux errno and does not advance the offset. */
    file_host.next_status = HL_STATUS_WOULD_BLOCK;
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, buffer, 1) == -HL_LINUX_EAGAIN);
    HL_CHECK(file_host.offset == 4);
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, NULL, 1) == -HL_LINUX_EINVAL);
    file_host.next_value = 9;
    HL_CHECK(hl_linux_read(&linux_abi, duplicate, buffer, 1) == -HL_LINUX_EIO);
    HL_CHECK(file_host.offset == 4);

    closed = HL_HOST_HANDLE_INVALID;
    HL_CHECK(hl_linux_fd_close(&linux_abi, original, &closed) == HL_STATUS_OK);
    HL_CHECK(closed == HL_HOST_HANDLE_INVALID);
    HL_CHECK(hl_linux_fd_close(&linux_abi, duplicate, &closed) == HL_STATUS_OK);
    HL_CHECK(closed == 55);
    HL_CHECK(hl_linux_fd_close(&linux_abi, duplicate, &closed) == HL_STATUS_NOT_FOUND);
    HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);

    /* Exec removes only CLOEXEC descriptors and preserves a shared OFD until its final reference. */
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDWR, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, HL_LINUX_FD_CLOEXEC, &duplicate) == HL_STATUS_OK);
    {
        uint32_t removed = 99;
        uint32_t closes_before = file_host.closes;
        HL_CHECK(hl_linux_fd_exec(&linux_abi, original, &removed) == HL_STATUS_OK && removed == 0);
        HL_CHECK(hl_linux_fd_exec(&linux_abi, duplicate, &removed) == HL_STATUS_OK && removed == 1);
        HL_CHECK(file_host.closes == closes_before);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK);
        HL_CHECK(snapshot.descriptor_references == 1 && snapshot.status_flags == HL_LINUX_O_RDWR);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, duplicate, &snapshot) == HL_STATUS_NOT_FOUND);
        HL_CHECK(hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
        HL_CHECK(hl_linux_close(&linux_abi, original) == 0 && file_host.closes == closes_before + 1);
        file_host.closes = 0;
    }

    /* Syscall close closes the host handle exactly once, at the final OFD reference. */
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_HOST_FILE_READ, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(hl_linux_close(&linux_abi, original) == 0 && file_host.closes == 0);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == 0 && file_host.closes == 1);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == -HL_LINUX_EBADF);
    HL_CHECK(file_host.bad_handles == 0);

    /* write shares its OFD offset across dup; pwrite is positional and leaves it unchanged. */
    file_host.size = 0;
    file_host.offset = 0;
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDWR, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(hl_linux_write(&linux_abi, original, "xy", 2) == 2);
    HL_CHECK(hl_linux_write(&linux_abi, duplicate, "z", 1) == 1);
    HL_CHECK(file_host.offset == 3);
    HL_CHECK(hl_linux_pwrite64(&linux_abi, duplicate, "Q", 1, 0) == 1);
    HL_CHECK(memcmp(file_host.bytes, "Qyz", 3) == 0);
    HL_CHECK(file_host.offset == 3);
    file_host.next_status = HL_STATUS_WOULD_BLOCK;
    HL_CHECK(hl_linux_write(&linux_abi, original, "!", 1) == -HL_LINUX_EAGAIN);
    HL_CHECK(file_host.offset == 3);
    HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
    HL_CHECK(hl_linux_close(&linux_abi, duplicate) == 0);

    /* lseek changes the shared OFD offset; SEEK_END uses portable host metadata. */
    file_host.size = 6;
    file_host.offset = 0;
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDWR, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_dup(&linux_abi, original) == 1);
    duplicate = 1;
    HL_CHECK(hl_linux_lseek(&linux_abi, original, 2, HL_LINUX_SEEK_SET) == 2);
    HL_CHECK(hl_linux_lseek(&linux_abi, duplicate, 1, HL_LINUX_SEEK_CUR) == 3);
    HL_CHECK(hl_linux_lseek(&linux_abi, original, -1, HL_LINUX_SEEK_END) == 5);
    HL_CHECK(file_host.offset == 5);
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

    /* O_APPEND delegates each write as one atomic append to the host's authoritative open description. */
    file_host.size = 3;
    file_host.offset = 0;
    HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_WRONLY | HL_LINUX_O_APPEND, 0, &original) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_dup(&linux_abi, original, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(hl_linux_write(&linux_abi, original, "A", 1) == 1);
    HL_CHECK(hl_linux_write(&linux_abi, duplicate, "B", 1) == 1);
    HL_CHECK(file_host.appends == 2 && memcmp(file_host.bytes, "QyzAB", 5) == 0);
    HL_CHECK(file_host.offset == 5);
    HL_CHECK(hl_linux_pwrite64(&linux_abi, duplicate, "P", 1, 1) == 1);
    HL_CHECK(file_host.bytes[1] == 'P');
    HL_CHECK(file_host.offset == 5);
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
        const struct {
            hl_status status;
            int64_t result;
        } errors[] = {{HL_STATUS_NOT_DIRECTORY, -HL_LINUX_ENOTDIR},
                      {HL_STATUS_IS_DIRECTORY, -HL_LINUX_EISDIR},
                      {HL_STATUS_NAME_TOO_LONG, -HL_LINUX_ENAMETOOLONG},
                      {HL_STATUS_SYMLINK_LOOP, -HL_LINUX_ELOOP},
                      {HL_STATUS_READ_ONLY, -HL_LINUX_EROFS}};

        size_t i;
        for (i = 0; i < HL_ARRAY_COUNT(errors); ++i) {
            file_host.next_status = errors[i].status;
            HL_CHECK(hl_linux_openat(&linux_abi, HL_LINUX_AT_FDCWD, "file", 4, HL_LINUX_O_RDONLY, 0) ==
                     errors[i].result);
        }
    }

    {
        hl_host_iovec vectors[2] = {{(uint64_t)(uintptr_t)buffer, 2}, {(uint64_t)(uintptr_t)(buffer + 2), 3}};
        file_host.offset = 0;
        file_host.next_value = 3;
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDONLY, 0, &original) == HL_STATUS_OK);
        HL_CHECK(hl_linux_readv(&linux_abi, original, vectors, 2) == 3);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK && snapshot.offset == 0);
        file_host.next_status = HL_STATUS_WOULD_BLOCK;
        HL_CHECK(hl_linux_preadv(&linux_abi, original, vectors, 2, 1) == -HL_LINUX_EAGAIN);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK && snapshot.offset == 0);
        HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
        file_host.closes = 0;

        file_host.size = 0;
        file_host.appends = 0;
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_WRONLY | HL_LINUX_O_APPEND, 0, &original) ==
                 HL_STATUS_OK);
        HL_CHECK(hl_linux_writev(&linux_abi, original, vectors, 2) == 5 && file_host.appends == 1);
        HL_CHECK(hl_linux_pwritev(&linux_abi, original, vectors, 1, 1) == 2 && file_host.appends == 1);
        HL_CHECK(hl_linux_writev(&linux_abi, original, vectors, HL_LINUX_IOV_MAX + 1u) == -HL_LINUX_EINVAL);
        vectors[0].size = (uint64_t)INT64_MAX;
        vectors[1].size = 1;
        HL_CHECK(hl_linux_writev(&linux_abi, original, vectors, 2) == -HL_LINUX_EINVAL);
        HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
        file_host.closes = 0;

        vectors[0] = (hl_host_iovec){(uint64_t)(uintptr_t)buffer, 1};
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_WRONLY, 0, &original) == HL_STATUS_OK);
        HL_CHECK(hl_linux_readv(&linux_abi, original, vectors, 1) == -HL_LINUX_EBADF);
        HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
        file_host.closes = 0;
    }

    {
        hl_linux_file_status control_status;
        file_host.size = 3;
        file_host.offset = 0;
        file_host.truncates = 0;
        file_host.syncs = 0;
        file_host.data_syncs = 0;
        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDWR, 0, &original) == HL_STATUS_OK);
        HL_CHECK(hl_linux_lseek(&linux_abi, original, 2, HL_LINUX_SEEK_SET) == 2);
        HL_CHECK(hl_linux_ftruncate(&linux_abi, original, 6) == 0 && file_host.truncates == 1);
        HL_CHECK(hl_linux_lseek(&linux_abi, original, 0, HL_LINUX_SEEK_CUR) == 2);
        HL_CHECK(file_host.bytes[3] == 0 && file_host.bytes[4] == 0 && file_host.bytes[5] == 0);
        HL_CHECK(hl_linux_fstat(&linux_abi, original, &control_status) == 0 && control_status.size == 6);
        HL_CHECK(hl_linux_ftruncate(&linux_abi, original, 1) == 0 && file_host.truncates == 2);
        HL_CHECK(hl_linux_fsync(&linux_abi, original) == 0 && file_host.syncs == 1);
        HL_CHECK(hl_linux_fdatasync(&linux_abi, original) == 0 && file_host.data_syncs == 1);
        file_host.next_status = HL_STATUS_IO;
        HL_CHECK(hl_linux_fsync(&linux_abi, original) == -HL_LINUX_EIO);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, original, &snapshot) == HL_STATUS_OK);
        HL_CHECK(hl_linux_ftruncate(&linux_abi, original, (uint64_t)INT64_MAX + 1u) == -HL_LINUX_EINVAL);
        HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
        file_host.closes = 0;
        HL_CHECK(hl_linux_fsync(&linux_abi, original) == -HL_LINUX_EBADF);
        HL_CHECK(hl_linux_fdatasync(&linux_abi, original) == -HL_LINUX_EBADF);
        HL_CHECK(hl_linux_ftruncate(&linux_abi, original, 0) == -HL_LINUX_EBADF);

        HL_CHECK(hl_linux_fd_install(&linux_abi, 55, HL_LINUX_O_RDONLY, 0, &original) == HL_STATUS_OK);
        HL_CHECK(hl_linux_ftruncate(&linux_abi, original, 0) == -HL_LINUX_EBADF);
        HL_CHECK(hl_linux_close(&linux_abi, original) == 0);
        file_host.closes = 0;
    }

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
        file_host.offset = 0;
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
        file_host.offset = 0;
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
    {
        test_object object = {.byte = 'q'};
        hl_linux_object_pin pin;
        hl_linux_poll_entry poll_entries[2];
        hl_linux_fd typed;
        hl_linux_fd alias;
        hl_linux_fork_record records[8];
        hl_linux_fork_plan plan = {
            .abi = HL_LINUX_ABI_VERSION, .size = sizeof(plan), .records = records, .capacity = HL_ARRAY_COUNT(records)};
        uint32_t old_generation;
        char byte = 0;
        HL_CHECK(hl_linux_object_install(&linux_abi, &object_ops, &object, 77, HL_LINUX_O_RDWR, 0, &typed) ==
                 HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, typed, &snapshot) == HL_STATUS_OK && snapshot.kind == 77 &&
                 snapshot.host_handle == HL_HOST_HANDLE_INVALID);
        old_generation = snapshot.descriptor_generation;
        HL_CHECK(hl_linux_fd_dup(&linux_abi, typed, 0, &alias) == HL_STATUS_OK);
        HL_CHECK(hl_linux_read(&linux_abi, alias, &byte, 1) == 1 && byte == 'q' && object.reads == 1);
        byte = 'z';
        HL_CHECK(hl_linux_write(&linux_abi, typed, &byte, 1) == 1 && object.byte == 'z' && object.writes == 1);
        HL_CHECK(hl_linux_object_pin_fd(&linux_abi, alias, &pin) == HL_STATUS_OK);
        HL_CHECK(hl_linux_object_ready(&pin, HL_LINUX_READY_READ) == HL_LINUX_READY_READ);
        hl_linux_object_unpin(&pin);
        poll_entries[0] = (hl_linux_poll_entry){typed, HL_LINUX_READY_READ, 0};
        poll_entries[1] = (hl_linux_poll_entry){7, HL_LINUX_READY_READ, 0};
        HL_CHECK(hl_linux_object_poll(&linux_abi, poll_entries, 2, 0) == 2);
        HL_CHECK(poll_entries[0].readiness == HL_LINUX_READY_READ && poll_entries[1].readiness == HL_LINUX_READY_ERROR);
        HL_CHECK(hl_linux_abi_fork_prepare(&linux_abi, &plan) == HL_STATUS_OK && object.clones == 1);
        HL_CHECK(hl_linux_abi_fork_parent(&linux_abi, &plan) == HL_STATUS_OK && object.closes == 1);
        object.clone_status = HL_STATUS_OUT_OF_MEMORY;
        HL_CHECK(hl_linux_abi_fork_prepare(&linux_abi, &plan) == HL_STATUS_OUT_OF_MEMORY && object.clones == 2);
        object.clone_status = HL_STATUS_OK;
        HL_CHECK(hl_linux_close(&linux_abi, typed) == 0 && object.closes == 1);
        object.close_status = HL_STATUS_IO;
        HL_CHECK(hl_linux_close(&linux_abi, alias) == -HL_LINUX_EIO && object.closes == 2);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, typed, &snapshot) == HL_STATUS_NOT_FOUND);
        object.close_status = HL_STATUS_OK;
        HL_CHECK(hl_linux_object_install_at(&linux_abi, typed, &object_ops, &object, 77, HL_LINUX_O_RDWR, 0) ==
                 HL_STATUS_OK);
        HL_CHECK(hl_linux_fd_snapshot_get(&linux_abi, typed, &snapshot) == HL_STATUS_OK &&
                 snapshot.descriptor_generation != old_generation);
        HL_CHECK(hl_linux_close(&linux_abi, typed) == 0 && object.closes == 3);
        HL_CHECK(hl_linux_object_install(&linux_abi, &object_ops, &object, 77, HL_LINUX_O_RDWR, 0, &typed) ==
                 HL_STATUS_OK);
        HL_CHECK(hl_linux_object_pin_fd(&linux_abi, typed, &pin) == HL_STATUS_OK);
        HL_CHECK(hl_linux_close(&linux_abi, typed) == 0 && object.closes == 3);
        hl_linux_object_unpin(&pin);
        HL_CHECK(object.closes == 4 && hl_linux_abi_validate_fds(&linux_abi) == HL_STATUS_OK);
        HL_CHECK(hl_linux_object_install(&linux_abi, &object_ops, &object, 77, HL_LINUX_O_RDWR, HL_LINUX_FD_CLOEXEC,
                                         &typed) == HL_STATUS_OK);
        object.close_status = HL_STATUS_IO;
        {
            uint32_t removed = 0;
            HL_CHECK(hl_linux_fd_exec(&linux_abi, typed, &removed) == HL_STATUS_IO && removed == 1);
        }
        object.close_status = HL_STATUS_OK;
    }
    HL_CHECK(hl_linux_abi_destroy(&linux_abi) == HL_STATUS_OK);
    HL_CHECK(file_host.fake.live_mutexes == 0);
    return EXIT_SUCCESS;
}
