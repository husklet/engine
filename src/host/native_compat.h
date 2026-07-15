#ifndef HL_HOST_NATIVE_COMPAT_H
#define HL_HOST_NATIVE_COMPAT_H

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/socket.h>

#define HL_NATIVE_RENAME_NOREPLACE RENAME_EXCL
#define HL_NATIVE_RENAME_EXCHANGE RENAME_SWAP
#define HL_NATIVE_SEEK_DATA SEEK_DATA
#define HL_NATIVE_SEEK_HOLE SEEK_HOLE

static inline int hl_native_fd_path(int descriptor, char *path, size_t capacity) {
    (void)capacity;
    return fcntl(descriptor, F_GETPATH, path);
}
static inline int hl_native_open_watch(const char *path) { return open(path, O_EVTONLY); }
static inline int hl_native_set_no_sigpipe(int descriptor) {
    int enabled = 1;
    return setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
}
static inline int hl_native_birthtime(const struct stat *status, struct timespec *time) {
    *time = status->st_birthtimespec;
    return 0;
}
#elif defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

/* renameat2 consumes the Linux guest flag values directly.  Darwin's
   renameatx_np uses different values, so callers must use these host-native
   constants instead of carrying the Darwin spelling into common code. */
#define HL_NATIVE_RENAME_NOREPLACE 1u
#define HL_NATIVE_RENAME_EXCHANGE 2u
#define HL_NATIVE_SEEK_DATA SEEK_DATA
#define HL_NATIVE_SEEK_HOLE SEEK_HOLE

/* Temporary shape-compatible event seam for the production unity runtime.
   Linux-native epoll/inotify/timerfd implementations will replace these calls;
   keeping the Darwin vocabulary here prevents it leaking into target roots. */
struct kevent {
    uintptr_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    intptr_t data;
    void *udata;
};

#define EVFILT_READ (-1)
#define EVFILT_WRITE (-2)
#define EVFILT_VNODE (-4)
#define EVFILT_TIMER (-7)
#define EVFILT_USER (-10)
#define EV_ADD UINT16_C(0x0001)
#define EV_DELETE UINT16_C(0x0002)
#define EV_ENABLE UINT16_C(0x0004)
#define EV_ONESHOT UINT16_C(0x0010)
#define EV_CLEAR UINT16_C(0x0020)
#define EV_EOF UINT16_C(0x8000)
#define EV_ERROR UINT16_C(0x4000)
#define NOTE_DELETE UINT32_C(0x0001)
#define NOTE_WRITE UINT32_C(0x0002)
#define NOTE_EXTEND UINT32_C(0x0004)
#define NOTE_ATTRIB UINT32_C(0x0008)
#define NOTE_LINK UINT32_C(0x0010)
#define NOTE_RENAME UINT32_C(0x0020)
#define NOTE_REVOKE UINT32_C(0x0040)
#define NOTE_TRIGGER UINT32_C(0x01000000)
#define NOTE_NSECONDS UINT32_C(0x00000004)

#define EV_SET(event, identifier, event_filter, event_flags, event_fflags, event_data, event_udata)                  \
    (*(event) = (struct kevent){(uintptr_t)(identifier), (int16_t)(event_filter), (uint16_t)(event_flags),          \
                                (uint32_t)(event_fflags), (intptr_t)(event_data), (void *)(event_udata)})

typedef struct hl_native_kregistration {
    dev_t device;
    ino_t inode;
    int target;
    int wake;
    int16_t filter;
    uint32_t read;
    uint32_t write;
    uint16_t flags;
    void *udata;
    struct hl_native_kregistration *next;
} hl_native_kregistration;

static pthread_mutex_t hl_native_klock = PTHREAD_MUTEX_INITIALIZER;
static hl_native_kregistration *hl_native_kregistrations;

static inline int hl_native_kidentity(int descriptor, dev_t *device, ino_t *inode) {
    struct stat status;
    if (fstat(descriptor, &status) != 0) return -1;
    *device = status.st_dev;
    *inode = status.st_ino;
    return 0;
}

static inline hl_native_kregistration *hl_native_kfind(dev_t device, ino_t inode, int target) {
    hl_native_kregistration *entry;
    for (entry = hl_native_kregistrations; entry != NULL; entry = entry->next)
        if (entry->device == device && entry->inode == inode && entry->target == target) return entry;
    return NULL;
}

static inline int hl_native_kevent_rehome(int descriptor, int old_target, int new_target) {
    dev_t device;
    ino_t inode;
    hl_native_kregistration *entry;
    struct epoll_event event = {0};
    if (hl_native_kidentity(descriptor, &device, &inode) != 0) return -1;
    pthread_mutex_lock(&hl_native_klock);
    entry = hl_native_kfind(device, inode, old_target);
    if (entry == NULL) {
        pthread_mutex_unlock(&hl_native_klock);
        errno = ENOENT;
        return -1;
    }
    event.events = (entry->read ? EPOLLIN : 0u) | (entry->write ? EPOLLOUT : 0u);
    if ((entry->flags & EV_CLEAR) != 0) event.events |= EPOLLET;
    if ((entry->flags & EV_ONESHOT) != 0) event.events |= EPOLLONESHOT;
    event.data.ptr = entry;
    if (epoll_ctl(descriptor, EPOLL_CTL_DEL, old_target, NULL) != 0 ||
        epoll_ctl(descriptor, EPOLL_CTL_ADD, new_target, &event) != 0) {
        pthread_mutex_unlock(&hl_native_klock);
        return -1;
    }
    entry->target = new_target;
    pthread_mutex_unlock(&hl_native_klock);
    return 0;
}

static inline int kqueue(void) {
    return epoll_create1(EPOLL_CLOEXEC);
}

static inline int kevent(int descriptor, const struct kevent *changes, int change_count, struct kevent *events,
                         int event_count, const struct timespec *timeout) {
    dev_t device;
    ino_t inode;
    int index;
    if (hl_native_kidentity(descriptor, &device, &inode) != 0) return -1;
    if (change_count < 0 || event_count < 0 || (change_count != 0 && changes == NULL) ||
        (event_count != 0 && events == NULL)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&hl_native_klock);
    for (index = 0; index < change_count; ++index) {
        const struct kevent *change = &changes[index];
        hl_native_kregistration *entry;
        struct epoll_event event = {0};
        int operation;
        int target;
        if (change->filter == EVFILT_VNODE) {
            pthread_mutex_unlock(&hl_native_klock);
            errno = ENOTSUP;
            return -1;
        }
        if (change->filter != EVFILT_READ && change->filter != EVFILT_WRITE && change->filter != EVFILT_USER &&
            change->filter != EVFILT_TIMER) {
            pthread_mutex_unlock(&hl_native_klock);
            errno = ENOTSUP;
            return -1;
        }
        target = change->filter == EVFILT_USER ? -1 : change->filter == EVFILT_TIMER ? -2 : (int)change->ident;
        entry = hl_native_kfind(device, inode, target);
        if (change->filter == EVFILT_USER && (change->fflags & NOTE_TRIGGER) != 0) {
            uint64_t one = 1;
            if (entry == NULL || write(entry->wake, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
                pthread_mutex_unlock(&hl_native_klock);
                errno = entry == NULL ? ENOENT : errno;
                return -1;
            }
            continue;
        }
        if ((change->flags & EV_DELETE) != 0) {
            if (entry == NULL) {
                pthread_mutex_unlock(&hl_native_klock);
                errno = ENOENT;
                return -1;
            }
            if (change->filter == EVFILT_READ) entry->read = 0;
            else if (change->filter == EVFILT_WRITE) entry->write = 0;
            else entry->read = 0;
        } else if ((change->flags & EV_ADD) != 0) {
            if (entry == NULL) {
                entry = calloc(1, sizeof(*entry));
                if (entry == NULL) {
                    pthread_mutex_unlock(&hl_native_klock);
                    errno = ENOMEM;
                    return -1;
                }
                entry->device = device;
                entry->inode = inode;
                entry->target = target;
                entry->wake = -1;
                entry->filter = change->filter;
                entry->next = hl_native_kregistrations;
                hl_native_kregistrations = entry;
            }
            entry->flags = change->flags;
            entry->udata = change->udata;
            if (change->filter == EVFILT_READ) entry->read = 1;
            else if (change->filter == EVFILT_WRITE) entry->write = 1;
            else {
                if (entry->wake < 0)
                    entry->wake = change->filter == EVFILT_TIMER
                                      ? timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)
                                      : eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                if (entry->wake < 0) {
                    pthread_mutex_unlock(&hl_native_klock);
                    return -1;
                }
                entry->read = 1;
                if (change->filter == EVFILT_TIMER) {
                    struct itimerspec setting = {0};
                    int64_t nanoseconds = change->data;
                    if ((change->fflags & NOTE_NSECONDS) == 0 || nanoseconds < 0) {
                        pthread_mutex_unlock(&hl_native_klock);
                        errno = ENOTSUP;
                        return -1;
                    }
                    if (nanoseconds == 0) nanoseconds = 1;
                    setting.it_value.tv_sec = (time_t)(nanoseconds / INT64_C(1000000000));
                    setting.it_value.tv_nsec = (long)(nanoseconds % INT64_C(1000000000));
                    if ((change->flags & EV_ONESHOT) == 0) setting.it_interval = setting.it_value;
                    if (timerfd_settime(entry->wake, 0, &setting, NULL) != 0) {
                        pthread_mutex_unlock(&hl_native_klock);
                        return -1;
                    }
                }
            }
        } else {
            continue;
        }
        event.events = (entry->read ? EPOLLIN : 0u) | (entry->write ? EPOLLOUT : 0u);
        if ((entry->flags & EV_CLEAR) != 0) event.events |= EPOLLET;
        if ((entry->flags & EV_ONESHOT) != 0) event.events |= EPOLLONESHOT;
        event.data.ptr = entry;
        operation = event.events == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
        if (entry->wake >= 0) target = entry->wake;
        int control = epoll_ctl(descriptor, operation, target, operation == EPOLL_CTL_DEL ? NULL : &event);
        if (control != 0 && operation == EPOLL_CTL_MOD && errno == ENOENT) {
            operation = EPOLL_CTL_ADD;
            control = epoll_ctl(descriptor, operation, target, &event);
        }
        if (control != 0 && operation == EPOLL_CTL_ADD && errno == EEXIST) control = 0;
        if (control != 0 && !(operation == EPOLL_CTL_DEL && errno == ENOENT)) {
            pthread_mutex_unlock(&hl_native_klock);
            return -1;
        }
        if (operation == EPOLL_CTL_DEL && entry->filter == EVFILT_TIMER && entry->wake >= 0) {
            close(entry->wake);
            entry->wake = -1;
        }
    }
    pthread_mutex_unlock(&hl_native_klock);
    if (event_count == 0) return 0;
    {
        struct epoll_event native_events[256];
        int timeout_ms = -1;
        int count;
        if (event_count > 256) event_count = 256;
        if (timeout != NULL) {
            int64_t milliseconds = (int64_t)timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
            timeout_ms = milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
        }
        count = epoll_wait(descriptor, native_events, event_count, timeout_ms);
        if (count < 0) return -1;
        for (index = 0; index < count; ++index) {
            hl_native_kregistration *entry = native_events[index].data.ptr;
            uint32_t ready = native_events[index].events;
            int16_t filter = entry->filter == EVFILT_TIMER
                                 ? EVFILT_TIMER
                                 : (ready & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0 ? EVFILT_READ : EVFILT_WRITE;
            events[index] = (struct kevent){(uintptr_t)(entry->target < 0 ? descriptor : entry->target), filter, 0, 0,
                                            0, entry->udata};
            if ((ready & (EPOLLHUP | EPOLLRDHUP)) != 0) events[index].flags |= EV_EOF;
            if ((ready & EPOLLERR) != 0) events[index].flags |= EV_ERROR;
            if (entry->filter == EVFILT_TIMER) {
                uint64_t expirations = 0;
                if (read(entry->wake, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations))
                    events[index].data = (intptr_t)expirations;
            } else if (entry->target < 0) {
                uint64_t value;
                ssize_t consumed = read(entry->wake, &value, sizeof(value));
                if (consumed < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    events[index].flags |= EV_ERROR;
                    events[index].data = errno;
                }
                events[index].filter = EVFILT_USER;
            }
        }
        return count;
    }
}
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim

static inline int hl_native_fd_path(int descriptor, char *path, size_t capacity) {
    char link[64];
    ssize_t count;
    int length;
    if (path == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(link, sizeof(link), "/proc/self/fd/%d", descriptor);
    if (length < 0 || (size_t)length >= sizeof(link)) {
        errno = EOVERFLOW;
        return -1;
    }
    count = readlink(link, path, capacity - 1);
    if (count < 0) return -1;
    if ((size_t)count >= capacity - 1) {
        path[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    path[count] = '\0';
    return 0;
}

static inline int hl_native_open_watch(const char *path) {
    (void)path;
    errno = ENOTSUP;
    return -1;
}

static inline int hl_native_set_no_sigpipe(int descriptor) {
    (void)descriptor;
    return 0; /* Linux callers suppress SIGPIPE per send with MSG_NOSIGNAL. */
}

static inline int hl_native_birthtime(const struct stat *status, struct timespec *time) {
    (void)status;
    time->tv_sec = 0;
    time->tv_nsec = 0;
    errno = ENOTSUP;
    return -1;
}

static inline int hl_native_setxattr(const char *path, const char *name, const void *value, size_t size, int position,
                                    int options) {
    (void)position;
    return options != 0 ? lsetxattr(path, name, value, size, 0) : setxattr(path, name, value, size, 0);
}
static inline int hl_native_fsetxattr(int fd, const char *name, const void *value, size_t size, int position,
                                     int options) {
    (void)position;
    (void)options;
    return fsetxattr(fd, name, value, size, 0);
}
static inline ssize_t hl_native_getxattr(const char *path, const char *name, void *value, size_t size, int position,
                                        int options) {
    (void)position;
    return options != 0 ? lgetxattr(path, name, value, size) : getxattr(path, name, value, size);
}
static inline ssize_t hl_native_fgetxattr(int fd, const char *name, void *value, size_t size, int position,
                                         int options) {
    (void)position;
    (void)options;
    return fgetxattr(fd, name, value, size);
}
static inline ssize_t hl_native_listxattr(const char *path, char *list, size_t size, int options) {
    return options != 0 ? llistxattr(path, list, size) : listxattr(path, list, size);
}
static inline int hl_native_removexattr(const char *path, const char *name, int options) {
    return options != 0 ? lremovexattr(path, name) : removexattr(path, name);
}
#define XATTR_NOFOLLOW 1
#define setxattr hl_native_setxattr
#define fsetxattr hl_native_fsetxattr
#define getxattr hl_native_getxattr
#define fgetxattr hl_native_fgetxattr
#define listxattr hl_native_listxattr
#define removexattr hl_native_removexattr
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#ifndef O_SYMLINK
#define O_SYMLINK O_PATH
#endif

static inline int renameatx_np(int old_directory, const char *old_path, int new_directory, const char *new_path,
                               unsigned flags) {
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, old_directory, old_path, new_directory, new_path, flags);
#else
    if (flags == 0) return renameat(old_directory, old_path, new_directory, new_path);
    errno = ENOSYS;
    return -1;
#endif
}

#ifndef SIGEMT
#define SIGEMT (SIGRTMIN + 6)
#endif
#ifndef SIGINFO
#define SIGINFO (SIGRTMIN + 7)
#endif
#endif

#endif
