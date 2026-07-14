/* Bridge opaque host-file bindings into the legacy native-fd guest runtime. */

#include "../object.h"
#include "../epoll.h"

static int g_bound_sentinel = -1;

typedef struct bound_mapping_object {
    hl_host_handle handle;
    uint64_t address;
    uint64_t size;
    size_t references;
} bound_mapping_object;

typedef struct bound_mapping {
    uint64_t address;
    uint64_t size;
    uint64_t object_offset;
    bound_mapping_object *object;
    struct bound_mapping *next;
} bound_mapping;

static bound_mapping **bound_mapping_head(void) {
    size_t required = offsetof(hl_linux_abi, vma_state) + sizeof(g_linux_box->vma_state);
    if (g_linux_box == NULL || g_linux_box->abi != HL_LINUX_ABI_VERSION || g_linux_box->size < required) return NULL;
    return (bound_mapping **)&g_linux_box->vma_state;
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
                *tail = (bound_mapping){end, mapped_end - end, entry->object_offset + end - base, entry->object,
                                        entry->next};
                entry->object->references++;
                entry->next = tail;
                entry->size = address - base;
                previous = tail;
            }
        } else if (address <= base) {
            uint64_t cut = end - base;
            entry->address += cut;
            entry->object_offset += cut;
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
    while (*head != NULL) bound_mapping_drop(*head, NULL);
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
    int bus_prepared = 0;
    if (head == NULL || g_host_services == NULL || g_host_services->memory == NULL ||
        g_host_services->memory->map_file == NULL)
        return -ENOSYS;
    if (linux_flags & 0x10u) flags |= HL_HOST_MEMORY_FIXED;
    if (linux_flags & 0x100000u) flags = (flags & ~HL_HOST_MEMORY_FIXED) | HL_HOST_MEMORY_FIXED_NOREPLACE;
    object = calloc(1, sizeof(*object));
    entry = calloc(1, sizeof(*entry));
    if (object == NULL || entry == NULL) {
        free(object);
        free(entry);
        return -ENOMEM;
    }
    if (g_host_services->file != NULL && g_host_services->file->metadata != NULL) {
        hl_host_file_metadata metadata;
        hl_host_result status =
            g_host_services->file->metadata(g_host_services->context, file->host_handle, &metadata);
        if (status.status == HL_STATUS_OK) {
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
        return result;
    }
    if (linux_flags & (0x10u | 0x100000u)) bound_mapping_retire(mapped.address, mapped.mapped_size);
    *object = (bound_mapping_object){mapped.handle, mapped.address, mapped.mapped_size, 1};
    *entry = (bound_mapping){mapped.address, mapped.mapped_size, mapped.reserved, object, *head};
    *head = entry;
    if (mapped.address == 0 || mapped.mapped_size < size || mapped.address > UINT64_MAX - size) {
        if (bus_prepared) gbus_prepare_release();
        bound_mapping_drop(entry, NULL);
        return -EIO;
    }
    gmap_add(mapped.address, mapped.mapped_size);
    gmap_set_glen(mapped.address, size);
    gbus_clear(mapped.address, mapped.address + size);
    if (bus_prepared && gbus_add(mapped.address + bus_accessible, mapped.address + size) != 0) {
        gbus_prepare_release();
        bound_mapping_drop(entry, NULL);
        gmap_split_unmap(mapped.address, mapped.address + mapped.mapped_size);
        return -ENOMEM;
    }
    if (bus_prepared) gbus_prepare_release();
    return (int64_t)mapped.address;
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

static int64_t bound_open_handle(hl_host_handle directory, const char *path, size_t path_size, uint32_t flags,
                                 uint32_t mode) {
    hl_linux_fd_reservation reservation;
    hl_status status;
    int shadow = bound_shadow_reserve(0);
    int64_t result;
    if (shadow < 0 || shadow >= guest_nofile_cur()) {
        if (shadow >= 0) close(shadow);
        return -EMFILE;
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
        return -EMFILE;
    }
    result = hl_linux_openat_handle_reserved(g_linux_box, &reservation, directory, path, path_size, flags, mode);
    if (result < 0) {
        (void)hl_linux_fd_cancel(g_linux_box, &reservation);
        close(shadow);
    }
    return result;
}

/* Resolution may temporarily occupy low native descriptors. Once its opaque
 * handles are closed, republish the new typed OFD at the true lowest logical
 * guest slot and retire the temporary shadow. */
static int64_t bound_relocate_lowest(int64_t opened) {
    int shadow;
    int64_t duplicated;
    if (opened < 0) return opened;
    shadow = bound_shadow_reserve(0);
    if (shadow < 0) return opened;
    duplicated = hl_linux_dup3(g_linux_box, (hl_linux_fd)opened, (hl_linux_fd)shadow, 0);
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

static int bound_route(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    hl_linux_fd_snapshot source;
    int64_t result;
    int source_bound = bound_snapshot(a0, &source);
    if (nr == 73 && bound_poll_references(a0, a1)) {
        G_RET(c) = (uint64_t)bound_ppoll(c, a0, a1, a2, a3);
        return 1;
    }
    if (nr == 72 && bound_fdsets_reference(a0, a1, a2, a3)) {
        G_RET(c) = (uint64_t)bound_pselect(c, a0, a1, a2, a3);
        return 1;
    }
    if (nr == 211 && bound_rights_reference(a1)) {
        G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
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
        bound_mapping *mapping = bound_mapping_find(a0, a1);
        if (mapping != NULL) {
            uint64_t offset = a0 - mapping->address;
            hl_host_result operation;
            /* Guest mprotect is modeled by the 4 KiB Linux VMA/SMC registries in svc_mem. Routing a
             * typed file mapping to host protect applies macOS's 16 KiB granularity and can protect
             * adjacent ELF segments, breaking ld.so RELRO. Keep the typed mapping ledger, but let the
             * common guest-logical path validate the range and update permissions. */
            if (nr == 226) return 0;
            if (nr == 227 && (((a2 & ~(uint64_t)7u) != 0) || (a2 & 5u) == 0 || (a2 & 5u) == 5u)) {
                G_RET(c) = (uint64_t)(int64_t)(-EINVAL);
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
            return 1;
        }
    }
    if (nr == 71 || nr == 77) {
        hl_linux_fd_snapshot second;
        if (bound_snapshot(a1, &second)) {
            G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
            return 1;
        }
    }
    if (nr == 76) {
        hl_linux_fd_snapshot second;
        if (bound_snapshot(a2, &second)) {
            G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
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
        if (g_host_services != NULL && g_host_services->file != NULL &&
            g_host_services->file->metadata != NULL) {
            hl_host_file_metadata metadata;
            hl_host_result status =
                g_host_services->file->metadata(g_host_services->context, source.host_handle, &metadata);
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
    case 46: result = hl_linux_ftruncate(g_linux_box, source.fd, a1); break;
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
        if (result == 0) fill_linux_bound_stat((uint8_t *)(uintptr_t)a1, &status);
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
            result = lock_result;
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
    case 29:           /* ioctl */
    case 32:           /* flock */
    case 44:           /* fstatfs */
    case 52:           /* fchmod */
    case 55:           /* fchown */
    case 61:           /* getdents64 */
    case 71:           /* sendfile */
    case 75:           /* vmsplice */
    case 76:           /* splice */
    case 77:           /* tee */
    case 79:           /* newfstatat directory */
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
    case 213:          /* readahead */
    case 286:          /* preadv2 */
    case 287:          /* pwritev2 */
        /* A bound slot is never a native descriptor. Unsupported fd operations cannot touch its shadow. */
        result = -ENOSYS;
        break;
    default: return 0;
    }
    G_RET(c) = (uint64_t)result;
    return 1;
}
