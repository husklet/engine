#include "hl/linux_abi.h"

#include <string.h>

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
        if (linux_abi->ofds[ofd].references == 0) {
            *out_ofd = ofd;
            return HL_STATUS_OK;
        }
    }
    return HL_STATUS_RESOURCE_LIMIT;
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
    return HL_STATUS_OK;
}

hl_status hl_linux_fd_install(hl_linux_abi *linux_abi, hl_host_handle host_handle, uint32_t status_flags,
                              uint32_t descriptor_flags, hl_linux_fd *out_fd) {
    hl_linux_fd fd;
    hl_linux_ofd ofd;
    hl_status status;
    if (linux_abi == NULL || host_handle == HL_HOST_HANDLE_INVALID || out_fd == NULL) return HL_STATUS_INVALID_ARGUMENT;
    status = hl_linux_find_fd(linux_abi, &fd);
    if (status != HL_STATUS_OK) return status;
    status = hl_linux_find_ofd(linux_abi, &ofd);
    if (status != HL_STATUS_OK) return status;
    linux_abi->ofds[ofd].host_handle = host_handle;
    linux_abi->ofds[ofd].status_flags = status_flags;
    linux_abi->ofds[ofd].references = 1;
    linux_abi->ofds[ofd].generation++;
    linux_abi->fds[fd].ofd = ofd;
    linux_abi->fds[fd].descriptor_flags = descriptor_flags;
    linux_abi->fds[fd].generation++;
    *out_fd = fd;
    return HL_STATUS_OK;
}

hl_status hl_linux_fd_get(const hl_linux_abi *linux_abi, hl_linux_fd fd, const hl_linux_fd_entry **fd_entry,
                          const hl_linux_ofd_entry **ofd_entry) {
    hl_linux_ofd ofd;
    if (linux_abi == NULL || fd >= linux_abi->fd_capacity || linux_abi->fds[fd].ofd == 0) return HL_STATUS_NOT_FOUND;
    ofd = linux_abi->fds[fd].ofd;
    if (ofd >= linux_abi->ofd_capacity || linux_abi->ofds[ofd].references == 0) return HL_STATUS_CORRUPT;
    if (fd_entry != NULL) *fd_entry = &linux_abi->fds[fd];
    if (ofd_entry != NULL) *ofd_entry = &linux_abi->ofds[ofd];
    return HL_STATUS_OK;
}

hl_status hl_linux_fd_dup(hl_linux_abi *linux_abi, hl_linux_fd source, uint32_t descriptor_flags, hl_linux_fd *out_fd) {
    const hl_linux_fd_entry *source_entry;
    hl_linux_fd fd;
    hl_status status;
    if (out_fd == NULL) return HL_STATUS_INVALID_ARGUMENT;
    status = hl_linux_fd_get(linux_abi, source, &source_entry, NULL);
    if (status != HL_STATUS_OK) return status;
    status = hl_linux_find_fd(linux_abi, &fd);
    if (status != HL_STATUS_OK) return status;
    linux_abi->fds[fd].ofd = source_entry->ofd;
    linux_abi->fds[fd].descriptor_flags = descriptor_flags;
    linux_abi->fds[fd].generation++;
    linux_abi->ofds[source_entry->ofd].references++;
    *out_fd = fd;
    return HL_STATUS_OK;
}

hl_status hl_linux_fd_close(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_host_handle *last_host_handle) {
    hl_linux_ofd ofd;
    hl_linux_ofd_entry *ofd_entry;
    if (last_host_handle != NULL) *last_host_handle = HL_HOST_HANDLE_INVALID;
    if (linux_abi == NULL || fd >= linux_abi->fd_capacity || linux_abi->fds[fd].ofd == 0) return HL_STATUS_NOT_FOUND;
    ofd = linux_abi->fds[fd].ofd;
    if (ofd >= linux_abi->ofd_capacity) return HL_STATUS_CORRUPT;
    ofd_entry = &linux_abi->ofds[ofd];
    if (ofd_entry->references == 0) return HL_STATUS_CORRUPT;
    memset(&linux_abi->fds[fd], 0, sizeof(linux_abi->fds[fd]));
    ofd_entry->references--;
    if (ofd_entry->references == 0) {
        if (last_host_handle != NULL) *last_host_handle = ofd_entry->host_handle;
        memset(ofd_entry, 0, sizeof(*ofd_entry));
    }
    return HL_STATUS_OK;
}
