#define _GNU_SOURCE

#include "hl/host_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HL_LINUX_HANDLE_CAPACITY 4096u

typedef enum hl_linux_handle_kind {
    HL_LINUX_HANDLE_NONE = 0,
    HL_LINUX_HANDLE_MAPPING = 1,
    HL_LINUX_HANDLE_FILE = 2,
    HL_LINUX_HANDLE_SOCKET = 3,
    HL_LINUX_HANDLE_POLLSET = 4,
    HL_LINUX_HANDLE_SHARED_MEMORY = 5
} hl_linux_handle_kind;

typedef struct hl_linux_handle_entry {
    uint32_t generation;
    uint16_t kind;
    uint16_t reserved;
    int descriptor;
    void *address;
    void *executable_address;
    uint64_t size;
    int wake_descriptor;
} hl_linux_handle_entry;

struct hl_host_linux {
    pthread_mutex_t lock;
    hl_linux_handle_entry handles[HL_LINUX_HANDLE_CAPACITY];
};

static hl_host_result hl_linux_result(hl_status status, uint64_t value, uint64_t detail) {
    return (hl_host_result){status, 1, value, detail};
}

static hl_status hl_linux_status_from_errno(int error) {
    switch (error) {
    case 0: return HL_STATUS_OK;
    case EINVAL: return HL_STATUS_INVALID_ARGUMENT;
    case ENOMEM: return HL_STATUS_OUT_OF_MEMORY;
    case EMFILE:
    case ENFILE: return HL_STATUS_RESOURCE_LIMIT;
    case ENOENT: return HL_STATUS_NOT_FOUND;
    case EEXIST: return HL_STATUS_ALREADY_EXISTS;
    case EACCES:
    case EPERM: return HL_STATUS_PERMISSION_DENIED;
    case EAGAIN: return HL_STATUS_WOULD_BLOCK;
    case EINTR: return HL_STATUS_INTERRUPTED;
    case ENOTSUP:
    case ENOSYS: return HL_STATUS_NOT_SUPPORTED;
    case EBUSY: return HL_STATUS_BUSY;
    default: return HL_STATUS_IO;
    }
}

static hl_host_result hl_linux_errno_result(void) {
    const int error = errno;
    return hl_linux_result(hl_linux_status_from_errno(error), 0, (uint64_t)(unsigned int)error);
}

static hl_host_handle hl_linux_encode_handle(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static hl_linux_handle_entry *hl_linux_lookup_locked(hl_host_linux *host, hl_host_handle handle,
                                                     hl_linux_handle_kind kind) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    uint32_t generation = (uint32_t)(handle >> 32);
    hl_linux_handle_entry *entry;
    if (low == 0) return NULL;
    index = low - 1u;
    if (index >= HL_LINUX_HANDLE_CAPACITY) return NULL;
    entry = &host->handles[index];
    if (entry->generation != generation || entry->kind != kind) return NULL;
    return entry;
}

static hl_host_result hl_linux_allocate_handle(hl_host_linux *host, hl_linux_handle_kind kind, int descriptor,
                                               void *address, void *executable_address, uint64_t size,
                                               int wake_descriptor) {
    uint32_t index;
    hl_host_handle handle = 0;
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_LINUX_HANDLE_CAPACITY; ++index) {
        hl_linux_handle_entry *entry = &host->handles[index];
        if (entry->kind == HL_LINUX_HANDLE_NONE) {
            entry->generation++;
            if (entry->generation == 0) entry->generation = 1;
            entry->kind = (uint16_t)kind;
            entry->descriptor = descriptor;
            entry->address = address;
            entry->executable_address = executable_address;
            entry->size = size;
            entry->wake_descriptor = wake_descriptor;
            handle = hl_linux_encode_handle(index, entry->generation);
            break;
        }
    }
    pthread_mutex_unlock(&host->lock);
    if (handle == 0) return hl_linux_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
    return hl_linux_result(HL_STATUS_OK, handle, 0);
}

static int hl_linux_protection(uint32_t flags) {
    int protection = 0;
    if ((flags & HL_HOST_MEMORY_READ) != 0) protection |= PROT_READ;
    if ((flags & HL_HOST_MEMORY_WRITE) != 0) protection |= PROT_WRITE;
    if ((flags & HL_HOST_MEMORY_EXECUTE) != 0) protection |= PROT_EXEC;
    return protection;
}

static hl_host_result hl_linux_memory_reserve(void *context, uint64_t size, uint64_t alignment, uint32_t flags) {
    hl_host_linux *host = context;
    void *address;
    long page = sysconf(_SC_PAGESIZE);
    if (size == 0 || size > SIZE_MAX || page <= 0 || alignment > (uint64_t)page)
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    address = mmap(NULL, (size_t)size, hl_linux_protection(flags), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) return hl_linux_errno_result();
    hl_host_result result = hl_linux_allocate_handle(host, HL_LINUX_HANDLE_MAPPING, -1, address, NULL, size, -1);
    if (result.status != HL_STATUS_OK) munmap(address, (size_t)size);
    return result;
}

static hl_host_result hl_linux_memory_protect(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size,
                                              uint32_t flags) {
    hl_host_linux *host = context;
    hl_linux_handle_entry *entry;
    int result;
    pthread_mutex_lock(&host->lock);
    entry = hl_linux_lookup_locked(host, mapping, HL_LINUX_HANDLE_MAPPING);
    if (entry == NULL || offset > entry->size || size > entry->size - offset) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = mprotect((char *)entry->address + offset, (size_t)size, hl_linux_protection(flags));
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_memory_release(void *context, hl_host_handle mapping) {
    hl_host_linux *host = context;
    hl_linux_handle_entry *entry;
    int result;
    pthread_mutex_lock(&host->lock);
    entry = hl_linux_lookup_locked(host, mapping, HL_LINUX_HANDLE_MAPPING);
    if (entry == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = munmap(entry->address, (size_t)entry->size);
    if (result == 0 && entry->executable_address != NULL && entry->executable_address != entry->address)
        result = munmap(entry->executable_address, (size_t)entry->size);
    if (result == 0 && entry->descriptor >= 0) result = close(entry->descriptor);
    if (result == 0) {
        entry->kind = HL_LINUX_HANDLE_NONE;
        entry->address = NULL;
        entry->executable_address = NULL;
        entry->size = 0;
        entry->descriptor = -1;
    }
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_memory_publish(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size) {
    hl_host_linux *host = context;
    hl_linux_handle_entry *entry;
    pthread_mutex_lock(&host->lock);
    entry = hl_linux_lookup_locked(host, mapping, HL_LINUX_HANDLE_MAPPING);
    if (entry == NULL || offset > entry->size || size > entry->size - offset) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    char *address = entry->executable_address != NULL ? entry->executable_address : entry->address;
    __builtin___clear_cache(address + offset, address + offset + size);
    pthread_mutex_unlock(&host->lock);
    return hl_linux_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_linux_memory_reserve_code(void *context, uint64_t size, uint64_t alignment, uint32_t flags,
                                                   hl_host_code_mapping *output) {
    hl_host_linux *host = context;
    long page = sysconf(_SC_PAGESIZE);
    int descriptor;
    void *writable;
    void *executable;
    hl_host_result handle;
    if (output == NULL || size == 0 || size > SIZE_MAX || size > INT64_MAX || page <= 0 || alignment > (uint64_t)page)
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memset(output, 0, sizeof(*output));
    if ((flags & HL_HOST_CODE_DUAL_ALIAS) == 0) {
        writable = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (writable == MAP_FAILED) return hl_linux_errno_result();
        handle = hl_linux_allocate_handle(host, HL_LINUX_HANDLE_MAPPING, -1, writable, writable, size, -1);
        if (handle.status != HL_STATUS_OK) {
            munmap(writable, (size_t)size);
            return handle;
        }
        output->abi = 1;
        output->size = sizeof(*output);
        output->handle = handle.value;
        output->writable_address = (uint64_t)(uintptr_t)writable;
        output->executable_address = (uint64_t)(uintptr_t)writable;
        output->mapped_size = size;
        return handle;
    }
    descriptor = memfd_create("hl-code", MFD_CLOEXEC);
    if (descriptor < 0) return hl_linux_errno_result();
    if (ftruncate(descriptor, (off_t)size) != 0) {
        close(descriptor);
        return hl_linux_errno_result();
    }
    writable = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (writable == MAP_FAILED) {
        close(descriptor);
        return hl_linux_errno_result();
    }
    executable = mmap(NULL, (size_t)size, PROT_READ | PROT_EXEC, MAP_SHARED, descriptor, 0);
    if (executable == MAP_FAILED) {
        munmap(writable, (size_t)size);
        close(descriptor);
        return hl_linux_errno_result();
    }
    handle = hl_linux_allocate_handle(host, HL_LINUX_HANDLE_MAPPING, descriptor, writable, executable, size, -1);
    if (handle.status != HL_STATUS_OK) {
        munmap(executable, (size_t)size);
        munmap(writable, (size_t)size);
        close(descriptor);
        return handle;
    }
    output->abi = 1;
    output->size = sizeof(*output);
    output->handle = handle.value;
    output->writable_address = (uint64_t)(uintptr_t)writable;
    output->executable_address = (uint64_t)(uintptr_t)executable;
    output->mapped_size = size;
    return hl_linux_result(HL_STATUS_OK, handle.value, 0);
}

static hl_host_result hl_linux_memory_repair_code(void *context, hl_host_code_mapping *mapping, uint32_t preserve) {
    hl_host_linux *host = context;
    hl_linux_handle_entry *entry;
    (void)preserve;
    if (mapping == NULL || mapping->abi != 1 || mapping->size < sizeof(*mapping))
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    entry = hl_linux_lookup_locked(host, mapping->handle, HL_LINUX_HANDLE_MAPPING);
    if (entry == NULL || entry->executable_address == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    mapping->writable_address = (uint64_t)(uintptr_t)entry->address;
    mapping->executable_address = (uint64_t)(uintptr_t)entry->executable_address;
    mapping->mapped_size = entry->size;
    pthread_mutex_unlock(&host->lock);
    return hl_linux_result(HL_STATUS_OK, mapping->handle, 0);
}

static hl_host_result hl_linux_clock(int clock_id) {
    struct timespec time;
    if (clock_gettime(clock_id, &time) != 0) return hl_linux_errno_result();
    return hl_linux_result(HL_STATUS_OK, (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec, 0);
}

static hl_host_result hl_linux_monotonic(void *context) {
    (void)context;
    return hl_linux_clock(CLOCK_MONOTONIC);
}

static hl_host_result hl_linux_realtime(void *context) {
    (void)context;
    return hl_linux_clock(CLOCK_REALTIME);
}

static int hl_linux_descriptor(hl_host_linux *host, hl_host_handle handle, hl_linux_handle_kind first,
                               hl_linux_handle_kind second) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    hl_linux_handle_entry *entry;
    if (handle == HL_HOST_HANDLE_CWD) return AT_FDCWD;
    if (low == 0) return -1;
    index = low - 1u;
    if (index >= HL_LINUX_HANDLE_CAPACITY) return -1;
    entry = &host->handles[index];
    if (entry->generation != (uint32_t)(handle >> 32) || (entry->kind != first && entry->kind != second)) return -1;
    return entry->descriptor;
}

static hl_host_result hl_linux_file_open(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                         uint32_t access, uint32_t creation) {
    hl_host_linux *host = context;
    char local[PATH_MAX];
    int directory_fd;
    int descriptor;
    if (path == NULL || path_size == 0 || path_size >= sizeof(local))
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memcpy(local, path, path_size);
    local[path_size] = '\0';
    pthread_mutex_lock(&host->lock);
    directory_fd = hl_linux_descriptor(host, directory, HL_LINUX_HANDLE_FILE, HL_LINUX_HANDLE_FILE);
    pthread_mutex_unlock(&host->lock);
    if (directory_fd < 0 && directory != HL_HOST_HANDLE_CWD) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    int flags;
    if ((access & HL_HOST_FILE_READ) != 0 && (access & HL_HOST_FILE_WRITE) != 0)
        flags = O_RDWR;
    else if ((access & HL_HOST_FILE_WRITE) != 0)
        flags = O_WRONLY;
    else if ((access & HL_HOST_FILE_READ) != 0)
        flags = O_RDONLY;
    else
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if ((access & HL_HOST_FILE_APPEND) != 0) flags |= O_APPEND;
    if ((access & HL_HOST_FILE_DIRECTORY) != 0) flags |= O_DIRECTORY;
    if ((creation & HL_HOST_FILE_CREATE) != 0) flags |= O_CREAT;
    if ((creation & HL_HOST_FILE_EXCLUSIVE) != 0) flags |= O_EXCL;
    if ((creation & HL_HOST_FILE_TRUNCATE) != 0) flags |= O_TRUNC;
    descriptor = openat(directory_fd, local, flags | O_CLOEXEC, 0600);
    if (descriptor < 0) return hl_linux_errno_result();
    hl_host_result result = hl_linux_allocate_handle(host, HL_LINUX_HANDLE_FILE, descriptor, NULL, NULL, 0, -1);
    if (result.status != HL_STATUS_OK) close(descriptor);
    return result;
}

static hl_host_result hl_linux_file_read(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output) {
    hl_host_linux *host = context;
    int descriptor;
    ssize_t count;
    if (output.size != 0 && output.data == NULL) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    descriptor = hl_linux_descriptor(host, file, HL_LINUX_HANDLE_FILE, HL_LINUX_HANDLE_SHARED_MEMORY);
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0 || offset > INT64_MAX) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = pread(descriptor, output.data, output.size, (off_t)offset);
    return count >= 0 ? hl_linux_result(HL_STATUS_OK, (uint64_t)count, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_file_write(void *context, hl_host_handle file, uint64_t offset,
                                          hl_host_const_bytes input) {
    hl_host_linux *host = context;
    int descriptor;
    ssize_t count;
    if (input.size != 0 && input.data == NULL) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    descriptor = hl_linux_descriptor(host, file, HL_LINUX_HANDLE_FILE, HL_LINUX_HANDLE_SHARED_MEMORY);
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0 || offset > INT64_MAX) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = pwrite(descriptor, input.data, input.size, (off_t)offset);
    return count >= 0 ? hl_linux_result(HL_STATUS_OK, (uint64_t)count, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_file_metadata_get(void *context, hl_host_handle file, hl_host_file_metadata *output) {
    hl_host_linux *host = context;
    struct stat status;
    int descriptor;
    if (output == NULL) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    descriptor = hl_linux_descriptor(host, file, HL_LINUX_HANDLE_FILE, HL_LINUX_HANDLE_SHARED_MEMORY);
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (fstat(descriptor, &status) != 0) return hl_linux_errno_result();
    memset(output, 0, sizeof(*output));
    output->stable_device = (uint64_t)status.st_dev;
    output->stable_object = (uint64_t)status.st_ino;
    output->size = (uint64_t)status.st_size;
    output->allocated_size = (uint64_t)status.st_blocks * 512u;
    output->modified_ns = (uint64_t)status.st_mtim.tv_sec * UINT64_C(1000000000) + (uint64_t)status.st_mtim.tv_nsec;
    output->permissions = (uint32_t)status.st_mode & 07777u;
    output->type = (uint32_t)status.st_mode & S_IFMT;
    return hl_linux_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_linux_close_descriptor(void *context, hl_host_handle handle) {
    hl_host_linux *host = context;
    uint32_t low = (uint32_t)handle;
    hl_linux_handle_entry *entry;
    int descriptor;
    int wake_descriptor;
    int result;
    if (low == 0 || low - 1u >= HL_LINUX_HANDLE_CAPACITY) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    entry = &host->handles[low - 1u];
    if (entry->generation != (uint32_t)(handle >> 32) || entry->kind == HL_LINUX_HANDLE_NONE ||
        entry->kind == HL_LINUX_HANDLE_MAPPING) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    descriptor = entry->descriptor;
    wake_descriptor = entry->wake_descriptor;
    entry->kind = HL_LINUX_HANDLE_NONE;
    entry->descriptor = -1;
    entry->wake_descriptor = -1;
    pthread_mutex_unlock(&host->lock);
    result = close(descriptor);
    if (wake_descriptor >= 0) close(wake_descriptor);
    return result == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_event_create(void *context) {
    hl_host_linux *host = context;
    struct epoll_event event = {0};
    int pollset = epoll_create1(EPOLL_CLOEXEC);
    int wake;
    if (pollset < 0) return hl_linux_errno_result();
    wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake < 0) {
        close(pollset);
        return hl_linux_errno_result();
    }
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (epoll_ctl(pollset, EPOLL_CTL_ADD, wake, &event) != 0) {
        close(wake);
        close(pollset);
        return hl_linux_errno_result();
    }
    hl_host_result result = hl_linux_allocate_handle(host, HL_LINUX_HANDLE_POLLSET, pollset, NULL, NULL, 0, wake);
    if (result.status != HL_STATUS_OK) {
        close(wake);
        close(pollset);
    }
    return result;
}

static uint32_t hl_linux_epoll_events(uint32_t interests) {
    uint32_t events = 0;
    if ((interests & HL_HOST_READY_READ) != 0) events |= EPOLLIN;
    if ((interests & HL_HOST_READY_WRITE) != 0) events |= EPOLLOUT;
    if ((interests & HL_HOST_READY_EDGE) != 0) events |= EPOLLET;
    if ((interests & HL_HOST_READY_ONESHOT) != 0) events |= EPOLLONESHOT;
    return events;
}

static hl_host_result hl_linux_event_control(void *context, hl_host_handle pollset, uint32_t operation,
                                             hl_host_handle object, uint64_t token, uint32_t interests) {
    hl_host_linux *host = context;
    struct epoll_event event = {hl_linux_epoll_events(interests), {.u64 = token}};
    int pollset_fd;
    int object_fd;
    int native_operation;
    pthread_mutex_lock(&host->lock);
    pollset_fd = hl_linux_descriptor(host, pollset, HL_LINUX_HANDLE_POLLSET, HL_LINUX_HANDLE_POLLSET);
    object_fd = hl_linux_descriptor(host, object, HL_LINUX_HANDLE_FILE, HL_LINUX_HANDLE_SOCKET);
    pthread_mutex_unlock(&host->lock);
    if (pollset_fd < 0 || object_fd < 0) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (token == 0) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (operation == HL_HOST_EVENT_ADD)
        native_operation = EPOLL_CTL_ADD;
    else if (operation == HL_HOST_EVENT_MODIFY)
        native_operation = EPOLL_CTL_MOD;
    else if (operation == HL_HOST_EVENT_DELETE)
        native_operation = EPOLL_CTL_DEL;
    else
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return epoll_ctl(pollset_fd, native_operation, object_fd, &event) == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0)
                                                                           : hl_linux_errno_result();
}

static hl_host_result hl_linux_event_wait(void *context, hl_host_handle pollset, hl_host_event_record *events,
                                          size_t event_capacity, uint64_t deadline_ns) {
    hl_host_linux *host = context;
    struct epoll_event native_events[64];
    int pollset_fd;
    int wake_descriptor;
    int timeout;
    int count;
    size_t i;
    if (events == NULL || event_capacity == 0) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (event_capacity > HL_ARRAY_COUNT(native_events)) event_capacity = HL_ARRAY_COUNT(native_events);
    pthread_mutex_lock(&host->lock);
    hl_linux_handle_entry *pollset_entry = hl_linux_lookup_locked(host, pollset, HL_LINUX_HANDLE_POLLSET);
    pollset_fd = pollset_entry == NULL ? -1 : pollset_entry->descriptor;
    wake_descriptor = pollset_entry == NULL ? -1 : pollset_entry->wake_descriptor;
    pthread_mutex_unlock(&host->lock);
    if (pollset_fd < 0) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (deadline_ns == UINT64_MAX)
        timeout = -1;
    else if (deadline_ns / UINT64_C(1000000) > INT_MAX)
        timeout = INT_MAX;
    else
        timeout = (int)(deadline_ns / UINT64_C(1000000));
    count = epoll_wait(pollset_fd, native_events, (int)event_capacity, timeout);
    if (count < 0) return hl_linux_errno_result();
    size_t output_count = 0;
    for (i = 0; i < (size_t)count; ++i) {
        uint32_t ready = 0;
        if (native_events[i].data.u64 == 0) {
            uint64_t ignored;
            while (read(wake_descriptor, &ignored, sizeof(ignored)) == (ssize_t)sizeof(ignored)) {}
            continue;
        }
        if ((native_events[i].events & EPOLLIN) != 0) ready |= HL_HOST_READY_READ;
        if ((native_events[i].events & EPOLLOUT) != 0) ready |= HL_HOST_READY_WRITE;
        if ((native_events[i].events & EPOLLERR) != 0) ready |= HL_HOST_READY_ERROR;
        if ((native_events[i].events & EPOLLHUP) != 0) ready |= HL_HOST_READY_HANGUP;
        events[output_count++] = (hl_host_event_record){native_events[i].data.u64, ready, 0};
    }
    return hl_linux_result(HL_STATUS_OK, output_count, 0);
}

static hl_host_result hl_linux_event_wake(void *context, hl_host_handle pollset) {
    hl_host_linux *host = context;
    hl_linux_handle_entry *entry;
    uint64_t one = 1;
    ssize_t result;
    pthread_mutex_lock(&host->lock);
    entry = hl_linux_lookup_locked(host, pollset, HL_LINUX_HANDLE_POLLSET);
    if (entry == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = write(entry->wake_descriptor, &one, sizeof(one));
    pthread_mutex_unlock(&host->lock);
    return result == (ssize_t)sizeof(one) ? hl_linux_result(HL_STATUS_OK, 0, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_network_socket(void *context, uint32_t family, uint32_t type, uint32_t protocol) {
    hl_host_linux *host = context;
    int native_family;
    int native_type;
    int descriptor;
    if (family == HL_HOST_NETWORK_IPV4)
        native_family = AF_INET;
    else if (family == HL_HOST_NETWORK_IPV6)
        native_family = AF_INET6;
    else if (family == HL_HOST_NETWORK_LOCAL)
        native_family = AF_UNIX;
    else
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (type == HL_HOST_NETWORK_STREAM)
        native_type = SOCK_STREAM;
    else if (type == HL_HOST_NETWORK_DATAGRAM)
        native_type = SOCK_DGRAM;
    else
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    descriptor = socket(native_family, native_type | SOCK_CLOEXEC, (int)protocol);
    if (descriptor < 0) return hl_linux_errno_result();
    hl_host_result result = hl_linux_allocate_handle(host, HL_LINUX_HANDLE_SOCKET, descriptor, NULL, NULL, 0, -1);
    if (result.status != HL_STATUS_OK) close(descriptor);
    return result;
}

static int hl_linux_socket_descriptor(hl_host_linux *host, hl_host_handle socket_handle) {
    int descriptor;
    pthread_mutex_lock(&host->lock);
    descriptor = hl_linux_descriptor(host, socket_handle, HL_LINUX_HANDLE_SOCKET, HL_LINUX_HANDLE_SOCKET);
    pthread_mutex_unlock(&host->lock);
    return descriptor;
}

static hl_status hl_linux_network_address(const hl_host_network_address *address, struct sockaddr_storage *storage,
                                          socklen_t *size) {
    memset(storage, 0, sizeof(*storage));
    if (address == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (address->family == HL_HOST_NETWORK_IPV4 && address->size == 4) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)storage;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(address->port);
        memcpy(&ipv4->sin_addr, address->address, 4);
        *size = sizeof(*ipv4);
        return HL_STATUS_OK;
    }
    if (address->family == HL_HOST_NETWORK_IPV6 && address->size == 16) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)storage;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(address->port);
        memcpy(&ipv6->sin6_addr, address->address, 16);
        *size = sizeof(*ipv6);
        return HL_STATUS_OK;
    }
    if (address->family == HL_HOST_NETWORK_LOCAL && address->size > 0 && address->size < sizeof(address->local_path)) {
        struct sockaddr_un *local = (struct sockaddr_un *)storage;
        local->sun_family = AF_UNIX;
        memcpy(local->sun_path, address->local_path, address->size);
        local->sun_path[address->size] = '\0';
        *size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + address->size + 1u);
        return HL_STATUS_OK;
    }
    return HL_STATUS_INVALID_ARGUMENT;
}

static hl_host_result hl_linux_network_bind(void *context, hl_host_handle socket_handle,
                                            const hl_host_network_address *address) {
    int descriptor = hl_linux_socket_descriptor(context, socket_handle);
    struct sockaddr_storage storage;
    socklen_t size;
    hl_status status = hl_linux_network_address(address, &storage, &size);
    if (descriptor < 0 || status != HL_STATUS_OK) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return bind(descriptor, (const struct sockaddr *)&storage, size) == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0)
                                                                          : hl_linux_errno_result();
}

static hl_host_result hl_linux_network_connect(void *context, hl_host_handle socket_handle,
                                               const hl_host_network_address *address) {
    int descriptor = hl_linux_socket_descriptor(context, socket_handle);
    struct sockaddr_storage storage;
    socklen_t size;
    hl_status status = hl_linux_network_address(address, &storage, &size);
    if (descriptor < 0 || status != HL_STATUS_OK) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return connect(descriptor, (const struct sockaddr *)&storage, size) == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0)
                                                                             : hl_linux_errno_result();
}

static hl_host_result hl_linux_network_send(void *context, hl_host_handle socket_handle, hl_host_const_bytes data,
                                            uint32_t flags) {
    int descriptor = hl_linux_socket_descriptor(context, socket_handle);
    ssize_t count;
    if (descriptor < 0 || (data.size != 0 && data.data == NULL))
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = send(descriptor, data.data, data.size, (int)flags);
    return count >= 0 ? hl_linux_result(HL_STATUS_OK, (uint64_t)count, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_network_receive(void *context, hl_host_handle socket_handle, hl_host_bytes data,
                                               uint32_t flags) {
    int descriptor = hl_linux_socket_descriptor(context, socket_handle);
    ssize_t count;
    if (descriptor < 0 || (data.size != 0 && data.data == NULL))
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = recv(descriptor, data.data, data.size, (int)flags);
    return count >= 0 ? hl_linux_result(HL_STATUS_OK, (uint64_t)count, 0) : hl_linux_errno_result();
}

static hl_host_result hl_linux_shared_create(void *context, uint64_t size, uint32_t flags) {
    hl_host_linux *host = context;
    int descriptor;
    (void)flags;
    if (size > INT64_MAX) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    descriptor = memfd_create("hl-engine", MFD_CLOEXEC);
    if (descriptor < 0) return hl_linux_errno_result();
    if (ftruncate(descriptor, (off_t)size) != 0) {
        close(descriptor);
        return hl_linux_errno_result();
    }
    hl_host_result result =
        hl_linux_allocate_handle(host, HL_LINUX_HANDLE_SHARED_MEMORY, descriptor, NULL, NULL, size, -1);
    if (result.status != HL_STATUS_OK) close(descriptor);
    return result;
}

static hl_host_result hl_linux_shared_open(void *context, uint64_t identity, uint32_t flags) {
    (void)context;
    (void)identity;
    (void)flags;
    return hl_linux_result(HL_STATUS_NOT_SUPPORTED, 0, 0);
}

static hl_host_result hl_linux_shared_resize(void *context, hl_host_handle object, uint64_t size) {
    hl_host_linux *host = context;
    hl_linux_handle_entry *entry;
    int result;
    if (size > INT64_MAX) return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    entry = hl_linux_lookup_locked(host, object, HL_LINUX_HANDLE_SHARED_MEMORY);
    if (entry == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_linux_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = ftruncate(entry->descriptor, (off_t)size);
    if (result == 0) entry->size = size;
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_linux_result(HL_STATUS_OK, 0, 0) : hl_linux_errno_result();
}

hl_status hl_host_linux_create(hl_host_linux **out_host, hl_host_services *out_services) {
    static const hl_host_memory_services memory = {
        HL_HOST_MEMORY_ABI,      sizeof(memory),          hl_linux_memory_reserve,      hl_linux_memory_protect,
        hl_linux_memory_release, hl_linux_memory_publish, hl_linux_memory_reserve_code, hl_linux_memory_repair_code};
    static const hl_host_clock_services clock = {HL_HOST_CLOCK_ABI, sizeof(clock), hl_linux_monotonic,
                                                 hl_linux_realtime};
    static const hl_host_file_services file = {HL_HOST_FILE_ABI,         sizeof(file),
                                               hl_linux_file_open,       hl_linux_file_read,
                                               hl_linux_file_write,      hl_linux_file_metadata_get,
                                               hl_linux_close_descriptor};
    static const hl_host_event_services event = {HL_HOST_EVENT_ABI,        sizeof(event),       hl_linux_event_create,
                                                 hl_linux_event_control,   hl_linux_event_wait, hl_linux_event_wake,
                                                 hl_linux_close_descriptor};
    static const hl_host_network_services network = {
        HL_HOST_NETWORK_ABI,      sizeof(network),       hl_linux_network_socket,  hl_linux_network_bind,
        hl_linux_network_connect, hl_linux_network_send, hl_linux_network_receive, hl_linux_close_descriptor};
    static const hl_host_shared_memory_services shared_memory = {HL_HOST_SHARED_MEMORY_ABI, sizeof(shared_memory),
                                                                 hl_linux_shared_create,    hl_linux_shared_open,
                                                                 hl_linux_shared_resize,    hl_linux_close_descriptor};
    hl_host_linux *host;
    uint32_t i;
    if (out_host == NULL || out_services == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_host = NULL;
    memset(out_services, 0, sizeof(*out_services));
    host = calloc(1, sizeof(*host));
    if (host == NULL) return HL_STATUS_OUT_OF_MEMORY;
    if (pthread_mutex_init(&host->lock, NULL) != 0) {
        free(host);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    for (i = 0; i < HL_LINUX_HANDLE_CAPACITY; ++i) {
        host->handles[i].descriptor = -1;
        host->handles[i].wake_descriptor = -1;
    }
    out_services->abi = HL_HOST_SERVICES_ABI;
    out_services->size = sizeof(*out_services);
    out_services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_FILE | HL_HOST_CAP_EVENT |
                                 HL_HOST_CAP_NETWORK | HL_HOST_CAP_SHARED_MEMORY | HL_HOST_CAP_CODE_MAPPING;
    out_services->context = host;
    out_services->memory = &memory;
    out_services->clock = &clock;
    out_services->file = &file;
    out_services->event = &event;
    out_services->network = &network;
    out_services->shared_memory = &shared_memory;
    *out_host = host;
    return HL_STATUS_OK;
}

void hl_host_linux_destroy(hl_host_linux *host) {
    uint32_t i;
    if (host == NULL) return;
    for (i = 0; i < HL_LINUX_HANDLE_CAPACITY; ++i) {
        hl_linux_handle_entry *entry = &host->handles[i];
        if (entry->kind == HL_LINUX_HANDLE_MAPPING) {
            munmap(entry->address, (size_t)entry->size);
            if (entry->executable_address != NULL && entry->executable_address != entry->address)
                munmap(entry->executable_address, (size_t)entry->size);
            if (entry->descriptor >= 0) close(entry->descriptor);
        } else if (entry->kind != HL_LINUX_HANDLE_NONE) {
            if (entry->descriptor >= 0) close(entry->descriptor);
            if (entry->wake_descriptor >= 0) close(entry->wake_descriptor);
        }
    }
    pthread_mutex_destroy(&host->lock);
    free(host);
}
