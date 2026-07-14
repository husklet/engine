/* Bridge opaque host-file bindings into the legacy native-fd guest runtime. */

#include "../object.h"
#include "../epoll.h"
#include "../watch.h"
#include "../bus.h"

static int g_bound_sentinel = -1;

typedef struct bound_watch_source {
    uint64_t token;
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    hl_host_handle file;
    hl_host_handle watch;
    size_t references;
    struct bound_watch_source *next;
} bound_watch_source;

typedef struct bound_watch_state {
    pthread_mutex_t lock;
    pthread_t thread;
    hl_linux_watch_set changes;
    bound_watch_source *sources;
    hl_host_handle pollset;
    int initialized;
    int running;
    int stopping;
} bound_watch_state;

static bound_watch_state g_bound_watches = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .pollset = HL_HOST_HANDLE_INVALID,
};
static pthread_mutex_t g_bound_mapping_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_bound_mapping_gate = PTHREAD_MUTEX_INITIALIZER;

typedef struct bound_mapping_object {
    hl_host_handle handle;
    hl_host_handle file;
    uint64_t address;
    uint64_t size;
    uint64_t device;
    uint64_t inode;
    uint64_t known_size;
    bound_watch_source *source;
    uint32_t identity_valid;
    uint32_t shared;
    size_t references;
} bound_mapping_object;

typedef struct bound_mapping {
    uint64_t address;
    uint64_t size;
    uint64_t object_offset;
    uint64_t file_offset;
    uint64_t follow_lo;
    uint64_t follow_hi;
    bound_mapping_object *object;
    struct bound_mapping *next;
} bound_mapping;

static void bound_watch_release(bound_watch_source *source);

static bound_mapping **bound_mapping_head(void) {
    size_t required = offsetof(hl_linux_abi, vma_state) + sizeof(g_linux_box->vma_state);
    if (g_linux_box == NULL || g_linux_box->abi != HL_LINUX_ABI_VERSION || g_linux_box->size < required) return NULL;
    return (bound_mapping **)&g_linux_box->vma_state;
}

static void bound_mapping_file_size_changed(const hl_linux_fd_snapshot *file,
                                            const hl_host_file_metadata *metadata, int have_metadata,
                                            uint64_t old_size, uint64_t new_size,
                                            hl_linux_bus_transition *transition);
static void bound_mapping_file_data_changed(const hl_linux_fd_snapshot *file, uint64_t device, uint64_t inode);

static bound_watch_source *bound_watch_find_token(uint64_t token) {
    for (bound_watch_source *source = g_bound_watches.sources; source != NULL; source = source->next)
        if (source->token == token) return source;
    return NULL;
}

static bound_watch_source *bound_watch_find_identity(uint64_t device, uint64_t inode) {
    for (bound_watch_source *source = g_bound_watches.sources; source != NULL; source = source->next)
        if (source->device == device && source->inode == inode) return source;
    return NULL;
}

static void bound_watch_publish_size(uint64_t device, uint64_t inode, uint64_t size) {
    pthread_mutex_lock(&g_bound_watches.lock);
    bound_watch_source *source = bound_watch_find_identity(device, inode);
    if (source != NULL) source->size = size;
    pthread_mutex_unlock(&g_bound_watches.lock);
}

static void bound_watch_apply(void *opaque, const hl_linux_watch_change *change) {
    (void)opaque;
    pthread_mutex_lock(&g_bound_watches.lock);
    bound_watch_source *source = bound_watch_find_token(change->token);
    if (source == NULL) {
        pthread_mutex_unlock(&g_bound_watches.lock);
        return;
    }
    if (change->new_size == source->size) {
        if ((change->flags & HL_HOST_WATCH_DATA) == 0) {
            pthread_mutex_unlock(&g_bound_watches.lock);
            return;
        }
        source->references++;
        hl_linux_fd_snapshot file = {.host_handle = source->file};
        uint64_t device = source->device, inode = source->inode;
        pthread_mutex_unlock(&g_bound_watches.lock);
        bound_mapping_file_data_changed(&file, device, inode);
        bound_watch_release(source);
        return;
    }
    source->references++;
    hl_linux_fd_snapshot file = {.host_handle = source->file};
    hl_host_file_metadata metadata = {.stable_device = source->device, .stable_object = source->inode};
    uint64_t old_size = source->size;
    source->size = change->new_size;
    pthread_mutex_unlock(&g_bound_watches.lock);
    hl_linux_bus_transition transition = {0};
    if (hl_linux_bus_transition_begin(&transition) != 0) {
        bound_watch_release(source);
        return;
    }
    pthread_mutex_lock(&g_bound_mapping_lock);
    bound_mapping_file_size_changed(&file, &metadata, 1, old_size, change->new_size, &transition);
    pthread_mutex_unlock(&g_bound_mapping_lock);
    bound_watch_release(source);
    hl_linux_bus_transition_end(&transition);
}

static void *bound_watch_waiter(void *opaque) {
    (void)opaque;
    for (;;) {
        hl_host_event_record events[16];
        hl_host_result ready = g_host_services->event->wait(g_host_services->context, g_bound_watches.pollset,
                                                             events, 16, HL_HOST_DEADLINE_INFINITE);
        int drain_changes = 0;
        pthread_mutex_lock(&g_bound_watches.lock);
        if (g_bound_watches.stopping) {
            pthread_mutex_unlock(&g_bound_watches.lock);
            break;
        }
        if (ready.status == HL_STATUS_OK) {
            for (uint64_t index = 0; index < ready.value; ++index) {
                bound_watch_source *source = bound_watch_find_token(events[index].token);
                hl_host_watch_record record = {0};
                if (source == NULL) continue;
                hl_host_result drained =
                    g_host_services->watch->drain(g_host_services->context, source->watch, &record, 1);
                if (drained.status == HL_STATUS_OK && drained.value != 0)
                    drain_changes |= hl_linux_watch_enqueue(&g_bound_watches.changes, source->token, record.size,
                                                            record.changes) == HL_STATUS_OK;
            }
        }
        pthread_mutex_unlock(&g_bound_watches.lock);
        if (drain_changes) {
            size_t count = 0;
            (void)hl_linux_watch_drain(&g_bound_watches.changes, bound_watch_apply, NULL, &count);
        }
        if (ready.status != HL_STATUS_OK && ready.status != HL_STATUS_INTERRUPTED) break;
    }
    return NULL;
}

static int bound_watch_host_start_locked(void) {
    if (g_bound_watches.running || g_bound_watches.sources == NULL) return 1;
    hl_host_result pollset = g_host_services->event->create(g_host_services->context);
    if (pollset.status != HL_STATUS_OK) return 0;
    g_bound_watches.pollset = pollset.value;
    for (bound_watch_source *source = g_bound_watches.sources; source != NULL; source = source->next) {
        hl_host_result watched = g_host_services->watch->open(g_host_services->context, source->file);
        if (watched.status != HL_STATUS_OK) goto fail;
        source->watch = watched.value;
        if (g_host_services->event
                ->control(g_host_services->context, g_bound_watches.pollset, HL_HOST_EVENT_ADD, source->watch,
                          source->token, HL_HOST_READY_READ | HL_HOST_READY_EDGE)
                .status != HL_STATUS_OK)
            goto fail;
    }
    g_bound_watches.stopping = 0;
    if (pthread_create(&g_bound_watches.thread, NULL, bound_watch_waiter, NULL) != 0) goto fail;
    g_bound_watches.running = 1;
    return 1;
fail:
    for (bound_watch_source *source = g_bound_watches.sources; source != NULL; source = source->next) {
        if (source->watch == HL_HOST_HANDLE_INVALID) continue;
        (void)g_host_services->watch->close(g_host_services->context, source->watch);
        source->watch = HL_HOST_HANDLE_INVALID;
    }
    (void)g_host_services->event->close(g_host_services->context, g_bound_watches.pollset);
    g_bound_watches.pollset = HL_HOST_HANDLE_INVALID;
    return 0;
}

static void bound_watch_host_stop(int close_handles) {
    pthread_mutex_lock(&g_bound_watches.lock);
    if (g_bound_watches.running) {
        g_bound_watches.stopping = 1;
        (void)g_host_services->event->wake(g_host_services->context, g_bound_watches.pollset);
        pthread_mutex_unlock(&g_bound_watches.lock);
        (void)pthread_join(g_bound_watches.thread, NULL);
        pthread_mutex_lock(&g_bound_watches.lock);
        g_bound_watches.running = 0;
    }
    if (close_handles && g_bound_watches.pollset != HL_HOST_HANDLE_INVALID) {
        for (bound_watch_source *source = g_bound_watches.sources; source != NULL; source = source->next) {
            if (source->watch == HL_HOST_HANDLE_INVALID) continue;
            (void)g_host_services->event->control(g_host_services->context, g_bound_watches.pollset,
                                                  HL_HOST_EVENT_DELETE, source->watch, source->token,
                                                  HL_HOST_READY_READ);
            (void)g_host_services->watch->close(g_host_services->context, source->watch);
            source->watch = HL_HOST_HANDLE_INVALID;
        }
        (void)g_host_services->event->close(g_host_services->context, g_bound_watches.pollset);
        g_bound_watches.pollset = HL_HOST_HANDLE_INVALID;
    }
    pthread_mutex_unlock(&g_bound_watches.lock);
}

static bound_watch_source *bound_watch_retain(const hl_linux_fd_snapshot *file, uint64_t device, uint64_t inode,
                                               uint64_t size) {
    if (g_host_services == NULL || g_host_services->watch == NULL || g_host_services->event == NULL ||
        g_host_services->file == NULL || g_host_services->file->clone_for_fork == NULL ||
        (g_host_services->capabilities & (HL_HOST_CAP_WATCH | HL_HOST_CAP_EVENT)) !=
            (HL_HOST_CAP_WATCH | HL_HOST_CAP_EVENT))
        return NULL;
    pthread_mutex_lock(&g_bound_watches.lock);
    bound_watch_source *source = bound_watch_find_identity(device, inode);
    if (source != NULL) {
        source->references++;
        pthread_mutex_unlock(&g_bound_watches.lock);
        return source;
    }
    if (!g_bound_watches.initialized) {
        if (hl_linux_watch_init(&g_bound_watches.changes) != HL_STATUS_OK) {
            pthread_mutex_unlock(&g_bound_watches.lock);
            return NULL;
        }
        g_bound_watches.initialized = 1;
    }
    hl_host_result cloned = g_host_services->file->clone_for_fork(g_host_services->context, file->host_handle);
    source = cloned.status == HL_STATUS_OK ? calloc(1, sizeof(*source)) : NULL;
    if (source == NULL) {
        if (cloned.status == HL_STATUS_OK) (void)g_host_services->file->close(g_host_services->context, cloned.value);
        pthread_mutex_unlock(&g_bound_watches.lock);
        return NULL;
    }
    int created = 0;
    if (hl_linux_watch_retain(&g_bound_watches.changes, device, inode, size, &source->token, &created) !=
            HL_STATUS_OK ||
        !created) {
        (void)g_host_services->file->close(g_host_services->context, cloned.value);
        free(source);
        pthread_mutex_unlock(&g_bound_watches.lock);
        return NULL;
    }
    *source = (bound_watch_source){source->token, device, inode, size, cloned.value, HL_HOST_HANDLE_INVALID, 1,
                                   g_bound_watches.sources};
    g_bound_watches.sources = source;
    int attached = 1;
    if (g_bound_watches.running) {
        hl_host_result watched = g_host_services->watch->open(g_host_services->context, source->file);
        attached = watched.status == HL_STATUS_OK;
        if (attached) {
            source->watch = watched.value;
            attached = g_host_services->event
                           ->control(g_host_services->context, g_bound_watches.pollset, HL_HOST_EVENT_ADD,
                                     source->watch, source->token, HL_HOST_READY_READ | HL_HOST_READY_EDGE)
                           .status == HL_STATUS_OK;
        }
        if (!attached && source->watch != HL_HOST_HANDLE_INVALID) {
            (void)g_host_services->watch->close(g_host_services->context, source->watch);
            source->watch = HL_HOST_HANDLE_INVALID;
        }
    } else {
        attached = bound_watch_host_start_locked();
    }
    if (!attached) {
        int removed = 0;
        g_bound_watches.sources = source->next;
        (void)hl_linux_watch_release(&g_bound_watches.changes, source->token, &removed);
        (void)g_host_services->file->close(g_host_services->context, source->file);
        free(source);
        source = NULL;
    }
    pthread_mutex_unlock(&g_bound_watches.lock);
    return source;
}

static void bound_watch_release(bound_watch_source *source) {
    if (source == NULL) return;
    pthread_mutex_lock(&g_bound_watches.lock);
    if (--source->references == 0) {
        bound_watch_source **link = &g_bound_watches.sources;
        while (*link != NULL && *link != source) link = &(*link)->next;
        if (*link == source) *link = source->next;
        if (source->watch != HL_HOST_HANDLE_INVALID) {
            (void)g_host_services->event->control(g_host_services->context, g_bound_watches.pollset,
                                                  HL_HOST_EVENT_DELETE, source->watch, source->token,
                                                  HL_HOST_READY_READ);
            (void)g_host_services->watch->close(g_host_services->context, source->watch);
        }
        int removed = 0;
        (void)hl_linux_watch_release(&g_bound_watches.changes, source->token, &removed);
        (void)g_host_services->file->close(g_host_services->context, source->file);
        free(source);
    }
    pthread_mutex_unlock(&g_bound_watches.lock);
}

static size_t bound_mapping_watch_capacity(void) {
    return g_linux_box == NULL ? 0 : g_linux_box->ofd_capacity;
}

static void bound_watch_fork_rebuild(void *opaque, uint64_t token, uint64_t device, uint64_t object) {
    (void)opaque;
    bound_watch_source *source = bound_watch_find_identity(device, object);
    if (source != NULL) source->token = token;
}

static int bound_mapping_fork_prepare(hl_linux_watch_fork_plan *plan) {
    if (!g_bound_watches.initialized) return 0;
    pthread_mutex_lock(&g_bound_mapping_gate);
    bound_watch_host_stop(1);
    pthread_mutex_lock(&g_bound_mapping_lock);
    if (hl_linux_watch_fork_snapshot(&g_bound_watches.changes, plan) == HL_STATUS_OK) {
        return 0;
    }
    pthread_mutex_unlock(&g_bound_mapping_lock);
    pthread_mutex_unlock(&g_bound_mapping_gate);
    return -1;
}

static int bound_mapping_fork_complete(hl_linux_watch_fork_plan *plan, int child) {
    hl_status status;
    if (!g_bound_watches.initialized) return 0;
    if (child) {
        pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
        memcpy(&g_bound_watches.lock, &fresh, sizeof(fresh));
        g_bound_watches.running = 0;
        g_bound_watches.stopping = 0;
        g_bound_watches.pollset = HL_HOST_HANDLE_INVALID;
        status = hl_linux_watch_fork_child(&g_bound_watches.changes, plan, bound_watch_fork_rebuild, NULL);
    } else {
        status = HL_STATUS_OK;
    }
    if (status != HL_STATUS_OK) {
        pthread_mutex_unlock(&g_bound_mapping_lock);
        pthread_mutex_unlock(&g_bound_mapping_gate);
        return -1;
    }
    pthread_mutex_lock(&g_bound_watches.lock);
    int started = bound_watch_host_start_locked();
    pthread_mutex_unlock(&g_bound_watches.lock);
    pthread_mutex_unlock(&g_bound_mapping_lock);
    pthread_mutex_unlock(&g_bound_mapping_gate);
    return started ? 0 : -1;
}

static int64_t bound_host_error(int32_t status) {
    switch ((hl_status)status) {
    case HL_STATUS_OK: return 0;
    case HL_STATUS_INVALID_ARGUMENT: return -EINVAL;
    case HL_STATUS_NOT_FOUND: return -ENOENT;
    case HL_STATUS_PERMISSION_DENIED: return -EACCES;
    case HL_STATUS_ALREADY_EXISTS: return -EEXIST;
    case HL_STATUS_RESOURCE_LIMIT: return -ENOMEM;
    case HL_STATUS_NOT_SUPPORTED: return -ENOTSUP;
    case HL_STATUS_INTERRUPTED: return -EINTR;
    default: return -EIO;
    }
}

static int bound_file_abi14(void) {
    const hl_host_file_services *file = g_host_services != NULL ? g_host_services->file : NULL;
    return file != NULL && file->abi == HL_HOST_FILE_ABI && file->size >= sizeof(*file);
}

static void bound_fill_statfs(uint8_t *output, const hl_host_filesystem_metadata *metadata) {
    memset(output, 0, 120);
    *(int64_t *)(output + 0) = INT64_C(0x01021994);
    *(uint64_t *)(output + 8) = metadata->block_size;
    *(uint64_t *)(output + 16) = metadata->blocks;
    *(uint64_t *)(output + 24) = metadata->blocks_free;
    *(uint64_t *)(output + 32) = metadata->blocks_available;
    *(uint64_t *)(output + 40) = metadata->files;
    *(uint64_t *)(output + 48) = metadata->files_free;
    *(uint32_t *)(output + 56) = (uint32_t)metadata->filesystem_id[0];
    *(uint32_t *)(output + 60) = (uint32_t)metadata->filesystem_id[1];
    *(uint64_t *)(output + 64) = metadata->name_max;
    *(uint64_t *)(output + 72) = metadata->fragment_size;
    *(uint64_t *)(output + 80) = metadata->flags;
}

static void bound_fill_statx(uint8_t *output, const hl_linux_file_status *status) {
    memset(output, 0, 256);
    *(uint32_t *)(output + 0) = 0x7ffu | 0x800u;
    *(uint32_t *)(output + 4) = 4096;
    *(uint32_t *)(output + 16) = (uint32_t)status->link_count;
    *(uint32_t *)(output + 20) = status->user;
    *(uint32_t *)(output + 24) = status->group;
    *(uint16_t *)(output + 28) = (uint16_t)status->mode;
    *(uint64_t *)(output + 32) = status->object;
    *(uint64_t *)(output + 40) = status->size;
    *(uint64_t *)(output + 48) = status->blocks_512;
    const uint64_t timestamps[4] = {status->accessed_ns, status->created_ns, status->changed_ns,
                                    status->modified_ns};
    for (size_t index = 0; index < 4; ++index) {
        size_t offset = 64 + index * 16;
        *(int64_t *)(output + offset) = (int64_t)(timestamps[index] / UINT64_C(1000000000));
        *(uint32_t *)(output + offset + 8) = (uint32_t)(timestamps[index] % UINT64_C(1000000000));
    }
    *(uint32_t *)(output + 128) = hl_linux_device_major(status->special_device);
    *(uint32_t *)(output + 132) = hl_linux_device_minor(status->special_device);
    *(uint32_t *)(output + 136) = hl_linux_device_major(status->device);
    *(uint32_t *)(output + 140) = hl_linux_device_minor(status->device);
}

static void bound_virtualize_owner(const hl_linux_fd_snapshot *file, hl_linux_file_status *status) {
    char path[HL_LINUX_PATH_MAX + 1];
    hl_host_result named = g_host_services->file->path(
        g_host_services->context, file->host_handle, (hl_host_bytes){path, HL_LINUX_PATH_MAX});
    if (named.status == HL_STATUS_OK && named.value <= HL_LINUX_PATH_MAX) {
        int uid, gid;
        struct stat native;
        path[named.value] = 0;
        uint64_t device = status->device, object = status->object;
        if (stat(path, &native) == 0) {
            device = (uint64_t)native.st_dev;
            object = (uint64_t)native.st_ino;
        }
        if (chown_xattr_get(path, -1, device, object, &uid, &gid)) {
            if (uid >= 0) status->user = (uint32_t)uid;
            if (gid >= 0) status->group = (uint32_t)gid;
        }
    }
}

static uint32_t bound_mode_type(uint32_t type) {
    switch (type) {
    case HL_HOST_FILE_TYPE_REGULAR: return 0100000;
    case HL_HOST_FILE_TYPE_DIRECTORY: return 0040000;
    case HL_HOST_FILE_TYPE_SYMLINK: return 0120000;
    case HL_HOST_FILE_TYPE_CHARACTER: return 0020000;
    case HL_HOST_FILE_TYPE_BLOCK: return 0060000;
    case HL_HOST_FILE_TYPE_FIFO: return 0010000;
    case HL_HOST_FILE_TYPE_SOCKET: return 0140000;
    default: return 0;
    }
}

static void bound_status_from_metadata(hl_linux_file_status *status, const hl_host_file_metadata *metadata) {
    memset(status, 0, sizeof(*status));
    status->device = metadata->stable_device;
    status->object = metadata->stable_object;
    status->size = metadata->size;
    status->blocks_512 = metadata->allocated_size / 512u;
    status->modified_ns = metadata->modified_ns;
    status->accessed_ns = metadata->accessed_ns;
    status->changed_ns = metadata->changed_ns;
    status->created_ns = metadata->created_ns;
    status->special_device = metadata->device;
    status->link_count = metadata->link_count;
    status->mode = bound_mode_type(metadata->type) | (metadata->permissions & 07777u);
    status->user = metadata->user;
    status->group = metadata->group;
}

static bound_mapping *bound_mapping_find(uint64_t address, uint64_t size) {
    bound_mapping **head = bound_mapping_head();
    bound_mapping *entry;
    if (head == NULL || size == 0) return NULL;
    for (entry = *head; entry != NULL; entry = entry->next)
        if (address >= entry->address && size <= entry->size && address - entry->address <= entry->size - size)
            return entry;
    return NULL;
}

static void bound_mapping_drop(bound_mapping *entry, bound_mapping *previous) {
    bound_mapping **head = bound_mapping_head();
    bound_mapping_object *object = entry->object;
    if (head == NULL) return;
    if (previous != NULL) previous->next = entry->next;
    else *head = entry->next;
    free(entry);
    if (--object->references == 0) {
        bound_watch_release(object->source);
        (void)g_host_services->memory->release(g_host_services->context, object->handle);
        free(object);
    }
}

static void bound_mapping_retire(uint64_t address, uint64_t size) {
    bound_mapping **head = bound_mapping_head();
    uint64_t end;
    bound_mapping *entry, *previous = NULL;
    if (head == NULL || size == 0 || address > UINT64_MAX - size) return;
    end = address + size;
    entry = *head;
    while (entry != NULL) {
        bound_mapping *next = entry->next;
        uint64_t base = entry->address, mapped_end = base + entry->size;
        if (end <= base || address >= mapped_end) {
            previous = entry;
        } else if (address <= base && end >= mapped_end) {
            bound_mapping_drop(entry, previous);
        } else if (address > base && end < mapped_end) {
            bound_mapping *tail = malloc(sizeof(*tail));
            if (tail != NULL) {
                *tail = (bound_mapping){end, mapped_end - end, entry->object_offset + end - base,
                                        entry->file_offset + end - base, 0, 0, entry->object, entry->next};
                entry->object->references++;
                entry->next = tail;
                entry->size = address - base;
                previous = tail;
            }
        } else if (address <= base) {
            uint64_t cut = end - base;
            entry->address += cut;
            entry->object_offset += cut;
            entry->file_offset += cut;
            entry->size -= cut;
            previous = entry;
        } else {
            entry->size = address - base;
            previous = entry;
        }
        entry = next;
    }
}

static void bound_mapping_reset(void) {
    bound_mapping **head = bound_mapping_head();
    if (head == NULL) return;
    pthread_mutex_lock(&g_bound_mapping_gate);
    bound_watch_host_stop(1);
    pthread_mutex_lock(&g_bound_mapping_lock);
    while (*head != NULL) bound_mapping_drop(*head, NULL);
    pthread_mutex_lock(&g_bound_watches.lock);
    if (g_bound_watches.initialized) {
        hl_linux_watch_close(&g_bound_watches.changes);
        g_bound_watches.initialized = 0;
    }
    pthread_mutex_unlock(&g_bound_watches.lock);
    pthread_mutex_unlock(&g_bound_mapping_lock);
    pthread_mutex_unlock(&g_bound_mapping_gate);
}

static int64_t bound_mmap_file(const hl_linux_fd_snapshot *file, uint64_t address, uint64_t size, uint32_t protection,
                               uint32_t linux_flags, uint64_t offset) {
    hl_host_file_mapping mapped = {HL_HOST_FILE_MAPPING_ABI, sizeof(mapped), 0, 0, 0, 0};
    uint32_t flags = (linux_flags & 1u) ? HL_HOST_MEMORY_SHARED : HL_HOST_MEMORY_PRIVATE;
    bound_mapping_object *object;
    bound_mapping *entry;
    bound_mapping **head = bound_mapping_head();
    int64_t result;
    uint64_t bus_accessible = size;
    uint64_t stable_device = 0, stable_object = 0, known_size = 0;
    int identity_valid = 0;
    int bus_prepared = 0;
    if (head == NULL || g_host_services == NULL || g_host_services->memory == NULL ||
        g_host_services->memory->map_file == NULL)
        return -ENOSYS;
    pthread_mutex_lock(&g_bound_mapping_gate);
    if (linux_flags & 0x10u) flags |= HL_HOST_MEMORY_FIXED;
    if (linux_flags & 0x100000u) flags = (flags & ~HL_HOST_MEMORY_FIXED) | HL_HOST_MEMORY_FIXED_NOREPLACE;
    object = calloc(1, sizeof(*object));
    entry = calloc(1, sizeof(*entry));
    if (object == NULL || entry == NULL) {
        free(object);
        free(entry);
        pthread_mutex_unlock(&g_bound_mapping_gate);
        return -ENOMEM;
    }
    if (g_host_services->file != NULL && g_host_services->file->metadata != NULL) {
        hl_host_file_metadata metadata;
        hl_host_result status =
            g_host_services->file->metadata(g_host_services->context, file->host_handle, &metadata);
        if (status.status == HL_STATUS_OK) {
            stable_device = metadata.stable_device;
            stable_object = metadata.stable_object;
            known_size = metadata.size;
            identity_valid = 1;
            uint64_t available = metadata.size > offset ? metadata.size - offset : 0;
            bus_accessible = available > UINT64_MAX - UINT64_C(4095)
                                 ? UINT64_MAX
                                 : (available + UINT64_C(4095)) & ~UINT64_C(4095);
            if (bus_accessible < size) {
                gbus_prepare();
                bus_prepared = 1;
            }
        }
    }
    result = hl_linux_map_file(g_linux_box, file->fd, address, offset, size, protection & 7u, flags, &mapped);
    if (result < 0) {
        if (bus_prepared) gbus_prepare_release();
        free(object);
        free(entry);
        pthread_mutex_unlock(&g_bound_mapping_gate);
        return result;
    }
    pthread_mutex_lock(&g_bound_mapping_lock);
    if (linux_flags & (0x10u | 0x100000u)) bound_mapping_retire(mapped.address, mapped.mapped_size);
    bound_watch_source *source =
        identity_valid ? bound_watch_retain(file, stable_device, stable_object, known_size) : NULL;
    *object = (bound_mapping_object){mapped.handle, file->host_handle, mapped.address, mapped.mapped_size,
                                     stable_device, stable_object, known_size,
                                     source, (uint32_t)identity_valid, (uint32_t)((linux_flags & 1u) != 0), 1};
    *entry = (bound_mapping){mapped.address, mapped.mapped_size, mapped.reserved, offset, 0, 0, object, *head};
    *head = entry;
    if (mapped.address == 0 || mapped.mapped_size < size || mapped.address > UINT64_MAX - size) {
        if (bus_prepared) gbus_prepare_release();
        bound_mapping_drop(entry, NULL);
        pthread_mutex_unlock(&g_bound_mapping_lock);
        pthread_mutex_unlock(&g_bound_mapping_gate);
        return -EIO;
    }
    gmap_add(mapped.address, mapped.mapped_size);
    gmap_set_glen(mapped.address, size);
    gbus_clear(mapped.address, mapped.address + size);
    if (bus_prepared && gbus_add(mapped.address + bus_accessible, mapped.address + size) != 0) {
        gbus_prepare_release();
        bound_mapping_drop(entry, NULL);
        gmap_split_unmap(mapped.address, mapped.address + mapped.mapped_size);
        pthread_mutex_unlock(&g_bound_mapping_lock);
        pthread_mutex_unlock(&g_bound_mapping_gate);
        return -ENOMEM;
    }
    if (bus_prepared) gbus_prepare_release();
    pthread_mutex_unlock(&g_bound_mapping_lock);
    pthread_mutex_unlock(&g_bound_mapping_gate);
    return (int64_t)mapped.address;
}

static uint64_t bound_file_accessible(const bound_mapping *mapping, uint64_t file_size) {
    uint64_t available, rounded;
    if (file_size <= mapping->file_offset) return 0;
    available = file_size - mapping->file_offset;
    if (available > UINT64_MAX - UINT64_C(4095)) return mapping->size;
    rounded = (available + UINT64_C(4095)) & ~UINT64_C(4095);
    return rounded < mapping->size ? rounded : mapping->size;
}

static int bound_mapping_same_file(const bound_mapping_object *object, const hl_linux_fd_snapshot *file,
                                   const hl_host_file_metadata *metadata, int have_metadata) {
    if (have_metadata && object->identity_valid)
        return object->device == metadata->stable_device && object->inode == metadata->stable_object;
    return object->file == file->host_handle;
}

/* Recompute every VMA of the truncated inode, including mappings made through
   dup'd descriptors or a separately opened handle with the same stable host
   identity.  Shrink is called while gbus_prepare owns the activation
   transition, so no old translated block can touch the host mapping before the
   newly invalid pages are published. */
static void bound_mapping_file_size_changed(const hl_linux_fd_snapshot *file,
                                            const hl_host_file_metadata *metadata, int have_metadata,
                                            uint64_t old_size, uint64_t new_size,
                                            hl_linux_bus_transition *transition) {
    bound_mapping **head = bound_mapping_head();
    if (head == NULL) return;
    for (bound_mapping *entry = *head; entry != NULL; entry = entry->next) {
        uint64_t old_accessible, new_accessible;
        if (!bound_mapping_same_file(entry->object, file, metadata, have_metadata)) continue;
        entry->object->known_size = new_size;
        old_accessible = bound_file_accessible(entry, old_size);
        new_accessible = bound_file_accessible(entry, new_size);
        if (new_size < old_size && new_size > entry->file_offset &&
            new_size < entry->file_offset + entry->size) {
            uint64_t tail = new_size - entry->file_offset;
            uint64_t partial_end = (tail + UINT64_C(4095)) & ~UINT64_C(4095);
            if (partial_end > entry->size) partial_end = entry->size;
            if (partial_end > tail)
                memset((void *)(uintptr_t)(entry->address + tail), 0, (size_t)(partial_end - tail));
        }
        if (new_accessible < old_accessible) {
            if (transition != NULL)
                (void)hl_linux_bus_transition_add(transition, entry->address + new_accessible,
                                                  entry->address + entry->size);
            else
                (void)gbus_add(entry->address + new_accessible, entry->address + entry->size);
        }
        else if (new_accessible > old_accessible) {
            if (!entry->object->shared) {
                entry->follow_lo = old_accessible;
                entry->follow_hi = new_accessible;
            }
            if (transition != NULL)
                hl_linux_bus_transition_clear(transition, entry->address + old_accessible,
                                              entry->address + new_accessible);
            else
                gbus_clear(entry->address + old_accessible, entry->address + new_accessible);
        }
    }
}

static void bound_mapping_file_written(const hl_linux_fd_snapshot *file, uint64_t offset, uint64_t size) {
    bound_mapping **head = bound_mapping_head();
    hl_host_file_metadata metadata = {0};
    int have_metadata = 0;
    if (head == NULL || size == 0 || offset > UINT64_MAX - size || g_host_services == NULL ||
        g_host_services->file == NULL || g_host_services->file->read_at == NULL)
        return;
    if (g_host_services->file->metadata != NULL) {
        hl_host_result status =
            g_host_services->file->metadata(g_host_services->context, file->host_handle, &metadata);
        have_metadata = status.status == HL_STATUS_OK;
    }
    uint64_t end = offset + size;
    pthread_mutex_lock(&g_bound_mapping_gate);
    pthread_mutex_lock(&g_bound_mapping_lock);
    for (bound_mapping *entry = *head; entry != NULL; entry = entry->next) {
        if (entry->object->shared || entry->follow_hi <= entry->follow_lo ||
            !bound_mapping_same_file(entry->object, file, &metadata, have_metadata))
            continue;
        uint64_t map_lo = entry->file_offset + entry->follow_lo;
        uint64_t map_hi = entry->file_offset + entry->follow_hi;
        uint64_t lo = offset > map_lo ? offset : map_lo;
        uint64_t hi = end < map_hi ? end : map_hi;
        if (hi > lo) {
            hl_host_bytes output = {(void *)(uintptr_t)(entry->address + lo - entry->file_offset),
                                    (size_t)(hi - lo)};
            (void)g_host_services->file->read_at(g_host_services->context, file->host_handle, lo, output);
        }
    }
    pthread_mutex_unlock(&g_bound_mapping_lock);
    pthread_mutex_unlock(&g_bound_mapping_gate);
}

/* A size-preserving external write may populate pages that became accessible
 * after EOF was extended. Refresh only that clean private follow range; pages
 * dirtied before the resize remain private and untouched. */
static void bound_mapping_file_data_changed(const hl_linux_fd_snapshot *file, uint64_t device, uint64_t inode) {
    bound_mapping **head = bound_mapping_head();
    hl_host_file_metadata metadata = {.stable_device = device, .stable_object = inode};
    if (head == NULL || g_host_services == NULL || g_host_services->file == NULL ||
        g_host_services->file->read_at == NULL)
        return;
    pthread_mutex_lock(&g_bound_mapping_gate);
    pthread_mutex_lock(&g_bound_mapping_lock);
    for (bound_mapping *entry = *head; entry != NULL; entry = entry->next) {
        if (entry->object->shared || entry->follow_hi <= entry->follow_lo ||
            !bound_mapping_same_file(entry->object, file, &metadata, 1))
            continue;
        hl_host_bytes output = {(void *)(uintptr_t)(entry->address + entry->follow_lo),
                                (size_t)(entry->follow_hi - entry->follow_lo)};
        (void)g_host_services->file->read_at(g_host_services->context, file->host_handle,
                                             entry->file_offset + entry->follow_lo, output);
    }
    pthread_mutex_unlock(&g_bound_mapping_lock);
    pthread_mutex_unlock(&g_bound_mapping_gate);
}

static int bound_snapshot(uint64_t value, hl_linux_fd_snapshot *snapshot) {
    if (g_linux_box == NULL || value > UINT32_MAX) return 0;
    return hl_linux_fd_snapshot_get(g_linux_box, (hl_linux_fd)value, snapshot) == HL_STATUS_OK;
}

static int bound_shadow_reserve(int minimum) {
    int candidate;
    if (g_bound_sentinel < 0 || minimum < 0) {
        errno = EBADF;
        return -1;
    }
    /* Allocate in the guest namespace, not by the host kernel's lowest native
     * descriptor. Opaque and engine-private descriptors may occupy low host
     * numbers but are not guest-visible. Relocate known engine descriptors,
     * skip live native guest descriptors and typed reservations, then install
     * the sentinel shadow at the exact lowest logical slot. */
    for (candidate = minimum; candidate < guest_nofile_cur(); ++candidate) {
        hl_linux_fd_snapshot snapshot;
        int shadow;
        if (bound_snapshot((uint64_t)(unsigned)candidate, &snapshot)) continue;
        engine_fd_vacate(candidate);
        if (fcntl(candidate, F_GETFD) >= 0 || errno != EBADF) continue;
        shadow = dup2(g_bound_sentinel, candidate);
        if (shadow < 0) return -1;
        if (fcntl(shadow, F_SETFD, FD_CLOEXEC) != 0) {
            int error = errno;
            close(shadow);
            errno = error;
            return -1;
        }
        return shadow;
    }
    errno = EMFILE;
    return -1;
}

static int bound_shadow_matches(int fd) {
    struct stat sentinel_status;
    struct stat shadow_status;
    return g_bound_sentinel >= 0 && fstat(g_bound_sentinel, &sentinel_status) == 0 && fstat(fd, &shadow_status) == 0 &&
           sentinel_status.st_dev == shadow_status.st_dev && sentinel_status.st_ino == shadow_status.st_ino &&
           sentinel_status.st_rdev == shadow_status.st_rdev &&
           (sentinel_status.st_mode & S_IFMT) == (shadow_status.st_mode & S_IFMT);
}

static int bound_private_dup(int source, int minimum) {
    hl_linux_fd_snapshot snapshot;
    int candidate = minimum;
    for (;;) {
        int duplicate = fcntl(source, F_DUPFD_CLOEXEC, candidate);
        if (duplicate < 0) return -1;
        if (!bound_snapshot((uint64_t)(unsigned)duplicate, &snapshot)) return duplicate;
        close(duplicate);
        if (duplicate == INT_MAX) {
            errno = EMFILE;
            return -1;
        }
        candidate = duplicate + 1;
    }
}

/* Called once in the isolated worker, before any guest-visible native descriptor allocation. */
static int bound_shadow_activate(void) {
    hl_linux_fd_snapshot snapshot;
    uint32_t fd;
    int opened;
    if (g_linux_box == NULL) return 0;
    /* Typed stdio alone still requires a sentinel so dup/F_DUPFD can allocate guest-number shadows. */
    if (g_bound_sentinel >= 0) {
        for (fd = 0; fd < g_linux_box->fd_capacity; ++fd) {
            if (hl_linux_fd_snapshot_get(g_linux_box, fd, &snapshot) == HL_STATUS_OK && fd >= 3 &&
                !bound_shadow_matches((int)fd))
                return -1;
        }
        return 0;
    }
    opened = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (opened < 0) return -1;
    g_bound_sentinel = bound_private_dup(opened, 64);
    if (g_bound_sentinel < 0) {
        int error = errno;
        close(opened);
        errno = error;
        return -1;
    }
    close(opened);
    for (fd = 0; fd < g_linux_box->fd_capacity; ++fd) {
        int shadow;
        if (hl_linux_fd_snapshot_get(g_linux_box, fd, &snapshot) != HL_STATUS_OK) continue;
        if (fd < 3) continue;
        shadow = bound_shadow_reserve((int)fd);
        if (shadow != (int)fd) {
            int error = shadow < 0 ? errno : EBUSY;
            if (shadow >= 0) close(shadow);
            errno = error;
            goto activation_failed;
        }
    }
    return 0;

activation_failed: {
    int error = errno;
    uint32_t rollback;
    for (rollback = 3; rollback < fd; ++rollback)
        if (hl_linux_fd_snapshot_get(g_linux_box, rollback, &snapshot) == HL_STATUS_OK) close((int)rollback);
    close(g_bound_sentinel);
    g_bound_sentinel = -1;
    errno = error;
    return -1;
}
}

static int64_t bound_dup_at_least(hl_linux_fd source, int minimum, uint32_t descriptor_flags) {
    int shadow = bound_shadow_reserve(minimum);
    int64_t result;
    if (shadow < 0) return -(int64_t)errno;
    if (shadow >= guest_nofile_cur()) {
        close(shadow);
        return -EMFILE;
    }
    result = hl_linux_dup3(g_linux_box, source, (hl_linux_fd)shadow, descriptor_flags != 0 ? HL_LINUX_O_CLOEXEC : 0);
    if (result < 0) close(shadow);
    return result;
}

static int bound_handle_reserve(void *opaque) {
    bound_handle_slot *slot = opaque;
    hl_status status;
    int shadow = bound_shadow_reserve(0);
    if (slot == NULL || slot->active) return -EINVAL;
    if (shadow < 0 || shadow >= guest_nofile_cur()) {
        if (shadow >= 0) close(shadow);
        return -EMFILE;
    }
    for (;;) {
        status = hl_linux_fd_reserve_at(g_linux_box, (hl_linux_fd)shadow, &slot->reservation);
        if (status != HL_STATUS_ALREADY_EXISTS) break;
        close(shadow);
        shadow = bound_shadow_reserve(shadow + 1);
        if (shadow < 0 || shadow >= guest_nofile_cur()) break;
    }
    if (status != HL_STATUS_OK || shadow < 0 || shadow >= guest_nofile_cur()) {
        if (shadow >= 0) close(shadow);
        return -EMFILE;
    }
    slot->shadow = shadow;
    slot->active = 1;
    return 0;
}

static void bound_handle_cancel(bound_handle_slot *slot) {
    if (slot == NULL || !slot->active) return;
    (void)hl_linux_fd_cancel(g_linux_box, &slot->reservation);
    close(slot->shadow);
    slot->active = 0;
}

static int64_t bound_adopt_handle(bound_handle_slot *slot, hl_host_handle file, uint32_t flags) {
    if (slot == NULL || !slot->active) return -EMFILE;
    int64_t result = hl_linux_file_adopt_reserved(g_linux_box, &slot->reservation, file, flags);
    if (result < 0) {
        bound_handle_cancel(slot);
    } else {
        slot->active = 0;
    }
    return result;
}

static int bound_handle_dirfd_error(int fd) {
    hl_linux_fd_snapshot snapshot;
    hl_host_file_metadata metadata;
    if (fd < 0 || !bound_snapshot((uint64_t)(uint32_t)fd, &snapshot)) return -EBADF;
    if (g_host_services == NULL || g_host_services->file == NULL || g_host_services->file->metadata == NULL)
        return -ENOTDIR;
    hl_host_result result =
        g_host_services->file->metadata(g_host_services->context, snapshot.host_handle, &metadata);
    if (result.status != HL_STATUS_OK) return bound_host_error(result.status);
    return metadata.type == HL_HOST_FILE_TYPE_DIRECTORY ? -EACCES : -ENOTDIR;
}

/* Resolution may temporarily occupy low native descriptors. Once its opaque
 * handles are closed, republish the new typed OFD at the true lowest logical
 * guest slot and retire the temporary shadow. */
static int64_t bound_relocate_lowest(int64_t opened) {
    int shadow;
    int64_t duplicated;
    hl_linux_fd_snapshot snapshot;
    if (opened < 0) return opened;
    shadow = bound_shadow_reserve(0);
    if (shadow < 0) return opened;
    uint32_t flags = hl_linux_fd_snapshot_get(g_linux_box, (hl_linux_fd)opened, &snapshot) == HL_STATUS_OK &&
                             snapshot.descriptor_flags != 0
                         ? HL_LINUX_O_CLOEXEC
                         : 0;
    duplicated = hl_linux_dup3(g_linux_box, (hl_linux_fd)opened, (hl_linux_fd)shadow, flags);
    if (duplicated < 0) {
        close(shadow);
        return opened;
    }
    (void)hl_linux_close(g_linux_box, (hl_linux_fd)opened);
    (void)close((int)opened);
    return duplicated;
}

static int bound_path_copy(uint64_t address, char path[HL_LINUX_PATH_MAX + 1], size_t *path_size) {
    size_t index;
    if (address == 0 || path == NULL || path_size == NULL) return -HL_LINUX_EFAULT;
    for (index = 0; index < HL_LINUX_PATH_MAX; ++index) {
        if (address > UINTPTR_MAX - index) return -HL_LINUX_EFAULT;
        const char *byte = (const char *)(uintptr_t)(address + index);
        if (!host_range_mapped((uintptr_t)byte, 1)) return -HL_LINUX_EFAULT;
        path[index] = *byte;
        if (path[index] == 0) {
            if (index == 0) return -HL_LINUX_ENOENT;
            *path_size = index;
            return 0;
        }
    }
    return -HL_LINUX_ENAMETOOLONG;
}

static int bound_vectors_copy(uint64_t address, uint64_t count, hl_host_iovec vectors[HL_LINUX_IOV_MAX]) {
    uint64_t index;
    size_t array_size;
    if (count > HL_LINUX_IOV_MAX) return -HL_LINUX_EINVAL;
    if (count == 0) return 0;
    if (address == 0 || count > SIZE_MAX / sizeof(*vectors)) return -HL_LINUX_EFAULT;
    array_size = (size_t)count * sizeof(*vectors);
    if (address > UINTPTR_MAX || array_size > UINTPTR_MAX - (uintptr_t)address ||
        !host_range_mapped((uintptr_t)address, array_size))
        return -HL_LINUX_EFAULT;
    memcpy(vectors, (const void *)(uintptr_t)address, array_size);
    for (index = 0; index < count; ++index) {
        uint64_t base = vectors[index].address;
        uint64_t size = vectors[index].size;
        if (size > SIZE_MAX || base > UINTPTR_MAX || (size != 0 && base == 0) || size > UINTPTR_MAX - (uintptr_t)base ||
            (size != 0 && !host_range_mapped((uintptr_t)base, (size_t)size)))
            return -HL_LINUX_EFAULT;
    }
    return 0;
}

static int bound_poll_references(uint64_t address, uint64_t count) {
    struct pollfd *fds;
    uint64_t index;
    hl_linux_fd_snapshot snapshot;
    if (count > SIZE_MAX / sizeof(*fds) ||
        (count != 0 && !host_range_mapped((uintptr_t)address, (size_t)count * sizeof(*fds))))
        return 0;
    fds = (struct pollfd *)(uintptr_t)address;
    for (index = 0; index < count; ++index)
        if (fds[index].fd >= 0 && bound_snapshot((uint64_t)(unsigned)fds[index].fd, &snapshot)) return 1;
    return 0;
}

static int bound_fdsets_reference(uint64_t count, uint64_t read_set, uint64_t write_set, uint64_t except_set) {
    uint64_t fd;
    size_t bytes;
    hl_linux_fd_snapshot snapshot;
    if (count > HL_LINUX_FD_LIMIT) count = HL_LINUX_FD_LIMIT;
    bytes = (size_t)((count + 7u) / 8u);
    if ((read_set != 0 && !host_range_mapped((uintptr_t)read_set, bytes)) ||
        (write_set != 0 && !host_range_mapped((uintptr_t)write_set, bytes)) ||
        (except_set != 0 && !host_range_mapped((uintptr_t)except_set, bytes)))
        return 0;
    for (fd = 0; fd < count; ++fd) {
        uint8_t mask = (uint8_t)(1u << (fd & 7u));
        size_t byte = (size_t)(fd >> 3);
        if (((read_set != 0 && (((uint8_t *)(uintptr_t)read_set)[byte] & mask) != 0) ||
             (write_set != 0 && (((uint8_t *)(uintptr_t)write_set)[byte] & mask) != 0) ||
             (except_set != 0 && (((uint8_t *)(uintptr_t)except_set)[byte] & mask) != 0)) &&
            bound_snapshot(fd, &snapshot))
            return 1;
    }
    return 0;
}

static uint32_t bound_poll_interests(short events) {
    uint32_t interests = 0;
    if ((events & POLLIN) != 0) interests |= HL_LINUX_READY_READ;
    if ((events & POLLOUT) != 0) interests |= HL_LINUX_READY_WRITE;
    if ((events & POLLPRI) != 0) interests |= HL_LINUX_READY_PRIORITY;
    return interests;
}

static short bound_poll_readiness(uint32_t readiness) {
    short events = 0;
    if ((readiness & HL_LINUX_READY_READ) != 0) events |= POLLIN;
    if ((readiness & HL_LINUX_READY_WRITE) != 0) events |= POLLOUT;
    if ((readiness & HL_LINUX_READY_PRIORITY) != 0) events |= POLLPRI;
    if ((readiness & HL_LINUX_READY_ERROR) != 0) events |= POLLERR;
    if ((readiness & HL_LINUX_READY_HANGUP) != 0) events |= POLLHUP;
    return events;
}

static uint64_t bound_now_ns(void) {
    struct timespec now = {0, 0};
    if (hl_production_clock_gettime(effective_host_services(), HL_PRODUCTION_CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t bound_deadline(const struct timespec *timeout) {
    uint64_t now;
    uint64_t delta;
    if (timeout == NULL) return UINT64_MAX;
    if (timeout->tv_sec < 0) return 0;
    now = bound_now_ns();
    if ((uint64_t)timeout->tv_sec > UINT64_MAX / UINT64_C(1000000000)) return UINT64_MAX;
    delta = (uint64_t)timeout->tv_sec * UINT64_C(1000000000) + (uint64_t)timeout->tv_nsec;
    return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
}

/* Poll native descriptors from a private copy: typed guest slots are never host descriptors. */
static int64_t bound_ppoll(struct cpu *c, uint64_t address, uint64_t count, uint64_t timeout_address,
                           uint64_t mask_address) {
    struct pollfd *guest = (struct pollfd *)(uintptr_t)address;
    struct timespec *timeout = (struct timespec *)(uintptr_t)timeout_address;
    struct pollfd *native;
    hl_linux_poll_entry *objects;
    uint32_t *object_indices;
    uint64_t deadline;
    uint64_t index;
    uint32_t object_count = 0;
    uint64_t saved = 0;
    int mask_on;
    int64_t result = 0;
    if (count > (uint64_t)guest_nofile_cur()) return -EINVAL;
    if (count > SIZE_MAX / sizeof(*guest) || (count != 0 && guest_bad_ptr(address, (size_t)count * sizeof(*guest))) ||
        (timeout != NULL && guest_bad_ptr(timeout_address, sizeof(*timeout))))
        return -EFAULT;
    if (timeout != NULL && (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L)) return -EINVAL;
    if (mask_address != 0 && (size_t)G_A4(c) != 8) return -EINVAL;
    if (mask_address != 0 && guest_bad_ptr(mask_address, 8)) return -EFAULT;
    native = calloc(count != 0 ? (size_t)count : 1, sizeof(*native));
    objects = calloc(count != 0 ? (size_t)count : 1, sizeof(*objects));
    object_indices = calloc(count != 0 ? (size_t)count : 1, sizeof(*object_indices));
    if (native == NULL || objects == NULL || object_indices == NULL) {
        free(native);
        free(objects);
        free(object_indices);
        return -ENOMEM;
    }
    memcpy(native, guest, (size_t)count * sizeof(*native));
    for (index = 0; index < count; ++index) {
        hl_linux_fd_snapshot snapshot;
        guest[index].revents = 0;
        if (guest[index].fd >= 0 && bound_snapshot((uint64_t)(unsigned)guest[index].fd, &snapshot)) {
            object_indices[object_count] = (uint32_t)index;
            objects[object_count++] = (hl_linux_poll_entry){snapshot.fd, bound_poll_interests(guest[index].events), 0};
            native[index].fd = -1;
        }
    }
    deadline = bound_deadline(timeout);
    mask_on = poll_sigmask_enter(c, mask_address, &saved);
    for (;;) {
        int native_ready;
        int64_t object_ready = hl_linux_object_poll(g_linux_box, objects, object_count, 0);
        int wait_ms = 0;
        uint64_t now = bound_now_ns();
        if (object_ready < 0) {
            result = object_ready;
            break;
        }
        if (object_ready == 0 && deadline != 0 && now < deadline) wait_ms = 1;
        native_ready = poll(native, (nfds_t)count, wait_ms);
        if (native_ready < 0) {
            if (svc_poll_retry(c)) continue;
            result = -errno;
            break;
        }
        if (object_ready != 0 || native_ready != 0 || deadline == 0 ||
            (deadline != UINT64_MAX && bound_now_ns() >= deadline)) {
            result = native_ready + object_ready;
            for (index = 0; index < count; ++index)
                guest[index].revents = native[index].revents;
            for (index = 0; index < object_count; ++index)
                guest[object_indices[index]].revents = bound_poll_readiness(objects[index].readiness);
            break;
        }
    }
    if (mask_on) poll_sigmask_leave(c, saved);
    if (result >= 0 && timeout != NULL) {
        uint64_t now = bound_now_ns();
        uint64_t left = deadline != UINT64_MAX && deadline > now ? deadline - now : 0;
        timeout->tv_sec = (time_t)(left / UINT64_C(1000000000));
        timeout->tv_nsec = (long)(left % UINT64_C(1000000000));
    }
    free(objects);
    free(object_indices);
    free(native);
    return result;
}

static int bound_set_test(const uint8_t *set, uint32_t fd) {
    return set != NULL && (set[fd >> 3] & (uint8_t)(1u << (fd & 7u))) != 0;
}

static void bound_set_mark(uint8_t *set, uint32_t fd) {
    if (set != NULL) set[fd >> 3] |= (uint8_t)(1u << (fd & 7u));
}

static int64_t bound_pselect(struct cpu *c, uint64_t count_value, uint64_t read_address, uint64_t write_address,
                             uint64_t except_address) {
    uint32_t count = count_value > HL_LINUX_FD_LIMIT ? HL_LINUX_FD_LIMIT : (uint32_t)count_value;
    size_t bytes = ((size_t)count + 7u) / 8u;
    uint8_t *guest_read = (uint8_t *)(uintptr_t)read_address;
    uint8_t *guest_write = (uint8_t *)(uintptr_t)write_address;
    uint8_t *guest_except = (uint8_t *)(uintptr_t)except_address;
    struct timespec *timeout = (struct timespec *)(uintptr_t)G_A4(c);
    uint64_t mask_pair_address = G_A5(c);
    uint8_t *requested;
    struct pollfd *native;
    hl_linux_poll_entry *objects;
    uint32_t *object_indices;
    uint32_t object_count = 0;
    uint32_t fd;
    uint64_t deadline;
    uint64_t mask_address = 0;
    uint64_t saved = 0;
    int mask_on;
    int64_t result = 0;
    if (count_value > INT_MAX) return -EINVAL;
    if ((read_address != 0 && guest_bad_ptr(read_address, bytes)) ||
        (write_address != 0 && guest_bad_ptr(write_address, bytes)) ||
        (except_address != 0 && guest_bad_ptr(except_address, bytes)) ||
        (timeout != NULL && guest_bad_ptr(G_A4(c), sizeof(*timeout))))
        return -EFAULT;
    if (timeout != NULL && (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L)) return -EINVAL;
    if (mask_pair_address != 0) {
        const uint64_t *pair;
        if (guest_bad_ptr(mask_pair_address, 16)) return -EFAULT;
        pair = (const uint64_t *)(uintptr_t)mask_pair_address;
        if (pair[0] != 0) {
            if (pair[1] != 8) return -EINVAL;
            if (guest_bad_ptr(pair[0], 8)) return -EFAULT;
            mask_address = pair[0];
        }
    }
    requested = calloc(bytes != 0 ? bytes * 3 : 1, 1);
    native = calloc(count != 0 ? count : 1, sizeof(*native));
    objects = calloc(count != 0 ? count : 1, sizeof(*objects));
    object_indices = calloc(count != 0 ? count : 1, sizeof(*object_indices));
    if (requested == NULL || native == NULL || objects == NULL || object_indices == NULL) {
        result = -ENOMEM;
        goto done;
    }
    if (guest_read != NULL) memcpy(requested, guest_read, bytes);
    if (guest_write != NULL) memcpy(requested + bytes, guest_write, bytes);
    if (guest_except != NULL) memcpy(requested + bytes * 2, guest_except, bytes);
    for (fd = 0; fd < count; ++fd) {
        uint32_t interests = 0;
        hl_linux_fd_snapshot snapshot;
        if (bound_set_test(requested, fd)) interests |= HL_LINUX_READY_READ;
        if (bound_set_test(requested + bytes, fd)) interests |= HL_LINUX_READY_WRITE;
        if (bound_set_test(requested + bytes * 2, fd)) interests |= HL_LINUX_READY_PRIORITY;
        native[fd] = (struct pollfd){.fd = interests != 0 ? (int)fd : -1, .events = bound_poll_readiness(interests)};
        if (interests != 0 && bound_snapshot(fd, &snapshot)) {
            object_indices[object_count] = fd;
            objects[object_count++] = (hl_linux_poll_entry){snapshot.fd, interests, 0};
            native[fd].fd = -1;
        }
    }
    deadline = bound_deadline(timeout);
    mask_on = poll_sigmask_enter(c, mask_address, &saved);
    for (;;) {
        int native_ready;
        int64_t object_ready = hl_linux_object_poll(g_linux_box, objects, object_count, 0);
        uint64_t now = bound_now_ns();
        if (object_ready < 0) {
            result = object_ready;
            break;
        }
        native_ready = poll(native, count, object_ready == 0 && deadline != 0 && now < deadline ? 1 : 0);
        if (native_ready < 0) {
            if (svc_poll_retry(c)) continue;
            result = -errno;
            break;
        }
        for (fd = 0; fd < count; ++fd)
            if ((native[fd].revents & POLLNVAL) != 0) {
                result = -EBADF;
                goto waited;
            }
        if (native_ready != 0 || object_ready != 0 || deadline == 0 ||
            (deadline != UINT64_MAX && bound_now_ns() >= deadline)) {
            if (guest_read != NULL) memset(guest_read, 0, bytes);
            if (guest_write != NULL) memset(guest_write, 0, bytes);
            if (guest_except != NULL) memset(guest_except, 0, bytes);
            result = 0;
            for (fd = 0; fd < count; ++fd) {
                int ready = 0;
                if ((native[fd].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && bound_set_test(requested, fd)) {
                    bound_set_mark(guest_read, fd);
                    ready = 1;
                }
                if ((native[fd].revents & (POLLOUT | POLLERR)) != 0 && bound_set_test(requested + bytes, fd)) {
                    bound_set_mark(guest_write, fd);
                    ready = 1;
                }
                if ((native[fd].revents & POLLPRI) != 0 && bound_set_test(requested + bytes * 2, fd)) {
                    bound_set_mark(guest_except, fd);
                    ready = 1;
                }
                result += ready;
            }
            for (fd = 0; fd < object_count; ++fd) {
                uint32_t descriptor = object_indices[fd];
                int ready = 0;
                if ((objects[fd].readiness & (HL_LINUX_READY_READ | HL_LINUX_READY_HANGUP | HL_LINUX_READY_ERROR)) !=
                        0 &&
                    bound_set_test(requested, descriptor)) {
                    bound_set_mark(guest_read, descriptor);
                    ready = 1;
                }
                if ((objects[fd].readiness & (HL_LINUX_READY_WRITE | HL_LINUX_READY_ERROR)) != 0 &&
                    bound_set_test(requested + bytes, descriptor)) {
                    bound_set_mark(guest_write, descriptor);
                    ready = 1;
                }
                if ((objects[fd].readiness & HL_LINUX_READY_PRIORITY) != 0 &&
                    bound_set_test(requested + bytes * 2, descriptor)) {
                    bound_set_mark(guest_except, descriptor);
                    ready = 1;
                }
                result += ready;
            }
            break;
        }
    }
waited:
    if (mask_on) poll_sigmask_leave(c, saved);
    if (result >= 0 && timeout != NULL) {
        uint64_t now = bound_now_ns();
        uint64_t left = deadline != UINT64_MAX && deadline > now ? deadline - now : 0;
        timeout->tv_sec = (time_t)(left / UINT64_C(1000000000));
        timeout->tv_nsec = (long)(left % UINT64_C(1000000000));
    }
done:
    free(object_indices);
    free(objects);
    free(native);
    free(requested);
    return result;
}

static int bound_rights_reference(uint64_t message_address) {
    const uint8_t *message = (const uint8_t *)(uintptr_t)message_address;
    const uint8_t *control;
    uint64_t control_address;
    uint64_t control_size;
    uint64_t offset = 0;
    hl_linux_fd_snapshot snapshot;
    if (!host_range_mapped((uintptr_t)message_address, 56)) return 0;
    memcpy(&control_address, message + 32, sizeof(control_address));
    memcpy(&control_size, message + 40, sizeof(control_size));
#if SIZE_MAX < UINT64_MAX
    if (control_size > SIZE_MAX) return 0;
#endif
    if (control_address == 0 || control_size < 16 ||
        !host_range_mapped((uintptr_t)control_address, (size_t)control_size))
        return 0;
    control = (const uint8_t *)(uintptr_t)control_address;
    while (offset + 16 <= control_size) {
        uint64_t length;
        int32_t level;
        int32_t type;
        uint64_t data;
        memcpy(&length, control + offset, sizeof(length));
        memcpy(&level, control + offset + 8, sizeof(level));
        memcpy(&type, control + offset + 12, sizeof(type));
        if (length < 16 || length > control_size - offset) break;
        if (level == LX_SOL_SOCKET && type == SCM_RIGHTS) {
            for (data = 16; data + sizeof(int32_t) <= length; data += sizeof(int32_t)) {
                int32_t fd;
                memcpy(&fd, control + offset + data, sizeof(fd));
                if (fd >= 0 && bound_snapshot((uint64_t)(uint32_t)fd, &snapshot)) return 1;
            }
        }
        if (length > UINT64_MAX - 7u) break;
        offset += (length + 7u) & ~UINT64_C(7);
    }
    return 0;
}

/* Return 1 with a scoped native alias for a typed file, 0 for an already-native fd, or -errno. */
static int bound_attachment_borrow(int guest_fd, int *native_fd) {
    hl_linux_fd_snapshot snapshot;
    const hl_host_posix_attachment_services *attachments;
    hl_host_result borrowed;
    if (native_fd == NULL || guest_fd < 0) return -EBADF;
    if (!bound_snapshot((uint64_t)(uint32_t)guest_fd, &snapshot)) {
        if (fcntl(guest_fd, F_GETFD) < 0) return -EBADF;
        *native_fd = guest_fd;
        return 0;
    }
    attachments = g_host_services == NULL ? NULL : g_host_services->posix_attachment;
    if (attachments == NULL || attachments->abi != HL_HOST_POSIX_ATTACHMENT_ABI ||
        attachments->size < sizeof(*attachments) || attachments->borrow_file == NULL)
        return -EOPNOTSUPP;
    borrowed = attachments->borrow_file(g_host_services->context, snapshot.host_handle);
    if (borrowed.status != HL_STATUS_OK) return bound_host_error(borrowed.status);
    if (borrowed.value > INT_MAX) {
        if (attachments->release != NULL) (void)attachments->release(g_host_services->context, borrowed.value);
        return -EIO;
    }
    *native_fd = (int)borrowed.value;
    return 1;
}

static void bound_attachment_release(int native_fd) {
    const hl_host_posix_attachment_services *attachments =
        g_host_services == NULL ? NULL : g_host_services->posix_attachment;
    if (attachments != NULL && attachments->release != NULL)
        (void)attachments->release(g_host_services->context, (uint64_t)(unsigned)native_fd);
    else
        close(native_fd);
}

static int64_t bound_stream_read(const hl_linux_fd_snapshot *file, int native_fd, void *buffer, size_t size,
                                 off_t *offset) {
    if (file != NULL)
        return offset != NULL ? hl_linux_pread64(g_linux_box, file->fd, buffer, size, (uint64_t)*offset)
                              : hl_linux_read(g_linux_box, file->fd, buffer, size);
    ssize_t count = offset != NULL ? pread(native_fd, buffer, size, *offset) : read(native_fd, buffer, size);
    return count < 0 ? -errno : count;
}

static int64_t bound_stream_write(const hl_linux_fd_snapshot *file, int native_fd, const void *buffer, size_t size,
                                  off_t *offset) {
    if (file != NULL)
        return offset != NULL ? hl_linux_pwrite64(g_linux_box, file->fd, buffer, size, (uint64_t)*offset)
                              : hl_linux_write(g_linux_box, file->fd, buffer, size);
    ssize_t count = offset != NULL ? pwrite(native_fd, buffer, size, *offset) : write(native_fd, buffer, size);
    return count < 0 ? -errno : count;
}

static int64_t bound_sendfile(const hl_linux_fd_snapshot *output, int output_fd,
                              const hl_linux_fd_snapshot *input, int input_fd, uint64_t offset_address,
                              uint64_t count) {
    off_t supplied_offset = 0;
    off_t *input_offset = NULL;
    uint64_t done = 0;
    int64_t error = 0;
    char buffer[8192];
    if (input == NULL) {
        struct stat metadata;
        if (fstat(input_fd, &metadata) != 0) return -errno;
        if (!S_ISREG(metadata.st_mode)) return -EINVAL;
    } else if (g_host_services != NULL && g_host_services->file != NULL &&
               g_host_services->file->metadata != NULL) {
        hl_host_file_metadata metadata;
        hl_host_result status =
            g_host_services->file->metadata(g_host_services->context, input->host_handle, &metadata);
        if (status.status != HL_STATUS_OK) return bound_host_error(status.status);
        if (metadata.type != HL_HOST_FILE_TYPE_REGULAR) return -EINVAL;
    }
    if (offset_address != 0) {
        if (!host_range_mapped((uintptr_t)offset_address, sizeof(off_t))) return -EFAULT;
        supplied_offset = *(off_t *)(uintptr_t)offset_address;
        if (supplied_offset < 0) return -EINVAL;
        input_offset = &supplied_offset;
    }
    if (count > UINT64_C(0x7ffff000)) count = UINT64_C(0x7ffff000); /* Linux MAX_RW_COUNT */
    while (done < count) {
        uint64_t remaining = count - done;
        size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        int64_t read_count = bound_stream_read(input, input_fd, buffer, chunk, input_offset);
        if (read_count <= 0) {
            error = read_count;
            break;
        }
        int64_t written = bound_stream_write(output, output_fd, buffer, (size_t)read_count, NULL);
        if (written <= 0) {
            error = written;
            if (input_offset == NULL)
                (void)(input != NULL ? hl_linux_lseek(g_linux_box, input->fd, -read_count, SEEK_CUR)
                                     : lseek(input_fd, (off_t)-read_count, SEEK_CUR));
            break;
        }
        if (input_offset != NULL) *input_offset += (off_t)written;
        if (output != NULL)
            bound_mapping_file_written(output, output->offset + done, (uint64_t)written);
        done += (uint64_t)written;
        if (written != read_count) {
            if (input_offset == NULL)
                (void)(input != NULL ? hl_linux_lseek(g_linux_box, input->fd, written - read_count, SEEK_CUR)
                                     : lseek(input_fd, (off_t)(written - read_count), SEEK_CUR));
            break;
        }
    }
    if (offset_address != 0)
        *(off_t *)(uintptr_t)offset_address = supplied_offset;
    return done != 0 ? (int64_t)done : error;
}

static int bound_native_pipe(int fd) {
    struct stat metadata;
    return fstat(fd, &metadata) == 0 && S_ISFIFO(metadata.st_mode);
}

static int64_t bound_splice(const hl_linux_fd_snapshot *input, int input_fd, uint64_t input_offset_address,
                            const hl_linux_fd_snapshot *output, int output_fd, uint64_t output_offset_address,
                            uint64_t size, uint64_t flags) {
    off_t *input_offset = (off_t *)(uintptr_t)input_offset_address;
    off_t *output_offset = (off_t *)(uintptr_t)output_offset_address;
    int input_pipe = input == NULL && bound_native_pipe(input_fd);
    int output_pipe = output == NULL && bound_native_pipe(output_fd);
    static _Thread_local char buffer[65536];
    int64_t read_count, write_count, write_error = 0;
    size_t pushed = 0;
    if (flags & ~UINT64_C(0xf)) return -EINVAL;
    if (!input_pipe && !output_pipe) return -EINVAL;
    if ((input_pipe && input_offset != NULL) || (output_pipe && output_offset != NULL)) return -ESPIPE;
    if ((input_offset != NULL && !host_range_mapped((uintptr_t)input_offset, sizeof(*input_offset))) ||
        (output_offset != NULL && !host_range_mapped((uintptr_t)output_offset, sizeof(*output_offset))))
        return -EFAULT;
    if (size > UINT64_C(0x7ffff000)) size = UINT64_C(0x7ffff000);
    if (size > sizeof(buffer)) size = sizeof(buffer);
    if (size == 0) return 0;
    if (input_pipe) pushed = pipe_pushback_take(input_fd, buffer, (size_t)size);
    read_count = pushed != 0 ? (int64_t)pushed
                             : bound_stream_read(input, input_fd, buffer, (size_t)size, input_offset);
    if (read_count <= 0) return read_count;
    write_count = bound_stream_write(output, output_fd, buffer, (size_t)read_count, output_offset);
    if (write_count < 0) {
        write_error = write_count;
        write_count = 0;
    }
    if (write_count < read_count) {
        size_t remainder = (size_t)(read_count - write_count);
        if (input_pipe)
            pipe_pushback_set(input_fd, buffer + write_count, remainder);
        else if (input_offset == NULL)
            (void)(input != NULL ? hl_linux_lseek(g_linux_box, input->fd, write_count - read_count, SEEK_CUR)
                                 : lseek(input_fd, (off_t)(write_count - read_count), SEEK_CUR));
    }
    if (write_count == 0) return write_error;
    if (input_offset != NULL) *input_offset += (off_t)write_count;
    if (output != NULL)
        bound_mapping_file_written(output,
                                   output_offset != NULL ? (uint64_t)*output_offset : output->offset,
                                   (uint64_t)write_count);
    if (output_offset != NULL) *output_offset += (off_t)write_count;
    return write_count;
}

static int bound_route(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    hl_linux_fd_snapshot source;
    int64_t result;
    int source_bound = bound_snapshot(a0, &source);
    if (nr == 78 && a1 != 0 && a2 != 0 && (int64_t)a3 > 0 &&
        host_range_mapped((uintptr_t)a1, 1) && host_range_mapped((uintptr_t)a2, (size_t)a3)) {
        int guest_fd = procfd_num((const char *)(uintptr_t)a1);
        hl_linux_fd_snapshot target;
        if (guest_fd >= 0 && bound_snapshot((uint64_t)(uint32_t)guest_fd, &target)) {
            int native_fd;
            int borrowed = bound_attachment_borrow(guest_fd, &native_fd);
            if (borrowed < 0) {
                G_RET(c) = (uint64_t)(int64_t)borrowed;
                return 1;
            }
            char host_path[4200];
            int path_status = hl_native_fd_path(native_fd, host_path, sizeof(host_path));
            if (borrowed > 0) bound_attachment_release(native_fd);
            if (path_status != 0) {
                G_RET(c) = (uint64_t)(int64_t)(-ENOENT);
                return 1;
            }
            char guest_path[4200];
            guest_from_host(host_path, guest_path, sizeof(guest_path));
            size_t length = strlen(guest_path);
            if (length > (size_t)a3) length = (size_t)a3;
            memcpy((void *)(uintptr_t)a2, guest_path, length);
            G_RET(c) = (uint64_t)length;
            return 1;
        }
    }
    if (nr == 73 && bound_poll_references(a0, a1)) {
        G_RET(c) = (uint64_t)bound_ppoll(c, a0, a1, a2, a3);
        return 1;
    }
    if (nr == 72 && bound_fdsets_reference(a0, a1, a2, a3)) {
        G_RET(c) = (uint64_t)bound_pselect(c, a0, a1, a2, a3);
        return 1;
    }
    if (nr == 222 && (a3 & 0x20u) == 0) {
        hl_linux_fd_snapshot mapped;
        if (bound_snapshot(G_A4(c), &mapped)) {
            G_RET(c) = (uint64_t)bound_mmap_file(&mapped, a0, a1, (uint32_t)a2, (uint32_t)a3, G_A5(c));
            return 1;
        }
    }
    if (nr == 215 || nr == 226 || nr == 227) {
        pthread_mutex_lock(&g_bound_mapping_gate);
        pthread_mutex_lock(&g_bound_mapping_lock);
        bound_mapping *mapping = bound_mapping_find(a0, a1);
        if (mapping != NULL) {
            uint64_t offset = a0 - mapping->address;
            hl_host_result operation;
            /* Guest mprotect is modeled by the 4 KiB Linux VMA/SMC registries in svc_mem. Routing a
             * typed file mapping to host protect applies macOS's 16 KiB granularity and can protect
             * adjacent ELF segments, breaking ld.so RELRO. Keep the typed mapping ledger, but let the
             * common guest-logical path validate the range and update permissions. */
            if (nr == 226) {
                pthread_mutex_unlock(&g_bound_mapping_lock);
                pthread_mutex_unlock(&g_bound_mapping_gate);
                return 0;
            }
            if (nr == 227 && (((a2 & ~(uint64_t)7u) != 0) || (a2 & 5u) == 0 || (a2 & 5u) == 5u)) {
                G_RET(c) = (uint64_t)(int64_t)(-EINVAL);
                pthread_mutex_unlock(&g_bound_mapping_lock);
                pthread_mutex_unlock(&g_bound_mapping_gate);
                return 1;
            }
            if (nr == 215)
                operation = g_host_services->memory->unmap_range(g_host_services->context, mapping->object->handle,
                                                                 mapping->object_offset + offset, a1);
            else
                operation = g_host_services->memory->sync(g_host_services->context, mapping->object->handle,
                                                          mapping->object_offset + offset, a1);
            if (operation.status == HL_STATUS_OK && nr == 215) {
                bound_mapping_retire(a0, a1);
                gmap_split_unmap(a0, a0 + a1);
                gbus_clear(a0, a0 + a1);
            }
            G_RET(c) = (uint64_t)bound_host_error(operation.status);
            pthread_mutex_unlock(&g_bound_mapping_lock);
            pthread_mutex_unlock(&g_bound_mapping_gate);
            return 1;
        }
        pthread_mutex_unlock(&g_bound_mapping_lock);
        pthread_mutex_unlock(&g_bound_mapping_gate);
    }
    if (nr == 71) {
        hl_linux_fd_snapshot second;
        int second_bound = bound_snapshot(a1, &second);
        if (source_bound || second_bound) {
            G_RET(c) = (uint64_t)bound_sendfile(source_bound ? &source : NULL, (int)a0,
                                                second_bound ? &second : NULL, (int)a1, a2, a3);
            return 1;
        }
    }
    if (nr == 76) {
        hl_linux_fd_snapshot second;
        int second_bound = bound_snapshot(a2, &second);
        if (source_bound || second_bound) {
            G_RET(c) = (uint64_t)bound_splice(source_bound ? &source : NULL, (int)a0, a1,
                                              second_bound ? &second : NULL, (int)a2, a3, G_A4(c), G_A5(c));
            return 1;
        }
    }
    if ((nr == 75 || nr == 77) && source_bound) {
        /* vmsplice and tee require pipe endpoints. Typed descriptors currently name ordinary files. */
        G_RET(c) = (uint64_t)(int64_t)(-EINVAL);
        return 1;
    }
    if (nr == 77) {
        hl_linux_fd_snapshot second;
        if (bound_snapshot(a1, &second)) {
            G_RET(c) = (uint64_t)(int64_t)(-EINVAL);
            return 1;
        }
    }
    if (nr == 24 && !source_bound) {
        hl_linux_fd_snapshot target;
        if (bound_snapshot(a1, &target)) {
            G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
            return 1;
        }
    }
    if (nr == 21 && !source_bound) {
        hl_linux_fd_snapshot watched;
        if (bound_snapshot(a2, &watched)) {
            int64_t epoll_result = -ENOSYS;
            if (a1 == HL_LINUX_EPOLL_ADD && g_host_services != NULL && g_host_services->file != NULL &&
                g_host_services->file->metadata != NULL) {
                hl_host_file_metadata metadata;
                hl_host_result status =
                    g_host_services->file->metadata(g_host_services->context, watched.host_handle, &metadata);
                if (status.status == HL_STATUS_OK &&
                    (metadata.type == HL_HOST_FILE_TYPE_REGULAR || metadata.type == HL_HOST_FILE_TYPE_DIRECTORY))
                    epoll_result = -EPERM;
            }
            G_RET(c) = (uint64_t)epoll_result;
            return 1;
        }
    }
    if (!source_bound) return 0;
    switch (nr) {
    case 35: {
        char path[HL_LINUX_PATH_MAX + 1];
        size_t path_size;
        if ((a2 & ~UINT64_C(0x200)) != 0) {
            result = -EINVAL;
            break;
        }
        result = bound_path_copy(a1, path, &path_size);
        if (result != 0) break;
        if (path[0] == '/') return 0;
        if ((a2 & UINT64_C(0x200)) != 0 || g_host_services->file->unlink_relative == NULL) {
            result = -ENOSYS;
            break;
        }
        result = bound_host_error(g_host_services->file
                                      ->unlink_relative(g_host_services->context, source.host_handle, path, path_size)
                                      .status);
        break;
    }
    case 53:
    case 452: {
        char path[HL_LINUX_PATH_MAX + 1];
        size_t path_size;
        uint64_t flags = nr == 452 ? a3 : 0;
        if ((flags & ~UINT64_C(0x1100)) != 0) {
            result = -EINVAL;
            break;
        }
        result = bound_path_copy(a1, path, &path_size);
        if (result != 0) break;
        if (path[0] == '/') return 0;
        if (g_host_services->file->open_relative == NULL || g_host_services->file->set_permissions == NULL) {
            result = -ENOSYS;
            break;
        }
        uint32_t access = HL_HOST_FILE_PATH_ONLY;
        if ((flags & UINT64_C(0x100)) != 0) access |= HL_HOST_FILE_NOFOLLOW;
        hl_host_result opened = g_host_services->file->open_relative(
            g_host_services->context, source.host_handle, path, path_size, access, 0, 0);
        if (opened.status != HL_STATUS_OK) {
            result = bound_host_error(opened.status);
            break;
        }
        result = bound_host_error(g_host_services->file
                                      ->set_permissions(g_host_services->context, opened.value, (uint32_t)a2 & 07777u)
                                      .status);
        (void)g_host_services->file->close(g_host_services->context, opened.value);
        break;
    }
    case 48:
    case 439: {
        char path[HL_LINUX_PATH_MAX + 1];
        size_t path_size;
        uint64_t flags = nr == 439 ? a3 : 0;
        if (a2 > 7 || (flags & ~UINT64_C(0x1200)) != 0) {
            result = -EINVAL;
            break;
        }
        result = bound_path_copy(a1, path, &path_size);
        if (result != 0) break;
        if (path[0] == '/') return 0;
        uint32_t access = a2 == 0 ? HL_HOST_FILE_PATH_ONLY : 0;
        if ((a2 & 4u) != 0) access |= HL_HOST_FILE_READ;
        if ((a2 & 2u) != 0) access |= HL_HOST_FILE_WRITE;
        if ((a2 & 1u) != 0) access |= HL_HOST_FILE_PATH_ONLY;
        hl_host_result opened = g_host_services->file->open_relative(
            g_host_services->context, source.host_handle, path, path_size, access, 0, 0);
        result = bound_host_error(opened.status);
        if (opened.status == HL_STATUS_OK) {
            if ((a2 & 1u) != 0) {
                hl_host_file_metadata metadata;
                hl_host_result measured =
                    g_host_services->file->metadata(g_host_services->context, opened.value, &metadata);
                if (measured.status != HL_STATUS_OK)
                    result = bound_host_error(measured.status);
                else if (metadata.type != HL_HOST_FILE_TYPE_DIRECTORY && (metadata.permissions & 0111u) == 0)
                    result = -EACCES;
            }
            (void)g_host_services->file->close(g_host_services->context, opened.value);
        }
        break;
    }
    case 79: {
        char path[HL_LINUX_PATH_MAX + 1];
        size_t path_size;
        if ((a3 & ~UINT64_C(0x1900)) != 0) {
            result = -EINVAL;
            break;
        }
        if (!host_range_mapped((uintptr_t)a2, GUEST_LINUX_STAT_BYTES)) {
            result = -EFAULT;
            break;
        }
        result = bound_path_copy(a1, path, &path_size);
        int empty = result == -HL_LINUX_ENOENT && (a3 & UINT64_C(0x1000)) != 0;
        if (result != 0 && !empty) break;
        if (!empty && path[0] == '/') return 0;
        hl_host_handle target = source.host_handle;
        int close_target = 0;
        if (!empty) {
            uint32_t access = HL_HOST_FILE_PATH_ONLY;
            if ((a3 & UINT64_C(0x100)) != 0) access |= HL_HOST_FILE_NOFOLLOW;
            hl_host_result opened = g_host_services->file->open_relative(
                g_host_services->context, source.host_handle, path, path_size, access, 0, 0);
            if (opened.status != HL_STATUS_OK) {
                result = bound_host_error(opened.status);
                break;
            }
            target = opened.value;
            close_target = 1;
        }
        hl_host_file_metadata metadata;
        hl_host_result measured = g_host_services->file->metadata(g_host_services->context, target, &metadata);
        if (measured.status != HL_STATUS_OK) {
            result = bound_host_error(measured.status);
        } else {
            hl_linux_file_status status;
            hl_linux_fd_snapshot target_snapshot = {.host_handle = target};
            bound_status_from_metadata(&status, &metadata);
            bound_virtualize_owner(&target_snapshot, &status);
            fill_linux_bound_stat((uint8_t *)(uintptr_t)a2, &status);
            result = 0;
        }
        if (close_target) (void)g_host_services->file->close(g_host_services->context, target);
        break;
    }
    case 56: {
        const uint32_t supported = HL_LINUX_O_ACCMODE | HL_LINUX_O_CREAT | HL_LINUX_O_EXCL | HL_LINUX_O_TRUNC |
                                   HL_LINUX_O_APPEND | HL_LINUX_O_DIRECTORY | HL_LINUX_O_CLOEXEC;
        size_t path_size;
        char path[HL_LINUX_PATH_MAX + 1];
        int shadow;
        hl_linux_fd_reservation reservation;
        hl_status status;
        result = bound_path_copy(a1, path, &path_size);
        if (result != 0) break;
        if (path[0] == '/') return 0;
        if ((a2 & ~(uint64_t)supported) != 0) {
            result = -HL_LINUX_EINVAL;
            break;
        }
        shadow = bound_shadow_reserve(0);
        if (shadow < 0) {
            result = -(int64_t)errno;
            break;
        }
        if (shadow >= guest_nofile_cur()) {
            close(shadow);
            result = -HL_LINUX_EMFILE;
            break;
        }
        for (;;) {
            status = hl_linux_fd_reserve_at(g_linux_box, (hl_linux_fd)shadow, &reservation);
            if (status != HL_STATUS_ALREADY_EXISTS) break;
            close(shadow);
            shadow = bound_shadow_reserve(shadow + 1);
            if (shadow < 0 || shadow >= guest_nofile_cur()) break;
        }
        if (status != HL_STATUS_OK || shadow < 0 || shadow >= guest_nofile_cur()) {
            if (shadow >= 0) close(shadow);
            result = -HL_LINUX_EMFILE;
            break;
        }
        result = hl_linux_openat_reserved(g_linux_box, &reservation, (int32_t)source.fd, path, path_size, (uint32_t)a2,
                                          (uint32_t)a3);
        if (result < 0) {
            (void)hl_linux_fd_cancel(g_linux_box, &reservation);
            close(shadow);
        }
        break;
    }
    case 57: /* close */
        flock_broker_detach(&source);
        if (g_host_services != NULL && g_host_services->file != NULL &&
            g_host_services->file->metadata != NULL) {
            hl_host_file_metadata metadata;
            hl_host_result status =
                g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
            if (status.status == HL_STATUS_OK && metadata.type == HL_HOST_FILE_TYPE_REGULAR)
                flock_on_close_identity((int)source.fd, metadata.stable_device, metadata.stable_object);
            if (status.status == HL_STATUS_OK && metadata.type == HL_HOST_FILE_TYPE_REGULAR)
                poslk_on_close_identity(metadata.stable_device, metadata.stable_object);
        }
        result = hl_linux_close(g_linux_box, source.fd);
        (void)close((int)source.fd);
        break;
    case 62: result = hl_linux_lseek(g_linux_box, source.fd, (int64_t)a1, (int32_t)a2); break;
    case 63:
        if (a2 != 0 && !host_range_mapped((uintptr_t)a1, (size_t)a2))
            result = -EFAULT;
        else
            result = hl_linux_read(g_linux_box, source.fd, (void *)(uintptr_t)a1, (size_t)a2);
        break;
    case 64:
        if (a2 != 0 && !host_range_mapped((uintptr_t)a1, (size_t)a2))
            result = -EFAULT;
        else
            result = hl_linux_write(g_linux_box, source.fd, (const void *)(uintptr_t)a1, (size_t)a2);
        break;
    case 67:
        if (a2 != 0 && !host_range_mapped((uintptr_t)a1, (size_t)a2))
            result = -EFAULT;
        else
            result = hl_linux_pread64(g_linux_box, source.fd, (void *)(uintptr_t)a1, (size_t)a2, a3);
        break;
    case 68:
        if (a2 != 0 && !host_range_mapped((uintptr_t)a1, (size_t)a2))
            result = -EFAULT;
        else
            result = hl_linux_pwrite64(g_linux_box, source.fd, (const void *)(uintptr_t)a1, (size_t)a2, a3);
        if (result > 0) bound_mapping_file_written(&source, a3, (uint64_t)result);
        break;
    case 65:
    case 66:
    case 69:
    case 70: {
        static _Thread_local hl_host_iovec vectors[HL_LINUX_IOV_MAX];
        result = bound_vectors_copy(a1, a2, vectors);
        if (result != 0) break;
        if (nr == 65)
            result = hl_linux_readv(g_linux_box, source.fd, vectors, (uint32_t)a2);
        else if (nr == 66)
            result = hl_linux_writev(g_linux_box, source.fd, vectors, (uint32_t)a2);
        else if (nr == 69)
            result = hl_linux_preadv(g_linux_box, source.fd, vectors, (uint32_t)a2, a3);
        else
            result = hl_linux_pwritev(g_linux_box, source.fd, vectors, (uint32_t)a2, a3);
        break;
    }
    case 213: {
        hl_host_file_metadata metadata;
        if ((int64_t)a1 < 0)
            result = -EINVAL;
        else if (g_host_services == NULL || g_host_services->file == NULL ||
                 g_host_services->file->metadata == NULL)
            result = -95; /* Linux EOPNOTSUPP; this route bypasses the native-to-Linux errno mapper. */
        else {
            hl_host_result status =
                g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
            result = status.status != HL_STATUS_OK ? bound_host_error(status.status)
                     : metadata.type != HL_HOST_FILE_TYPE_REGULAR ? -EINVAL
                     : hl_linux_pread64(g_linux_box, source.fd, NULL, 0, a1) < 0 ? -EBADF
                                                                                 : 0;
        }
        break;
    }
    case 286:
    case 287: {
        static _Thread_local hl_host_iovec vectors[HL_LINUX_IOV_MAX];
#ifdef CANON_X86ONLY
        uint64_t vector_offset = a3; /* x86-64 passes the complete 64-bit offset in argument 4. */
#else
        uint64_t vector_offset =
            (uint64_t)(uint32_t)a3 | ((uint64_t)(uint32_t)G_A4(c) << 32); /* AArch64 split offset. */
#endif
        result = bound_vectors_copy(a1, a2, vectors);
        if (result != 0) break;
        /* Flags are semantic requirements, not hints. Do not silently erase RWF_NOWAIT/APPEND/SYNC. */
        if (G_A5(c) != 0) {
            result = -95; /* Linux EOPNOTSUPP; macOS's native value is 102. */
            break;
        }
        if (vector_offset == UINT64_MAX)
            result = nr == 286 ? hl_linux_readv(g_linux_box, source.fd, vectors, (uint32_t)a2)
                               : hl_linux_writev(g_linux_box, source.fd, vectors, (uint32_t)a2);
        else
            result = nr == 286 ? hl_linux_preadv(g_linux_box, source.fd, vectors, (uint32_t)a2, vector_offset)
                               : hl_linux_pwritev(g_linux_box, source.fd, vectors, (uint32_t)a2, vector_offset);
        if (nr == 287 && result > 0)
            bound_mapping_file_written(&source, vector_offset == UINT64_MAX ? source.offset : vector_offset,
                                       (uint64_t)result);
        break;
    }
    case 46: {
        hl_host_file_metadata metadata = {0};
        int have_metadata = 0, prepared = 0;
        if (g_host_services != NULL && g_host_services->file != NULL &&
            g_host_services->file->metadata != NULL) {
            hl_host_result status =
                g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
            have_metadata = status.status == HL_STATUS_OK;
        }
        if (have_metadata && a1 < metadata.size) {
            gbus_prepare();
            prepared = 1;
        }
        result = hl_linux_ftruncate(g_linux_box, source.fd, a1);
        if (result == 0 && have_metadata) {
            /* The local truncate is authoritative. Publish its generation
             * before releasing the BUS transition so the host watcher drops
             * the matching notification instead of replaying the shrink. */
            bound_watch_publish_size(metadata.stable_device, metadata.stable_object, a1);
            pthread_mutex_lock(&g_bound_mapping_gate);
            pthread_mutex_lock(&g_bound_mapping_lock);
            bound_mapping_file_size_changed(&source, &metadata, 1, metadata.size, a1, NULL);
            pthread_mutex_unlock(&g_bound_mapping_lock);
            pthread_mutex_unlock(&g_bound_mapping_gate);
        }
        if (prepared) {
            gbus_prepare_release();
        }
        break;
    }
    case 82: result = hl_linux_fsync(g_linux_box, source.fd); break;
    case 83: result = hl_linux_fdatasync(g_linux_box, source.fd); break;
    case 84:
        if ((G_A3(c) & ~(uint64_t)7u) != 0)
            result = -EINVAL;
        else
            result = hl_linux_sync_range(g_linux_box, source.fd, a1, a2, (uint32_t)G_A3(c));
        break;
    case 267: result = hl_linux_sync_filesystem(g_linux_box, source.fd); break;
    case 80: {
        hl_linux_file_status status;
        result = hl_linux_fstat(g_linux_box, source.fd, &status);
        if (result == 0 && !host_range_mapped((uintptr_t)a1, GUEST_LINUX_STAT_BYTES)) result = -EFAULT;
        if (result == 0) bound_virtualize_owner(&source, &status);
        if (result == 0) fill_linux_bound_stat((uint8_t *)(uintptr_t)a1, &status);
        break;
    }
    case 291: {
        const char *path = (const char *)(uintptr_t)a1;
        uint64_t flags = a2;
        uint64_t mask = a3;
        uint64_t output = G_A4(c);
        if ((flags & ~UINT64_C(0x7900)) != 0 || (flags & UINT64_C(0x6000)) == UINT64_C(0x6000) ||
            (mask & UINT64_C(0x80000000)) != 0) {
            result = -EINVAL;
            break;
        }
        if (path == NULL || !host_range_mapped((uintptr_t)path, 1)) {
            result = -EFAULT;
            break;
        }
        if (path[0] != 0 || (flags & UINT64_C(0x1000)) == 0) return 0;
        if (!host_range_mapped((uintptr_t)output, 256)) {
            result = -EFAULT;
            break;
        }
        hl_linux_file_status status;
        result = hl_linux_fstat(g_linux_box, source.fd, &status);
        if (result == 0) {
            bound_virtualize_owner(&source, &status);
            bound_fill_statx((uint8_t *)(uintptr_t)output, &status);
        }
        break;
    }
    case 44: {
        hl_host_filesystem_metadata metadata;
        hl_host_result status;
        if (!bound_file_abi14() || g_host_services->file->filesystem_metadata == NULL) {
            result = -ENOSYS;
            break;
        }
        status = g_host_services->file->filesystem_metadata(g_host_services->context, source.host_handle, &metadata);
        if (status.status != HL_STATUS_OK) {
            result = bound_host_error(status.status);
            break;
        }
        if (!host_range_mapped((uintptr_t)a1, 120)) {
            result = -EFAULT;
            break;
        }
        bound_fill_statfs((uint8_t *)(uintptr_t)a1, &metadata);
        result = 0;
        break;
    }
    case 47: {
        hl_host_file_metadata before = {0}, after = {0};
        hl_host_result status;
        uint32_t mode = (uint32_t)a1;
        int prepared = 0;
        if (a1 > UINT32_MAX || a2 > INT64_MAX || a3 == 0 || a3 > INT64_MAX) {
            result = -EINVAL;
            break;
        }
        if (a2 > INT64_MAX - a3) {
            result = -EFBIG;
            break;
        }
        if (!bound_file_abi14() || g_host_services->file->allocate_range == NULL) {
            result = -ENOSYS;
            break;
        }
        status = g_host_services->file->metadata(g_host_services->context, source.host_handle, &before);
        if (status.status != HL_STATUS_OK) {
            result = bound_host_error(status.status);
            break;
        }
        if ((mode & HL_HOST_FILE_ALLOC_COLLAPSE_RANGE) != 0) {
            gbus_prepare();
            prepared = 1;
        }
        status = g_host_services->file->allocate_range(g_host_services->context, source.host_handle, mode, a2, a3);
        result = bound_host_error(status.status);
        if (status.status == HL_STATUS_OK &&
            g_host_services->file->metadata(g_host_services->context, source.host_handle, &after).status ==
                HL_STATUS_OK) {
            bound_watch_publish_size(after.stable_device, after.stable_object, after.size);
            pthread_mutex_lock(&g_bound_mapping_gate);
            pthread_mutex_lock(&g_bound_mapping_lock);
            if (before.size != after.size)
                bound_mapping_file_size_changed(&source, &after, 1, before.size, after.size, NULL);
            pthread_mutex_unlock(&g_bound_mapping_lock);
            pthread_mutex_unlock(&g_bound_mapping_gate);
            bound_mapping_file_data_changed(&source, after.stable_device, after.stable_object);
        }
        if (prepared) gbus_prepare_release();
        break;
    }
    case 52: {
        if (g_host_services->file->set_permissions == NULL) {
            result = -ENOSYS;
            break;
        }
        hl_host_result status =
            g_host_services->file->set_permissions(g_host_services->context, source.host_handle, (uint32_t)a1 & 07777u);
        result = bound_host_error(status.status);
        if (result == 0) {
            char path[HL_LINUX_PATH_MAX + 1];
            hl_host_result named = g_host_services->file->path(
                g_host_services->context, source.host_handle, (hl_host_bytes){path, HL_LINUX_PATH_MAX});
            if (named.status == HL_STATUS_OK && named.value <= HL_LINUX_PATH_MAX) {
                path[named.value] = 0;
                fc_evict_path(path);
            }
        }
        break;
    }
    case 55: {
        char path[HL_LINUX_PATH_MAX + 1];
        hl_host_result status = g_host_services->file->path(
            g_host_services->context, source.host_handle, (hl_host_bytes){path, HL_LINUX_PATH_MAX});
        if (status.status != HL_STATUS_OK || status.value > HL_LINUX_PATH_MAX) {
            result = bound_host_error(status.status);
            break;
        }
        path[status.value] = 0;
        chown_xattr_set_path(path, (int)(int32_t)(uint32_t)a1, (int)(int32_t)(uint32_t)a2, 0);
        hl_xattr_cache_invalidate();
        result = 0;
        break;
    }
    case 88: {
        hl_host_file_time times[2];
        const struct timespec *guest = (const struct timespec *)(uintptr_t)a2;
        char relative[HL_LINUX_PATH_MAX + 1];
        size_t relative_size = 0;
        hl_host_handle target = source.host_handle;
        int close_target = 0;
        if (a3 & ~UINT64_C(0x100)) {
            result = -EINVAL;
            break;
        }
        if (a1 != 0) {
            result = bound_path_copy(a1, relative, &relative_size);
            if (result != 0) {
                break;
            }
            /* Absolute paths ignore dirfd and remain on the common namespace route. Relative paths
             * resolve beneath the opaque directory and update the independently opened target. */
            if (relative[0] == '/') return 0;
            if (g_host_services->file->open_relative == NULL) {
                result = -ENOSYS;
                break;
            }
            uint32_t access = HL_HOST_FILE_PATH_ONLY;
            if (a3 & UINT64_C(0x100)) access |= HL_HOST_FILE_NOFOLLOW;
            hl_host_result opened = g_host_services->file->open_relative(
                g_host_services->context, source.host_handle, relative, relative_size, access, 0, 0);
            if (opened.status != HL_STATUS_OK) {
                result = bound_host_error(opened.status);
                break;
            }
            target = opened.value;
            close_target = 1;
        }
        if (g_host_services->file->set_times == NULL) {
            result = -ENOSYS;
            goto bound_set_times_done;
        }
        if (guest != NULL && !host_range_mapped((uintptr_t)guest, sizeof(struct timespec) * 2)) {
            result = -EFAULT;
            goto bound_set_times_done;
        }
        for (int index = 0; index < 2; ++index) {
            int64_t nanoseconds = guest == NULL ? INT64_C(0x3fffffff) : (int64_t)guest[index].tv_nsec;
            times[index].seconds = guest == NULL ? 0 : (int64_t)guest[index].tv_sec;
            times[index].nanoseconds = 0;
            if (nanoseconds == INT64_C(0x3fffffff))
                times[index].mode = HL_HOST_FILE_TIME_NOW;
            else if (nanoseconds == INT64_C(0x3ffffffe))
                times[index].mode = HL_HOST_FILE_TIME_OMIT;
            else if (nanoseconds >= 0 && nanoseconds < INT64_C(1000000000)) {
                times[index].nanoseconds = (uint32_t)nanoseconds;
                times[index].mode = HL_HOST_FILE_TIME_EXPLICIT;
            } else {
                result = -EINVAL;
                goto bound_set_times_done;
            }
        }
        result = bound_host_error(g_host_services->file->set_times(g_host_services->context, target, times).status);
    bound_set_times_done:
        if (close_target) (void)g_host_services->file->close(g_host_services->context, target);
        break;
    }
    case 32: {
        hl_host_file_metadata metadata;
        hl_host_result status =
            g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
        if (status.status != HL_STATUS_OK) {
            result = bound_host_error(status.status);
            break;
        }
        result = hl_flock_identity(&source, metadata.stable_device, metadata.stable_object, (int)a1) < 0
                     ? -(int64_t)(errno == EWOULDBLOCK ? 11 : errno)
                     : 0;
        break;
    }
    case 29: {
        uint32_t request = (uint32_t)a1;
        if (request == 0x5451u || request == 0x5450u) { /* FIOCLEX / FIONCLEX */
            result = hl_linux_fcntl(g_linux_box, source.fd, HL_LINUX_F_SETFD,
                                    request == 0x5451u ? HL_LINUX_FD_CLOEXEC : 0);
        } else if (request == 0x5421u) { /* FIONBIO */
            if (!host_range_mapped((uintptr_t)a2, sizeof(int))) {
                result = -EFAULT;
                break;
            }
            int64_t flags = hl_linux_fcntl(g_linux_box, source.fd, HL_LINUX_F_GETFL, 0);
            if (flags < 0) {
                result = flags;
                break;
            }
            if (*(const int *)(uintptr_t)a2)
                flags |= HL_LINUX_O_NONBLOCK;
            else
                flags &= ~(int64_t)HL_LINUX_O_NONBLOCK;
            result = hl_linux_fcntl(g_linux_box, source.fd, HL_LINUX_F_SETFL, (uint64_t)flags);
        } else if (request == 0x541bu) { /* FIONREAD */
            if (!host_range_mapped((uintptr_t)a2, sizeof(int))) {
                result = -EFAULT;
                break;
            }
            hl_host_file_metadata metadata;
            hl_host_result status =
                g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
            int64_t offset = hl_linux_lseek(g_linux_box, source.fd, 0, SEEK_CUR);
            if (status.status != HL_STATUS_OK)
                result = bound_host_error(status.status);
            else if (metadata.type != HL_HOST_FILE_TYPE_REGULAR || offset < 0)
                result = metadata.type != HL_HOST_FILE_TYPE_REGULAR ? -ENOTTY : offset;
            else {
                uint64_t available = metadata.size > (uint64_t)offset ? metadata.size - (uint64_t)offset : 0;
                *(int *)(uintptr_t)a2 = available > INT_MAX ? INT_MAX : (int)available;
                result = 0;
            }
        } else {
            result = -ENOTTY;
        }
        break;
    }
    case 61: {
        uint64_t byte_capacity = a2 > UINT32_C(1 << 20) ? UINT32_C(1 << 20) : a2;
        if (a2 < 24) {
            result = -EINVAL;
            break;
        }
        if (a1 == 0 || byte_capacity > SIZE_MAX ||
            !host_range_mapped((uintptr_t)a1, (size_t)byte_capacity)) {
            result = -EFAULT;
            break;
        }
        uint32_t capacity = (uint32_t)(byte_capacity / 24);
        hl_host_file_entry *entries = calloc(capacity, sizeof(*entries));
        if (entries == NULL) {
            result = -ENOMEM;
            break;
        }
        hl_host_result read = g_host_services->file->read_directory(
            g_host_services->context, source.host_handle, entries, capacity, (uint32_t)byte_capacity);
        if (read.status != HL_STATUS_OK) {
            result = bound_host_error(read.status);
            free(entries);
            break;
        }
        if (read.value > capacity) {
            result = -EIO;
            free(entries);
            break;
        }
        uint8_t *output = (uint8_t *)(uintptr_t)a1;
        size_t used = 0;
        result = 0;
        for (uint32_t index = 0; index < (uint32_t)read.value; ++index) {
            size_t record_size = (19u + entries[index].name_size + 1u + 7u) & ~(size_t)7u;
            if (entries[index].name_size > 255 || record_size > byte_capacity - used) {
                result = -EIO;
                break;
            }
            uint8_t *record = output + used;
            memset(record, 0, record_size);
            *(uint64_t *)(record + 0) = entries[index].object;
            *(uint64_t *)(record + 8) = entries[index].next_offset;
            *(uint16_t *)(record + 16) = (uint16_t)record_size;
            record[18] = (uint8_t)entries[index].type;
            memcpy(record + 19, entries[index].name, entries[index].name_size);
            used += record_size;
        }
        if (result == 0) result = (int64_t)used;
        free(entries);
        break;
    }
    case 23: result = bound_dup_at_least(source.fd, 0, 0); break;
    case 24: {
        uint32_t flags = (uint32_t)a2;
        int is_dup2 = (flags & 0x40000000u) != 0;
        int target = (int)a1;
        flags &= ~0x40000000u;
        if (source.fd == (hl_linux_fd)target) {
            result = is_dup2 ? (int64_t)source.fd : -EINVAL;
        } else if ((!is_dup2 && (flags & ~HL_LINUX_O_CLOEXEC) != 0) || target < 0 || target >= guest_nofile_cur()) {
            result = target < 0 || target >= guest_nofile_cur() ? -EBADF : -EINVAL;
        } else {
            hl_linux_fd_snapshot target_snapshot;
            int target_bound = bound_snapshot((uint64_t)(uint32_t)target, &target_snapshot);
            int shadow;
            if (target_bound) {
                shadow = target;
            } else {
                engine_fd_vacate(target);
                fd_reset_emul(target);
                shadow = dup2(g_bound_sentinel, target);
                if (shadow < 0) {
                    result = -(int64_t)errno;
                    break;
                }
                (void)fcntl(target, F_SETFD, FD_CLOEXEC);
            }
            result = hl_linux_dup3(g_linux_box, source.fd, (hl_linux_fd)target,
                                   flags & HL_LINUX_O_CLOEXEC ? HL_LINUX_O_CLOEXEC : 0);
            if (result < 0 && !target_bound) close(shadow);
        }
        break;
    }
    case 25:
        if ((int32_t)a1 == HL_LINUX_F_DUPFD || (int32_t)a1 == HL_LINUX_F_DUPFD_CLOEXEC) {
            if (a2 > INT_MAX)
                result = -EINVAL;
            else
                result = bound_dup_at_least(source.fd, (int)a2,
                                            (int32_t)a1 == HL_LINUX_F_DUPFD_CLOEXEC ? HL_LINUX_FD_CLOEXEC : 0);
        } else if (a1 == 5 || a1 == 6 || a1 == 7) {
            uint8_t *lock = (uint8_t *)(uintptr_t)a2;
            hl_host_file_metadata metadata;
            hl_host_result status;
            int64_t current = 0;
            int lock_result = 0;
            if (!host_range_mapped((uintptr_t)lock, 32)) {
                result = -EFAULT;
                break;
            }
            short whence = *(short *)(lock + 2);
            if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
                result = -EINVAL;
                break;
            }
            if (g_host_services == NULL || g_host_services->file == NULL ||
                g_host_services->file->metadata == NULL) {
                result = -ENOSYS;
                break;
            }
            status = g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
            if (status.status != HL_STATUS_OK) {
                result = bound_host_error(status.status);
                break;
            }
            if (metadata.type != HL_HOST_FILE_TYPE_REGULAR) {
                result = -EBADF;
                break;
            }
            if (whence == SEEK_CUR) {
                current = hl_linux_lseek(g_linux_box, source.fd, 0, SEEK_CUR);
                if (current < 0) {
                    result = current;
                    break;
                }
            }
            for (;;) {
                (void)poslk_op_identity(metadata.stable_device, metadata.stable_object, current, metadata.size,
                                        (int)a1, lock, &lock_result);
                if (a1 != 7 || lock_result != -EAGAIN) break;
                uint64_t pending = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST) |
                                   __atomic_load_n(&c->tpending, __ATOMIC_SEQ_CST);
                int interrupted = 0;
                for (int signal_number = 1; signal_number < 64; ++signal_number)
                    if ((pending & (UINT64_C(1) << signal_number)) &&
                        !(c->sigmask & (UINT64_C(1) << (signal_number - 1)))) {
                        interrupted = 1;
                        break;
                    }
                if (interrupted) {
                    lock_result = -EINTR;
                    break;
                }
                struct timespec delay = {0, 1000000};
                nanosleep(&delay, NULL);
            }
            /* poslk_apply is shared with the legacy Darwin syscall path and therefore reports native
             * errno numbers. This typed route bypasses svc_done(), so translate at this boundary. */
            result = lock_result < 0 ? -hl_linux_errno_from_macos(-lock_result) : lock_result;
        } else {
            result = hl_linux_fcntl(g_linux_box, source.fd, (int32_t)a1, a2);
        }
        break;
    case 285: {
        hl_linux_fd_snapshot output;
        off_t *input_offset = (off_t *)(uintptr_t)a1;
        off_t *output_offset = (off_t *)(uintptr_t)a3;
        size_t done = 0;
        char buffer[8192];
        result = 0;
        if (!bound_snapshot(a2, &output)) { result = -ENOSYS; break; }
        if ((input_offset && !host_range_mapped((uintptr_t)input_offset, sizeof(*input_offset))) ||
            (output_offset && !host_range_mapped((uintptr_t)output_offset, sizeof(*output_offset)))) {
            result = -EFAULT;
            break;
        }
        while (done < (size_t)G_A4(c)) {
            size_t chunk = (size_t)G_A4(c) - done;
            if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
            int64_t nr_read = input_offset
                                  ? hl_linux_pread64(g_linux_box, source.fd, buffer, chunk, (uint64_t)*input_offset)
                                  : hl_linux_read(g_linux_box, source.fd, buffer, chunk);
            if (nr_read <= 0) { if (!done) result = nr_read; break; }
            int64_t nr_written = output_offset
                                     ? hl_linux_pwrite64(g_linux_box, output.fd, buffer, (size_t)nr_read,
                                                         (uint64_t)*output_offset)
                                     : hl_linux_write(g_linux_box, output.fd, buffer, (size_t)nr_read);
            if (nr_written < 0) { if (!done) result = nr_written; break; }
            done += (size_t)nr_written;
            if (input_offset) *input_offset += (off_t)nr_written;
            if (output_offset) *output_offset += (off_t)nr_written;
            result = (int64_t)done;
            if (nr_written < nr_read) break;
        }
        break;
    }
    case 20: return 0; /* epoll_create1: a0 is flags, not an fd */
    case 21:           /* epoll_ctl */
    case 22:           /* epoll_pwait */
    case 71:           /* sendfile */
    case 75:           /* vmsplice */
    case 76:           /* splice */
    case 77:           /* tee */
    case 200:          /* bind */
    case 201:          /* listen */
    case 202:          /* accept */
    case 203:          /* connect */
    case 204:          /* getsockname */
    case 205:          /* getpeername */
    case 206:          /* sendto */
    case 207:          /* recvfrom */
    case 208:          /* setsockopt */
    case 209:          /* getsockopt */
    case 210:          /* shutdown */
    case 211:          /* sendmsg */
    case 212:          /* recvmsg */
        /* A bound slot is never a native descriptor. Unsupported fd operations cannot touch its shadow. */
        result = -ENOSYS;
        break;
    default: return 0;
    }
    G_RET(c) = (uint64_t)result;
    return 1;
}
