// translator/guest/aarch64/stat.c -- the aarch64 Linux `struct stat` layout (per-arch: x86_64 differs).
// Provided by the frontend so os/linux/ (vfs synth + the stat syscalls) stays layout-agnostic.

// container uid/gid virtualization (cuid/cgid defined in os/linux/container/state.c, included later in
// the unity TU): files the container creates live in the writable upper owned by the engine's REAL host
// uid -- report them as the container's uid/gid so guest ownership checks pass (e.g. postgres initdb
// "data directory has wrong ownership" when running as a non-root HL_UID).
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
    hl_linux_stat_record record = {
        s->st_dev,       s->st_ino,               s->st_nlink,       s->st_rdev,
        (uint64_t)s->st_size, (uint64_t)s->st_blocks, s->st_atimespec.tv_sec, (uint64_t)s->st_atimespec.tv_nsec,
        s->st_mtimespec.tv_sec, (uint64_t)s->st_mtimespec.tv_nsec, s->st_ctimespec.tv_sec,
        (uint64_t)s->st_ctimespec.tv_nsec, s->st_mode, uid, gid};
    (void)hl_linux_stat_encode_aarch64(&record, d, 128);
}

static void fill_linux_bound_stat(uint8_t *destination, const hl_linux_file_status *status) {
    (void)hl_linux_stat_aarch64(status, destination, HL_LINUX_STAT_AARCH64_SIZE);
}
