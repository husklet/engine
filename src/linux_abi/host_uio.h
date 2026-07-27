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
 *     population entirely, and it splits by SEGMENT COUNT rather than by host.
 *
 * The split is where an earlier revision of this file was wrong, and the
 * correction is worth recording because it was found by running a guest rather
 * than by reading code.  That revision refused both calls outright on the
 * grounds that "a guest fd number names no host object on Windows".  That is
 * true of a descriptor the engine would have to open through the file seam, and
 * false of the ones a guest actually starts with: the standard streams and
 * everything the CRT opens ARE ordinary CRT descriptors here, and write(2) on
 * them works.  The measured consequence of the blanket refusal was a guest whose
 * write(1, "hi\n", 3) returned -ENOSYS -- because this layer routes every plain
 * write through guest_fd_write, which uses writev.
 *
 * So:
 *
 *   - count <= 1 is REAL.  A one-segment vector is a plain read/write by
 *     definition; there is no gather and therefore nothing to be atomic about.
 *     This is not an approximation of writev, it IS writev for that input.
 *
 *   - count > 1 stays a REFUSAL, and the original reasoning is unchanged and
 *     still correct: POSIX writev is atomic with respect to the file offset on a
 *     regular file and atomic up to PIPE_BUF on a pipe.  A loop of writes is
 *     neither, so a guest relying on either would see interleaving -- a wrong
 *     answer -- instead of an error.  The seam already carries readv/writev/
 *     readv_at/writev_at in the file group keyed on an hl_host_handle, so the
 *     gather becomes REAL the moment the descriptor-to-HANDLE binding exists;
 *     there is no missing seam operation here, only a missing binding.
 */

#if !defined(_WIN32)

#include <sys/uio.h>

#else /* Windows */

#include <errno.h>
#include <io.h>
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

/* A zero-length transfer still has to reach the descriptor: write(fd, ., 0) is
 * how a caller probes that fd is open for writing, and returning 0 without
 * asking would answer that probe wrongly for a closed or read-only fd. */
#define HL_LINUX_UIO_BASE(v, n) ((n) > 0 ? (v)[0].iov_base : NULL)
#define HL_LINUX_UIO_LEN(v, n) ((n) > 0 ? (v)[0].iov_len : (size_t)0)

/* REAL for one segment; REFUSAL above it -- see the header note.  ENOSYS and not
 * EBADF for the refusal: EBADF would tell the guest its descriptor is wrong,
 * which it is not.  EINVAL is reserved for a genuinely malformed count. */
static inline ssize_t readv(int fd, const struct iovec *vectors, int count) {
    if (count < 0 || (count > 0 && vectors == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (count > 1) {
        errno = ENOSYS;
        return -1;
    }
    return (ssize_t)_read(fd, HL_LINUX_UIO_BASE(vectors, count), (unsigned int)HL_LINUX_UIO_LEN(vectors, count));
}

static inline ssize_t writev(int fd, const struct iovec *vectors, int count) {
    if (count < 0 || (count > 0 && vectors == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (count > 1) {
        errno = ENOSYS;
        return -1;
    }
    return (ssize_t)_write(fd, HL_LINUX_UIO_BASE(vectors, count), (unsigned int)HL_LINUX_UIO_LEN(vectors, count));
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
