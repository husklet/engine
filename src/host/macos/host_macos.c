#define _DARWIN_C_SOURCE

#include "hl/host_macos.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define HL_MACOS_MAPPING_CAPACITY 4096u

typedef struct hl_macos_mapping {
    uint32_t generation;
    uint32_t active;
    void *writable;
    void *executable;
    uint64_t size;
} hl_macos_mapping;

struct hl_host_macos {
    pthread_mutex_t lock;
    hl_macos_mapping mappings[HL_MACOS_MAPPING_CAPACITY];
};

static hl_host_result hl_macos_result(hl_status status, uint64_t value, uint64_t detail) {
    return (hl_host_result){(int32_t)status, 2, value, detail};
}

static hl_status hl_macos_status(int error) {
    switch (error) {
    case 0: return HL_STATUS_OK;
    case EINVAL: return HL_STATUS_INVALID_ARGUMENT;
    case ENOMEM: return HL_STATUS_OUT_OF_MEMORY;
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

hl_status hl_host_macos_create(hl_host_macos **out_host, hl_host_services *out_services) {
    static const hl_host_memory_services memory = {HL_HOST_MEMORY_ABI,    sizeof(memory),      hl_macos_reserve,
                                                   hl_macos_protect,      hl_macos_release,    hl_macos_publish,
                                                   hl_macos_reserve_code, hl_macos_repair_code};
    static const hl_host_clock_services clock = {HL_HOST_CLOCK_ABI, sizeof(clock), hl_macos_monotonic,
                                                 hl_macos_realtime};
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
    out_services->abi = HL_HOST_SERVICES_ABI;
    out_services->size = sizeof(*out_services);
    out_services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_CODE_MAPPING;
    out_services->context = host;
    out_services->memory = &memory;
    out_services->clock = &clock;
    *out_host = host;
    return HL_STATUS_OK;
}

void hl_host_macos_destroy(hl_host_macos *host) {
    uint32_t index;
    if (host == NULL) return;
    for (index = 0; index < HL_MACOS_MAPPING_CAPACITY; index++) {
        hl_macos_mapping *mapping = &host->mappings[index];
        if (!mapping->active) continue;
        munmap(mapping->writable, (size_t)mapping->size);
        if (mapping->executable != NULL && mapping->executable != mapping->writable)
            munmap(mapping->executable, (size_t)mapping->size);
    }
    pthread_mutex_destroy(&host->lock);
    free(host);
}
