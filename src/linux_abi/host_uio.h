#ifndef HL_LINUX_ABI_HOST_UIO_H
#define HL_LINUX_ABI_HOST_UIO_H

/*
 * <sys/uio.h> for this layer.  Same construction and the same REAL/SHAPE/REFUSAL
 * labelling as host_mman.h; see that file's header for why this vocabulary lives
 * in src/linux_abi rather than in src/host/native_compat.h.
 *
 * The split here is unusually clean, and worth stating because it is the reason
 * this block is small rather than large.  `struct iovec` appears in this layer in
 * two roles that happen to share a spelling:
 *
 *   - As the GUEST's iovec.  syscall/guest_copy.c's guest_iov_import and every
 *     iovec path in sentry.c decode a Linux `struct iovec[]` out of guest memory.
 *     The layout that matters there is the guest's -- {void *, size_t}, 16 bytes
 *     on both supported guest ISAs -- and the host type is used only because on
 *     Linux and macOS it happens to be identical.  Defining that layout on a host
 *     that does not supply one is not an approximation of anything; it is the
 *     guest ABI, written down.  SHAPE.
 *
 *   - As an argument to a host readv/writev on an int fd.  That is the other
 *     population entirely, and it is blocked for the same reason the socket block
 *     is: on Windows a guest fd number names no host object, because the engine
 *     has no descriptor-to-HANDLE table.  The seam DOES carry readv/writev/
 *     readv_at/writev_at in the file group, keyed on an hl_host_handle, so these
 *     become REAL the moment that table exists -- there is no missing seam
 *     operation here, only a missing binding.  REFUSAL until then.
 *
 * A loop of single-segment reads was considered for the second population and
 * rejected: POSIX writev is atomic with respect to the file offset on a regular
 * file and atomic up to PIPE_BUF on a pipe, a loop is neither, and a guest that
 * relies on either would see interleaving rather than an error.
 */

#if !defined(_WIN32)

#include <sys/uio.h>

#else /* Windows */

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

/* SHAPE.  The guest ABI's iovec, which is also POSIX's. */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#define UIO_MAXIOV IOV_MAX

/* SHAPE: preadv2/pwritev2 flag words, Linux values. */
#define RWF_HIPRI 0x00000001
#define RWF_DSYNC 0x00000002
#define RWF_SYNC 0x00000004
#define RWF_NOWAIT 0x00000008
#define RWF_APPEND 0x00000010

/* REFUSAL -- no descriptor-to-HANDLE table.  ENOSYS and not EBADF: EBADF would
 * tell the guest its descriptor is wrong, which it is not. */
static inline ssize_t readv(int fd, const struct iovec *vectors, int count) {
    (void)fd;
    (void)vectors;
    (void)count;
    errno = ENOSYS;
    return -1;
}

static inline ssize_t writev(int fd, const struct iovec *vectors, int count) {
    (void)fd;
    (void)vectors;
    (void)count;
    errno = ENOSYS;
    return -1;
}

static inline ssize_t preadv(int fd, const struct iovec *vectors, int count, off_t offset) {
    (void)fd;
    (void)vectors;
    (void)count;
    (void)offset;
    errno = ENOSYS;
    return -1;
}

static inline ssize_t pwritev(int fd, const struct iovec *vectors, int count, off_t offset) {
    (void)fd;
    (void)vectors;
    (void)count;
    (void)offset;
    errno = ENOSYS;
    return -1;
}

static inline ssize_t preadv2(int fd, const struct iovec *vectors, int count, off_t offset, int flags) {
    (void)flags;
    return preadv(fd, vectors, count, offset);
}

static inline ssize_t pwritev2(int fd, const struct iovec *vectors, int count, off_t offset, int flags) {
    (void)flags;
    return pwritev(fd, vectors, count, offset);
}

#endif /* _WIN32 */

#endif
