#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sync.h"

#include <errno.h>
#include <stdlib.h>

/*
 * Everything below the primitive layer -- chunked storage, the
 * (generation << 32) | (index + 1) encoding, refcounting and the free-slot scan
 * -- is host independent, so the primitive layer is the only part a new host
 * supplies. POSIX gets an errorcheck pthread mutex, which already reports
 * self-relock and unlock-by-non-owner.
 *
 * Windows has neither. CRITICAL_SECTION is recursive, which the opaque-mutex
 * contract forbids outright, and SRWLOCK is non-recursive but silently corrupts
 * on misuse rather than reporting it. The SRWLOCK below therefore carries an
 * owner thread id and synthesizes the same two verdicts an errorcheck mutex
 * returns, so both hosts reject the same programs.
 */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* Registry-internal lock. No ownership discipline is asked of it, so exclusive
 * mode on a bare slim lock is the whole implementation. */
typedef SRWLOCK hl_sync_lock;

typedef struct hl_sync_mutex_primitive {
    SRWLOCK lock;
    volatile LONG owner;
} hl_sync_mutex_primitive;

/* A private error space rather than errno: these values travel no further than
 * hl_sync_status and the opaque hl_host_result.detail, and mapping Win32 codes
 * here would leak a native error number through that field. */
enum { HL_SYNC_ERROR_NONE = 0, HL_SYNC_ERROR_INVALID = 1, HL_SYNC_ERROR_BUSY = 2, HL_SYNC_ERROR_MEMORY = 3 };

static hl_status hl_sync_status(int error) {
    switch (error) {
    case HL_SYNC_ERROR_NONE: return HL_STATUS_OK;
    case HL_SYNC_ERROR_INVALID: return HL_STATUS_INVALID_ARGUMENT;
    case HL_SYNC_ERROR_BUSY: return HL_STATUS_BUSY;
    case HL_SYNC_ERROR_MEMORY: return HL_STATUS_OUT_OF_MEMORY;
    default: return HL_STATUS_PLATFORM_FAILURE;
    }
}

static int hl_sync_lock_init(hl_sync_lock *lock) {
    InitializeSRWLock(lock);
    return 0;
}

static void hl_sync_lock_destroy(hl_sync_lock *lock) {
    (void)lock;
}

static int hl_sync_lock_acquire(hl_sync_lock *lock) {
    AcquireSRWLockExclusive(lock);
    return 0;
}

static int hl_sync_lock_release(hl_sync_lock *lock) {
    ReleaseSRWLockExclusive(lock);
    return 0;
}

static int hl_sync_mutex_init(hl_sync_mutex_primitive *mutex) {
    InitializeSRWLock(&mutex->lock);
    mutex->owner = 0;
    return HL_SYNC_ERROR_NONE;
}

static void hl_sync_mutex_destroy(hl_sync_mutex_primitive *mutex) {
    (void)mutex;
}

/* Read the owner through an interlocked compare so the field is never torn
 * against a concurrent release from the thread that holds the lock. */
static LONG hl_sync_mutex_owner(hl_sync_mutex_primitive *mutex) {
    return InterlockedCompareExchange(&mutex->owner, 0, 0);
}

static int hl_sync_mutex_acquire(hl_sync_mutex_primitive *mutex) {
    const LONG self = (LONG)GetCurrentThreadId();
    if (hl_sync_mutex_owner(mutex) == self) return HL_SYNC_ERROR_BUSY;
    AcquireSRWLockExclusive(&mutex->lock);
    InterlockedExchange(&mutex->owner, self);
    return HL_SYNC_ERROR_NONE;
}

static int hl_sync_mutex_release(hl_sync_mutex_primitive *mutex) {
    const LONG self = (LONG)GetCurrentThreadId();
    if (hl_sync_mutex_owner(mutex) != self) return HL_SYNC_ERROR_INVALID;
    InterlockedExchange(&mutex->owner, 0);
    ReleaseSRWLockExclusive(&mutex->lock);
    return HL_SYNC_ERROR_NONE;
}

static int hl_sync_mutex_try_acquire(hl_sync_mutex_primitive *mutex) {
    const LONG self = (LONG)GetCurrentThreadId();
    if (hl_sync_mutex_owner(mutex) == self) return HL_SYNC_ERROR_BUSY;
    if (!TryAcquireSRWLockExclusive(&mutex->lock)) return HL_SYNC_ERROR_BUSY;
    InterlockedExchange(&mutex->owner, self);
    return HL_SYNC_ERROR_NONE;
}

#else

#include <pthread.h>

typedef pthread_mutex_t hl_sync_lock;
typedef pthread_mutex_t hl_sync_mutex_primitive;

enum { HL_SYNC_ERROR_NONE = 0, HL_SYNC_ERROR_BUSY = EBUSY, HL_SYNC_ERROR_MEMORY = ENOMEM };

static hl_status hl_sync_status(int error) {
    switch (error) {
    case 0: return HL_STATUS_OK;
    case EINVAL:
    case EPERM: return HL_STATUS_INVALID_ARGUMENT;
    case ENOMEM: return HL_STATUS_OUT_OF_MEMORY;
    case EAGAIN: return HL_STATUS_RESOURCE_LIMIT;
    case EBUSY:
    case EDEADLK: return HL_STATUS_BUSY;
    default: return HL_STATUS_PLATFORM_FAILURE;
    }
}

static int hl_sync_lock_init(hl_sync_lock *lock) {
    return pthread_mutex_init(lock, NULL);
}

static void hl_sync_lock_destroy(hl_sync_lock *lock) {
    (void)pthread_mutex_destroy(lock);
}

static int hl_sync_lock_acquire(hl_sync_lock *lock) {
    return pthread_mutex_lock(lock);
}

static int hl_sync_lock_release(hl_sync_lock *lock) {
    return pthread_mutex_unlock(lock);
}

static int hl_sync_mutex_init(hl_sync_mutex_primitive *mutex) {
    pthread_mutexattr_t attributes;
    int error = pthread_mutexattr_init(&attributes);
    if (error != 0) return error;
    error = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    if (error == 0) error = pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return error;
}

static void hl_sync_mutex_destroy(hl_sync_mutex_primitive *mutex) {
    (void)pthread_mutex_destroy(mutex);
}

static int hl_sync_mutex_acquire(hl_sync_mutex_primitive *mutex) {
    return pthread_mutex_lock(mutex);
}

static int hl_sync_mutex_release(hl_sync_mutex_primitive *mutex) {
    return pthread_mutex_unlock(mutex);
}

static int hl_sync_mutex_try_acquire(hl_sync_mutex_primitive *mutex) {
    return pthread_mutex_trylock(mutex);
}

#endif

enum { HL_SYNC_CHUNK_SIZE = 256, HL_SYNC_CHUNK_COUNT = 256 };

typedef struct hl_host_mutex_entry {
    hl_sync_mutex_primitive mutex;
    uint32_t generation;
    uint32_t active;
    uint32_t users;
} hl_host_mutex_entry;

struct hl_host_sync_registry {
    hl_sync_lock lock;
    uint32_t destroying;
    uint32_t next_free;
    hl_host_mutex_entry *chunks[HL_SYNC_CHUNK_COUNT];
};

static hl_host_result hl_sync_result(hl_status status, uint64_t value, int detail) {
    return (hl_host_result){(int32_t)status, 0, value, (uint64_t)(unsigned int)detail};
}

static hl_host_mutex_entry *hl_sync_lookup(hl_host_sync_registry *registry, hl_host_handle handle) {
    uint32_t low = (uint32_t)handle;
    uint32_t index;
    hl_host_mutex_entry *chunk;
    hl_host_mutex_entry *entry;
    if (low == 0) return NULL;
    index = low - 1u;
    chunk = registry->chunks[index / HL_SYNC_CHUNK_SIZE];
    if (chunk == NULL) return NULL;
    entry = &chunk[index % HL_SYNC_CHUNK_SIZE];
    if (!entry->active || entry->generation != (uint32_t)(handle >> 32)) return NULL;
    return entry;
}

hl_status hl_host_sync_registry_create(hl_host_sync_registry **output) {
    hl_host_sync_registry *registry;
    if (output == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *output = NULL;
    registry = calloc(1, sizeof(*registry));
    if (registry == NULL) return HL_STATUS_OUT_OF_MEMORY;
    if (hl_sync_lock_init(&registry->lock) != 0) {
        free(registry);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    *output = registry;
    return HL_STATUS_OK;
}

void hl_host_sync_registry_destroy(hl_host_sync_registry *registry) {
    uint32_t chunk_index;
    if (registry == NULL) return;
    hl_sync_lock_acquire(&registry->lock);
    registry->destroying = 1;
    hl_sync_lock_release(&registry->lock);
    for (chunk_index = 0; chunk_index < HL_SYNC_CHUNK_COUNT; ++chunk_index) {
        hl_host_mutex_entry *chunk = registry->chunks[chunk_index];
        uint32_t entry_index;
        if (chunk == NULL) continue;
        for (entry_index = 0; entry_index < HL_SYNC_CHUNK_SIZE; ++entry_index)
            if (chunk[entry_index].active) hl_sync_mutex_destroy(&chunk[entry_index].mutex);
        free(chunk);
    }
    hl_sync_lock_destroy(&registry->lock);
    free(registry);
}

hl_host_result hl_host_sync_mutex_create(hl_host_sync_registry *registry) {
    uint32_t scan;
    int error;
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    hl_sync_lock_acquire(&registry->lock);
    if (registry->destroying) {
        hl_sync_lock_release(&registry->lock);
        return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    for (scan = 0; scan < HL_SYNC_CHUNK_COUNT * HL_SYNC_CHUNK_SIZE; ++scan) {
        uint32_t index = (registry->next_free + scan) % (HL_SYNC_CHUNK_COUNT * HL_SYNC_CHUNK_SIZE);
        uint32_t chunk_index = index / HL_SYNC_CHUNK_SIZE;
        uint32_t entry_index = index % HL_SYNC_CHUNK_SIZE;
        hl_host_mutex_entry *chunk = registry->chunks[chunk_index];
        if (chunk == NULL) {
            chunk = calloc(HL_SYNC_CHUNK_SIZE, sizeof(*chunk));
            if (chunk == NULL) {
                hl_sync_lock_release(&registry->lock);
                return hl_sync_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
            }
            registry->chunks[chunk_index] = chunk;
        }
        {
            hl_host_mutex_entry *entry = &chunk[entry_index];
            if (entry->active) continue;
            error = hl_sync_mutex_init(&entry->mutex);
            if (error != 0) {
                hl_sync_lock_release(&registry->lock);
                return hl_sync_result(hl_sync_status(error), 0, error);
            }
            entry->generation++;
            if (entry->generation == 0) entry->generation = 1;
            entry->active = 1;
            registry->next_free = (index + 1u) % (HL_SYNC_CHUNK_COUNT * HL_SYNC_CHUNK_SIZE);
            hl_sync_lock_release(&registry->lock);
            return hl_sync_result(HL_STATUS_OK, ((uint64_t)entry->generation << 32) | (uint64_t)(index + 1u), 0);
        }
    }
    hl_sync_lock_release(&registry->lock);
    return hl_sync_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
}

static hl_host_mutex_entry *hl_sync_ref(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry;
    if (registry == NULL) return NULL;
    hl_sync_lock_acquire(&registry->lock);
    entry = registry->destroying ? NULL : hl_sync_lookup(registry, handle);
    if (entry != NULL) entry->users++;
    hl_sync_lock_release(&registry->lock);
    return entry;
}

static void hl_sync_unref(hl_host_sync_registry *registry, hl_host_mutex_entry *entry) {
    hl_sync_lock_acquire(&registry->lock);
    if (entry->users != 0) entry->users--;
    hl_sync_lock_release(&registry->lock);
}

hl_host_result hl_host_sync_mutex_lock(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry = hl_sync_ref(registry, handle);
    int error;
    if (entry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    error = hl_sync_mutex_acquire(&entry->mutex);
    hl_sync_unref(registry, entry);
    return hl_sync_result(hl_sync_status(error), 0, error);
}

hl_host_result hl_host_sync_mutex_unlock(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry = hl_sync_ref(registry, handle);
    int error;
    if (entry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    error = hl_sync_mutex_release(&entry->mutex);
    hl_sync_unref(registry, entry);
    return hl_sync_result(hl_sync_status(error), 0, error);
}

hl_host_result hl_host_sync_mutex_close(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry;
    int error;
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    hl_sync_lock_acquire(&registry->lock);
    entry = registry->destroying ? NULL : hl_sync_lookup(registry, handle);
    if (entry == NULL) {
        hl_sync_lock_release(&registry->lock);
        return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    error = entry->users == 0 ? hl_sync_mutex_try_acquire(&entry->mutex) : HL_SYNC_ERROR_BUSY;
    if (error != 0) {
        hl_sync_lock_release(&registry->lock);
        return hl_sync_result(HL_STATUS_BUSY, 0, error);
    }
    hl_sync_mutex_release(&entry->mutex);
    hl_sync_mutex_destroy(&entry->mutex);
    entry->active = 0;
    {
        uint32_t index = (uint32_t)handle - 1u;
        if (index < registry->next_free) registry->next_free = index;
    }
    hl_sync_lock_release(&registry->lock);
    return hl_sync_result(HL_STATUS_OK, 0, 0);
}

hl_host_result hl_host_sync_fork_prepare(hl_host_sync_registry *registry) {
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (hl_sync_lock_acquire(&registry->lock) != 0) return hl_sync_result(HL_STATUS_PLATFORM_FAILURE, 0, errno);
    return hl_sync_result(HL_STATUS_OK, 0, 0);
}

hl_host_result hl_host_sync_fork_complete(hl_host_sync_registry *registry) {
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return hl_sync_result(hl_sync_status(hl_sync_lock_release(&registry->lock)), 0, 0);
}
