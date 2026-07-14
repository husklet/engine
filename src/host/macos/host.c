#define _DARWIN_C_SOURCE

#include "hl/macos.h"
#include "../resolve.h"
#include "../sync.h"

#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HL_MACOS_MAPPING_CAPACITY 4096u
#define HL_MACOS_FILE_CAPACITY 1024u
#define HL_MACOS_PROCESS_CAPACITY 1024u
#define HL_MACOS_EVENT_CAPACITY 64u
#define HL_MACOS_TIMER_CAPACITY 32u
#define HL_MACOS_COUNTER_CAPACITY 128u
#define HL_MACOS_TRANSFER_CAPACITY 64u
#define HL_MACOS_DIRECTORY_CAPACITY 128u
#define HL_MACOS_DIRECTORY_WATCH_CAPACITY 256u
#define HL_MACOS_COUNTER_SUBSCRIPTIONS 128u

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
    uint32_t shared;
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

typedef struct hl_macos_timer {
    uint64_t token;
    uint64_t interval_ns;
    uint32_t active;
} hl_macos_timer;

typedef struct hl_macos_event {
    uint32_t generation;
    uint32_t active;
    int descriptor;
    hl_macos_timer timers[HL_MACOS_TIMER_CAPACITY];
} hl_macos_event;

typedef struct hl_macos_counter_shared {
    pthread_mutex_t lock;
    uint64_t value;
    uint32_t flags;
    uint32_t references;
} hl_macos_counter_shared;

typedef struct hl_macos_counter_object {
    hl_macos_counter_shared *shared;
    int backing;
    int readable;
    int signal;
} hl_macos_counter_object;

typedef struct hl_macos_counter {
    uint32_t generation;
    uint32_t active;
    hl_macos_counter_object *object;
    uint32_t rights;
} hl_macos_counter;

typedef struct hl_macos_counter_subscription {
    uint32_t generation;
    uint32_t active;
    hl_host_handle counter;
    int descriptor;
    int wake[2];
    pthread_t thread;
    void (*notify)(void *, uint64_t);
    void *observer;
    uint64_t token;
} hl_macos_counter_subscription;

typedef struct hl_macos_transfer {
    uint32_t generation;
    uint32_t active;
    int descriptor;
} hl_macos_transfer;

typedef struct hl_macos_directory_watch {
    uint64_t token;
    uint32_t interests;
    int descriptor;
    uint32_t active;
} hl_macos_directory_watch;

typedef struct hl_macos_directory_object {
    uint32_t references;
    int descriptor;
    hl_macos_directory_watch watches[HL_MACOS_DIRECTORY_WATCH_CAPACITY];
} hl_macos_directory_object;

typedef struct hl_macos_directory {
    uint32_t generation;
    uint32_t active;
    hl_macos_directory_object *object;
} hl_macos_directory;

struct hl_host_macos {
    pthread_mutex_t lock;
    pthread_mutex_t fork_gate;
    pthread_cond_t process_changed;
    uint32_t destroying;
    hl_host_sync_registry *sync;
    hl_macos_mapping mappings[HL_MACOS_MAPPING_CAPACITY];
    hl_macos_file files[HL_MACOS_FILE_CAPACITY];
    hl_macos_process processes[HL_MACOS_PROCESS_CAPACITY];
    hl_macos_event events[HL_MACOS_EVENT_CAPACITY];
    hl_macos_counter counters[HL_MACOS_COUNTER_CAPACITY];
    hl_macos_counter_subscription counter_subscriptions[HL_MACOS_COUNTER_SUBSCRIPTIONS];
    hl_macos_transfer transfers[HL_MACOS_TRANSFER_CAPACITY];
    hl_macos_directory directories[HL_MACOS_DIRECTORY_CAPACITY];
};

static hl_host_result hl_macos_fork_complete(void *context);
static hl_host_result hl_macos_fork_child(void *context);
static hl_host_result hl_macos_counter_unsubscribe(void *context, hl_host_handle subscription);
static hl_host_result hl_macos_file_close(void *context, hl_host_handle handle);
static void hl_macos_counter_unsubscribe_all(hl_host_macos *host, hl_host_handle counter);

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
    case ENOTDIR: return HL_STATUS_NOT_DIRECTORY;
    case EISDIR: return HL_STATUS_IS_DIRECTORY;
    case ENAMETOOLONG: return HL_STATUS_NAME_TOO_LONG;
    case ELOOP: return HL_STATUS_SYMLINK_LOOP;
    case EROFS: return HL_STATUS_READ_ONLY;
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

static hl_host_result hl_macos_begin_code_write(void *context) {
    (void)context;
    pthread_jit_write_protect_np(0);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_end_code_write(void *context) {
    (void)context;
    pthread_jit_write_protect_np(1);
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

static hl_host_result hl_macos_raw_monotonic(void *context) {
    (void)context;
    return hl_macos_clock(CLOCK_MONOTONIC_RAW);
}

static hl_host_result hl_macos_process_cpu(void *context) {
    (void)context;
    return hl_macos_clock(CLOCK_PROCESS_CPUTIME_ID);
}

static hl_host_result hl_macos_thread_cpu(void *context) {
    (void)context;
    return hl_macos_clock(CLOCK_THREAD_CPUTIME_ID);
}

static void hl_macos_precise_sleep_begin(void) {
    mach_timebase_info_data_t timebase;
    thread_time_constraint_policy_data_t policy;
    double nanoseconds_to_ticks;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) return;
    nanoseconds_to_ticks = (double)timebase.denom / (double)timebase.numer;
    policy.period = (uint32_t)(500000.0 * nanoseconds_to_ticks);
    policy.computation = (uint32_t)(100000.0 * nanoseconds_to_ticks);
    policy.constraint = (uint32_t)(500000.0 * nanoseconds_to_ticks);
    policy.preemptible = 1;
    (void)thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
                            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static void hl_macos_precise_sleep_end(void) {
    thread_standard_policy_data_t policy = {0};
    (void)thread_policy_set(mach_thread_self(), THREAD_STANDARD_POLICY, (thread_policy_t)&policy,
                            THREAD_STANDARD_POLICY_COUNT);
}

static hl_host_result hl_macos_clock_sleep_until(void *context, uint32_t clock_kind, uint64_t deadline_ns) {
    clockid_t clock_id;
    struct timespec now, delay;
    uint64_t now_ns, remaining;
    (void)context;
    switch (clock_kind) {
    case HL_HOST_CLOCK_MONOTONIC: clock_id = CLOCK_MONOTONIC; break;
    /* A relative nanosleep preserves an absolute monotonic deadline and EINTR, but cannot promise
     * realtime-adjustment wakeups. Do not advertise unsupported absolute-clock semantics. */
    default: return hl_macos_result(HL_STATUS_NOT_SUPPORTED, 0, 0);
    }
    if (clock_gettime(clock_id, &now) != 0) return hl_macos_errno();
    now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    if (now_ns >= deadline_ns) return hl_macos_result(HL_STATUS_OK, 0, 0);
    remaining = deadline_ns - now_ns;
    delay.tv_sec = (time_t)(remaining / UINT64_C(1000000000));
    delay.tv_nsec = (long)(remaining % UINT64_C(1000000000));
    /* Match Linux high-resolution timer wakeups without leaking a Darwin scheduler policy into linux_abi. */
    hl_macos_precise_sleep_begin();
    if (nanosleep(&delay, NULL) != 0) {
        hl_host_result result = hl_macos_errno();
        hl_macos_precise_sleep_end();
        return result;
    }
    hl_macos_precise_sleep_end();
    return hl_macos_result(HL_STATUS_OK, 0, 0);
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

static hl_host_result hl_macos_file_register(hl_host_macos *host, int descriptor, int append_descriptor,
                                             uint32_t shared) {
    uint32_t index;
    hl_host_handle handle = 0;
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_FILE_CAPACITY; ++index) {
        hl_macos_file *file = &host->files[index];
        if (!file->active) {
            file->generation++;
            if (file->generation == 0) file->generation = 1;
            file->active = 1;
            file->shared = shared;
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
    if ((access & HL_HOST_FILE_PATH_ONLY) != 0)
#ifdef O_SYMLINK
        flags = (access & HL_HOST_FILE_NOFOLLOW) != 0 ? O_SYMLINK : O_RDONLY;
#else
        flags = O_RDONLY;
#endif
    else if ((access & (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE)) == (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE))
        flags = O_RDWR;
    else if ((access & HL_HOST_FILE_WRITE) != 0)
        flags = O_WRONLY;
    else if ((access & HL_HOST_FILE_READ) != 0)
        flags = O_RDONLY;
    else
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
#ifdef O_NOFOLLOW
    if ((access & HL_HOST_FILE_NOFOLLOW) != 0 && (access & HL_HOST_FILE_PATH_ONLY) == 0) flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    if ((access & HL_HOST_FILE_DIRECTORY) != 0) flags |= O_DIRECTORY;
#endif
    if ((creation & HL_HOST_FILE_CREATE) != 0) flags |= O_CREAT;
    if ((creation & HL_HOST_FILE_EXCLUSIVE) != 0) flags |= O_EXCL;
    if ((creation & HL_HOST_FILE_TRUNCATE) != 0) flags |= O_TRUNC;
    if ((access & HL_HOST_FILE_APPEND) != 0) flags |= O_APPEND;
    descriptor = openat(directory_fd, local, flags | O_CLOEXEC, (mode_t)(permissions & 07777u));
    if (descriptor < 0) return hl_macos_errno();
    if ((access & HL_HOST_FILE_APPEND) != 0) {
        append_descriptor = dup(descriptor);
        if (append_descriptor < 0) {
            hl_host_result error = hl_macos_errno();
            close(descriptor);
            return error;
        }
    }
    hl_host_result result = hl_macos_file_register(host, descriptor, append_descriptor, 0);
    if (result.status != HL_STATUS_OK) {
        close(descriptor);
        if (append_descriptor >= 0) close(append_descriptor);
    }
    return result;
}

static int hl_macos_file_descriptor(hl_host_macos *host, hl_host_handle handle, int append);

static hl_host_result hl_macos_file_standard_stream(void *context, uint32_t stream) {
    hl_host_macos *host = context;
    int flags;
    int descriptor;
    int append_descriptor = -1;
    uint32_t detail = 0;
    hl_host_result result;
    if (stream > HL_HOST_STANDARD_ERROR) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    flags = fcntl((int)stream, F_GETFL);
    if (flags < 0) return hl_macos_errno();
    descriptor = fcntl((int)stream, F_DUPFD_CLOEXEC, 0);
    if (descriptor < 0) return hl_macos_errno();
    if ((flags & O_ACCMODE) == O_RDONLY)
        detail |= HL_HOST_FILE_READ;
    else if ((flags & O_ACCMODE) == O_WRONLY)
        detail |= HL_HOST_FILE_WRITE;
    else if ((flags & O_ACCMODE) == O_RDWR)
        detail |= HL_HOST_FILE_READ | HL_HOST_FILE_WRITE;
    if ((flags & O_APPEND) != 0) {
        detail |= HL_HOST_FILE_APPEND;
        append_descriptor = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
        if (append_descriptor < 0) {
            hl_host_result error = hl_macos_errno();
            close(descriptor);
            return error;
        }
    }
    if ((flags & O_NONBLOCK) != 0) detail |= HL_HOST_FILE_NONBLOCK;
    result = hl_macos_file_register(host, descriptor, append_descriptor, 0);
    if (result.status != HL_STATUS_OK) {
        close(descriptor);
        if (append_descriptor >= 0) close(append_descriptor);
        return result;
    }
    result.detail = detail;
    return result;
}

static hl_host_result hl_macos_file_readlink(void *context, hl_host_handle file, hl_host_bytes output) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    char path[PATH_MAX];
    ssize_t count;
    if ((output.size != 0 && output.data == NULL) || output.size > SSIZE_MAX || descriptor < 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (fcntl(descriptor, F_GETPATH, path) != 0) return hl_macos_errno();
    do
        count = readlink(path, output.data, output.size);
    while (count < 0 && errno == EINTR);
    return count < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_file_set_owner(void *context, hl_host_handle file, uint32_t uid, uint32_t gid) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    int status;
    if (descriptor < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    do
        status = fchown(descriptor, (uid_t)uid, (gid_t)gid);
    while (status != 0 && errno == EINTR);
    return status == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
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

static hl_host_result hl_macos_file_clone_for_fork(void *context, hl_host_handle file) {
    hl_host_macos *host = context;
    hl_macos_file *entry;
    int descriptor = -1;
    int append_descriptor = -1;
    int needs_append = 0;
    uint32_t shared = 0;
    pthread_mutex_lock(&host->lock);
    entry = hl_macos_file_lookup(host, file);
    if (entry != NULL) {
        needs_append = entry->append_descriptor >= 0;
        shared = entry->shared;
        descriptor = dup(entry->descriptor);
        if (descriptor >= 0 && needs_append) append_descriptor = dup(entry->append_descriptor);
    }
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0 || (needs_append && append_descriptor < 0)) {
        hl_host_result error = hl_macos_errno();
        if (descriptor >= 0) close(descriptor);
        return error;
    }
    {
        hl_host_result result = hl_macos_file_register(host, descriptor, append_descriptor, shared);
        if (result.status != HL_STATUS_OK) {
            close(descriptor);
            if (append_descriptor >= 0) close(append_descriptor);
        }
        return result;
    }
}

static hl_host_result hl_macos_file_seek(void *context, hl_host_handle file, int64_t offset, uint32_t whence) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    off_t result;
    if (descriptor < 0 || whence > UINT32_C(2)) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    result = lseek(descriptor, (off_t)offset, (int)whence);
    return result < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_OK, (uint64_t)result, 0);
}

static hl_host_result hl_macos_file_append(void *context, hl_host_handle file, hl_host_const_bytes input) {
    int descriptor = hl_macos_file_descriptor(context, file, 1);
    ssize_t count;
    if ((input.size != 0 && input.data == NULL) || descriptor < 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    count = write(descriptor, input.data, input.size);
    if (count < 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_file_vector(void *context, hl_host_handle file, const hl_host_iovec *vectors,
                                           uint32_t count, uint64_t offset, int operation) {
    struct iovec native[HL_HOST_FILE_IOV_MAX];
    int descriptor = hl_macos_file_descriptor(context, file, operation == 4);
    ssize_t transferred;
    uint32_t index;
    if ((count != 0 && vectors == NULL) || count > HL_HOST_FILE_IOV_MAX || descriptor < 0 ||
        ((operation == 2 || operation == 3) && offset > INT64_MAX))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    for (index = 0; index < count; ++index) {
        if (vectors[index].size > SIZE_MAX || vectors[index].address > UINTPTR_MAX)
            return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
        native[index].iov_base = (void *)(uintptr_t)vectors[index].address;
        native[index].iov_len = (size_t)vectors[index].size;
    }
    switch (operation) {
    case 0: transferred = readv(descriptor, native, (int)count); break;
    case 1: transferred = writev(descriptor, native, (int)count); break;
    case 2: transferred = preadv(descriptor, native, (int)count, (off_t)offset); break;
    case 3: transferred = pwritev(descriptor, native, (int)count, (off_t)offset); break;
    default: transferred = writev(descriptor, native, (int)count); break;
    }
    if (transferred < 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, (uint64_t)transferred, 0);
}

#define HL_MACOS_VECTOR_WRAPPER(name, operation)                                                                       \
    static hl_host_result name(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count) {     \
        return hl_macos_file_vector(context, file, vectors, count, 0, operation);                                      \
    }
HL_MACOS_VECTOR_WRAPPER(hl_macos_file_readv, 0)
HL_MACOS_VECTOR_WRAPPER(hl_macos_file_writev, 1)
HL_MACOS_VECTOR_WRAPPER(hl_macos_file_appendv, 4)

static hl_host_result hl_macos_file_truncate(void *context, hl_host_handle file, uint64_t size) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    if (descriptor < 0 || size > INT64_MAX) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return ftruncate(descriptor, (off_t)size) == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_host_result hl_macos_file_sync(void *context, hl_host_handle file) {
    int descriptor = hl_macos_file_descriptor(context, file, 0);
    if (descriptor < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return fsync(descriptor) == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static int hl_macos_file_directory(hl_host_macos *host, hl_host_handle directory) {
    int descriptor = AT_FDCWD;
    if (directory == HL_HOST_HANDLE_CWD) return descriptor;
    pthread_mutex_lock(&host->lock);
    hl_macos_file *file = hl_macos_file_lookup(host, directory);
    descriptor = file != NULL ? file->descriptor : -1;
    pthread_mutex_unlock(&host->lock);
    return descriptor;
}

static hl_host_result hl_macos_file_rename(void *context, hl_host_handle old_directory, const char *old_path,
                                           size_t old_path_size, hl_host_handle new_directory, const char *new_path,
                                           size_t new_path_size) {
    hl_host_macos *host = context;
    char old_local[PATH_MAX];
    char new_local[PATH_MAX];
    int old_fd;
    int new_fd;
    if (old_path == NULL || new_path == NULL || old_path_size == 0 || new_path_size == 0 ||
        old_path_size >= sizeof(old_local) || new_path_size >= sizeof(new_local))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memcpy(old_local, old_path, old_path_size);
    old_local[old_path_size] = '\0';
    memcpy(new_local, new_path, new_path_size);
    new_local[new_path_size] = '\0';
    old_fd = hl_macos_file_directory(host, old_directory);
    new_fd = hl_macos_file_directory(host, new_directory);
    if ((old_fd < 0 && old_directory != HL_HOST_HANDLE_CWD) || (new_fd < 0 && new_directory != HL_HOST_HANDLE_CWD))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (renameat(old_fd, old_local, new_fd, new_local) != 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_file_unlink(void *context, hl_host_handle directory, const char *path,
                                           size_t path_size) {
    hl_host_macos *host = context;
    char local[PATH_MAX];
    int directory_fd;
    if (path == NULL || path_size == 0 || path_size >= sizeof(local))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memcpy(local, path, path_size);
    local[path_size] = '\0';
    directory_fd = hl_macos_file_directory(host, directory);
    if (directory_fd < 0 && directory != HL_HOST_HANDLE_CWD) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (unlinkat(directory_fd, local, 0) != 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_file_readv_at(void *context, hl_host_handle file, const hl_host_iovec *vectors,
                                             uint32_t count, uint64_t offset) {
    return hl_macos_file_vector(context, file, vectors, count, offset, 2);
}

static hl_host_result hl_macos_file_writev_at(void *context, hl_host_handle file, const hl_host_iovec *vectors,
                                              uint32_t count, uint64_t offset) {
    return hl_macos_file_vector(context, file, vectors, count, offset, 3);
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

static hl_host_result hl_macos_file_resolve_beneath(void *context, hl_host_handle root, const char *path,
                                                    size_t path_size, uint32_t policy,
                                                    hl_host_file_resolution *output) {
    hl_host_macos *host = context;
    hl_host_resolved_path resolved;
    hl_host_result parent;
    hl_host_result target = {HL_STATUS_OK, 0, HL_HOST_HANDLE_INVALID, 0};
    char local[PATH_MAX];
    int root_fd = hl_macos_file_descriptor(host, root, 0);
    /* Resolution is a metadata probe, never an I/O open.  O_NONBLOCK prevents a FIFO/device final
     * component from stalling the resolver before the Linux ABI can apply its actual open flags. */
    int target_flags = O_RDONLY | O_NONBLOCK;
    if (root_fd < 0 || output == NULL || path == NULL || path_size == 0 || path_size >= sizeof(local) ||
        (policy & ~(uint32_t)(HL_HOST_RESOLVE_NOFOLLOW_FINAL | HL_HOST_RESOLVE_NO_SYMLINKS |
                              HL_HOST_RESOLVE_ALLOW_MISSING)) != 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    memcpy(local, path, path_size);
    local[path_size] = '\0';
#ifdef O_SYMLINK
    if ((policy & HL_HOST_RESOLVE_NOFOLLOW_FINAL) != 0) target_flags = O_SYMLINK;
#endif
    if ((policy & HL_HOST_RESOLVE_ALLOW_MISSING) != 0) target_flags = -1;
    if (hl_host_resolve_beneath(root_fd, local, policy & (HL_HOST_RESOLVE_NOFOLLOW_FINAL | HL_HOST_RESOLVE_NO_SYMLINKS),
                                target_flags, &resolved) != 0)
        return hl_macos_errno();
    if (strlen(resolved.leaf) >= sizeof(output->final)) {
        hl_host_resolved_path_destroy(&resolved);
        return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
    }
    parent = hl_macos_file_register(host, resolved.parent_fd, -1, 0);
    if (parent.status != HL_STATUS_OK) {
        hl_host_resolved_path_destroy(&resolved);
        return parent;
    }
    resolved.parent_fd = -1;
    if (resolved.target_fd >= 0) {
        target = hl_macos_file_register(host, resolved.target_fd, -1, 0);
        if (target.status != HL_STATUS_OK) {
            (void)hl_macos_file_close(host, parent.value);
            hl_host_resolved_path_destroy(&resolved);
            return target;
        }
        resolved.target_fd = -1;
    }
    memset(output, 0, sizeof(*output));
    output->parent = parent.value;
    output->target = target.value;
    output->final_size = strlen(resolved.leaf);
    memcpy(output->final, resolved.leaf, output->final_size + 1);
    if (output->target != HL_HOST_HANDLE_INVALID) {
        hl_host_file_metadata metadata;
        if (hl_macos_file_metadata_get(host, output->target, &metadata).status == HL_STATUS_OK)
            output->target_type = metadata.type;
    }
    hl_host_resolved_path_destroy(&resolved);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_file_path(void *context, hl_host_handle handle, hl_host_bytes output) {
    hl_host_macos *host = context;
    char path[PATH_MAX];
    hl_macos_file *file;
    int error = 0;
    size_t length;
    if ((output.size != 0 && output.data == 0) || output.size > SIZE_MAX)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    file = hl_macos_file_lookup(host, handle);
    if (file == NULL)
        error = EBADF;
    else if (fcntl(file->descriptor, F_GETPATH, path) != 0)
        error = errno;
    pthread_mutex_unlock(&host->lock);
    if (error != 0) {
        errno = error;
        return hl_macos_errno();
    }
    length = strnlen(path, sizeof path);
    if (length > output.size) return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, length, 0);
    if (length != 0) memcpy(output.data, path, length);
    return hl_macos_result(HL_STATUS_OK, length, 0);
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
    file->shared = 0;
    file->descriptor = -1;
    file->append_descriptor = -1;
    pthread_mutex_unlock(&host->lock);
    if (close(descriptor) != 0) return hl_macos_errno();
    if (append_descriptor >= 0 && close(append_descriptor) != 0) return hl_macos_errno();
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_shared_create(void *context, uint64_t size, uint32_t flags) {
    hl_host_macos *host = context;
    char path[] = "/tmp/hl-engine-shared-XXXXXX";
    int descriptor;
    hl_host_result result;
    if (size > INT64_MAX || flags != 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    descriptor = mkstemp(path);
    if (descriptor < 0) return hl_macos_errno();
    if (unlink(path) != 0 || fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 || ftruncate(descriptor, (off_t)size) != 0) {
        hl_host_result error = hl_macos_errno();
        close(descriptor);
        return error;
    }
    result = hl_macos_file_register(host, descriptor, -1, 1);
    if (result.status != HL_STATUS_OK)
        close(descriptor);
    else
        result.detail = result.value;
    return result;
}

static hl_host_result hl_macos_shared_open(void *context, uint64_t identity, uint32_t flags) {
    hl_host_macos *host = context;
    hl_macos_file *source;
    int descriptor;
    int valid;
    hl_host_result result;
    if (flags != 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    source = hl_macos_file_lookup(host, identity);
    valid = source != NULL && source->shared;
    descriptor = valid ? fcntl(source->descriptor, F_DUPFD_CLOEXEC, 0) : -1;
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0) return valid ? hl_macos_errno() : hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    result = hl_macos_file_register(host, descriptor, -1, 1);
    if (result.status != HL_STATUS_OK)
        close(descriptor);
    else
        result.detail = identity;
    return result;
}

static hl_host_result hl_macos_shared_resize(void *context, hl_host_handle object, uint64_t size) {
    hl_host_macos *host = context;
    hl_macos_file *entry;
    int result;
    if (size > INT64_MAX) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    entry = hl_macos_file_lookup(host, object);
    if (entry == NULL || !entry->shared) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    result = ftruncate(entry->descriptor, (off_t)size);
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_macos_event *hl_macos_event_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    if (low == 0) return NULL;
    index = low - 1u;
    if (index >= HL_MACOS_EVENT_CAPACITY || !host->events[index].active ||
        host->events[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->events[index];
}

static hl_macos_counter *hl_macos_counter_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    if (low == 0) return NULL;
    index = low - 1u;
    if (index >= HL_MACOS_COUNTER_CAPACITY || !host->counters[index].active ||
        host->counters[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->counters[index];
}

static hl_macos_directory *hl_macos_directory_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    if (low == 0) return NULL;
    uint32_t index = low - 1u;
    if (index >= HL_MACOS_DIRECTORY_CAPACITY || !host->directories[index].active ||
        host->directories[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->directories[index];
}

static hl_host_result hl_macos_directory_register(hl_host_macos *host, hl_macos_directory_object *object) {
    hl_host_handle handle = HL_HOST_HANDLE_INVALID;
    pthread_mutex_lock(&host->lock);
    for (uint32_t index = 0; index < HL_MACOS_DIRECTORY_CAPACITY; ++index) {
        hl_macos_directory *directory = &host->directories[index];
        if (directory->active) continue;
        directory->generation++;
        if (directory->generation == 0) directory->generation = 1;
        directory->active = 1;
        directory->object = object;
        object->references++;
        handle = hl_macos_handle(index, directory->generation);
        break;
    }
    pthread_mutex_unlock(&host->lock);
    return handle == HL_HOST_HANDLE_INVALID ? hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0)
                                            : hl_macos_result(HL_STATUS_OK, handle, 0);
}

static hl_host_result hl_macos_directory_create(void *context) {
    hl_host_macos *host = context;
    hl_macos_directory_object *object = calloc(1, sizeof(*object));
    if (object == NULL) return hl_macos_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
    object->descriptor = kqueue();
    if (object->descriptor < 0) {
        free(object);
        return hl_macos_errno();
    }
    (void)fcntl(object->descriptor, F_SETFD, FD_CLOEXEC);
    hl_host_result result = hl_macos_directory_register(host, object);
    if (result.status != HL_STATUS_OK) {
        close(object->descriptor);
        free(object);
    }
    return result;
}

static hl_macos_directory_object *hl_macos_directory_object_get(hl_host_macos *host, hl_host_handle handle) {
    pthread_mutex_lock(&host->lock);
    hl_macos_directory *directory = hl_macos_directory_lookup(host, handle);
    hl_macos_directory_object *object = directory == NULL ? NULL : directory->object;
    pthread_mutex_unlock(&host->lock);
    return object;
}

static uint32_t hl_macos_directory_native(uint32_t interests) {
    uint32_t flags = 0;
    if ((interests & HL_HOST_DIRECTORY_ACCESS) != 0) flags |= NOTE_ATTRIB;
    if ((interests & HL_HOST_DIRECTORY_MODIFY) != 0) flags |= NOTE_WRITE | NOTE_EXTEND;
    if ((interests & (HL_HOST_DIRECTORY_CREATE | HL_HOST_DIRECTORY_DELETE)) != 0) flags |= NOTE_WRITE | NOTE_LINK;
    if ((interests & HL_HOST_DIRECTORY_RENAME) != 0) flags |= NOTE_RENAME;
    if ((interests & HL_HOST_DIRECTORY_ATTRIB) != 0) flags |= NOTE_ATTRIB;
    return flags == 0 ? NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND | NOTE_LINK : flags;
}

static hl_host_result hl_macos_directory_add(void *context, hl_host_handle instance, hl_host_handle file,
                                             uint64_t token, uint32_t interests) {
    hl_host_macos *host = context;
    hl_macos_directory_object *object = hl_macos_directory_object_get(host, instance);
    pthread_mutex_lock(&host->lock);
    hl_macos_file *file_entry = hl_macos_file_lookup(host, file);
    int descriptor = file_entry == NULL ? -1 : file_entry->descriptor;
    pthread_mutex_unlock(&host->lock);
    if (object == NULL || descriptor < 0 || token == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    for (uint32_t index = 0; index < HL_MACOS_DIRECTORY_WATCH_CAPACITY; ++index) {
        hl_macos_directory_watch *watch = &object->watches[index];
        if (watch->active && watch->token != token) continue;
        if (!watch->active || watch->token == token) {
            struct kevent change;
            uint16_t flags =
                (uint16_t)(EV_ADD | EV_CLEAR | ((interests & HL_HOST_DIRECTORY_ONESHOT) != 0 ? EV_ONESHOT : 0));
            EV_SET(&change, descriptor, EVFILT_VNODE, flags, hl_macos_directory_native(interests), 0,
                   (void *)(uintptr_t)token);
            if (kevent(object->descriptor, &change, 1, NULL, 0, NULL) != 0) return hl_macos_errno();
            *watch = (hl_macos_directory_watch){token, interests, descriptor, 1};
            return hl_macos_result(HL_STATUS_OK, 0, 0);
        }
    }
    return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
}

static hl_host_result hl_macos_directory_modify(void *context, hl_host_handle instance, uint64_t token,
                                                uint32_t interests) {
    hl_macos_directory_object *object = hl_macos_directory_object_get(context, instance);
    if (object == NULL || token == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    for (uint32_t index = 0; index < HL_MACOS_DIRECTORY_WATCH_CAPACITY; ++index) {
        hl_macos_directory_watch *watch = &object->watches[index];
        if (!watch->active || watch->token != token) continue;
        struct kevent change;
        uint16_t flags =
            (uint16_t)(EV_ADD | EV_CLEAR | ((interests & HL_HOST_DIRECTORY_ONESHOT) != 0 ? EV_ONESHOT : 0));
        EV_SET(&change, watch->descriptor, EVFILT_VNODE, flags, hl_macos_directory_native(interests), 0,
               (void *)(uintptr_t)token);
        if (kevent(object->descriptor, &change, 1, NULL, 0, NULL) != 0) return hl_macos_errno();
        watch->interests = interests;
        return hl_macos_result(HL_STATUS_OK, 0, 0);
    }
    return hl_macos_result(HL_STATUS_NOT_FOUND, 0, 0);
}

static hl_host_result hl_macos_directory_remove(void *context, hl_host_handle instance, uint64_t token) {
    hl_macos_directory_object *object = hl_macos_directory_object_get(context, instance);
    if (object == NULL || token == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    for (uint32_t index = 0; index < HL_MACOS_DIRECTORY_WATCH_CAPACITY; ++index) {
        hl_macos_directory_watch *watch = &object->watches[index];
        if (!watch->active || watch->token != token) continue;
        struct kevent change;
        EV_SET(&change, watch->descriptor, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
        if (kevent(object->descriptor, &change, 1, NULL, 0, NULL) != 0 && errno != ENOENT) return hl_macos_errno();
        watch->active = 0;
        return hl_macos_result(HL_STATUS_OK, 0, 0);
    }
    return hl_macos_result(HL_STATUS_NOT_FOUND, 0, 0);
}

static uint32_t hl_macos_directory_changes(uint32_t flags, uint32_t interests) {
    uint32_t changes = 0;
    if ((flags & (NOTE_WRITE | NOTE_EXTEND | NOTE_LINK)) != 0)
        changes |= interests & (HL_HOST_DIRECTORY_MODIFY | HL_HOST_DIRECTORY_CREATE | HL_HOST_DIRECTORY_DELETE);
    if ((flags & NOTE_ATTRIB) != 0) changes |= HL_HOST_DIRECTORY_ATTRIB;
    if ((flags & NOTE_DELETE) != 0) changes |= HL_HOST_DIRECTORY_DELETE;
    if ((flags & NOTE_RENAME) != 0) changes |= HL_HOST_DIRECTORY_RENAME;
    return changes;
}

static hl_host_result hl_macos_directory_read(void *context, hl_host_handle instance, hl_host_directory_record *records,
                                              uint32_t capacity) {
    hl_macos_directory_object *object = hl_macos_directory_object_get(context, instance);
    struct kevent native[64];
    struct timespec zero = {0, 0};
    if (object == NULL || records == NULL || capacity == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (capacity > 64) capacity = 64;
    int count = kevent(object->descriptor, NULL, 0, native, (int)capacity, &zero);
    if (count < 0) return hl_macos_errno();
    if (count == 0) return hl_macos_result(HL_STATUS_WOULD_BLOCK, 0, 0);
    for (int index = 0; index < count; ++index) {
        uint64_t token = (uint64_t)(uintptr_t)native[index].udata;
        uint32_t interests = 0;
        for (uint32_t watch_index = 0; watch_index < HL_MACOS_DIRECTORY_WATCH_CAPACITY; ++watch_index) {
            hl_macos_directory_watch *watch = &object->watches[watch_index];
            if (watch->active && watch->token == token) interests = watch->interests;
            if (watch->active && watch->token == token && (watch->interests & HL_HOST_DIRECTORY_ONESHOT) != 0) {
                watch->active = 0;
                interests |= HL_HOST_DIRECTORY_IGNORED;
            }
        }
        records[index] =
            (hl_host_directory_record){token, hl_macos_directory_changes(native[index].fflags, interests), 0};
        if ((interests & HL_HOST_DIRECTORY_IGNORED) != 0) records[index].changes |= HL_HOST_DIRECTORY_IGNORED;
    }
    return hl_macos_result(HL_STATUS_OK, (uint64_t)count, 0);
}

static hl_host_result hl_macos_directory_duplicate(void *context, hl_host_handle instance) {
    hl_host_macos *host = context;
    hl_macos_directory_object *object = hl_macos_directory_object_get(host, instance);
    if (object == NULL) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return hl_macos_directory_register(host, object);
}

static hl_host_result hl_macos_directory_close(void *context, hl_host_handle instance) {
    hl_host_macos *host = context;
    hl_macos_directory_object *object;
    int final = 0;
    pthread_mutex_lock(&host->lock);
    hl_macos_directory *directory = hl_macos_directory_lookup(host, instance);
    if (directory == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    object = directory->object;
    directory->active = 0;
    directory->object = NULL;
    final = --object->references == 0;
    pthread_mutex_unlock(&host->lock);
    if (final) {
        close(object->descriptor);
        free(object);
    }
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_macos_transfer *hl_macos_transfer_lookup(hl_host_macos *host, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    if (low == 0) return NULL;
    index = low - 1u;
    if (index >= HL_MACOS_TRANSFER_CAPACITY || !host->transfers[index].active ||
        host->transfers[index].generation != (uint32_t)(handle >> 32))
        return NULL;
    return &host->transfers[index];
}

static hl_host_result hl_macos_transfer_register(hl_host_macos *host, int descriptor) {
    uint32_t index;
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_TRANSFER_CAPACITY; ++index) {
        hl_macos_transfer *transfer = &host->transfers[index];
        if (transfer->active) continue;
        transfer->generation++;
        if (transfer->generation == 0) transfer->generation = 1;
        transfer->active = 1;
        transfer->descriptor = descriptor;
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_OK, hl_macos_handle(index, transfer->generation), 0);
    }
    pthread_mutex_unlock(&host->lock);
    return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
}

typedef struct hl_macos_transfer_wire {
    uint32_t data_size;
    uint32_t attachment_count;
    uint32_t flags[HL_HOST_TRANSFER_MAX_ATTACHMENTS];
    uint32_t rights[HL_HOST_TRANSFER_MAX_ATTACHMENTS];
    uint8_t data[HL_HOST_TRANSFER_MAX_DATA];
} hl_macos_transfer_wire;

static hl_host_result hl_macos_counter_register(hl_host_macos *host, hl_macos_counter_object *object, uint32_t rights);

static hl_host_result hl_macos_transfer_channel_pair(void *context) {
    hl_host_macos *host = context;
    int pair[2];
    hl_host_result first;
    hl_host_result second;
    /* Darwin does not provide AF_UNIX SOCK_SEQPACKET; datagrams retain message boundaries. */
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) return hl_macos_errno();
    (void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    first = hl_macos_transfer_register(host, pair[0]);
    if (first.status != HL_STATUS_OK) {
        close(pair[0]);
        close(pair[1]);
        return first;
    }
    second = hl_macos_transfer_register(host, pair[1]);
    if (second.status != HL_STATUS_OK) {
        close(pair[1]);
        return second;
    }
    return hl_macos_result(HL_STATUS_OK, first.value, second.value);
}

static hl_host_result hl_macos_transfer_send(void *context, hl_host_handle channel, hl_host_const_bytes data,
                                             const hl_host_transfer_attachment *attachments, uint32_t count) {
    hl_host_macos *host = context;
    hl_macos_transfer_wire wire = {0};
    uint8_t control[CMSG_SPACE(sizeof(int) * HL_HOST_TRANSFER_MAX_ATTACHMENTS * 3u)] = {0};
    struct iovec vector = {&wire, sizeof(wire)};
    struct msghdr message = {0};
    int descriptors[HL_HOST_TRANSFER_MAX_ATTACHMENTS * 3u];
    int channel_fd = -1;
    uint32_t index;
    if (data.size > HL_HOST_TRANSFER_MAX_DATA || (data.size != 0 && data.data == NULL) ||
        count > HL_HOST_TRANSFER_MAX_ATTACHMENTS || (count != 0 && attachments == NULL))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    {
        hl_macos_transfer *transfer = hl_macos_transfer_lookup(host, channel);
        if (transfer != NULL) channel_fd = transfer->descriptor;
    }
    for (index = 0; index < count && channel_fd >= 0; ++index) {
        hl_macos_counter *counter = hl_macos_counter_lookup(host, attachments[index].object);
        uint32_t valid =
            HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WRITE | HL_HOST_TRANSFER_WAIT | HL_HOST_TRANSFER_CONTROL;
        if (counter == NULL || attachments[index].kind != HL_HOST_TRANSFER_KIND_COUNTER ||
            (attachments[index].rights & ~valid) != 0 ||
            (attachments[index].rights & counter->rights) != attachments[index].rights) {
            channel_fd = -1;
            break;
        }
        descriptors[index * 3u] = counter->object->backing;
        descriptors[index * 3u + 1u] = counter->object->readable;
        descriptors[index * 3u + 2u] = counter->object->signal;
        wire.flags[index] = counter->object->shared->flags;
        wire.rights[index] = attachments[index].rights;
    }
    pthread_mutex_unlock(&host->lock);
    if (channel_fd < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    wire.data_size = (uint32_t)data.size;
    wire.attachment_count = count;
    if (data.size != 0) memcpy(wire.data, data.data, data.size);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (count != 0) {
        struct cmsghdr *header;
        size_t descriptor_bytes = sizeof(int) * count * 3u;
        message.msg_control = control;
        message.msg_controllen = (socklen_t)CMSG_SPACE(descriptor_bytes);
        header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = (socklen_t)CMSG_LEN(descriptor_bytes);
        memcpy(CMSG_DATA(header), descriptors, descriptor_bytes);
    }
    return sendmsg(channel_fd, &message, 0) == (ssize_t)sizeof(wire) ? hl_macos_result(HL_STATUS_OK, 0, 0)
                                                                     : hl_macos_errno();
}

static hl_host_result hl_macos_transfer_receive(void *context, hl_host_handle channel, hl_host_bytes data,
                                                hl_host_transfer_attachment *attachments, uint32_t capacity) {
    hl_host_macos *host = context;
    hl_macos_transfer_wire wire;
    uint8_t control[CMSG_SPACE(sizeof(int) * HL_HOST_TRANSFER_MAX_ATTACHMENTS * 3u)] = {0};
    struct iovec vector = {&wire, sizeof(wire)};
    struct msghdr message = {0};
    int received[HL_HOST_TRANSFER_MAX_ATTACHMENTS * 3u];
    int channel_fd = -1;
    uint32_t index;
    ssize_t bytes;
    pthread_mutex_lock(&host->lock);
    {
        hl_macos_transfer *transfer = hl_macos_transfer_lookup(host, channel);
        if (transfer != NULL) channel_fd = transfer->descriptor;
    }
    pthread_mutex_unlock(&host->lock);
    if (channel_fd < 0 || (data.size != 0 && data.data == NULL) || (capacity != 0 && attachments == NULL))
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    bytes = recv(channel_fd, &wire, sizeof(wire), MSG_PEEK);
    if (bytes < 0) return hl_macos_errno();
    if (bytes != (ssize_t)sizeof(wire) || wire.data_size > HL_HOST_TRANSFER_MAX_DATA ||
        wire.attachment_count > HL_HOST_TRANSFER_MAX_ATTACHMENTS)
        return hl_macos_result(HL_STATUS_CORRUPT, 0, 0);
    if (wire.data_size > data.size || wire.attachment_count > capacity)
        return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, wire.data_size, wire.attachment_count);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    bytes = recvmsg(channel_fd, &message, 0);
    if (bytes != (ssize_t)sizeof(wire)) return bytes < 0 ? hl_macos_errno() : hl_macos_result(HL_STATUS_CORRUPT, 0, 0);
    if (wire.attachment_count != 0) {
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        size_t descriptor_bytes = sizeof(int) * wire.attachment_count * 3u;
        if (header == NULL || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len != CMSG_LEN(descriptor_bytes))
            return hl_macos_result(HL_STATUS_CORRUPT, 0, 0);
        memcpy(received, CMSG_DATA(header), descriptor_bytes);
    }
    for (index = 0; index < wire.attachment_count; ++index) {
        hl_macos_counter_object *object = calloc(1, sizeof(*object));
        hl_host_result installed;
        if (object == NULL) return hl_macos_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
        object->backing = received[index * 3u];
        object->readable = received[index * 3u + 1u];
        object->signal = received[index * 3u + 2u];
        object->shared = mmap(NULL, sizeof(*object->shared), PROT_READ | PROT_WRITE, MAP_SHARED, object->backing, 0);
        if (object->shared == MAP_FAILED) {
            free(object);
            return hl_macos_errno();
        }
        installed = hl_macos_counter_register(host, object, wire.rights[index]);
        if (installed.status != HL_STATUS_OK) return installed;
        attachments[index] =
            (hl_host_transfer_attachment){installed.value, HL_HOST_TRANSFER_KIND_COUNTER, wire.rights[index]};
    }
    if (wire.data_size != 0) memcpy(data.data, wire.data, wire.data_size);
    return hl_macos_result(HL_STATUS_OK, wire.data_size, wire.attachment_count);
}

static hl_host_result hl_macos_transfer_close(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    hl_macos_transfer *transfer;
    int descriptor;
    pthread_mutex_lock(&host->lock);
    transfer = hl_macos_transfer_lookup(host, handle);
    if (transfer == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    descriptor = transfer->descriptor;
    transfer->active = 0;
    transfer->descriptor = -1;
    pthread_mutex_unlock(&host->lock);
    return close(descriptor) == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_host_result hl_macos_transfer_duplicate(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    hl_macos_transfer *transfer;
    int descriptor;
    pthread_mutex_lock(&host->lock);
    transfer = hl_macos_transfer_lookup(host, handle);
    descriptor = transfer == NULL ? -1 : dup(transfer->descriptor);
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0) return hl_macos_errno();
    {
        hl_host_result result = hl_macos_transfer_register(host, descriptor);
        if (result.status != HL_STATUS_OK) close(descriptor);
        return result;
    }
}

static hl_host_result hl_macos_counter_register(hl_host_macos *host, hl_macos_counter_object *object, uint32_t rights) {
    uint32_t index;
    hl_host_handle handle = HL_HOST_HANDLE_INVALID;
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_COUNTER_CAPACITY; ++index) {
        hl_macos_counter *counter = &host->counters[index];
        if (counter->active) continue;
        counter->generation++;
        if (counter->generation == 0) counter->generation = 1;
        counter->active = 1;
        counter->object = object;
        counter->rights = rights;
        object->shared->references++;
        handle = hl_macos_handle(index, counter->generation);
        break;
    }
    pthread_mutex_unlock(&host->lock);
    return handle == HL_HOST_HANDLE_INVALID ? hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0)
                                            : hl_macos_result(HL_STATUS_OK, handle, 0);
}

static hl_host_result hl_macos_counter_create(void *context, uint64_t initial, uint32_t flags) {
    hl_host_macos *host = context;
    hl_macos_counter_object *object;
    int descriptors[2];
    hl_host_result result;
    if (initial == UINT64_MAX || (flags & ~(uint32_t)(HL_HOST_COUNTER_SEMAPHORE | HL_HOST_COUNTER_NONBLOCK)) != 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (pipe(descriptors) != 0) return hl_macos_errno();
    (void)fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(descriptors[1], F_SETFL, O_NONBLOCK);
    (void)fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    object = calloc(1, sizeof(*object));
    if (object == NULL) {
        close(descriptors[0]);
        close(descriptors[1]);
        return hl_macos_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
    }
    {
        char name[64];
        static uint32_t sequence;
        snprintf(name, sizeof(name), "/hl-counter-%ld-%u", (long)getpid(), ++sequence);
        object->backing = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (object->backing >= 0) shm_unlink(name);
        if (object->backing < 0 || ftruncate(object->backing, (off_t)sizeof(*object->shared)) != 0) {
            if (object->backing >= 0) close(object->backing);
            close(descriptors[0]);
            close(descriptors[1]);
            free(object);
            return hl_macos_errno();
        }
        object->shared = mmap(NULL, sizeof(*object->shared), PROT_READ | PROT_WRITE, MAP_SHARED, object->backing, 0);
        if (object->shared == MAP_FAILED) {
            close(object->backing);
            close(descriptors[0]);
            close(descriptors[1]);
            free(object);
            return hl_macos_errno();
        }
    }
    {
        pthread_mutexattr_t attributes;
        int initialized = pthread_mutexattr_init(&attributes) == 0;
        if (!initialized || pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) != 0 ||
            pthread_mutex_init(&object->shared->lock, &attributes) != 0) {
            if (initialized) pthread_mutexattr_destroy(&attributes);
            close(descriptors[0]);
            close(descriptors[1]);
            munmap(object->shared, sizeof(*object->shared));
            close(object->backing);
            free(object);
            return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
        }
        pthread_mutexattr_destroy(&attributes);
    }
    object->shared->value = initial;
    object->shared->flags = flags;
    object->readable = descriptors[0];
    object->signal = descriptors[1];
    if (initial != 0) {
        const uint8_t byte = 1;
        (void)write(object->signal, &byte, 1);
    }
    result = hl_macos_counter_register(host, object,
                                       HL_HOST_TRANSFER_READ | HL_HOST_TRANSFER_WRITE | HL_HOST_TRANSFER_WAIT |
                                           HL_HOST_TRANSFER_CONTROL);
    if (result.status != HL_STATUS_OK) {
        pthread_mutex_destroy(&object->shared->lock);
        close(object->readable);
        close(object->signal);
        munmap(object->shared, sizeof(*object->shared));
        close(object->backing);
        free(object);
    }
    return result;
}

static hl_macos_counter_object *hl_macos_counter_object_get(hl_host_macos *host, hl_host_handle handle) {
    hl_macos_counter *counter;
    hl_macos_counter_object *object;
    pthread_mutex_lock(&host->lock);
    counter = hl_macos_counter_lookup(host, handle);
    object = counter == NULL ? NULL : counter->object;
    pthread_mutex_unlock(&host->lock);
    return object;
}

static hl_macos_counter_object *hl_macos_counter_object_with_right(void *context, hl_host_handle handle, uint32_t right,
                                                                   hl_status *status) {
    hl_host_macos *host = context;
    hl_macos_counter *counter;
    hl_macos_counter_object *object = NULL;
    pthread_mutex_lock(&host->lock);
    counter = hl_macos_counter_lookup(host, handle);
    if (counter == NULL)
        *status = HL_STATUS_INVALID_ARGUMENT;
    else if ((counter->rights & right) == 0)
        *status = HL_STATUS_PERMISSION_DENIED;
    else {
        object = counter->object;
        *status = HL_STATUS_OK;
    }
    pthread_mutex_unlock(&host->lock);
    return object;
}

static hl_host_result hl_macos_counter_read(void *context, hl_host_handle counter) {
    hl_status status;
    hl_macos_counter_object *object =
        hl_macos_counter_object_with_right(context, counter, HL_HOST_TRANSFER_READ, &status);
    uint64_t value;
    uint8_t bytes[32];
    if (object == NULL) return hl_macos_result(status, 0, 0);
    for (;;) {
        pthread_mutex_lock(&object->shared->lock);
        if (object->shared->value != 0) break;
        if ((object->shared->flags & HL_HOST_COUNTER_NONBLOCK) != 0) {
            pthread_mutex_unlock(&object->shared->lock);
            return hl_macos_result(HL_STATUS_WOULD_BLOCK, 0, 0);
        }
        pthread_mutex_unlock(&object->shared->lock);
        poll(&(struct pollfd){object->readable, POLLIN, 0}, 1, -1);
    }
    value = (object->shared->flags & HL_HOST_COUNTER_SEMAPHORE) != 0 ? 1 : object->shared->value;
    object->shared->value -= value;
    if (object->shared->value == 0)
        while (read(object->readable, bytes, sizeof(bytes)) > 0) {}
    pthread_mutex_unlock(&object->shared->lock);
    return hl_macos_result(HL_STATUS_OK, value, 0);
}

static hl_host_result hl_macos_counter_write(void *context, hl_host_handle counter, uint64_t value) {
    hl_status status;
    hl_macos_counter_object *object =
        hl_macos_counter_object_with_right(context, counter, HL_HOST_TRANSFER_WRITE, &status);
    uint8_t byte = 1;
    if (object == NULL) return hl_macos_result(status, 0, 0);
    if (value == UINT64_MAX || value == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&object->shared->lock);
    if (value > UINT64_MAX - 1u - object->shared->value) {
        pthread_mutex_unlock(&object->shared->lock);
        return hl_macos_result(HL_STATUS_WOULD_BLOCK, 0, 0);
    }
    if (object->shared->value == 0) (void)write(object->signal, &byte, 1);
    object->shared->value += value;
    pthread_mutex_unlock(&object->shared->lock);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_counter_get_flags(void *context, hl_host_handle counter) {
    hl_status status;
    hl_macos_counter_object *object =
        hl_macos_counter_object_with_right(context, counter, HL_HOST_TRANSFER_CONTROL, &status);
    uint32_t flags;
    if (object == NULL) return hl_macos_result(status, 0, 0);
    pthread_mutex_lock(&object->shared->lock);
    flags = object->shared->flags;
    pthread_mutex_unlock(&object->shared->lock);
    return hl_macos_result(HL_STATUS_OK, flags, 0);
}

static hl_host_result hl_macos_counter_set_flags(void *context, hl_host_handle counter, uint32_t flags) {
    hl_status status;
    hl_macos_counter_object *object =
        hl_macos_counter_object_with_right(context, counter, HL_HOST_TRANSFER_CONTROL, &status);
    if (object == NULL) return hl_macos_result(status, 0, 0);
    if ((flags & ~(uint32_t)(HL_HOST_COUNTER_SEMAPHORE | HL_HOST_COUNTER_NONBLOCK)) != 0)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&object->shared->lock);
    if ((object->shared->flags & HL_HOST_COUNTER_SEMAPHORE) != (flags & HL_HOST_COUNTER_SEMAPHORE)) {
        pthread_mutex_unlock(&object->shared->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    object->shared->flags = flags;
    pthread_mutex_unlock(&object->shared->lock);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_counter_duplicate(void *context, hl_host_handle counter) {
    hl_host_macos *host = context;
    hl_macos_counter_object *object = hl_macos_counter_object_get(host, counter);
    if (object == NULL) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    {
        uint32_t rights;
        pthread_mutex_lock(&host->lock);
        rights = hl_macos_counter_lookup(host, counter)->rights;
        pthread_mutex_unlock(&host->lock);
        return hl_macos_counter_register(host, object, rights);
    }
}

static hl_host_result hl_macos_counter_readiness(void *context, hl_host_handle counter, uint32_t interests) {
    hl_host_macos *host = context;
    hl_macos_counter *entry;
    struct pollfd descriptor;
    int result;
    if ((interests & ~(uint32_t)HL_HOST_READY_READ) != 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    entry = hl_macos_counter_lookup(host, counter);
    if (entry != NULL && (entry->rights & HL_HOST_TRANSFER_WAIT) == 0) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_PERMISSION_DENIED, 0, 0);
    }
    descriptor = (struct pollfd){entry == NULL ? -1 : entry->object->readable, POLLIN, 0};
    pthread_mutex_unlock(&host->lock);
    if (descriptor.fd < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    result = poll(&descriptor, 1, 0);
    return result < 0 ? hl_macos_errno()
                      : hl_macos_result(HL_STATUS_OK, result == 1 ? HL_HOST_READY_READ & interests : 0, 0);
}

static void *hl_macos_counter_subscription_main(void *opaque) {
    hl_macos_counter_subscription *subscription = opaque;
    int notified = 0;
    for (;;) {
        struct pollfd descriptors[2] = {{subscription->descriptor, POLLIN, 0}, {subscription->wake[0], POLLIN, 0}};
        int result = poll(descriptors, 2, notified ? 1 : -1);
        if (result < 0 && errno == EINTR) continue;
        if (descriptors[1].revents != 0) break;
        if (descriptors[0].revents & POLLIN) {
            if (!notified) subscription->notify(subscription->observer, subscription->token);
            notified = 1;
        } else if (result == 0) {
            struct pollfd probe = {subscription->descriptor, POLLIN, 0};
            notified = poll(&probe, 1, 0) == 1;
        }
    }
    return NULL;
}

static hl_host_result hl_macos_counter_subscribe(void *context, hl_host_handle counter,
                                                 void (*notify)(void *, uint64_t), void *observer, uint64_t token) {
    hl_host_macos *host = context;
    hl_macos_counter *entry;
    hl_macos_counter_subscription *subscription = NULL;
    uint32_t index;
    int descriptor;
    if (notify == NULL || token == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    entry = hl_macos_counter_lookup(host, counter);
    if (entry != NULL && (entry->rights & HL_HOST_TRANSFER_WAIT) == 0) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_PERMISSION_DENIED, 0, 0);
    }
    descriptor = entry == NULL ? -1 : dup(entry->object->readable);
    for (index = 0; descriptor >= 0 && index < HL_MACOS_COUNTER_SUBSCRIPTIONS; ++index)
        if (!host->counter_subscriptions[index].active) {
            subscription = &host->counter_subscriptions[index];
            break;
        }
    if (subscription != NULL) {
        subscription->generation++;
        if (subscription->generation == 0) subscription->generation = 1;
        subscription->active = 1;
        subscription->counter = counter;
        subscription->descriptor = descriptor;
        subscription->notify = notify;
        subscription->observer = observer;
        subscription->token = token;
        subscription->wake[0] = -1;
        subscription->wake[1] = -1;
    }
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (subscription == NULL) {
        close(descriptor);
        return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
    }
    if (pipe(subscription->wake) != 0 ||
        pthread_create(&subscription->thread, NULL, hl_macos_counter_subscription_main, subscription) != 0) {
        if (subscription->wake[0] >= 0) close(subscription->wake[0]);
        if (subscription->wake[1] >= 0) close(subscription->wake[1]);
        close(descriptor);
        pthread_mutex_lock(&host->lock);
        subscription->active = 0;
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    }
    return hl_macos_result(HL_STATUS_OK, ((uint64_t)subscription->generation << 32) | (uint64_t)(index + 1u), 0);
}

static hl_host_result hl_macos_counter_unsubscribe(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    uint32_t low = (uint32_t)handle;
    hl_macos_counter_subscription *subscription;
    uint8_t byte = 1;
    if (low == 0 || low > HL_MACOS_COUNTER_SUBSCRIPTIONS) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    subscription = &host->counter_subscriptions[low - 1u];
    if (!subscription->active || subscription->generation != (uint32_t)(handle >> 32)) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    subscription->active = 0;
    pthread_mutex_unlock(&host->lock);
    (void)write(subscription->wake[1], &byte, 1);
    (void)pthread_join(subscription->thread, NULL);
    close(subscription->wake[0]);
    close(subscription->wake[1]);
    close(subscription->descriptor);
    subscription->counter = HL_HOST_HANDLE_INVALID;
    subscription->notify = NULL;
    subscription->observer = NULL;
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static void hl_macos_counter_unsubscribe_all(hl_host_macos *host, hl_host_handle counter) {
    uint32_t index;
    for (index = 0; index < HL_MACOS_COUNTER_SUBSCRIPTIONS; ++index) {
        hl_host_handle subscription = HL_HOST_HANDLE_INVALID;
        pthread_mutex_lock(&host->lock);
        if (host->counter_subscriptions[index].active && host->counter_subscriptions[index].counter == counter)
            subscription = ((uint64_t)host->counter_subscriptions[index].generation << 32) | (uint64_t)(index + 1u);
        pthread_mutex_unlock(&host->lock);
        if (subscription != HL_HOST_HANDLE_INVALID) (void)hl_macos_counter_unsubscribe(host, subscription);
    }
}

static hl_host_result hl_macos_counter_close(void *context, hl_host_handle handle) {
    hl_host_macos *host = context;
    hl_macos_counter *counter;
    hl_macos_counter_object *object;
    int final;
    hl_macos_counter_unsubscribe_all(host, handle);
    pthread_mutex_lock(&host->lock);
    counter = hl_macos_counter_lookup(host, handle);
    if (counter == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    object = counter->object;
    counter->active = 0;
    counter->object = NULL;
    counter->rights = 0;
    final = --object->shared->references == 0;
    pthread_mutex_unlock(&host->lock);
    if (final) {
        close(object->readable);
        close(object->signal);
        /* A descriptor already queued through SCM_RIGHTS may still map this object. */
        munmap(object->shared, sizeof(*object->shared));
        close(object->backing);
        free(object);
    }
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_event_create(void *context) {
    hl_host_macos *host = context;
    struct kevent wake;
    hl_host_handle handle = HL_HOST_HANDLE_INVALID;
    uint32_t index;
    int descriptor = kqueue();
    if (descriptor < 0) return hl_macos_errno();
    EV_SET(&wake, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(descriptor, &wake, 1, NULL, 0, NULL) != 0) {
        hl_host_result error = hl_macos_errno();
        close(descriptor);
        return error;
    }
    pthread_mutex_lock(&host->lock);
    for (index = 0; index < HL_MACOS_EVENT_CAPACITY; ++index) {
        hl_macos_event *event = &host->events[index];
        if (event->active) continue;
        event->generation++;
        if (event->generation == 0) event->generation = 1;
        event->active = 1;
        event->descriptor = descriptor;
        memset(event->timers, 0, sizeof(event->timers));
        handle = hl_macos_handle(index, event->generation);
        break;
    }
    pthread_mutex_unlock(&host->lock);
    if (handle == HL_HOST_HANDLE_INVALID) {
        close(descriptor);
        return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
    }
    return hl_macos_result(HL_STATUS_OK, handle, 0);
}

static hl_macos_timer *hl_macos_event_timer(hl_macos_event *event, uint64_t token) {
    uint32_t index;
    for (index = 0; index < HL_MACOS_TIMER_CAPACITY; ++index)
        if (event->timers[index].active && event->timers[index].token == token) return &event->timers[index];
    return NULL;
}

static hl_host_result hl_macos_event_control(void *context, hl_host_handle pollset, uint32_t operation,
                                             hl_host_handle object_handle, uint64_t token, uint32_t interests) {
    hl_host_macos *host = context;
    hl_macos_event *event;
    hl_macos_counter *counter;
    hl_macos_directory *directory;
    hl_macos_transfer *transfer;
    struct kevent changes[2];
    int count = 0;
    int descriptor;
    uint16_t flags;
    pthread_mutex_lock(&host->lock);
    event = hl_macos_event_lookup(host, pollset);
    counter = hl_macos_counter_lookup(host, object_handle);
    directory = hl_macos_directory_lookup(host, object_handle);
    transfer = hl_macos_transfer_lookup(host, object_handle);
    descriptor = event == NULL || (counter == NULL && directory == NULL && transfer == NULL) ? -1 : event->descriptor;
    if (descriptor >= 0 && counter != NULL) object_handle = (hl_host_handle)counter->object->readable;
    if (descriptor >= 0 && directory != NULL) object_handle = (hl_host_handle)directory->object->descriptor;
    if (descriptor >= 0 && transfer != NULL) object_handle = (hl_host_handle)transfer->descriptor;
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0 || token == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (operation == HL_HOST_EVENT_DELETE)
        flags = EV_DELETE;
    else if (operation == HL_HOST_EVENT_ADD || operation == HL_HOST_EVENT_MODIFY)
        flags = (uint16_t)(EV_ADD | EV_ENABLE | ((interests & HL_HOST_READY_EDGE) ? EV_CLEAR : 0) |
                           ((interests & HL_HOST_READY_ONESHOT) ? EV_ONESHOT : 0));
    else
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if ((interests & HL_HOST_READY_READ) != 0 || operation == HL_HOST_EVENT_DELETE)
        EV_SET(&changes[count++], (uintptr_t)object_handle, EVFILT_READ, flags, 0, 0, (void *)(uintptr_t)token);
    if (count == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return kevent(descriptor, changes, count, NULL, 0, NULL) == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0)
                                                                  : hl_macos_errno();
}

static int hl_macos_event_submit_timer(int descriptor, uint64_t token, uint64_t delay_ns) {
    struct kevent change;
    if (delay_ns == 0) delay_ns = 1;
    if (delay_ns > INT64_MAX) delay_ns = INT64_MAX;
    EV_SET(&change, (uintptr_t)token, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS, (intptr_t)delay_ns,
           (void *)(uintptr_t)token);
    return kevent(descriptor, &change, 1, NULL, 0, NULL);
}

static hl_host_result hl_macos_event_arm_timer(void *context, hl_host_handle pollset, uint64_t token,
                                               uint64_t deadline_ns, uint64_t interval_ns) {
    hl_host_macos *host = context;
    hl_macos_event *event;
    hl_macos_timer *timer;
    uint32_t index;
    uint64_t now;
    int result;
    if (token == 0 || deadline_ns == HL_HOST_DEADLINE_INFINITE)
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    event = hl_macos_event_lookup(host, pollset);
    if (event == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    timer = hl_macos_event_timer(event, token);
    if (timer == NULL) {
        for (index = 0; index < HL_MACOS_TIMER_CAPACITY; ++index)
            if (!event->timers[index].active) {
                timer = &event->timers[index];
                break;
            }
    }
    if (timer == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
    }
    now = hl_macos_monotonic_value();
    result = hl_macos_event_submit_timer(event->descriptor, token, deadline_ns > now ? deadline_ns - now : 1);
    if (result == 0) {
        timer->active = 1;
        timer->token = token;
        timer->interval_ns = interval_ns;
    }
    pthread_mutex_unlock(&host->lock);
    return result == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_host_result hl_macos_event_disarm_timer(void *context, hl_host_handle pollset, uint64_t token) {
    hl_host_macos *host = context;
    hl_macos_event *event;
    hl_macos_timer *timer;
    struct kevent change;
    if (token == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&host->lock);
    event = hl_macos_event_lookup(host, pollset);
    timer = event == NULL ? NULL : hl_macos_event_timer(event, token);
    if (timer == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_NOT_FOUND, 0, 0);
    }
    EV_SET(&change, (uintptr_t)token, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    (void)kevent(event->descriptor, &change, 1, NULL, 0, NULL);
    memset(timer, 0, sizeof(*timer));
    pthread_mutex_unlock(&host->lock);
    return hl_macos_result(HL_STATUS_OK, 0, 0);
}

static hl_host_result hl_macos_event_wait(void *context, hl_host_handle pollset, hl_host_event_record *events,
                                          size_t event_capacity, uint64_t deadline_ns) {
    hl_host_macos *host = context;
    struct kevent native[64];
    struct timespec timeout;
    struct timespec *timeout_pointer = NULL;
    hl_macos_event *event;
    int descriptor;
    int count;
    int index;
    size_t output_count = 0;
    if (events == NULL || event_capacity == 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (event_capacity > HL_ARRAY_COUNT(native)) event_capacity = HL_ARRAY_COUNT(native);
    pthread_mutex_lock(&host->lock);
    event = hl_macos_event_lookup(host, pollset);
    descriptor = event == NULL ? -1 : event->descriptor;
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (deadline_ns != HL_HOST_DEADLINE_INFINITE) {
        uint64_t now = hl_macos_monotonic_value();
        uint64_t remaining = deadline_ns > now ? deadline_ns - now : 0;
        timeout.tv_sec = (time_t)(remaining / UINT64_C(1000000000));
        timeout.tv_nsec = (long)(remaining % UINT64_C(1000000000));
        timeout_pointer = &timeout;
    }
    count = kevent(descriptor, NULL, 0, native, (int)event_capacity, timeout_pointer);
    if (count < 0) return hl_macos_errno();
    for (index = 0; index < count; ++index) {
        uint32_t readiness = 0;
        uint64_t token = (uint64_t)(uintptr_t)native[index].udata;
        if (native[index].filter == EVFILT_USER) continue;
        if (native[index].filter == EVFILT_READ) readiness |= HL_HOST_READY_READ;
        if (native[index].filter == EVFILT_WRITE) readiness |= HL_HOST_READY_WRITE;
        if ((native[index].flags & EV_ERROR) != 0) readiness |= HL_HOST_READY_ERROR;
        if ((native[index].flags & EV_EOF) != 0) readiness |= HL_HOST_READY_HANGUP;
        if (native[index].filter == EVFILT_TIMER) {
            readiness |= HL_HOST_READY_TIMER;
            token = (uint64_t)native[index].ident;
            pthread_mutex_lock(&host->lock);
            event = hl_macos_event_lookup(host, pollset);
            hl_macos_timer *timer = event == NULL ? NULL : hl_macos_event_timer(event, token);
            if (timer != NULL && timer->interval_ns != 0)
                (void)hl_macos_event_submit_timer(event->descriptor, token, timer->interval_ns);
            else if (timer != NULL)
                memset(timer, 0, sizeof(*timer));
            pthread_mutex_unlock(&host->lock);
        }
        events[output_count++] = (hl_host_event_record){token, readiness, 0};
    }
    return hl_macos_result(HL_STATUS_OK, output_count, 0);
}

static hl_host_result hl_macos_event_wake(void *context, hl_host_handle pollset) {
    hl_host_macos *host = context;
    hl_macos_event *event;
    struct kevent trigger;
    int descriptor;
    pthread_mutex_lock(&host->lock);
    event = hl_macos_event_lookup(host, pollset);
    descriptor = event == NULL ? -1 : event->descriptor;
    pthread_mutex_unlock(&host->lock);
    if (descriptor < 0) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    EV_SET(&trigger, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    return kevent(descriptor, &trigger, 1, NULL, 0, NULL) == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
}

static hl_host_result hl_macos_event_close(void *context, hl_host_handle pollset) {
    hl_host_macos *host = context;
    hl_macos_event *event;
    int descriptor;
    pthread_mutex_lock(&host->lock);
    event = hl_macos_event_lookup(host, pollset);
    if (event == NULL) {
        pthread_mutex_unlock(&host->lock);
        return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    descriptor = event->descriptor;
    event->active = 0;
    event->descriptor = -1;
    memset(event->timers, 0, sizeof(event->timers));
    pthread_mutex_unlock(&host->lock);
    return close(descriptor) == 0 ? hl_macos_result(HL_STATUS_OK, 0, 0) : hl_macos_errno();
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

static hl_host_result hl_macos_process_spawn_mode(void *context, hl_host_process_entry entry, void *entry_context,
                                                  int prepared) {
    hl_host_macos *host = context;
    hl_host_handle handle = 0;
    uint32_t index;
    pid_t pid;
    int fork_error;
    if (entry == NULL) return hl_macos_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (!prepared && pthread_mutex_lock(&host->fork_gate) != 0)
        return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    pid = fork();
    fork_error = errno;
    if (prepared) {
        /* Only the parent retains its watcher threads.  The child must discard
         * inherited subscription slots before object teardown can join them. */
        hl_host_result completed = pid == 0 ? hl_macos_fork_child(host) : hl_macos_fork_complete(host);
        if (completed.status != HL_STATUS_OK) {
            if (pid > 0) {
                int status;
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
            if (pid == 0) _exit(255);
            return completed;
        }
    } else if (pthread_mutex_unlock(&host->fork_gate) != 0) {
        if (pid > 0) {
            int status;
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        if (pid == 0) _exit(255);
        return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    }
    if (pid < 0) {
        errno = fork_error;
        return hl_macos_errno();
    }
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

static hl_host_result hl_macos_process_spawn(void *context, hl_host_process_entry entry, void *entry_context) {
    return hl_macos_process_spawn_mode(context, entry, entry_context, 0);
}

static hl_host_result hl_macos_process_spawn_prepared(void *context, hl_host_process_entry entry, void *entry_context) {
    return hl_macos_process_spawn_mode(context, entry, entry_context, 1);
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

static hl_host_result hl_macos_fork_prepare(void *context) {
    hl_host_macos *host = context;
    hl_host_result result;
    if (pthread_mutex_lock(&host->fork_gate) != 0) return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    if (pthread_mutex_lock(&host->lock) != 0) {
        pthread_mutex_unlock(&host->fork_gate);
        return hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    }
    result = hl_host_sync_fork_prepare(host->sync);
    if (result.status == HL_STATUS_OK) {
        uint32_t index;
        for (index = 0; index < HL_MACOS_COUNTER_CAPACITY; ++index) {
            hl_macos_counter_object *object;
            uint32_t previous;
            if (!host->counters[index].active) continue;
            object = host->counters[index].object;
            for (previous = 0; previous < index; ++previous)
                if (host->counters[previous].active && host->counters[previous].object == object) break;
            if (previous == index) object->shared->references++;
        }
    }
    if (result.status != HL_STATUS_OK) {
        pthread_mutex_unlock(&host->lock);
        pthread_mutex_unlock(&host->fork_gate);
    }
    return result;
}

static hl_host_result hl_macos_fork_complete(void *context) {
    hl_host_macos *host = context;
    hl_host_result result = hl_host_sync_fork_complete(host->sync);
    if (pthread_mutex_unlock(&host->lock) != 0 && result.status == HL_STATUS_OK)
        result = hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    if (pthread_mutex_unlock(&host->fork_gate) != 0 && result.status == HL_STATUS_OK)
        result = hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    return result;
}

static hl_host_result hl_macos_fork_child(void *context) {
    hl_host_macos *host = context;
    hl_host_result result = hl_host_sync_fork_complete(host->sync);
    for (uint32_t index = 0; index < HL_MACOS_COUNTER_SUBSCRIPTIONS; ++index) {
        hl_macos_counter_subscription *subscription = &host->counter_subscriptions[index];
        if (!subscription->active) continue;
        close(subscription->descriptor);
        close(subscription->wake[0]);
        close(subscription->wake[1]);
        subscription->active = 0;
        subscription->counter = HL_HOST_HANDLE_INVALID;
        subscription->notify = NULL;
        subscription->observer = NULL;
    }
    for (uint32_t index = 0; index < HL_MACOS_DIRECTORY_CAPACITY && result.status == HL_STATUS_OK; ++index) {
        hl_macos_directory_object *object;
        uint32_t previous;
        if (!host->directories[index].active) continue;
        object = host->directories[index].object;
        for (previous = 0; previous < index; ++previous)
            if (host->directories[previous].active && host->directories[previous].object == object) break;
        if (previous != index) continue;
        int replacement = kqueue();
        if (replacement < 0) {
            result = hl_macos_errno();
            break;
        }
        (void)fcntl(replacement, F_SETFD, FD_CLOEXEC);
        for (uint32_t watch_index = 0; watch_index < HL_MACOS_DIRECTORY_WATCH_CAPACITY; ++watch_index) {
            hl_macos_directory_watch *watch = &object->watches[watch_index];
            if (!watch->active) continue;
            struct kevent change;
            uint16_t flags =
                (uint16_t)(EV_ADD | EV_CLEAR | ((watch->interests & HL_HOST_DIRECTORY_ONESHOT) != 0 ? EV_ONESHOT : 0));
            EV_SET(&change, watch->descriptor, EVFILT_VNODE, flags, hl_macos_directory_native(watch->interests), 0,
                   (void *)(uintptr_t)watch->token);
            if (kevent(replacement, &change, 1, NULL, 0, NULL) != 0) {
                close(replacement);
                result = hl_macos_errno();
                break;
            }
        }
        if (result.status != HL_STATUS_OK) break;
        close(object->descriptor);
        object->descriptor = replacement;
    }
    if (pthread_mutex_unlock(&host->lock) != 0 && result.status == HL_STATUS_OK)
        result = hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    if (pthread_mutex_unlock(&host->fork_gate) != 0 && result.status == HL_STATUS_OK)
        result = hl_macos_result(HL_STATUS_PLATFORM_FAILURE, 0, 0);
    return result;
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
    static const hl_host_memory_services memory = {
        HL_HOST_MEMORY_ABI,        sizeof(memory),         hl_macos_reserve,      hl_macos_protect,
        hl_macos_release,          hl_macos_publish,       hl_macos_reserve_code, hl_macos_repair_code,
        hl_macos_begin_code_write, hl_macos_end_code_write};
    static const hl_host_clock_services clock = {
        HL_HOST_CLOCK_ABI,      sizeof(clock),        hl_macos_monotonic,  hl_macos_realtime,
        hl_macos_raw_monotonic, hl_macos_process_cpu, hl_macos_thread_cpu, hl_macos_clock_sleep_until};
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
                                               hl_macos_file_write_sequential,
                                               hl_macos_file_clone_for_fork,
                                               hl_macos_file_seek,
                                               hl_macos_file_readv,
                                               hl_macos_file_writev,
                                               hl_macos_file_readv_at,
                                               hl_macos_file_writev_at,
                                               hl_macos_file_appendv,
                                               hl_macos_file_truncate,
                                               hl_macos_file_sync,
                                               hl_macos_file_sync,
                                               hl_macos_file_rename,
                                               hl_macos_file_unlink,
                                               hl_macos_file_path,
                                               hl_macos_file_standard_stream,
                                               hl_macos_file_readlink,
                                               hl_macos_file_set_owner,
                                               hl_macos_file_resolve_beneath};
    static const hl_host_process_services process = {
        HL_HOST_PROCESS_ABI,        sizeof(process),        hl_macos_process_spawn,         hl_macos_process_wait,
        hl_macos_process_terminate, hl_macos_process_close, hl_macos_process_spawn_prepared};
    static const hl_host_event_services event = {
        HL_HOST_EVENT_ABI,          sizeof(event),       hl_macos_event_create, hl_macos_event_control,
        hl_macos_event_wait,        hl_macos_event_wake, hl_macos_event_close,  hl_macos_event_arm_timer,
        hl_macos_event_disarm_timer};
    static const hl_host_shared_memory_services shared_memory = {HL_HOST_SHARED_MEMORY_ABI, sizeof(shared_memory),
                                                                 hl_macos_shared_create,    hl_macos_shared_open,
                                                                 hl_macos_shared_resize,    hl_macos_file_close};
    static const hl_host_sync_services sync = {HL_HOST_SYNC_ABI,      sizeof(sync),           hl_macos_mutex_create,
                                               hl_macos_mutex_lock,   hl_macos_mutex_unlock,  hl_macos_mutex_close,
                                               hl_macos_fork_prepare, hl_macos_fork_complete, hl_macos_fork_child};
    static const hl_host_counter_services counter = {
        HL_HOST_COUNTER_ABI,          sizeof(counter),
        hl_macos_counter_create,      hl_macos_counter_read,
        hl_macos_counter_write,       hl_macos_counter_get_flags,
        hl_macos_counter_set_flags,   hl_macos_counter_duplicate,
        hl_macos_counter_readiness,   hl_macos_counter_subscribe,
        hl_macos_counter_unsubscribe, hl_macos_counter_close,
    };
    static const hl_host_transfer_services transfer = {
        HL_HOST_TRANSFER_ABI,    sizeof(transfer),          hl_macos_transfer_channel_pair,
        hl_macos_transfer_send,  hl_macos_transfer_receive, hl_macos_transfer_duplicate,
        hl_macos_transfer_close,
    };
    static const hl_host_directory_services directory = {
        HL_HOST_DIRECTORY_ABI,     sizeof(directory),         hl_macos_directory_create, hl_macos_directory_add,
        hl_macos_directory_modify, hl_macos_directory_remove, hl_macos_directory_read,   hl_macos_directory_duplicate,
        hl_macos_directory_close};
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
    if (pthread_mutex_init(&host->fork_gate, NULL) != 0) {
        pthread_mutex_destroy(&host->lock);
        free(host);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (pthread_cond_init(&host->process_changed, NULL) != 0) {
        pthread_mutex_destroy(&host->fork_gate);
        pthread_mutex_destroy(&host->lock);
        free(host);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (hl_host_sync_registry_create(&host->sync) != HL_STATUS_OK) {
        pthread_cond_destroy(&host->process_changed);
        pthread_mutex_destroy(&host->fork_gate);
        pthread_mutex_destroy(&host->lock);
        free(host);
        return HL_STATUS_OUT_OF_MEMORY;
    }
    out_services->abi = HL_HOST_SERVICES_ABI;
    out_services->size = sizeof(*out_services);
    out_services->capabilities = HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_LOG | HL_HOST_CAP_FILE |
                                 HL_HOST_CAP_PROCESS | HL_HOST_CAP_EVENT_TIMER | HL_HOST_CAP_SHARED_MEMORY |
                                 HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_SYNC | HL_HOST_CAP_EVENT | HL_HOST_CAP_COUNTER |
                                 HL_HOST_CAP_DIRECTORY | HL_HOST_CAP_TRANSFER;
    out_services->context = host;
    out_services->memory = &memory;
    out_services->clock = &clock;
    out_services->log = &log;
    out_services->file = &file;
    out_services->process = &process;
    out_services->event = &event;
    out_services->shared_memory = &shared_memory;
    out_services->sync = &sync;
    out_services->counter = &counter;
    out_services->transfer = &transfer;
    out_services->directory = &directory;
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
    for (index = 0; index < HL_MACOS_EVENT_CAPACITY; ++index)
        if (host->events[index].active) close(host->events[index].descriptor);
    for (index = 0; index < HL_MACOS_COUNTER_CAPACITY; ++index) {
        hl_macos_counter *counter = &host->counters[index];
        hl_macos_counter_object *object;
        if (!counter->active) continue;
        object = counter->object;
        counter->active = 0;
        counter->object = NULL;
        if (--object->shared->references == 0) {
            close(object->readable);
            close(object->signal);
            pthread_mutex_destroy(&object->shared->lock);
            munmap(object->shared, sizeof(*object->shared));
            close(object->backing);
            free(object);
        }
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
    pthread_mutex_destroy(&host->fork_gate);
    pthread_mutex_destroy(&host->lock);
    free(host);
}
