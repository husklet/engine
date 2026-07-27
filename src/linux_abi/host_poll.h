#ifndef HL_LINUX_ABI_HOST_POLL_H
#define HL_LINUX_ABI_HOST_POLL_H

/*
 * <poll.h> (and the select vocabulary that travels with it) for this layer.
 * Same construction and the same REAL/SHAPE/REFUSAL labelling as host_mman.h.
 *
 * Windows is the one host where the readiness question is genuinely different
 * rather than merely unbound, so the refusal below is worth more than one line
 * of justification:
 *
 *   - WSAPoll exists and takes a pollfd-shaped array, but it accepts SOCKETs
 *     only.  Every other waitable thing on Windows -- a file, a pipe, a console,
 *     a timer, an event -- is a HANDLE, and no single call waits on a mixed set.
 *     poll(2) on this layer's descriptors is a mixed set by construction: the
 *     Linux ABI hands the same fd namespace to files, pipes, eventfds, timerfds,
 *     signalfds and sockets alike.
 *
 *   - The seam's answer to readiness is the event group, and it is a REGISTERED
 *     set (control/wait) rather than a per-call array.  That is a good fit for
 *     epoll, which is how src/linux_abi/epoll.c already consumes it, and a poor
 *     one for poll(2), whose whole shape is "here is a fresh array, tell me
 *     about it once".  Synthesizing poll on top of it would mean registering and
 *     tearing down a pollset per call.
 *
 *   - Both of the above are moot until the descriptor-to-HANDLE table exists,
 *     because a pollfd's fd is a guest number that names no host object on
 *     Windows today.
 *
 * So: the constants and the types are SHAPE, and the calls are REFUSAL.  What a
 * refusal must NOT be here is a return of 0 ("timed out, nothing ready") or a
 * return of nfds with POLLIN set ("everything ready"): the first turns every
 * guest event loop into a spin at the timeout period and the second into a spin
 * at full speed, and both look like a hang rather than an unimplemented call.
 */

#if !defined(_WIN32)

#include <poll.h>

#else /* Windows */

#include <errno.h>
#include <stddef.h>

/* SHAPE.  Linux values -- this layer translates guest poll bits to host poll
 * bits by identity in several places, exactly as it does for mmap protections. */
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
#define POLLRDHUP 0x2000

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

/* SHAPE.  select(2)'s descriptor set.  Sized to this layer's descriptor bound
 * rather than to a host FD_SETSIZE, because the numbers in it are guest
 * descriptors; a host-sized 64-entry winsock fd_set would silently truncate. */
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef struct {
    unsigned long fds_bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) / (8 * sizeof(unsigned long))];
} fd_set;

#define HL_LINUX_FD_WORD(d) ((unsigned)(d) / (8 * sizeof(unsigned long)))
#define HL_LINUX_FD_BIT(d) (1UL << ((unsigned)(d) % (8 * sizeof(unsigned long))))

#define FD_ZERO(set) memset((set), 0, sizeof(fd_set))
#define FD_SET(d, set) ((set)->fds_bits[HL_LINUX_FD_WORD(d)] |= HL_LINUX_FD_BIT(d))
#define FD_CLR(d, set) ((set)->fds_bits[HL_LINUX_FD_WORD(d)] &= ~HL_LINUX_FD_BIT(d))
#define FD_ISSET(d, set) (((set)->fds_bits[HL_LINUX_FD_WORD(d)] & HL_LINUX_FD_BIT(d)) != 0)

struct timespec;
struct timeval;

/* REFUSAL -- see the header note.  ENOSYS, never a timeout and never a
 * readiness. */
static inline int poll(struct pollfd *entries, nfds_t count, int timeout_ms) {
    (void)entries;
    (void)count;
    (void)timeout_ms;
    errno = ENOSYS;
    return -1;
}

static inline int ppoll(struct pollfd *entries, nfds_t count, const struct timespec *timeout, const void *mask) {
    (void)entries;
    (void)count;
    (void)timeout;
    (void)mask;
    errno = ENOSYS;
    return -1;
}

static inline int select(int bound, fd_set *readable, fd_set *writable, fd_set *failing, struct timeval *timeout) {
    (void)bound;
    (void)readable;
    (void)writable;
    (void)failing;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

static inline int pselect(int bound, fd_set *readable, fd_set *writable, fd_set *failing,
                          const struct timespec *timeout, const void *mask) {
    (void)bound;
    (void)readable;
    (void)writable;
    (void)failing;
    (void)timeout;
    (void)mask;
    errno = ENOSYS;
    return -1;
}

#endif /* _WIN32 */

#endif
