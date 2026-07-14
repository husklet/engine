// translator/guest/x86_64/stat.c -- the x86-64 Linux `struct stat` byte layout (per-arch; differs from
// aarch64). Provided to the shared os/linux/ (vfs synth + the stat syscalls), which stay layout-agnostic.

// container uid/gid virtualization (cuid/cgid in os/linux/container/state.c, included later in the TU):
// report files owned by the engine's REAL host uid as the container's uid/gid so guest ownership checks
// pass (postgres initdb "data directory has wrong ownership" under a non-root HL_UID).
static int cuid(void);
static int cgid(void);
// a guest chown is persisted as a host xattr on the overlay-upper file; prefer it over the
// cuid/cgid default (defined in os/linux/container/state.c, later in this unity TU). hostpath/fd
// identify the just-stat'd backing file (NULL/-1 when synthetic or unavailable -> default applies).
static int chown_xattr_get(const char *hostpath, int fd, uint64_t dev, uint64_t ino, int *uid, int *gid);
// shared owner virtualization (cuid/cgid default + guest-chown xattr override via the
// cache), defined in os/linux/container/state.c later in the unity TU. statx uses it too, so every
// stat-family syscall reports identical ownership for the same file.
static void stat_virt_ids(const struct stat *s, const char *hostpath, int fd, uint32_t *uid, uint32_t *gid);

#include "../../../linux_abi/encode.h"

static void fill_linux_stat(uint8_t *d, const struct stat *s, const char *hostpath, int fd) {
    uint32_t uid, gid;
    stat_virt_ids(s, hostpath, fd, &uid, &gid);
    hl_linux_stat_record record = {s->st_dev,
                                   s->st_ino,
                                   s->st_nlink,
                                   s->st_rdev,
                                   (uint64_t)s->st_size,
                                   (uint64_t)s->st_blocks,
                                   s->st_atimespec.tv_sec,
                                   (uint64_t)s->st_atimespec.tv_nsec,
                                   s->st_mtimespec.tv_sec,
                                   (uint64_t)s->st_mtimespec.tv_nsec,
                                   s->st_ctimespec.tv_sec,
                                   (uint64_t)s->st_ctimespec.tv_nsec,
                                   s->st_mode,
                                   uid,
                                   gid};
    (void)hl_linux_stat_encode_x86_64(&record, d, 144);
}

static void fill_linux_bound_stat(uint8_t *destination, const hl_linux_file_status *status) {
    hl_linux_stat_record record = {0};
    record.device = status->device;
    record.object = status->object;
    record.links = 1;
    record.size = status->size;
    record.blocks_512 = status->blocks_512;
    record.modified_seconds = (int64_t)(status->modified_ns / UINT64_C(1000000000));
    record.modified_nanoseconds = status->modified_ns % UINT64_C(1000000000);
    record.changed_seconds = record.modified_seconds;
    record.changed_nanoseconds = record.modified_nanoseconds;
    record.mode = status->mode;
    record.user = (uint32_t)cuid();
    record.group = (uint32_t)cgid();
    (void)hl_linux_stat_encode_x86_64(&record, destination, 144);
}
