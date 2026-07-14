#include "hl/linux_abi.h"

#include <string.h>

static void hl_linux_lock(hl_linux_abi *linux_abi) {
    while (atomic_flag_test_and_set_explicit(&linux_abi->table_lock, memory_order_acquire)) {}
}

static void hl_linux_unlock(hl_linux_abi *linux_abi) {
    atomic_flag_clear_explicit(&linux_abi->table_lock, memory_order_release);
}

static const hl_host_sync_services *hl_linux_sync(const hl_linux_abi *linux_abi) {
    const hl_host_services *host;
    if (linux_abi == NULL || (host = linux_abi->host) == NULL || (host->capabilities & HL_HOST_CAP_SYNC) == 0 ||
        host->sync == NULL || host->sync->abi != HL_HOST_SYNC_ABI || host->sync->size < sizeof(*host->sync))
        return NULL;
    return host->sync;
}

static void hl_linux_ofd_lock(hl_linux_abi *linux_abi, hl_linux_ofd_entry *ofd) {
    (void)hl_linux_sync(linux_abi)->mutex_lock(linux_abi->host->context, ofd->io_mutex);
}

static void hl_linux_ofd_unlock(hl_linux_abi *linux_abi, hl_linux_ofd_entry *ofd) {
    (void)hl_linux_sync(linux_abi)->mutex_unlock(linux_abi->host->context, ofd->io_mutex);
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

static hl_status hl_linux_find_fd_at_least(const hl_linux_abi *linux_abi, hl_linux_fd minimum, hl_linux_fd *out_fd) {
    uint32_t fd;
    if (minimum >= linux_abi->fd_capacity) return HL_STATUS_RESOURCE_LIMIT;
    for (fd = minimum; fd < linux_abi->fd_capacity; ++fd) {
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
            linux_abi->ofds[ofd].closing == 0 && linux_abi->ofds[ofd].io_mutex == HL_HOST_HANDLE_INVALID) {
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
    linux_abi->abi = HL_LINUX_ABI_VERSION;
    linux_abi->size = sizeof(*linux_abi);
    linux_abi->host = host;
    linux_abi->fds = fd_storage;
    linux_abi->fd_capacity = fd_capacity;
    linux_abi->ofds = ofd_storage;
    linux_abi->ofd_capacity = ofd_capacity;
    const hl_host_sync_services *sync = hl_linux_sync(linux_abi);
    if (sync == NULL || sync->mutex_create == NULL || sync->mutex_lock == NULL || sync->mutex_unlock == NULL ||
        sync->mutex_close == NULL) {
        linux_abi->abi = 0;
        return HL_STATUS_NOT_SUPPORTED;
    }
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
    linux_abi->abi = 0;
    return HL_STATUS_OK;
}

hl_status hl_linux_abi_fork_prepare(hl_linux_abi *linux_abi, hl_linux_fork_plan *plan) {
    const hl_host_file_services *files;
    const hl_host_sync_services *sync;
    uint32_t index;
    if (linux_abi == NULL || linux_abi->abi != HL_LINUX_ABI_VERSION || plan == NULL ||
        plan->abi != HL_LINUX_ABI_VERSION || plan->size < sizeof(*plan) ||
        (plan->capacity != 0 && plan->records == NULL))
        return HL_STATUS_INVALID_ARGUMENT;
    files = hl_linux_files(linux_abi);
    sync = hl_linux_sync(linux_abi);
    if (files == NULL || files->clone_for_fork == NULL || files->close == NULL || sync == NULL ||
        sync->fork_prepare == NULL)
        return HL_STATUS_NOT_SUPPORTED;
    plan->count = 0;
    plan->armed = 0;
    hl_linux_lock(linux_abi);
    for (index = 1; index < linux_abi->ofd_capacity; ++index) {
        hl_linux_ofd_entry *entry = &linux_abi->ofds[index];
        if (entry->references == 0) continue;
        if (entry->active_operations != 0 || entry->closing != 0) {
            hl_linux_unlock(linux_abi);
            plan->count = 0;
            return HL_STATUS_BUSY;
        }
        if (plan->count == plan->capacity) {
            hl_linux_unlock(linux_abi);
            plan->count = 0;
            return HL_STATUS_RESOURCE_LIMIT;
        }
        plan->records[plan->count++] = (hl_linux_fork_record){index, entry->generation, entry->host_handle,
                                                              HL_HOST_HANDLE_INVALID, HL_HOST_HANDLE_INVALID};
    }
    hl_linux_unlock(linux_abi);
    /* External quiescence keeps snapshots stable; host callbacks run without the ABI table lock. */
    for (index = 0; index < plan->count; ++index) {
        hl_host_result cloned = files->clone_for_fork(linux_abi->host->context, plan->records[index].parent_handle);
        if (cloned.status != HL_STATUS_OK || cloned.value == HL_HOST_HANDLE_INVALID) {
            hl_status failure = cloned.status == HL_STATUS_OK ? HL_STATUS_PLATFORM_FAILURE : (hl_status)cloned.status;
            while (index != 0)
                (void)files->close(linux_abi->host->context, plan->records[--index].child_handle);
            plan->count = 0;
            return failure;
        }
        plan->records[index].child_handle = cloned.value;
    }
    hl_linux_lock(linux_abi);
    /* Require a bijection: no live OFD may have appeared during the unlocked clone phase. */
    for (index = 1; index < linux_abi->ofd_capacity; ++index) {
        hl_linux_ofd_entry *entry = &linux_abi->ofds[index];
        uint32_t record_index;
        uint32_t matches = 0;
        if (entry->references == 0) continue;
        for (record_index = 0; record_index < plan->count; ++record_index) {
            hl_linux_fork_record *record = &plan->records[record_index];
            if (record->ofd == index && record->generation == entry->generation &&
                record->parent_handle == entry->host_handle)
                matches++;
        }
        if (matches != 1 || entry->active_operations != 0 || entry->closing != 0) goto arm_failed;
    }
    for (index = 0; index < plan->count; ++index) {
        hl_linux_fork_record *record = &plan->records[index];
        if (record->ofd >= linux_abi->ofd_capacity || linux_abi->ofds[record->ofd].references == 0 ||
            linux_abi->ofds[record->ofd].generation != record->generation ||
            linux_abi->ofds[record->ofd].host_handle != record->parent_handle)
            goto arm_failed;
    }
    {
        hl_host_result armed = sync->fork_prepare(linux_abi->host->context);
        if (armed.status != HL_STATUS_OK) goto arm_failed;
    }
    plan->armed = 1;
    return HL_STATUS_OK;
arm_failed:
    hl_linux_unlock(linux_abi);
    while (plan->count != 0)
        (void)files->close(linux_abi->host->context, plan->records[--plan->count].child_handle);
    return HL_STATUS_BUSY;
}

hl_status hl_linux_abi_fork_parent(hl_linux_abi *linux_abi, hl_linux_fork_plan *plan) {
    const hl_host_file_services *files;
    const hl_host_sync_services *sync;
    hl_status status = HL_STATUS_OK;
    if (linux_abi == NULL || plan == NULL || plan->abi != HL_LINUX_ABI_VERSION) return HL_STATUS_INVALID_ARGUMENT;
    files = hl_linux_files(linux_abi);
    sync = hl_linux_sync(linux_abi);
    if (files == NULL || files->close == NULL || sync == NULL || sync->fork_parent == NULL)
        return HL_STATUS_NOT_SUPPORTED;
    if (plan->armed == 0) return HL_STATUS_INVALID_ARGUMENT;
    {
        hl_host_result completed = sync->fork_parent(linux_abi->host->context);
        plan->armed = 0;
        hl_linux_unlock(linux_abi);
        if (completed.status != HL_STATUS_OK) status = (hl_status)completed.status;
    }
    while (plan->count != 0) {
        hl_host_result closed = files->close(linux_abi->host->context, plan->records[--plan->count].child_handle);
        if (closed.status != HL_STATUS_OK && status == HL_STATUS_OK) status = (hl_status)closed.status;
    }
    return status;
}

hl_status hl_linux_abi_fork_child(hl_linux_abi *linux_abi, hl_linux_fork_plan *plan) {
    const hl_host_file_services *files;
    const hl_host_sync_services *sync;
    uint32_t index;
    if (linux_abi == NULL || plan == NULL || plan->abi != HL_LINUX_ABI_VERSION) return HL_STATUS_INVALID_ARGUMENT;
    files = hl_linux_files(linux_abi);
    sync = hl_linux_sync(linux_abi);
    if (files == NULL || files->close == NULL || sync == NULL || sync->mutex_create == NULL ||
        sync->mutex_close == NULL || sync->fork_child == NULL || plan->armed == 0)
        return HL_STATUS_NOT_SUPPORTED;
    {
        hl_host_result completed = sync->fork_child(linux_abi->host->context);
        if (completed.status != HL_STATUS_OK) return (hl_status)completed.status;
    }
    plan->armed = 0;
    atomic_flag_clear(&linux_abi->table_lock);
    /* Phase one validates every record and allocates every replacement lock without mutating an OFD. */
    for (index = 0; index < plan->count; ++index) {
        hl_linux_fork_record *record = &plan->records[index];
        hl_host_result created;
        if (record->ofd >= linux_abi->ofd_capacity || linux_abi->ofds[record->ofd].generation != record->generation ||
            linux_abi->ofds[record->ofd].host_handle != record->parent_handle)
            goto corrupt;
        created = sync->mutex_create(linux_abi->host->context);
        if (created.status != HL_STATUS_OK || created.value == HL_HOST_HANDLE_INVALID) {
            hl_status status = created.status == HL_STATUS_OK ? HL_STATUS_PLATFORM_FAILURE : (hl_status)created.status;
            while (index != 0)
                (void)sync->mutex_close(linux_abi->host->context, plan->records[--index].child_mutex);
            return status;
        }
        record->child_mutex = created.value;
    }
    /* Phase two cannot fail: swap validated handles/locks, then release this child's inherited copies. */
    for (index = 0; index < plan->count; ++index) {
        hl_linux_fork_record *record = &plan->records[index];
        hl_linux_ofd_entry *entry = &linux_abi->ofds[record->ofd];
        (void)sync->mutex_close(linux_abi->host->context, entry->io_mutex);
        entry->io_mutex = record->child_mutex;
        entry->host_handle = record->child_handle;
        (void)files->close(linux_abi->host->context, record->parent_handle);
    }
    plan->count = 0;
    return HL_STATUS_OK;
corrupt:
    while (index != 0)
        (void)sync->mutex_close(linux_abi->host->context, plan->records[--index].child_mutex);
    return HL_STATUS_CORRUPT;
}

hl_status hl_linux_fd_install(hl_linux_abi *linux_abi, hl_host_handle host_handle, uint32_t status_flags,
                              uint32_t descriptor_flags, hl_linux_fd *out_fd) {
    hl_linux_fd fd;
    hl_linux_ofd ofd;
    hl_host_handle mutex;
    hl_host_result created;
    const hl_host_sync_services *sync;
    hl_status status;
    if (linux_abi == NULL || linux_abi->abi != HL_LINUX_ABI_VERSION || host_handle == HL_HOST_HANDLE_INVALID ||
        out_fd == NULL)
        return HL_STATUS_INVALID_ARGUMENT;
    sync = hl_linux_sync(linux_abi);
    if (sync == NULL || sync->mutex_create == NULL || sync->mutex_close == NULL) return HL_STATUS_NOT_SUPPORTED;
    created = sync->mutex_create(linux_abi->host->context);
    if (created.status != HL_STATUS_OK || created.value == HL_HOST_HANDLE_INVALID)
        return created.status == HL_STATUS_OK ? HL_STATUS_RESOURCE_LIMIT : (hl_status)created.status;
    mutex = created.value;
    hl_linux_lock(linux_abi);
    status = hl_linux_find_fd(linux_abi, &fd);
    if (status != HL_STATUS_OK) goto done;
    status = hl_linux_find_ofd(linux_abi, &ofd);
    if (status != HL_STATUS_OK) goto done;
    linux_abi->ofds[ofd].host_handle = host_handle;
    linux_abi->ofds[ofd].status_flags = status_flags;
    linux_abi->ofds[ofd].io_mutex = mutex;
    linux_abi->ofds[ofd].references = 1;
    linux_abi->ofds[ofd].generation++;
    linux_abi->fds[fd].ofd = ofd;
    linux_abi->fds[fd].descriptor_flags = descriptor_flags;
    linux_abi->fds[fd].generation++;
    *out_fd = fd;
done:
    hl_linux_unlock(linux_abi);
    if (status != HL_STATUS_OK) (void)sync->mutex_close(linux_abi->host->context, mutex);
    return status;
}

hl_status hl_linux_fd_install_at(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_host_handle host_handle,
                                 uint32_t status_flags, uint32_t descriptor_flags) {
    hl_linux_ofd ofd;
    hl_host_handle mutex;
    hl_host_result created;
    const hl_host_sync_services *sync;
    hl_status status;
    if (linux_abi == NULL || linux_abi->abi != HL_LINUX_ABI_VERSION || fd >= linux_abi->fd_capacity ||
        host_handle == HL_HOST_HANDLE_INVALID)
        return HL_STATUS_INVALID_ARGUMENT;
    sync = hl_linux_sync(linux_abi);
    if (sync == NULL || sync->mutex_create == NULL || sync->mutex_close == NULL) return HL_STATUS_NOT_SUPPORTED;
    created = sync->mutex_create(linux_abi->host->context);
    if (created.status != HL_STATUS_OK || created.value == HL_HOST_HANDLE_INVALID)
        return created.status == HL_STATUS_OK ? HL_STATUS_RESOURCE_LIMIT : (hl_status)created.status;
    mutex = created.value;
    hl_linux_lock(linux_abi);
    if (linux_abi->fds[fd].ofd != 0) {
        status = HL_STATUS_ALREADY_EXISTS;
        goto done;
    }
    status = hl_linux_find_ofd(linux_abi, &ofd);
    if (status != HL_STATUS_OK) goto done;
    linux_abi->ofds[ofd].host_handle = host_handle;
    linux_abi->ofds[ofd].status_flags = status_flags;
    linux_abi->ofds[ofd].io_mutex = mutex;
    linux_abi->ofds[ofd].references = 1;
    linux_abi->ofds[ofd].generation++;
    linux_abi->fds[fd].ofd = ofd;
    linux_abi->fds[fd].descriptor_flags = descriptor_flags;
    linux_abi->fds[fd].generation++;
done:
    hl_linux_unlock(linux_abi);
    if (status != HL_STATUS_OK) (void)sync->mutex_close(linux_abi->host->context, mutex);
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

static int64_t hl_linux_fd_dup_at_least(hl_linux_abi *linux_abi, hl_linux_fd source, hl_linux_fd minimum,
                                        uint32_t descriptor_flags) {
    const hl_linux_fd_entry *source_entry;
    hl_linux_fd fd;
    hl_status status;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, source, &source_entry, NULL);
    if (status != HL_STATUS_OK) {
        hl_linux_unlock(linux_abi);
        return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    }
    status = hl_linux_find_fd_at_least(linux_abi, minimum, &fd);
    if (status == HL_STATUS_OK) {
        linux_abi->fds[fd].ofd = source_entry->ofd;
        linux_abi->fds[fd].descriptor_flags = descriptor_flags;
        linux_abi->fds[fd].generation++;
        linux_abi->ofds[source_entry->ofd].references++;
    }
    hl_linux_unlock(linux_abi);
    return status == HL_STATUS_OK ? (int64_t)fd : -HL_LINUX_EMFILE;
}

/* Complete a final reference removal which already marked the OFD closing. */
static hl_status hl_linux_ofd_finalize(hl_linux_abi *linux_abi, hl_linux_ofd_entry *ofd_entry,
                                       hl_host_handle *last_host_handle) {
    const hl_host_sync_services *sync = hl_linux_sync(linux_abi);
    hl_host_handle host_handle;
    hl_host_handle mutex = ofd_entry->io_mutex;
    hl_host_result result;
    /* Wait only for this OFD. Operations drop their pin before releasing io_lock. */
    result = sync->mutex_lock(linux_abi->host->context, mutex);
    if (result.status != HL_STATUS_OK) return (hl_status)result.status;
    hl_linux_lock(linux_abi);
    if (ofd_entry->references != 0 || ofd_entry->active_operations != 0 || ofd_entry->closing == 0) {
        hl_linux_unlock(linux_abi);
        (void)sync->mutex_unlock(linux_abi->host->context, mutex);
        return HL_STATUS_CORRUPT;
    }
    host_handle = ofd_entry->host_handle;
    ofd_entry->host_handle = HL_HOST_HANDLE_INVALID;
    ofd_entry->offset = 0;
    ofd_entry->status_flags = 0;
    ofd_entry->kind = 0;
    hl_linux_unlock(linux_abi);
    result = sync->mutex_unlock(linux_abi->host->context, mutex);
    if (result.status != HL_STATUS_OK) return (hl_status)result.status;
    result = sync->mutex_close(linux_abi->host->context, mutex);
    if (result.status != HL_STATUS_OK) return (hl_status)result.status;
    hl_linux_lock(linux_abi);
    if (ofd_entry->references != 0 || ofd_entry->active_operations != 0 || ofd_entry->closing == 0 ||
        ofd_entry->io_mutex != mutex) {
        hl_linux_unlock(linux_abi);
        return HL_STATUS_CORRUPT;
    }
    ofd_entry->io_mutex = HL_HOST_HANDLE_INVALID;
    ofd_entry->closing = 0;
    ofd_entry->generation++;
    hl_linux_unlock(linux_abi);
    if (last_host_handle != NULL) *last_host_handle = host_handle;
    return HL_STATUS_OK;
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
    return final_reference ? hl_linux_ofd_finalize(linux_abi, ofd_entry, last_host_handle) : HL_STATUS_OK;
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
    hl_linux_ofd_lock(linux_abi, ofd);
    result = hl_linux_pread64_owned(linux_abi, ofd, buffer, size, offset);
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(linux_abi, ofd);
    return result;
}

int64_t hl_linux_read(hl_linux_abi *linux_abi, hl_linux_fd fd, void *buffer, size_t size) {
    const hl_host_file_services *files;
    const hl_linux_fd_entry *fd_entry;
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    hl_host_result host_result;
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
    ofd = &linux_abi->ofds[fd_entry->ofd];
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(linux_abi, ofd);
    files = hl_linux_files(linux_abi);
    if (size != 0 && buffer == NULL)
        result = -HL_LINUX_EINVAL;
    else if (files == NULL || files->read == NULL)
        result = -HL_LINUX_ENOSYS;
    else {
        host_result = files->read(linux_abi->host->context, ofd->host_handle, buffer, (uint64_t)size);
        result = host_result.status == HL_STATUS_OK ? (int64_t)host_result.value
                                                    : hl_linux_error((hl_status)host_result.status);
        if (host_result.status == HL_STATUS_OK && (host_result.value > size || host_result.value > INT64_MAX))
            result = -HL_LINUX_EIO;
    }
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(linux_abi, ofd);
    return result;
done:
    hl_linux_unlock(linux_abi);
    return result;
}

static int64_t hl_linux_write_owned(hl_linux_abi *linux_abi, hl_linux_ofd_entry *ofd, const void *buffer, size_t size,
                                    uint64_t offset, int append, uint64_t *resulting_offset) {
    const hl_host_file_services *files;
    hl_host_result result;
    if (size != 0 && buffer == NULL) return -HL_LINUX_EINVAL;
    files = hl_linux_files(linux_abi);
    if (files == NULL) return -HL_LINUX_ENOSYS;
    if (append) {
        if (files->append == NULL) return -HL_LINUX_ENOSYS;
        result = files->append(linux_abi->host->context, ofd->host_handle, (hl_host_const_bytes){buffer, size});
    } else {
        if (files->write_at == NULL) return -HL_LINUX_ENOSYS;
        result =
            files->write_at(linux_abi->host->context, ofd->host_handle, offset, (hl_host_const_bytes){buffer, size});
    }
    if (result.status != HL_STATUS_OK) return hl_linux_error((hl_status)result.status);
    if (result.value > size || result.value > (uint64_t)INT64_MAX) return -HL_LINUX_EIO;
    if (append) {
        if (result.detail < result.value || result.detail > (uint64_t)INT64_MAX) return -HL_LINUX_EIO;
        *resulting_offset = result.detail;
    }
    return (int64_t)result.value;
}

int64_t hl_linux_pwrite64(hl_linux_abi *linux_abi, hl_linux_fd fd, const void *buffer, size_t size, uint64_t offset) {
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    uint64_t ignored_offset = 0;
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
    if ((ofd->status_flags & HL_LINUX_O_ACCMODE) == HL_LINUX_O_RDONLY) {
        hl_linux_unlock(linux_abi);
        return -HL_LINUX_EBADF;
    }
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(linux_abi, ofd);
    result = hl_linux_write_owned(linux_abi, ofd, buffer, size, offset, 0, &ignored_offset);
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(linux_abi, ofd);
    return result;
}

int64_t hl_linux_write(hl_linux_abi *linux_abi, hl_linux_fd fd, const void *buffer, size_t size) {
    const hl_host_file_services *files;
    const hl_linux_fd_entry *fd_entry;
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    int append;
    int64_t result;
    hl_status status;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    if (size > (size_t)INT64_MAX) return -HL_LINUX_EINVAL;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, &fd_entry, &found);
    if (status != HL_STATUS_OK) {
        hl_linux_unlock(linux_abi);
        return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    }
    ofd = &linux_abi->ofds[fd_entry->ofd];
    if ((ofd->status_flags & HL_LINUX_O_ACCMODE) == HL_LINUX_O_RDONLY) {
        hl_linux_unlock(linux_abi);
        return -HL_LINUX_EBADF;
    }
    append = (ofd->status_flags & HL_LINUX_O_APPEND) != 0;
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(linux_abi, ofd);
    files = hl_linux_files(linux_abi);
    if (size != 0 && buffer == NULL)
        result = -HL_LINUX_EINVAL;
    else if (files == NULL)
        result = -HL_LINUX_ENOSYS;
    else {
        hl_host_result host_result;
        if (append)
            host_result =
                files->append(linux_abi->host->context, ofd->host_handle, (hl_host_const_bytes){buffer, size});
        else
            host_result = files->write(linux_abi->host->context, ofd->host_handle, buffer, (uint64_t)size);
        result = host_result.status == HL_STATUS_OK ? (int64_t)host_result.value
                                                    : hl_linux_error((hl_status)host_result.status);
        if (host_result.status == HL_STATUS_OK && (host_result.value > size || host_result.value > INT64_MAX))
            result = -HL_LINUX_EIO;
    }
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(linux_abi, ofd);
    return result;
}

int64_t hl_linux_openat(hl_linux_abi *linux_abi, int32_t directory_fd, const char *path, size_t path_size,
                        uint32_t flags, uint32_t mode) {
    const uint32_t supported = HL_LINUX_O_ACCMODE | HL_LINUX_O_CREAT | HL_LINUX_O_EXCL | HL_LINUX_O_TRUNC |
                               HL_LINUX_O_APPEND | HL_LINUX_O_DIRECTORY | HL_LINUX_O_CLOEXEC;
    const hl_host_file_services *files;
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *directory_ofd = NULL;
    hl_host_handle directory = HL_HOST_HANDLE_CWD;
    hl_host_result opened;
    hl_linux_fd installed;
    uint32_t access;
    uint32_t creation = 0;
    hl_status status;
    if (linux_abi == NULL || path == NULL || path_size == 0 || (flags & ~supported) != 0) return -HL_LINUX_EINVAL;
    switch (flags & HL_LINUX_O_ACCMODE) {
    case HL_LINUX_O_RDONLY: access = HL_HOST_FILE_READ; break;
    case HL_LINUX_O_WRONLY: access = HL_HOST_FILE_WRITE; break;
    case HL_LINUX_O_RDWR: access = HL_HOST_FILE_READ | HL_HOST_FILE_WRITE; break;
    default: return -HL_LINUX_EINVAL;
    }
    if ((flags & HL_LINUX_O_APPEND) != 0) access |= HL_HOST_FILE_APPEND;
    if ((flags & HL_LINUX_O_DIRECTORY) != 0) access |= HL_HOST_FILE_DIRECTORY;
    if ((flags & HL_LINUX_O_CREAT) != 0) creation |= HL_HOST_FILE_CREATE;
    if ((flags & HL_LINUX_O_EXCL) != 0) creation |= HL_HOST_FILE_EXCLUSIVE;
    if ((flags & HL_LINUX_O_TRUNC) != 0) creation |= HL_HOST_FILE_TRUNCATE;
    files = hl_linux_files(linux_abi);
    if (files == NULL || files->open_relative == NULL) return -HL_LINUX_ENOSYS;

    if (directory_fd != HL_LINUX_AT_FDCWD) {
        if (directory_fd < 0) return -HL_LINUX_EBADF;
        hl_linux_lock(linux_abi);
        status = hl_linux_fd_get_unlocked(linux_abi, (hl_linux_fd)directory_fd, NULL, &found);
        if (status != HL_STATUS_OK) {
            hl_linux_unlock(linux_abi);
            return -HL_LINUX_EBADF;
        }
        directory_ofd = &linux_abi->ofds[(size_t)(found - linux_abi->ofds)];
        directory_ofd->active_operations++;
        hl_linux_unlock(linux_abi);
        hl_linux_ofd_lock(linux_abi, directory_ofd);
        directory = directory_ofd->host_handle;
    }

    opened = files->open_relative(linux_abi->host->context, directory, path, path_size, access, creation, mode);
    if (directory_ofd != NULL) {
        hl_linux_lock(linux_abi);
        directory_ofd->active_operations--;
        hl_linux_unlock(linux_abi);
        hl_linux_ofd_unlock(linux_abi, directory_ofd);
    }
    if (opened.status != HL_STATUS_OK)
        return opened.status == HL_STATUS_NOT_FOUND ? -HL_LINUX_ENOENT : hl_linux_error((hl_status)opened.status);
    status = hl_linux_fd_install(linux_abi, opened.value, flags & ~(uint32_t)HL_LINUX_O_CLOEXEC,
                                 (flags & HL_LINUX_O_CLOEXEC) != 0 ? HL_LINUX_FD_CLOEXEC : 0, &installed);
    if (status != HL_STATUS_OK) {
        if (files->close != NULL) (void)files->close(linux_abi->host->context, opened.value);
        return hl_linux_error(status);
    }
    return (int64_t)installed;
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

int64_t hl_linux_dup(hl_linux_abi *linux_abi, hl_linux_fd fd) {
    return hl_linux_fd_dup_at_least(linux_abi, fd, 0, 0);
}

/*
 * Publish target's new OFD while holding table ownership. If target displaced
 * the final reference to another OFD, drain that OFD only after publication.
 */
static int64_t hl_linux_fd_replace(hl_linux_abi *linux_abi, hl_linux_fd source, hl_linux_fd target,
                                   uint32_t descriptor_flags, int reject_same) {
    const hl_linux_fd_entry *source_entry;
    hl_linux_ofd_entry *displaced = NULL;
    hl_linux_ofd source_ofd;
    hl_linux_ofd target_ofd;
    hl_host_handle displaced_handle = HL_HOST_HANDLE_INVALID;
    hl_status status;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    if (source == target && reject_same) return -HL_LINUX_EINVAL;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, source, &source_entry, NULL);
    if (status != HL_STATUS_OK || target >= linux_abi->fd_capacity) {
        hl_linux_unlock(linux_abi);
        return -HL_LINUX_EBADF;
    }
    if (source == target) {
        hl_linux_unlock(linux_abi);
        return (int64_t)target;
    }
    source_ofd = source_entry->ofd;
    target_ofd = linux_abi->fds[target].ofd;
    if (target_ofd != 0) {
        if (target_ofd >= linux_abi->ofd_capacity || linux_abi->ofds[target_ofd].references == 0) {
            hl_linux_unlock(linux_abi);
            return -HL_LINUX_EIO;
        }
        displaced = &linux_abi->ofds[target_ofd];
        displaced->references--;
        if (displaced->references == 0) displaced->closing = 1;
    }
    linux_abi->fds[target].ofd = source_ofd;
    linux_abi->fds[target].descriptor_flags = descriptor_flags;
    linux_abi->fds[target].generation++;
    linux_abi->ofds[source_ofd].references++;
    if (displaced != NULL && displaced->references != 0) displaced = NULL;
    hl_linux_unlock(linux_abi);

    if (displaced != NULL) {
        const hl_host_file_services *files;
        status = hl_linux_ofd_finalize(linux_abi, displaced, &displaced_handle);
        if (status != HL_STATUS_OK) return hl_linux_error(status);
        files = hl_linux_files(linux_abi);
        /* Linux dup2/dup3 intentionally discard errors from closing target. */
        if (files != NULL && files->close != NULL) (void)files->close(linux_abi->host->context, displaced_handle);
    }
    return (int64_t)target;
}

int64_t hl_linux_dup2(hl_linux_abi *linux_abi, hl_linux_fd source, hl_linux_fd target) {
    return hl_linux_fd_replace(linux_abi, source, target, 0, 0);
}

int64_t hl_linux_dup3(hl_linux_abi *linux_abi, hl_linux_fd source, hl_linux_fd target, uint32_t flags) {
    if ((flags & ~(uint32_t)HL_LINUX_O_CLOEXEC) != 0) return -HL_LINUX_EINVAL;
    return hl_linux_fd_replace(linux_abi, source, target, (flags & HL_LINUX_O_CLOEXEC) != 0 ? HL_LINUX_FD_CLOEXEC : 0,
                               1);
}

int64_t hl_linux_fcntl(hl_linux_abi *linux_abi, hl_linux_fd fd, int32_t command, uint64_t argument) {
    const hl_linux_fd_entry *fd_entry;
    const hl_linux_ofd_entry *ofd_entry;
    hl_status status;
    if (command == HL_LINUX_F_DUPFD || command == HL_LINUX_F_DUPFD_CLOEXEC) {
        if (linux_abi == NULL) return -HL_LINUX_EBADF;
        if (argument >= linux_abi->fd_capacity) return -HL_LINUX_EINVAL;
        return hl_linux_fd_dup_at_least(linux_abi, fd, (hl_linux_fd)argument,
                                        command == HL_LINUX_F_DUPFD_CLOEXEC ? HL_LINUX_FD_CLOEXEC : 0);
    }
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, &fd_entry, &ofd_entry);
    if (status != HL_STATUS_OK) {
        hl_linux_unlock(linux_abi);
        return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    }
    switch (command) {
    case HL_LINUX_F_GETFD: argument = fd_entry->descriptor_flags; break;
    case HL_LINUX_F_SETFD:
        linux_abi->fds[fd].descriptor_flags = (uint32_t)argument & HL_LINUX_FD_CLOEXEC;
        argument = 0;
        break;
    case HL_LINUX_F_GETFL: argument = ofd_entry->status_flags; break;
    default: hl_linux_unlock(linux_abi); return -HL_LINUX_EINVAL;
    }
    hl_linux_unlock(linux_abi);
    return (int64_t)argument;
}

static uint32_t hl_linux_mode_type(uint32_t host_type) {
    switch (host_type) {
    case HL_HOST_FILE_TYPE_REGULAR: return HL_LINUX_S_IFREG;
    case HL_HOST_FILE_TYPE_DIRECTORY: return HL_LINUX_S_IFDIR;
    case HL_HOST_FILE_TYPE_SYMLINK: return HL_LINUX_S_IFLNK;
    case HL_HOST_FILE_TYPE_CHARACTER: return HL_LINUX_S_IFCHR;
    case HL_HOST_FILE_TYPE_BLOCK: return HL_LINUX_S_IFBLK;
    case HL_HOST_FILE_TYPE_FIFO: return HL_LINUX_S_IFIFO;
    case HL_HOST_FILE_TYPE_SOCKET: return HL_LINUX_S_IFSOCK;
    default: return 0;
    }
}

static int64_t hl_linux_metadata_owned(hl_linux_abi *linux_abi, hl_linux_ofd_entry *ofd,
                                       hl_host_file_metadata *metadata) {
    const hl_host_file_services *files = hl_linux_files(linux_abi);
    hl_host_result result;
    if (files == NULL || files->metadata == NULL) return -HL_LINUX_ENOSYS;
    result = files->metadata(linux_abi->host->context, ofd->host_handle, metadata);
    return result.status == HL_STATUS_OK ? 0 : hl_linux_error((hl_status)result.status);
}

int64_t hl_linux_fstat(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_linux_file_status *output) {
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    hl_host_file_metadata metadata;
    hl_status status;
    int64_t result;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    if (output == NULL) return -HL_LINUX_EINVAL;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, NULL, &found);
    if (status != HL_STATUS_OK) {
        hl_linux_unlock(linux_abi);
        return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    }
    ofd = &linux_abi->ofds[(size_t)(found - linux_abi->ofds)];
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(linux_abi, ofd);
    result = hl_linux_metadata_owned(linux_abi, ofd, &metadata);
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(linux_abi, ofd);
    if (result != 0) return result;
    output->device = metadata.stable_device;
    output->object = metadata.stable_object;
    output->size = metadata.size;
    output->blocks_512 = metadata.allocated_size / 512u;
    output->modified_ns = metadata.modified_ns;
    output->mode = hl_linux_mode_type(metadata.type) | (metadata.permissions & 07777u);
    return 0;
}

int64_t hl_linux_lseek(hl_linux_abi *linux_abi, hl_linux_fd fd, int64_t offset, int32_t whence) {
    const hl_host_file_services *files;
    const hl_linux_ofd_entry *found;
    hl_linux_ofd_entry *ofd;
    hl_host_file_metadata metadata;
    hl_host_result host_result;
    hl_status status;
    int64_t result;
    if (linux_abi == NULL) return -HL_LINUX_EBADF;
    hl_linux_lock(linux_abi);
    status = hl_linux_fd_get_unlocked(linux_abi, fd, NULL, &found);
    if (status != HL_STATUS_OK) {
        hl_linux_unlock(linux_abi);
        return status == HL_STATUS_NOT_FOUND ? -HL_LINUX_EBADF : hl_linux_error(status);
    }
    ofd = &linux_abi->ofds[(size_t)(found - linux_abi->ofds)];
    ofd->active_operations++;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_lock(linux_abi, ofd);
    files = hl_linux_files(linux_abi);
    if (whence < HL_LINUX_SEEK_SET || whence > HL_LINUX_SEEK_END)
        result = -HL_LINUX_EINVAL;
    else if (files == NULL || files->seek == NULL)
        result = -HL_LINUX_ENOSYS;
    else if (hl_linux_metadata_owned(linux_abi, ofd, &metadata) != 0)
        result = -HL_LINUX_EIO;
    else if (metadata.type != HL_HOST_FILE_TYPE_REGULAR && metadata.type != HL_HOST_FILE_TYPE_BLOCK)
        result = -HL_LINUX_ESPIPE;
    else {
        host_result = files->seek(linux_abi->host->context, ofd->host_handle, offset, (uint32_t)whence);
        result = host_result.status == HL_STATUS_OK ? (int64_t)host_result.value
                                                    : hl_linux_error((hl_status)host_result.status);
        if (host_result.status == HL_STATUS_OK && host_result.value > INT64_MAX) result = -HL_LINUX_EOVERFLOW;
    }
    hl_linux_lock(linux_abi);
    ofd->active_operations--;
    hl_linux_unlock(linux_abi);
    hl_linux_ofd_unlock(linux_abi, ofd);
    return result;
}
