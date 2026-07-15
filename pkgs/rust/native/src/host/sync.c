#define _GNU_SOURCE

#include "sync.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

enum { HL_SYNC_CHUNK_SIZE = 256, HL_SYNC_CHUNK_COUNT = 256 };

typedef struct hl_host_mutex_entry {
    pthread_mutex_t mutex;
    uint32_t generation;
    uint32_t active;
    uint32_t users;
} hl_host_mutex_entry;

struct hl_host_sync_registry {
    pthread_mutex_t lock;
    uint32_t destroying;
    uint32_t next_free;
    hl_host_mutex_entry *chunks[HL_SYNC_CHUNK_COUNT];
};

static hl_host_result hl_sync_result(hl_status status, uint64_t value, int detail) {
    return (hl_host_result){(int32_t)status, 0, value, (uint64_t)(unsigned int)detail};
}

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
    if (pthread_mutex_init(&registry->lock, NULL) != 0) {
        free(registry);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    *output = registry;
    return HL_STATUS_OK;
}

void hl_host_sync_registry_destroy(hl_host_sync_registry *registry) {
    uint32_t chunk_index;
    if (registry == NULL) return;
    pthread_mutex_lock(&registry->lock);
    registry->destroying = 1;
    pthread_mutex_unlock(&registry->lock);
    for (chunk_index = 0; chunk_index < HL_SYNC_CHUNK_COUNT; ++chunk_index) {
        hl_host_mutex_entry *chunk = registry->chunks[chunk_index];
        uint32_t entry_index;
        if (chunk == NULL) continue;
        for (entry_index = 0; entry_index < HL_SYNC_CHUNK_SIZE; ++entry_index)
            if (chunk[entry_index].active) pthread_mutex_destroy(&chunk[entry_index].mutex);
        free(chunk);
    }
    pthread_mutex_destroy(&registry->lock);
    free(registry);
}

hl_host_result hl_host_sync_mutex_create(hl_host_sync_registry *registry) {
    pthread_mutexattr_t attributes;
    uint32_t scan;
    int error;
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&registry->lock);
    if (registry->destroying) {
        pthread_mutex_unlock(&registry->lock);
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
                pthread_mutex_unlock(&registry->lock);
                return hl_sync_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
            }
            registry->chunks[chunk_index] = chunk;
        }
        {
            hl_host_mutex_entry *entry = &chunk[entry_index];
            if (entry->active) continue;
            error = pthread_mutexattr_init(&attributes);
            if (error == 0) {
                error = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
                if (error == 0) error = pthread_mutex_init(&entry->mutex, &attributes);
                pthread_mutexattr_destroy(&attributes);
            }
            if (error != 0) {
                pthread_mutex_unlock(&registry->lock);
                return hl_sync_result(hl_sync_status(error), 0, error);
            }
            entry->generation++;
            if (entry->generation == 0) entry->generation = 1;
            entry->active = 1;
            registry->next_free = (index + 1u) % (HL_SYNC_CHUNK_COUNT * HL_SYNC_CHUNK_SIZE);
            pthread_mutex_unlock(&registry->lock);
            return hl_sync_result(HL_STATUS_OK, ((uint64_t)entry->generation << 32) | (uint64_t)(index + 1u), 0);
        }
    }
    pthread_mutex_unlock(&registry->lock);
    return hl_sync_result(HL_STATUS_RESOURCE_LIMIT, 0, 0);
}

static hl_host_mutex_entry *hl_sync_ref(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry;
    if (registry == NULL) return NULL;
    pthread_mutex_lock(&registry->lock);
    entry = registry->destroying ? NULL : hl_sync_lookup(registry, handle);
    if (entry != NULL) entry->users++;
    pthread_mutex_unlock(&registry->lock);
    return entry;
}

static void hl_sync_unref(hl_host_sync_registry *registry, hl_host_mutex_entry *entry) {
    pthread_mutex_lock(&registry->lock);
    if (entry->users != 0) entry->users--;
    pthread_mutex_unlock(&registry->lock);
}

hl_host_result hl_host_sync_mutex_lock(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry = hl_sync_ref(registry, handle);
    int error;
    if (entry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    error = pthread_mutex_lock(&entry->mutex);
    hl_sync_unref(registry, entry);
    return hl_sync_result(hl_sync_status(error), 0, error);
}

hl_host_result hl_host_sync_mutex_unlock(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry = hl_sync_ref(registry, handle);
    int error;
    if (entry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    error = pthread_mutex_unlock(&entry->mutex);
    hl_sync_unref(registry, entry);
    return hl_sync_result(hl_sync_status(error), 0, error);
}

hl_host_result hl_host_sync_mutex_close(hl_host_sync_registry *registry, hl_host_handle handle) {
    hl_host_mutex_entry *entry;
    int error;
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    pthread_mutex_lock(&registry->lock);
    entry = registry->destroying ? NULL : hl_sync_lookup(registry, handle);
    if (entry == NULL) {
        pthread_mutex_unlock(&registry->lock);
        return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    error = entry->users == 0 ? pthread_mutex_trylock(&entry->mutex) : EBUSY;
    if (error != 0) {
        pthread_mutex_unlock(&registry->lock);
        return hl_sync_result(HL_STATUS_BUSY, 0, error);
    }
    pthread_mutex_unlock(&entry->mutex);
    pthread_mutex_destroy(&entry->mutex);
    entry->active = 0;
    {
        uint32_t index = (uint32_t)handle - 1u;
        if (index < registry->next_free) registry->next_free = index;
    }
    pthread_mutex_unlock(&registry->lock);
    return hl_sync_result(HL_STATUS_OK, 0, 0);
}

hl_host_result hl_host_sync_fork_prepare(hl_host_sync_registry *registry) {
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (pthread_mutex_lock(&registry->lock) != 0) return hl_sync_result(HL_STATUS_PLATFORM_FAILURE, 0, errno);
    return hl_sync_result(HL_STATUS_OK, 0, 0);
}

hl_host_result hl_host_sync_fork_complete(hl_host_sync_registry *registry) {
    if (registry == NULL) return hl_sync_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    return hl_sync_result(hl_sync_status(pthread_mutex_unlock(&registry->lock)), 0, 0);
}
