#ifndef HL_HOST_NATIVE_COMPAT_H
#define HL_HOST_NATIVE_COMPAT_H

#include "system.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/xattr.h>

#define HL_NATIVE_RENAME_NOREPLACE RENAME_EXCL
#define HL_NATIVE_RENAME_EXCHANGE RENAME_SWAP
#define HL_NATIVE_SEEK_DATA SEEK_DATA
#define HL_NATIVE_SEEK_HOLE SEEK_HOLE

static inline void hl_native_kqueue_close(int descriptor) { (void)descriptor; }
static inline void hl_native_kqueue_duplicate(int source, int destination) {
    (void)source;
    (void)destination;
}

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
static inline int hl_native_setxattr(const char *path, const char *name, const void *value, size_t size, int position,
                                    int options) {
    return setxattr(path, name, value, size, position, options);
}
static inline int hl_native_fsetxattr(int fd, const char *name, const void *value, size_t size, int position,
                                     int options) {
    return fsetxattr(fd, name, value, size, position, options);
}
static inline ssize_t hl_native_getxattr(const char *path, const char *name, void *value, size_t size, int position,
                                        int options) {
    return getxattr(path, name, value, size, position, options);
}
static inline ssize_t hl_native_fgetxattr(int fd, const char *name, void *value, size_t size, int position,
                                         int options) {
    return fgetxattr(fd, name, value, size, position, options);
}
static inline ssize_t hl_native_listxattr(const char *path, char *list, size_t size, int options) {
    return listxattr(path, list, size, options);
}
static inline int hl_native_removexattr(const char *path, const char *name, int options) {
    return removexattr(path, name, options);
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
    uint64_t queue;
    uint64_t token;
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
typedef struct hl_native_kalias {
    int descriptor;
    uint64_t queue;
    struct hl_native_kalias *next;
} hl_native_kalias;
static hl_native_kalias *hl_native_kaliases;
static uint64_t hl_native_knext = 1;
static uint64_t hl_native_ktoken_next = 1;

static inline hl_native_kalias *hl_native_kalias_find(int descriptor) {
    for (hl_native_kalias *alias = hl_native_kaliases; alias != NULL; alias = alias->next)
        if (alias->descriptor == descriptor) return alias;
    return NULL;
}

static inline hl_native_kregistration *hl_native_kfind(uint64_t queue, int target) {
    hl_native_kregistration *entry;
    for (entry = hl_native_kregistrations; entry != NULL; entry = entry->next)
        if (entry->queue == queue && entry->target == target) return entry;
    return NULL;
}

static inline hl_native_kregistration *hl_native_ktoken_find(uint64_t token) {
    for (hl_native_kregistration *entry = hl_native_kregistrations; entry != NULL; entry = entry->next)
        if (entry->token == token) return entry;
    return NULL;
}

static inline void hl_native_kqueue_release_locked(int descriptor) {
    hl_native_kalias **alias_cursor = &hl_native_kaliases;
    while (*alias_cursor != NULL && (*alias_cursor)->descriptor != descriptor) alias_cursor = &(*alias_cursor)->next;
    if (*alias_cursor == NULL) return;
    hl_native_kalias *alias = *alias_cursor;
    uint64_t queue = alias->queue;
    *alias_cursor = alias->next;
    free(alias);
    for (hl_native_kalias *survivor = hl_native_kaliases; survivor != NULL; survivor = survivor->next)
        if (survivor->queue == queue) return;
    hl_native_kregistration **cursor = &hl_native_kregistrations;
    while (*cursor != NULL) {
        hl_native_kregistration *entry = *cursor;
        if (entry->queue != queue) {
            cursor = &entry->next;
            continue;
        }
        *cursor = entry->next;
        if (entry->wake >= 0) {
            hl_host_process_fd_private_remove(entry->wake);
            close(entry->wake);
        }
        free(entry);
    }
}

static inline void hl_native_kqueue_close(int descriptor) {
    pthread_mutex_lock(&hl_native_klock);
    hl_native_kqueue_release_locked(descriptor);
    pthread_mutex_unlock(&hl_native_klock);
}

/* A duplicated descriptor names the same epoll open-file description.  Keep a
   stable identity and retain registrations until the last alias closes. */
static inline void hl_native_kqueue_duplicate(int source, int destination) {
    if (source < 0 || destination < 0 || source == destination) return;
    pthread_mutex_lock(&hl_native_klock);
    hl_native_kalias *original = hl_native_kalias_find(source);
    if (original != NULL) {
        uint64_t queue = original->queue;
        hl_native_kqueue_release_locked(destination);
        hl_native_kalias *alias = calloc(1, sizeof(*alias));
        if (alias != NULL) {
            alias->descriptor = destination;
            alias->queue = queue;
            alias->next = hl_native_kaliases;
            hl_native_kaliases = alias;
        }
    }
    pthread_mutex_unlock(&hl_native_klock);
}

static inline int hl_native_kevent_rehome(int descriptor, int old_target, int new_target) {
    hl_native_kregistration *entry;
    struct epoll_event event = {0};
    pthread_mutex_lock(&hl_native_klock);
    hl_native_kalias *alias = hl_native_kalias_find(descriptor);
    entry = alias == NULL ? NULL : hl_native_kfind(alias->queue, old_target);
    if (entry == NULL) {
        pthread_mutex_unlock(&hl_native_klock);
        errno = ENOENT;
        return -1;
    }
    event.events = (entry->read ? EPOLLIN : 0u) | (entry->write ? EPOLLOUT : 0u);
    if ((entry->flags & EV_CLEAR) != 0) event.events |= EPOLLET;
    if ((entry->flags & EV_ONESHOT) != 0) event.events |= EPOLLONESHOT;
    event.data.u64 = entry->token;
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
    int descriptor = epoll_create1(EPOLL_CLOEXEC);
    if (descriptor < 0) return descriptor;
    hl_native_kalias *alias = calloc(1, sizeof(*alias));
    if (alias == NULL) {
        close(descriptor);
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_lock(&hl_native_klock);
    alias->descriptor = descriptor;
    alias->queue = hl_native_knext++;
    alias->next = hl_native_kaliases;
    hl_native_kaliases = alias;
    pthread_mutex_unlock(&hl_native_klock);
    return descriptor;
}

static inline int kevent(int descriptor, const struct kevent *changes, int change_count, struct kevent *events,
                         int event_count, const struct timespec *timeout) {
    int index;
    if (change_count < 0 || event_count < 0 || (change_count != 0 && changes == NULL) ||
        (event_count != 0 && events == NULL)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&hl_native_klock);
    hl_native_kalias *alias = hl_native_kalias_find(descriptor);
    if (alias == NULL) {
        pthread_mutex_unlock(&hl_native_klock);
        errno = EBADF;
        return -1;
    }
    uint64_t queue = alias->queue;
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
        entry = hl_native_kfind(queue, target);
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
                entry->queue = queue;
                entry->token = hl_native_ktoken_next++;
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
                if (entry->wake < 0) {
                    int wake = change->filter == EVFILT_TIMER
                                   ? timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)
                                   : eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    if (wake >= 0) {
                        int adopted = hl_host_process_fd_private_adopt(wake);
                        if (adopted < 0) {
                            close(wake);
                            errno = -adopted;
                            wake = -1;
                        } else {
                            wake = adopted;
                        }
                    }
                    entry->wake = wake;
                }
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
        // Always request EPOLLRDHUP on the read side so a peer half-close (shutdown SHUT_WR) surfaces as
        // EV_EOF, matching macOS kqueue's native EVFILT_READ EV_EOF-on-half-close. Without it Linux epoll
        // reports a half-close only as a plain EPOLLIN (readable-at-EOF) and never sets EV_EOF, so the engine
        // could not deliver EPOLLRDHUP to a guest that registered it.
        event.events = (entry->read ? (EPOLLIN | EPOLLRDHUP) : 0u) | (entry->write ? EPOLLOUT : 0u);
        if ((entry->flags & EV_CLEAR) != 0) event.events |= EPOLLET;
        if ((entry->flags & EV_ONESHOT) != 0) event.events |= EPOLLONESHOT;
        event.data.u64 = entry->token;
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
            hl_host_process_fd_private_remove(entry->wake);
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
        int delivered = 0;
        for (index = 0; index < count; ++index) {
            pthread_mutex_lock(&hl_native_klock);
            hl_native_kregistration *entry = hl_native_ktoken_find(native_events[index].data.u64);
            if (entry == NULL) {
                pthread_mutex_unlock(&hl_native_klock);
                continue; /* stale readiness raced the last alias close */
            }
            uint32_t ready = native_events[index].events;
            int16_t filter = entry->filter == EVFILT_TIMER
                                 ? EVFILT_TIMER
                                 : (ready & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0 ? EVFILT_READ : EVFILT_WRITE;
            events[delivered] = (struct kevent){(uintptr_t)(entry->target < 0 ? descriptor : entry->target), filter,
                                                0, 0, 0, entry->udata};
            if ((ready & (EPOLLHUP | EPOLLRDHUP)) != 0) events[delivered].flags |= EV_EOF;
            if ((ready & EPOLLERR) != 0) events[delivered].flags |= EV_ERROR;
            if (entry->filter == EVFILT_TIMER) {
                uint64_t expirations = 0;
                if (read(entry->wake, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations))
                    events[delivered].data = (intptr_t)expirations;
            } else if (entry->target < 0) {
                uint64_t value;
                ssize_t consumed = read(entry->wake, &value, sizeof(value));
                if (consumed < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    events[delivered].flags |= EV_ERROR;
                    events[delivered].data = errno;
                }
                events[delivered].filter = EVFILT_USER;
            }
            delivered++;
            pthread_mutex_unlock(&hl_native_klock);
        }
        return delivered;
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
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#ifndef O_SYMLINK
/* macOS O_SYMLINK opens the SYMLINK NODE itself (not its target). Linux has no such flag: an O_PATH
 * open follows a final symlink to its target, and only O_PATH|O_NOFOLLOW yields a handle that names
 * the link node (so readlinkat(fd,"",..) / fstatat(fd,"",AT_EMPTY_PATH) operate on the link). The
 * container-vfs open path uses O_SYMLINK for exactly the O_PATH|O_NOFOLLOW-on-a-symlink case, so the
 * Linux emulation must carry O_NOFOLLOW alongside O_PATH -- otherwise the bound/overlay open follows
 * the link and the empty-path readlink names the target. */
#define O_SYMLINK (O_PATH | O_NOFOLLOW)
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
