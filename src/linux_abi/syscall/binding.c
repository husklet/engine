/* Bridge opaque host-file bindings into the legacy native-fd guest runtime. */

static int g_bound_sentinel = -1;

static int bound_snapshot(uint64_t value, hl_linux_fd_snapshot *snapshot) {
    if (g_linux_box == NULL || value > UINT32_MAX) return 0;
    return hl_linux_fd_snapshot_get(g_linux_box, (hl_linux_fd)value, snapshot) == HL_STATUS_OK;
}

static int bound_shadow_reserve(int minimum, int descriptor_flags) {
    int fd;
    if (g_bound_sentinel < 0 || minimum < 0) {
        errno = EBADF;
        return -1;
    }
    fd = fcntl(g_bound_sentinel, F_DUPFD_CLOEXEC, minimum);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, descriptor_flags & HL_LINUX_FD_CLOEXEC ? FD_CLOEXEC : 0) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
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
    int stdio_backup[3] = {-1, -1, -1};
    int stdio_flags[3] = {0, 0, 0};
    uint8_t stdio_open[3] = {0, 0, 0};
    uint8_t stdio_installed[3] = {0, 0, 0};
    uint32_t fd;
    int opened;
    if (g_linux_box == NULL) return 0;
    if (g_bound_sentinel >= 0) {
        for (fd = 0; fd < g_linux_box->fd_capacity; ++fd) {
            if (hl_linux_fd_snapshot_get(g_linux_box, fd, &snapshot) == HL_STATUS_OK && !bound_shadow_matches((int)fd))
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
    for (fd = 0; fd < 3; ++fd) {
        if (hl_linux_fd_snapshot_get(g_linux_box, fd, &snapshot) != HL_STATUS_OK) continue;
        engine_fd_vacate((int)fd);
        stdio_flags[fd] = fcntl((int)fd, F_GETFD);
        stdio_backup[fd] = bound_private_dup((int)fd, 64);
        if (stdio_backup[fd] >= 0) {
            stdio_open[fd] = 1;
        } else if (errno != EBADF) {
            goto activation_failed;
        }
    }
    for (fd = 0; fd < g_linux_box->fd_capacity; ++fd) {
        int shadow;
        if (hl_linux_fd_snapshot_get(g_linux_box, fd, &snapshot) != HL_STATUS_OK) continue;
        if (fd < 3) {
            shadow = dup2(g_bound_sentinel, (int)fd);
            if (shadow == (int)fd) {
                stdio_installed[fd] = 1;
                if (fcntl(shadow, F_SETFD, (snapshot.descriptor_flags & HL_LINUX_FD_CLOEXEC) != 0 ? FD_CLOEXEC : 0) !=
                    0)
                    shadow = -1;
            }
        } else {
            shadow = bound_shadow_reserve((int)fd, (int)snapshot.descriptor_flags);
        }
        if (shadow != (int)fd) {
            int error = shadow < 0 ? errno : EBUSY;
            if (shadow >= 0) close(shadow);
            errno = error;
            goto activation_failed;
        }
    }
    for (fd = 0; fd < 3; ++fd)
        if (stdio_backup[fd] >= 0) close(stdio_backup[fd]);
    return 0;

activation_failed: {
    int error = errno;
    uint32_t rollback;
    for (rollback = 3; rollback < fd; ++rollback)
        if (hl_linux_fd_snapshot_get(g_linux_box, rollback, &snapshot) == HL_STATUS_OK) close((int)rollback);
    for (rollback = 0; rollback < 3; ++rollback) {
        if (stdio_installed[rollback]) {
            if (stdio_open[rollback])
                (void)dup2(stdio_backup[rollback], (int)rollback);
            else
                close((int)rollback);
            if (stdio_open[rollback]) (void)fcntl((int)rollback, F_SETFD, stdio_flags[rollback]);
        }
        if (stdio_backup[rollback] >= 0) close(stdio_backup[rollback]);
    }
    close(g_bound_sentinel);
    g_bound_sentinel = -1;
    errno = error;
    return -1;
}
}

static int64_t bound_dup_at_least(hl_linux_fd source, int minimum, uint32_t descriptor_flags) {
    int shadow = bound_shadow_reserve(minimum, (int)descriptor_flags);
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
    if ((nr == 73 && bound_poll_references(a0, a1)) || (nr == 72 && bound_fdsets_reference(a0, a1, a2, a3)) ||
        (nr == 211 && bound_rights_reference(a1))) {
        G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
        return 1;
    }
    if (nr == 222) {
        hl_linux_fd_snapshot mapped;
        if (bound_snapshot(G_A4(c), &mapped)) {
            G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
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
    if (nr == 76 || nr == 285) {
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
            G_RET(c) = (uint64_t)(int64_t)(-ENOSYS);
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
        shadow = bound_shadow_reserve(0, (a2 & HL_LINUX_O_CLOEXEC) != 0 ? HL_LINUX_FD_CLOEXEC : 0);
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
            shadow = bound_shadow_reserve(shadow + 1, (a2 & HL_LINUX_O_CLOEXEC) != 0 ? HL_LINUX_FD_CLOEXEC : 0);
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
                (void)fcntl(target, F_SETFD, flags & HL_LINUX_O_CLOEXEC ? FD_CLOEXEC : 0);
            }
            result = hl_linux_dup3(g_linux_box, source.fd, (hl_linux_fd)target,
                                   flags & HL_LINUX_O_CLOEXEC ? HL_LINUX_O_CLOEXEC : 0);
            if (result < 0 && !target_bound) close(shadow);
            if (result >= 0) (void)fcntl(target, F_SETFD, flags & HL_LINUX_O_CLOEXEC ? FD_CLOEXEC : 0);
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
        } else {
            result = hl_linux_fcntl(g_linux_box, source.fd, (int32_t)a1, a2);
            if (result >= 0 && (int32_t)a1 == HL_LINUX_F_SETFD)
                (void)fcntl((int)source.fd, F_SETFD, (a2 & HL_LINUX_FD_CLOEXEC) != 0 ? FD_CLOEXEC : 0);
        }
        break;
    case 20: return 0; /* epoll_create1: a0 is flags, not an fd */
    case 21:           /* epoll_ctl */
    case 22:           /* epoll_pwait */
    case 29:           /* ioctl */
    case 32:           /* flock */
    case 44:           /* fstatfs */
    case 46:           /* ftruncate */
    case 52:           /* fchmod */
    case 55:           /* fchown */
    case 61:           /* getdents64 */
    case 65:           /* readv */
    case 66:           /* writev */
    case 69:           /* preadv */
    case 70:           /* pwritev */
    case 71:           /* sendfile */
    case 75:           /* vmsplice */
    case 76:           /* splice */
    case 77:           /* tee */
    case 79:           /* newfstatat directory */
    case 82:           /* fsync */
    case 83:           /* fdatasync */
    case 84:           /* sync_file_range */
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
    case 267:          /* syncfs */
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
