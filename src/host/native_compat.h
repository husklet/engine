#ifndef HL_HOST_NATIVE_COMPAT_H
#define HL_HOST_NATIVE_COMPAT_H

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/socket.h>

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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

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

/* This vocabulary is still required by the inherited macOS event implementation.
   On Linux it is deliberately unavailable: an epoll/inotify/timerfd backend must
   replace that implementation before those guest syscalls are advertised. */
static inline int kqueue(void) {
    errno = ENOTSUP;
    return -1;
}

static inline int kevent(int descriptor, const struct kevent *changes, int change_count, struct kevent *events,
                         int event_count, const struct timespec *timeout) {
    (void)descriptor;
    (void)changes;
    (void)change_count;
    (void)events;
    (void)event_count;
    (void)timeout;
    errno = ENOTSUP;
    return -1;
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
