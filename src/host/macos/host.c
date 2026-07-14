#define _DARWIN_C_SOURCE

#include "hl/macos.h"
#include "../sync.h"

#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HL_MACOS_MAPPING_CAPACITY 4096u
#define HL_MACOS_FILE_CAPACITY 1024u
#define HL_MACOS_PROCESS_CAPACITY 1024u

typedef struct hl_macos_mapping {
    uint32_t generation;
    uint32_t active;
    void *writable;
    void *executable;
    uint64_t size;
} hl_macos_mapping;

typedef struct hl_macos_file {
    uint32_t generation;
    uint32_t active;
    int descriptor;
    int append_descriptor;
} hl_macos_file;

typedef struct hl_macos_process {
    uint32_t generation;
    uint32_t active;
    pid_t pid;
    uint32_t reaped;
    uint32_t waiting;
    uint32_t waiters;
    uint32_t exit_kind;
    uint32_t exit_value;
} hl_macos_process;

struct hl_host_macos {
    pthread_mutex_t lock;
    pthread_cond_t process_changed;
    uint32_t destroying;
    hl_host_sync_registry *sync;
    hl_macos_mapping mappings[HL_MACOS_MAPPING_CAPACITY];
    hl_macos_file files[HL_MACOS_FILE_CAPACITY];
    hl_macos_process processes[HL_MACOS_PROCESS_CAPACITY];
};

static uint64_t hl_macos_monotonic_value(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void hl_macos_sleep_until(uint64_t deadline_ns) {
    uint64_t now = hl_macos_monotonic_value();
    uint64_t remaining;
    struct timespec delay;
    if (now >= deadline_ns) return;
    remaining = deadline_ns - now;
    if (remaining > UINT64_C(1000000)) remaining = UINT64_C(1000000);
    delay.tv_sec = (time_t)(remaining / UINT64_C(1000000000));
    delay.tv_nsec = (long)(remaining % UINT64_C(1000000000));
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static void hl_macos_process_changed_wait(hl_host_macos *host, uint64_t deadline_ns) {
    struct timespec realtime;
    uint64_t now;
    uint64_t remaining;
    uint64_t absolute;
    if (deadline_ns == HL_HOST_DEADLINE_INFINITE) {
        pthread_cond_wait(&host->process_changed, &host->lock);
        return;
    }
    now = hl_macos_monotonic_value();
    if (now >= deadline_ns) return;
    remaining = deadline_ns - now;
    clock_gettime(CLOCK_REALTIME, &realtime);
    absolute = (uint64_t)realtime.tv_sec * UINT64_C(1000000000) + (uint64_t)realtime.tv_nsec + remaining;
    realtime.tv_sec = (time_t)(absolute / UINT64_C(1000000000));
    realtime.tv_nsec = (long)(absolute % UINT64_C(1000000000));
    pthread_cond_timedwait(&host->process_changed, &host->lock, &realtime);
}

static hl_host_result hl_macos_result(hl_status status, uint64_t value, uint64_t detail) {
    return (hl_host_result){(int32_t)status, 2, value, detail};
}

static hl_status hl_macos_status(int error) {
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
    default: return HL_STATUS_PLATFORM_FAILURE;
    }
}

static hl_host_result hl_macos_errno(void) {
    int error = errno;
    return hl_macos_result(hl_macos_status(error), 0, (uint64_t)(unsigned int)error);
}

static hl_host_handle hl_macos_handle(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint64_t)(index + 1u);
}

static hl_macos_mapping *hl_macos_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    if (low == 0) return NULL;
    index = low - 1u;
    if (index >= HL_MACOS_MAPPING_CAPACITY || !host->mappings[index].active ||
        host->mappings[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->mappings[index];
}

static hl_host_result hl_macos_register(hl_host_macos *host, void *writable, void *executable, uint64_t size) {
    uint32_t index;
    hl_host_handle handle = 0;
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_MAPPING_CAPACITY; index++) {
        hl_macos_mapping *mapping = &host->mappings[index];
        if (!mapping->active) {
            mapping->generation++;
            if (mapping->generation == 0) mapping->generation = 1;
            mapping->active = 1;
            mapping->writable = writable;
            mapping->executable = executable;
            mapping->size = size;
            handle = hl_macos_handle(index, mapping->generation);
            break;
        }
    }
    pthread_mutex_unlock(&host->lock);
    return handle != 0 ? hl_macos_result(HL_STATUS_OK, handle, 0) : hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
}

static int hl_macos_protection(uint32_t flags) {
    int protection = 0;
    if ((flags & HL_HOST_MEMORY_READ) != 0) protection |= PROT_READ;
    if ((flags & HL_HOST_MEMORY_WRITE) != 0) protection |= PROT_WRITE;
    if ((flags & HL_HOST_MEMORY_EXECUTE) != 0) protection |= PROT_EXEC;
    return protection;
}

static int hl_macos_dual_map(uint64_t size, vm_inherit_t inheritance, void **writable_out, void **executable_out) {
    void *writable = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mach_vm_address_t executable = 0;
    vm_prot_t current = 0;
    vm_prot_t maximum = 0;
    kern_return_t result;
    if (writable == MAP_FAILED) return -1;
    result = mach_vm_remap(mach_task_self(), &executable, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                           (mach_vm_address_t)writable, FALSE, &current, &maximum, inheritance);
    if (result == KERN_SUCCESS)
        result = mach_vm_protect(mach_task_self(), executable, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (result != KERN_SUCCESS) {
        if (executable != 0) mach_vm_deallocate(mach_task_self(), executable, size);
        munmap(writable, (size_t)size);
        return -1;
    }
    *writable_out = writable;
    *executable_out = (void *)(uintptr_t)executable;
    return 0;
}

static hl_host_result hl_macos_reserve(void *context, uint64_t size, uint64_t alignment, uint32_t flags) {
    hl_host_macos *host = context;
    long page = sysconf(_SC_PAGESIZE);
    void *address;
    hl_host_result handle;
    if (size == 0 || size > SIZE_MAX || page <= 0 || alignment > (uint64_t)page)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    address = mmap(NULL, (size_t)size, hl_macos_protection(flags), MAP_PRIVATE | MAP_ANON, -1, 0);
    if (address == MAP_FAILED) return hl_macos_errno();
    handle = hl_macos_register(host, address, NULL, size);
    if (handle.status != HL_STATUS_OK) munmap(address, (size_t)size);
    return handle;
}

static hl_host_result hl_macos_protect(void *context, hl_host_handle handle, uint64_t offset, uint64_t size,
                                       uint32_t flags) {
    hl_host_macos *host = context;
    hl_macos_mapping *mapping;
    int result;
    pthread_mutex_lock(&host->lock);
    mapping = hl_macos_lookup(host, handle);
    if (mapping == NULL || offset > mapping->size || size > mapping->size - offset) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = mprotect((char *)mapping->writable + offset, (size_t)size, hl_macos_protection(flags));
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_host_result hl_macos_release(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    hl_macos_mapping *mapping;
    int result;
    pthread_mutex_lock(&host->lock);
    mapping = hl_macos_lookup(host, handle);
    if (mapping == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = munmap(mapping->writable, (size_t)mapping->size);
    if (mapping->executable != NULL && mapping->executable != mapping->writable)
        (void)munmap(mapping->executable, (size_t)mapping->size);
    if (result == 0) {
        mapping->active = 0;
        mapping->writable = NULL;
        mapping->executable = NULL;
        mapping->size = 0;
    }
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_host_result hl_macos_publish(void *context, hl_host_handle handle, uint64_t offset, uint64_t size) {
    hl_host_macos *host = context;
    hl_macos_mapping *mapping;
    pthread_mutex_lock(&host->lock);
    mapping = hl_macos_lookup(host, handle);
    if (mapping == NULL || offset > mapping->size || size > mapping->size - offset) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    sys_icache_invalidate((char *)(mapping->executable != NULL ? mapping->executable : mapping->writable) + offset,
                          (size_t)size);
    pthread_mutex_unlock(&host->lock);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_reserve_code(void *context, uint64_t size, uint64_t alignment, uint32_t flags,
                                            hl_host_code_mapping *output) {
    hl_host_macos *host = context;
    void *writable;
    void *executable;
    hl_host_result handle;
    long page = sysconf(_SC_PAGESIZE);
    if (output == NULL || size == 0 || size > SIZE_MAX || page <= 0 || alignment > (uint64_t)page)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memset(output, 0, sizeof(*output));
    if ((flags & HL_HOST_CODE_DUAL_ALIAS) != 0) {
        if (hl_macos_dual_map(size, VM_INHERIT_NONE, &writable, &executable) != 0)
            return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    } else {
        writable =
            mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (writable == MAP_FAILED) return hl_macos_errno();
        executable = writable;
    }
    handle = hl_macos_register(host, writable, executable, size);
    if (handle.status != HL_STATUS_OK) {
        if (executable != writable) munmap(executable, (size_t)size);
        munmap(writable, (size_t)size);
        return handle;
    }
    output->abi = 1;
    output->size = sizeof(*output);
    output->handle = handle.value;
    output->writable_address = (uint64_t)(uintptr_t)writable;
    output->executable_address = (uint64_t)(uintptr_t)executable;
    output->mapped_size = size;
    return handle;
}

static hl_host_result hl_macos_repair_code(void *context, hl_host_code_mapping *public_mapping, uint32_t preserve) {
    hl_host_macos *host = context;
    hl_macos_mapping *mapping;
    void *writable;
    void *executable;
    kern_return_t result = KERN_FAILURE;
    if (public_mapping == NULL || public_mapping->abi != 1 || public_mapping->size < sizeof(*public_mapping))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_init(&host->lock, NULL);
    mapping = hl_macos_lookup(host, public_mapping->handle);
    if (mapping == NULL || mapping->executable == NULL) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (mapping->executable == mapping->writable) return hl_macos_result(HL_STATUS_OK, public_mapping->handle, 0);
    if (preserve != 0) {
        mach_vm_address_t target = (mach_vm_address_t)mapping->executable;
        vm_prot_t current = 0;
        vm_prot_t maximum = 0;
        int remapped = 0;
        result = mach_vm_remap(mach_task_self(), &target, mapping->size, 0, VM_FLAGS_FIXED, mach_task_self(),
                               (mach_vm_address_t)mapping->writable, FALSE, &current, &maximum, VM_INHERIT_NONE);
        if (result == KERN_SUCCESS) {
            remapped = 1;
            result = mach_vm_protect(mach_task_self(), target, mapping->size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        }
        if (result == KERN_SUCCESS) return hl_macos_result(HL_STATUS_OK, public_mapping->handle, 0);
        if (remapped) mach_vm_deallocate(mach_task_self(), target, mapping->size);
    }
    if (hl_macos_dual_map(mapping->size, VM_INHERIT_NONE, &writable, &executable) != 0)
        return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, (uint64_t)result);
    munmap(mapping->writable, (size_t)mapping->size);
    mapping->writable = writable;
    mapping->executable = executable;
    public_mapping->writable_address = (uint64_t)(uintptr_t)writable;
    public_mapping->executable_address = (uint64_t)(uintptr_t)executable;
    return hl_macos_result(HL_STATUS_OK, public_mapping->handle, 0);
}

static hl_host_result hl_macos_clock(clockid_t clock_id) {
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec, 0);
}

static hl_host_result hl_macos_monotonic(void *context) {
    (void)context;
    return hl_macos_clock(CLOCK_MONOTONIC);
}

static hl_host_result hl_macos_realtime(void *context) {
    (void)context;
    return hl_macos_clock(CLOCK_REALTIME);
}

static hl_macos_file *hl_macos_file_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    if (low <= HL_MACOS_MAPPING_CAPACITY) return NULL;
    index = low - HL_MACOS_MAPPING_CAPACITY - 1u;
    if (index >= HL_MACOS_FILE_CAPACITY || !host->files[index].active ||
        host->files[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->files[index];
}

static hl_host_result hl_macos_file_register(hl_host_macos *host, int descriptor, int append_descriptor) {
    uint32_t index;
    hl_host_handle handle = 0;
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_FILE_CAPACITY; ++index) {
        hl_macos_file *file = &host->files[index];
        if (!file->active) {
            file->generation++;
            if (file->generation == 0) file->generation = 1;
            file->active = 1;
            file->descriptor = descriptor;
            file->append_descriptor = append_descriptor;
            handle = ((uint64_t)file->generation << 32) | (HL_MACOS_MAPPING_CAPACITY + index + 1u);
            break;
        }
    }
    pthread_mutex_unlock(&host->lock);
    return handle != 0 ? hl_macos_result(HL_STATUS_OK, handle, 0) : hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
}

static hl_host_result hl_macos_file_open(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                         uint32_t access, uint32_t creation, uint32_t permissions) {
    hl_host_macos *host = context;
    char local[PATH_MAX];
    int directory_fd = AT_FDCWD;
    int flags;
    int descriptor;
    int append_descriptor = -1;
    if (path == NULL || path_size == 0 || path_size >= sizeof(local))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memcpy(local, path, path_size);
    local[path_size] = '\0';
    if (directory != HL_HOST_HANDLE_CWD) {
        pthread_mutex_lock(&host->lock);
        hl_macos_file *file = hl_macos_file_lookup(host, directory);
        directory_fd = file != NULL ? file->descriptor : -1;
        pthread_mutex_unlock(&host->lock);
        if (directory_fd < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    if ((access & (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE)) == (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE))
        flags = O_RDWR;
    else if ((access & HL_HOST_FILE_WRITE) != 0)
        flags = O_WRONLY;
    else if ((access & HL_HOST_FILE_READ) != 0)
        flags = O_RDONLY;
    else
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
#ifdef O_DIRECTORY
    if ((access & HL_HOST_FILE_DIRECTORY) != 0) flags |= O_DIRECTORY;
#endif
    if ((creation & HL_HOST_FILE_CREATE) != 0) flags |= O_CREAT;
    if ((creation & HL_HOST_FILE_EXCLUSIVE) != 0) flags |= O_EXCL;
    if ((creation & HL_HOST_FILE_TRUNCATE) != 0) flags |= O_TRUNC;
    descriptor = openat(directory_fd, local, flags | O_CLOEXEC, (mode_t)(permissions & 07777u));
    if (descriptor < 0) return hl_macos_errno();
    if ((access & HL_HOST_FILE_APPEND) != 0) {
        int append_flags = flags & ~(O_CREAT | O_EXCL | O_TRUNC);
        append_descriptor = openat(directory_fd, local, append_flags | O_APPEND | O_CLOEXEC, 0);
        if (append_descriptor < 0) {
            hl_host_result error = hl_macos_errno();
            close(descriptor);
            return error;
        }
    }
    hl_host_result result = hl_macos_file_register(host, descriptor, append_descriptor);
    if (result.status != HL_STATUS_OK) {
        close(descriptor);
        if (append_descriptor >= 0) close(append_descriptor);
    }
    return result;
}

static int hl_macos_file_descriptor(hl_host_macos *host, hl_host_handle handle, int append) {
    hl_macos_file *file;
    int descriptor;
    pthread_mutex_lock(&host->lock);
    file = hl_macos_file_lookup(host, handle);
    descriptor = file == NULL ? -1 : (append ? file->append_descriptor : file->descriptor);
    pthread_mutex_unlock(&host->lock);
    return descriptor;
}

static hl_host_result hl_macos_file_read(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    ssize_t count;
    if ((output.size != 0 && output.data == NULL) || descriptor < 0 || offset > INT64_MAX)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = pread(descriptor, output.data, output.size, (off_t)offset);
    return count < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_file_write(void *context, hl_host_handle file, uint64_t offset,
                                          hl_host_const_bytes input) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    ssize_t count;
    if ((input.size != 0 && input.data == NULL) || descriptor < 0 || offset > INT64_MAX)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = pwrite(descriptor, input.data, input.size, (off_t)offset);
    return count < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_file_read_sequential(void *context, hl_host_handle file, void *output,
                                                    uint64_t output_size) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    ssize_t count;
    if ((output_size != 0 && output == NULL) || output_size > SIZE_MAX || descriptor < 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = read(descriptor, output, (size_t)output_size);
    return count < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_file_write_sequential(void *context, hl_host_handle file, const void *input,
                                                     uint64_t input_size) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    ssize_t count;
    if ((input_size != 0 && input == NULL) || input_size > SIZE_MAX || descriptor < 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = write(descriptor, input, (size_t)input_size);
    return count < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_file_append(void *context, hl_host_handle file, hl_host_const_bytes input) {
    int descriptor = hl_macos_file_descriptor(context, file, 1);
    ssize_t count;
    off_t offset;
    if ((input.size != 0 && input.data == NULL) || descriptor < 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = write(descriptor, input.data, input.size);
    if (count < 0) return hl_macos_errno();
    offset = lseek(descriptor, 0, SEEK_CUR);
    return offset < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)count, (uint64_t)offset);
}

static hl_host_result hl_macos_file_metadata_get(void *context, hl_host_handle file, hl_host_file_metadata *output) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    struct stat status;
    if (descriptor < 0 || output == NULL) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (fstat(descriptor, &status) != 0) return hl_macos_errno();
    memset(output, 0, sizeof(*output));
    output->stable_device = (uint64_t)status.st_dev;
    output->stable_object = (uint64_t)status.st_ino;
    output->size = (uint64_t)status.st_size;
    output->allocated_size = (uint64_t)status.st_blocks * 512u;
    output->modified_ns =
        (uint64_t)status.st_mtimespec.tv_sec * UINT64_C(1000000000) + (uint64_t)status.st_mtimespec.tv_nsec;
    output->permissions = (uint32_t)status.st_mode & 07777u;
    if (S_ISREG(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_REGULAR;
    else if (S_ISDIR(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_DIRECTORY;
    else if (S_ISLNK(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_SYMLINK;
    else if (S_ISCHR(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_CHARACTER;
    else if (S_ISBLK(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_BLOCK;
    else if (S_ISFIFO(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_FIFO;
    else if (S_ISSOCK(status.st_mode))
        output->type = HL_HOST_FILE_TYPE_SOCKET;
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_file_close(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    hl_macos_file *file;
    int descriptor;
    int append_descriptor;
    pthread_mutex_lock(&host->lock);
    file = hl_macos_file_lookup(host, handle);
    if (file == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    descriptor = file->descriptor;
    append_descriptor = file->append_descriptor;
    file->active = 0;
    file->descriptor = -1;
    file->append_descriptor = -1;
    pthread_mutex_unlock(&host->lock);
    if (close(descriptor) != 0) return hl_macos_errno();
    if (append_descriptor >= 0 && close(append_descriptor) != 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_macos_process *hl_macos_process_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    if (low == 0) return NULL;
    index = low - 1u;
    if (index >= HL_MACOS_PROCESS_CAPACITY || !host->processes[index].active ||
        host->processes[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->processes[index];
}

static hl_host_result hl_macos_process_spawn(void *context, hl_host_process_entry entry, void *entry_context) {
    hl_host_macos *host = context;
    hl_host_handle handle = 0;
    uint32_t index;
    pid_t pid;
    if (entry == NULL) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pid = fork();
    if (pid < 0) return hl_macos_errno();
    if (pid == 0) _exit(entry(entry_context) & 255);
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_PROCESS_CAPACITY; ++index) {
        hl_macos_process *process = &host->processes[index];
        if (process->active) continue;
        process->generation++;
        if (process->generation == 0) process->generation = 1;
        process->active = 1;
        process->pid = pid;
        process->reaped = 0;
        process->waiting = 0;
        process->waiters = 0;
        process->exit_kind = 0;
        process->exit_value = 0;
        handle = hl_macos_handle(index, process->generation);
        break;
    }
    pthread_mutex_unlock(&host->lock);
    if (handle == 0) {
        int status;
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
    }
    return hl_macos_result(HL_STATUS_OK, handle, 0);
}

static hl_host_result hl_macos_process_wait(void *context, hl_host_handle handle, uint64_t deadline_ns) {
    hl_host_macos *host = context;
    hl_macos_process *process;
    pid_t pid;
    pid_t waited;
    int status;
    int options;
    pthread_mutex_lock(&host->lock);
    process = hl_macos_process_lookup(host, handle);
    if (process == NULL || host->destroying) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    process->waiters++;
    while (process != NULL && process->waiting && !process->reaped) {
        if (deadline_ns == 0 ||
            (deadline_ns != HL_HOST_DEADLINE_INFINITE && hl_macos_monotonic_value() >= deadline_ns)) {
            process->waiters--;
            pthread_cond_broadcast(&host->process_changed);
            pthread_mutex_unlock(&host->lock);
            return hl_macos_result(HL_STATUS_WOULD_BLOCK, 0, 0);
        }
        hl_macos_process_changed_wait(host, deadline_ns);
        process = hl_macos_process_lookup(host, handle);
    }
    if (process != NULL && process->reaped) {
        hl_host_result result = hl_macos_result(HL_STATUS_OK, process->exit_value, process->exit_kind);
        process->waiters--;
        pthread_cond_broadcast(&host->process_changed);
        pthread_mutex_unlock(&host->lock);
        return result;
    }
    pid = process != NULL ? process->pid : -1;
    if (process != NULL) process->waiting = 1;
    pthread_mutex_unlock(&host->lock);
    if (pid < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    options = deadline_ns == HL_HOST_DEADLINE_INFINITE ? 0 : WNOHANG;
    for (;;) {
        do {
            waited = waitpid(pid, &status, options);
        } while (waited < 0 && errno == EINTR);
        if (waited != 0) break;
        if (deadline_ns == 0 || hl_macos_monotonic_value() >= deadline_ns) break;
        hl_macos_sleep_until(deadline_ns);
    }
    pthread_mutex_lock(&host->lock);
    process = hl_macos_process_lookup(host, handle);
    if (process != NULL) {
        process->waiting = 0;
        process->waiters--;
    }
    if (waited > 0 && process != NULL) {
        process->reaped = 1;
        process->exit_kind = WIFEXITED(status) ? HL_HOST_PROCESS_EXIT_CODE : HL_HOST_PROCESS_EXIT_SIGNAL;
        process->exit_value = WIFEXITED(status) ? (uint32_t)WEXITSTATUS(status) : (uint32_t)WTERMSIG(status);
    }
    pthread_cond_broadcast(&host->process_changed);
    pthread_mutex_unlock(&host->lock);
    if (waited == 0) return hl_macos_result(HL_STATUS_WOULD_BLOCK, 0, 0);
    if (waited < 0) return hl_macos_errno();
    if (WIFEXITED(status))
        return hl_macos_result(HL_STATUS_OK, (uint64_t)WEXITSTATUS(status), HL_HOST_PROCESS_EXIT_CODE);
    if (WIFSIGNALED(status))
        return hl_macos_result(HL_STATUS_OK, (uint64_t)WTERMSIG(status), HL_HOST_PROCESS_EXIT_SIGNAL);
    return hl_macos_result(HL_STATUS_CORRUPT, 0, (uint64_t)(uint32_t)status);
}

static hl_host_result hl_macos_process_terminate(void *context, hl_host_handle handle, uint32_t reason) {
    hl_host_macos *host = context;
    hl_macos_process *process;
    pid_t pid;
    if (reason != HL_HOST_PROCESS_TERMINATE_INTERRUPT && reason != HL_HOST_PROCESS_TERMINATE_FORCE)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    process = hl_macos_process_lookup(host, handle);
    pid = process != NULL && !process->reaped && !host->destroying ? process->pid : -1;
    pthread_mutex_unlock(&host->lock);
    if (pid < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (kill(pid, reason == HL_HOST_PROCESS_TERMINATE_INTERRUPT ? SIGINT : SIGKILL) != 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_process_close(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    hl_macos_process *process;
    pthread_mutex_lock(&host->lock);
    process = hl_macos_process_lookup(host, handle);
    if (process == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    if (host->destroying || !process->reaped || process->waiting || process->waiters != 0) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_BUSY, 0, 0);
    }
    process->active = 0;
    process->pid = -1;
    process->reaped = 0;
    process->exit_kind = 0;
    process->exit_value = 0;
    pthread_mutex_unlock(&host->lock);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_mutex_create(void *context) {
    hl_host_macos *host = context;
    return hl_host_sync_mutex_create(host->sync);
}

static hl_host_result hl_macos_mutex_lock(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    return hl_host_sync_mutex_lock(host->sync, handle);
}

static hl_host_result hl_macos_mutex_unlock(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    return hl_host_sync_mutex_unlock(host->sync, handle);
}

static hl_host_result hl_macos_mutex_close(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    return hl_host_sync_mutex_close(host->sync, handle);
}

static void hl_macos_log(void *context, uint32_t event, const char *message, size_t message_size) {
    size_t written = 0;
    (void)context;
    (void)event;
    while (written < message_size) {
        ssize_t result = write(STDERR_FILENO, message + written, message_size - written);
        if (result > 0) {
            written += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        break;
    }
}

hl_status hl_host_macos_create(hl_host_macos **out_host, hl_host_services *out_services) {
    static const hl_host_memory_services memory = {HL_HOST_MEMORY_ABI,    sizeof(memory),      hl_macos_reserve,
                                                   hl_macos_protect,      hl_macos_release,    hl_macos_publish,
                                                   hl_macos_reserve_code, hl_macos_repair_code};
    static const hl_host_clock_services clock = {HL_HOST_CLOCK_ABI, sizeof(clock), hl_macos_monotonic,
                                                 hl_macos_realtime};
    static const hl_host_log_services log = {HL_HOST_LOG_ABI, sizeof(log), hl_macos_log};
    static const hl_host_file_services file = {HL_HOST_FILE_ABI,
                                               sizeof(file),
                                               hl_macos_file_open,
                                               hl_macos_file_read,
                                               hl_macos_file_write,
                                               hl_macos_file_append,
                                               hl_macos_file_metadata_get,
                                               hl_macos_file_close,
                                               hl_macos_file_read_sequential,
                                               hl_macos_file_write_sequential};
    static const hl_host_process_services process = {HL_HOST_PROCESS_ABI,        sizeof(process),
                                                     hl_macos_process_spawn,     hl_macos_process_wait,
                                                     hl_macos_process_terminate, hl_macos_process_close};
    static const hl_host_sync_services sync = {HL_HOST_SYNC_ABI,    sizeof(sync),          hl_macos_mutex_create,
                                               hl_macos_mutex_lock, hl_macos_mutex_unlock, hl_macos_mutex_close};
    hl_host_macos *host;
    if (out_host == NULL || out_services == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_host = NULL;
    memset(out_services, 0, sizeof(*out_services));
    host = calloc(1, sizeof(*host));
    if (host == NULL) return HL_STATUS_OUT_OF_MEMORY;
    if (pthread_mutex_init(&host->lock, NULL) != 0) {
        free(host);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (pthread_cond_init(&host->process_changed, NULL) != 0) {
        pthread_mutex_destroy(&host->lock);
        free(host);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (hl_host_sync_registry_create(&host->sync) != HL_STATUS_OK) {
        pthread_cond_destroy(&host->process_changed);
        pthread_mutex_destroy(&host->lock);
        free(host);
        return HL_STATUS_OUT_OF_MEMORY;
    }
    out_services->abi = HL_HOST_SERVICES_ABI;
    out_services->size = sizeof(*out_services);
    out_services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_LOG | HL_HOST_CAP_FILE |
                                 HL_HOST_CAP_PROCESS | HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_SYNC;
    out_services->context = host;
    out_services->memory = &memory;
    out_services->clock = &clock;
    out_services->log = &log;
    out_services->file = &file;
    out_services->process = &process;
    out_services->sync = &sync;
    *out_host = host;
    return HL_STATUS_OK;
}

void hl_host_macos_destroy(hl_host_macos *host) {
    uint32_t index;
    if (host == NULL) return;
    pthread_mutex_lock(&host->lock);
    host->destroying = 1;
    for (index = 0; index < HL_MACOS_PROCESS_CAPACITY; ++index) {
        hl_macos_process *process = &host->processes[index];
        if (process->active && !process->reaped) kill(process->pid, SIGKILL);
    }
    hl_host_sync_registry_destroy(host->sync);
    for (;;) {
        uint32_t waiters = 0;
        for (index = 0; index < HL_MACOS_PROCESS_CAPACITY; ++index)
            waiters += host->processes[index].waiters;
        if (waiters == 0) break;
        pthread_cond_wait(&host->process_changed, &host->lock);
    }
    pthread_mutex_unlock(&host->lock);
    for (index = 0; index < HL_MACOS_MAPPING_CAPACITY; index++) {
        hl_macos_mapping *mapping = &host->mappings[index];
        if (!mapping->active) continue;
        munmap(mapping->writable, (size_t)mapping->size);
        if (mapping->executable != NULL && mapping->executable != mapping->writable)
            munmap(mapping->executable, (size_t)mapping->size);
    }
    for (index = 0; index < HL_MACOS_FILE_CAPACITY; ++index) {
        hl_macos_file *file = &host->files[index];
        if (!file->active) continue;
        close(file->descriptor);
        if (file->append_descriptor >= 0) close(file->append_descriptor);
    }
    for (index = 0; index < HL_MACOS_PROCESS_CAPACITY; ++index) {
        hl_macos_process *process = &host->processes[index];
        int status;
        if (!process->active || process->reaped) continue;
        kill(process->pid, SIGKILL);
        while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {}
    }
    pthread_cond_destroy(&host->process_changed);
    pthread_mutex_destroy(&host->lock);
    free(host);
}
