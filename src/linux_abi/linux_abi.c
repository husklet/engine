#include "hl/linux_abi.h"

#include <string.h>

static void hl_linux_lock(hl_linux_abi *linux_abi) {
    while (atomic_flag_test_and_set_explicit(&linux_abi->table_lock, memory_order_acquire)) {}
}

static void hl_linux_unlock(hl_linux_abi *linux_abi) {
    atomic_flag_clear_explicit(&linux_abi->table_lock, memory_order_release);
}

static void hl_linux_ofd_lock(hl_linux_ofd_entry *ofd) {
    (void)mtx_lock(&ofd->io_lock);
}

static void hl_linux_ofd_unlock(hl_linux_ofd_entry *ofd) {
    (void)mtx_unlock(&ofd->io_lock);
}

static int64_t hl_linux_error(hl_status status) {
    switch (status) {
    case HL_STATUS_OK: return 0;
    case HL_STATUS_INTERRUPTED: return -HL_LINUX_EINTR;
    case HL_STATUS_NOT_FOUND: return -HL_LINUX_EBADF;
    case HL_STATUS_WOULD_BLOCK: return -HL_LINUX_EAGAIN;
    case HL_STATUS_OUT_OF_MEMORY: return -HL_LINUX_ENOMEM;
    case HL_STATUS_PERMISSION_DENIED: return -HL_LINUX_EACCES;
    case HL_STATUS_BUSY: return -HL_LINUX_EBUSY;
    case HL_STATUS_ALREADY_EXISTS: return -HL_LINUX_EEXIST;
    case HL_STATUS_RESOURCE_LIMIT: return -HL_LINUX_ENFILE;
    case HL_STATUS_INVALID_ARGUMENT:
    case HL_STATUS_ABI_MISMATCH:
    case HL_STATUS_CORRUPT: return -HL_LINUX_EINVAL;
    case HL_STATUS_NOT_SUPPORTED: return -HL_LINUX_ENOSYS;
    case HL_STATUS_IO:
    case HL_STATUS_PLATFORM_FAILURE:
    default: return -HL_LINUX_EIO;
    }
}

static const hl_host_file_services *hl_linux_files(const hl_linux_abi *linux_abi) {
    const hl_host_services *host;
    if (linux_abi == NULL || (host = linux_abi->host) == NULL || (host->capabilities & HL_HOST_CAP_FILE) == 0 ||
        host->file == NULL || host->file->abi != HL_HOST_FILE_ABI || host->file->size < sizeof(*host->file))
        return NULL;
    return host->file;
}

/* All helpers below through hl_linux_fd_get_unlocked require table_lock. */
static hl_status hl_linux_find_fd(const hl_linux_abi *linux_abi, hl_linux_fd *out_fd) {
    uint32_t fd;
    for (fd = 0; fd < linux_abi->fd_capacity; ++fd) {
        if (linux_abi->fds[fd].ofd == 0) {
            *out_fd = fd;
            return HL_STATUS_OK;
        }
    }
    return HL_STATUS_RESOURCE_LIMIT;
}

static hl_status hl_linux_find_ofd(const hl_linux_abi *linux_abi, hl_linux_ofd *out_ofd) {
    uint32_t ofd;
    for (ofd = 1; ofd < linux_abi->ofd_capacity; ++ofd) {
        if (linux_abi->ofds[ofd].references == 0 && linux_abi->ofds[ofd].active_operations == 0 &&
            linux_abi->ofds[ofd].closing == 0) {
            *out_ofd = ofd;
            return HL_STATUS_OK;
        }
    }
    return HL_STATUS_RESOURCE_LIMIT;
}

static hl_status hl_linux_fd_get_unlocked(const hl_linux_abi *linux_abi, hl_linux_fd fd,
                                          const hl_linux_fd_entry **fd_entry, const hl_linux_ofd_entry **ofd_entry) {
    hl_linux_ofd ofd;
    if (fd >= linux_abi->fd_capacity || linux_abi->fds[fd].ofd == 0) return HL_STATUS_NOT_FOUND;
    ofd = linux_abi->fds[fd].ofd;
    if (ofd >= linux_abi->ofd_capacity || linux_abi->ofds[ofd].references == 0) return HL_STATUS_CORRUPT;
    if (fd_entry != NULL) *fd_entry = &linux_abi->fds[fd];
    if (ofd_entry != NULL) *ofd_entry = &linux_abi->ofds[ofd];
    return HL_STATUS_OK;
}

hl_status hl_linux_abi_init(hl_linux_abi *linux_abi, const hl_host_services *host, hl_linux_fd_entry *fd_storage,
                            uint32_t fd_capacity, hl_linux_ofd_entry *ofd_storage, uint32_t ofd_capacity) {
    if (linux_abi == NULL || host == NULL || fd_storage == NULL || ofd_storage == NULL || fd_capacity == 0 ||
        fd_capacity > HL_LINUX_FD_LIMIT || ofd_capacity < 2 || ofd_capacity > HL_LINUX_OFD_LIMIT)
        return HL_STATUS_INVALID_ARGUMENT;
    memset(linux_abi, 0, sizeof(*linux_abi));
    memset(fd_storage, 0, sizeof(*fd_storage) * fd_capacity);
    memset(ofd_storage, 0, sizeof(*ofd_storage) * ofd_capacity);
    for (uint32_t ofd = 0; ofd < ofd_capacity; ++ofd) {
        if (mtx_init(&ofd_storage[ofd].io_lock, mtx_plain) != thrd_success) {
            while (ofd != 0)
                mtx_destroy(&ofd_storage[--ofd].io_lock);
            return HL_STATUS_RESOURCE_LIMIT;
        }
    }
    linux_abi->abi = HL_LINUX_ABI_VERSION;
    linux_abi->size = sizeof(*linux_abi);
    linux_abi->host = host;
    linux_abi->fds = fd_storage;
    linux_abi->fd_capacity = fd_capacity;
    linux_abi->ofds = ofd_storage;
    linux_abi->ofd_capacity = ofd_capacity;
    atomic_flag_clear(&linux_abi->table_lock);
    return HL_STATUS_OK;
}

hl_status hl_linux_abi_destroy(hl_linux_abi *linux_abi) {
    uint32_t ofd;
    if (linux_abi == NULL || linux_abi->abi != HL_LINUX_ABI_VERSION) return HL_STATUS_INVALID_ARGUMENT;
    hl_linux_lock(linux_abi);
    for (ofd = 1; ofd < linux_abi->ofd_capacity; ++ofd) {
        if (linux_abi->ofds[ofd].references != 0 || linux_abi->ofds[ofd].active_operations != 0 ||
            linux_abi->ofds[ofd].closing != 0) {
            hl_linux_unlock(linux_abi);
            return HL_STATUS_BUSY;
        }
    }
    hl_linux_unlock(linux_abi);
    for (ofd = 0; ofd < linux_abi->ofd_capacity; ++ofd)
        mtx_destroy(&linux_abi->ofds[ofd].io_lock);
    linux_abi->abi = 0;
    return HL_STATUS_OK;
}

hl_status hl_linux_fd_install(hl_linux_abi *linux_abi, hl_host_handle host_handle, uint32_t status_flags,
                              uint32_t descriptor_flags, hl_linux_fd *out_fd) {
    hl_linux_fd fd;
    hl_linux_ofd ofd;
    hl_status status;
    if (linux_abi == NULL || host_handle == HL_HOST_HANDLE_INVALID || out_fd == NULL) return HL_STATUS_INVALID_ARGUMENT;
    hl_linux_lock(linux_abi);
    status = hl_linux_find_fd(linux_abi, &fd);
    if (status != HL_STATUS_OK) goto done;
    status = hl_linux_find_ofd(linux_abi, &ofd);
    if (status != HL_STATUS_OK) goto done;
    linux_abi->ofds[ofd].host_handle = host_handle;
    linux_abi->ofds[ofd].status_flags = status_flags;
    linux_abi->ofds[ofd].references = 1;
    linux_abi->ofds[ofd].generation++;
    linux_abi->fds[fd].ofd = ofd;
    linux_abi->fds[fd].descriptor_flags = descriptor_flags;
    linux_abi->fds[fd].generation++;
    *out_fd = fd;
done:
    hl_linux_unlock(linux_abi);
    return status;
}

hl_status hl_linux_fd_snapshot_get(const hl_linux_abi *linux_abi, hl_linux_fd fd, hl_linux_fd_snapshot *snapshot) {
    const hl_linux_fd_entry *fd_entry;
    const hl_linux_ofd_entry *ofd_entry;
    hl_status status;
    if (linux_abi == NULL || snapshot == NULL) return HL_STATUS_INVALID_ARGUMENT;
    hl_linux_lock((hl_linux_abi *)linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, &fd_entry, &ofd_entry);
    if (status == HL_STATUS_OK) {
        snapshot->fd = fd;
        snapshot->ofd = fd_entry->ofd;
        snapshot->host_handle = ofd_entry->host_handle;
        snapshot->offset = ofd_entry->offset;
        snapshot->status_flags = ofd_entry->status_flags;
        snapshot->descriptor_flags = fd_entry->descriptor_flags;
        snapshot->descriptor_generation = fd_entry->generation;
        snapshot->ofd_generation = ofd_entry->generation;
        snapshot->descriptor_references = ofd_entry->references;
        snapshot->kind = ofd_entry->kind;
    }
    hl_linux_unlock((hl_linux_abi *)linux_abi);
    return status;
}

hl_status hl_linux_fd_dup(hl_linux_abi *linux_abi, hl_linux_fd source, uint32_t descriptor_flags, hl_linux_fd *out_fd) {
    const hl_linux_fd_entry *source_entry;
    hl_linux_fd fd;
    hl_status status;
    if (linux_abi == NULL || out_fd == NULL) return HL_STATUS_INVALID_ARGUMENT;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, source, &source_entry, NULL);
    if (status != HL_STATUS_OK) goto done;
    status = hl_linux_find_fd(linux_abi, &fd);
    if (status != HL_STATUS_OK) goto done;
    linux_abi->fds[fd].ofd = source_entry->ofd;
    linux_abi->fds[fd].descriptor_flags = descriptor_flags;
    linux_abi->fds[fd].generation++;
    linux_abi->ofds[source_entry->ofd].references++;
    *out_fd = fd;
done:
    hl_linux_unlock(linux_abi);
    return status;
}

hl_status hl_linux_fd_close(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_host_handle *last_host_handle) {
    hl_linux_ofd ofd;
    hl_linux_ofd_entry *ofd_entry;
    int final_reference;
    if (last_host_handle != NULL) *last_host_handle = HL_HOST_HANDLE_INVALID;
    if (linux_abi == NULL) return HL_STATUS_NOT_FOUND;
    hl_linux_lock(linux_abi);
    if (fd >= linux_abi->fd_capacity || linux_abi->fds[fd].ofd == 0) {
        hl_linux_unlock(linux_abi);
        return HL_STATUS_NOT_FOUND;
    }
    ofd = linux_abi->fds[fd].ofd;
    if (ofd >= linux_abi->ofd_capacity) {
        hl_linux_unlock(linux_abi);
        return HL_STATUS_CORRUPT;
    }
    ofd_entry = &linux_abi->ofds[ofd];
    if (ofd_entry->references == 0) {
        hl_linux_unlock(linux_abi);
        return HL_STATUS_CORRUPT;
    }
    memset(&linux_abi->fds[fd], 0, sizeof(linux_abi->fds[fd]));
    ofd_entry->references--;
    final_reference = ofd_entry->references == 0;
    if (final_reference) ofd_entry->closing = 1;
    hl_linux_unlock(linux_abi);
    if (!final_reference) return HL_STATUS_OK;

    /* Wait only for this OFD. Operations drop their pin before releasing io_lock. */
    hl_linux_ofd_lock(ofd_entry);
    hl_linux_lock(linux_abi);
    if (ofd_entry->references != 0 || ofd_entry->active_operations != 0 || ofd_entry->closing == 0) {
        hl_linux_unlock(linux_abi);
        hl_linux_ofd_unlock(ofd_entry);
        return HL_STATUS_CORRUPT;
    }
    if (last_host_handle != NULL) *last_host_handle = ofd_entry->host_handle;
    ofd_entry->host_handle = HL_HOST_HANDLE_INVALID;
    ofd_entry->offset = 0;
    ofd_entry->status_flags = 0;
    ofd_entry->closing = 0;
    ofd_entry->generation++;
    ofd_entry->kind = 0;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(ofd_entry);
    return HL_STATUS_OK;
}

static int64_t hl_linux_pread64_owned(hl_linux_abi *linux_abi, hl_linux_ofd_entry *ofd, void *buffer, size_t size,
                                      uint64_t offset) {
    const hl_host_file_services *files;
    hl_host_result result;
    if (size != 0 && buffer == NULL) return -HL_LINUX_EINVAL;
    files = hl_linux_files(linux_abi);
    if (files == NULL || files->read_at == NULL) return -HL_LINUX_ENOSYS;
    result = files->read_at(linux_abi->host->context, ofd->host_handle, offset, (hl_host_bytes){buffer, size});
    if (result.status != HL_STATUS_OK) return hl_linux_error((hl_status)result.status);
    if (result.value > size || result.value > (uint64_t)INT64_MAX) return -HL_LINUX_EIO;
    return (int64_t)result.value;
}

int64_t hl_linux_pread64(hl_linux_abi *linux_abi, hl_linux_fd fd, void *buffer, size_t size, uint64_t offset) {
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    int64_t result;
    hl_status status;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    if (size > (size_t)INT64_MAX) return -HL_LINUX_EINVAL;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, NULL, &found);
    if (status != HL_STATUS_OK) {
        hl_linux_unlock(linux_abi);
        return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    }
    ofd = &linux_abi->ofds[(size_t)(found - linux_abi->ofds)];
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(ofd);
    result = hl_linux_pread64_owned(linux_abi, ofd, buffer, size, offset);
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(ofd);
    return result;
}

int64_t hl_linux_read(hl_linux_abi *linux_abi, hl_linux_fd fd, void *buffer, size_t size) {
    const hl_linux_fd_entry *fd_entry;
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    hl_linux_ofd ofd_index;
    int64_t result;
    hl_status status;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    if (size > (size_t)INT64_MAX) return -HL_LINUX_EINVAL;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, &fd_entry, &found);
    if (status != HL_STATUS_OK) {
        result = status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
        goto done;
    }
    ofd_index = fd_entry->ofd;
    ofd = &linux_abi->ofds[ofd_index];
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(ofd);
    result = hl_linux_pread64_owned(linux_abi, ofd, buffer, size, ofd->offset);
    if (result > 0) {
        uint64_t count = (uint64_t)result;
        if (UINT64_MAX - linux_abi->ofds[ofd_index].offset < count) {
            result = -HL_LINUX_EIO;
            goto io_done;
        }
        linux_abi->ofds[ofd_index].offset += count;
    }
io_done:
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(ofd);
    return result;
done:
    hl_linux_unlock(linux_abi);
    return result;
}

int64_t hl_linux_close(hl_linux_abi *linux_abi, hl_linux_fd fd) {
    const hl_host_file_services *files;
    hl_host_handle handle = HL_HOST_HANDLE_INVALID;
    hl_status status = hl_linux_fd_close(linux_abi, fd, &handle);
    hl_host_result result;
    if (status != HL_STATUS_OK) return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    if (handle == HL_HOST_HANDLE_INVALID) return 0;
    files = hl_linux_files(linux_abi);
    if (files == NULL || files->close == NULL) return -HL_LINUX_ENOSYS;
    result = files->close(linux_abi->host->context, handle);
    return result.status == HL_STATUS_OK ? 0 : hl_linux_error((hl_status)result.status);
}
