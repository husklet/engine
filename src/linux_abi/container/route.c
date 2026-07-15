#include "route.h"

// POSIX shm / named-sem backing path. glibc's shm_open/sem_open create files under /dev/shm; the guest's
// synthesized /dev tmpfs has no real host tmpfs behind it, so /dev/shm/<name> is redirected to a REAL host
// file that MAP_SHARED + fork can share coherently. In CONTAINER mode the backing lives inside the overlay
// upper's own /dev/shm dir (<rootfs_canon>/dev/shm/<name>): the segment is then PER-CONTAINER (no
// cross-container /tmp collision -- two `postgres` containers no longer alias the same DSM segment), it is
// VISIBLE to `ls /dev/shm`/stat/df through the normal overlay machinery (the file physically sits in the
// upper), and it is cleared when the container rootfs is torn down -- matching docker's per-container tmpfs.
// In direct (no-rootfs) mode use the engine's private namespace identity. The key is minted uniquely for
// every standalone launch and inherited by its fork/exec descendants; an explicitly supplied HL_NETNS lets
// related launches (docker-exec shape) share it.  A former flat /tmp filename made independent engine
// instances alias the same POSIX shm/named-sem object during concurrent matrices. Embedded slashes in
// <name> are flattened so a segment can never escape the shm dir (glibc forbids them anyway).
// Returns buf, or NULL when `guest` is not a "/dev/shm/<name>" path. g_rootfs_canon is defined in vfs.c,
// which is #included ahead of this file in the unity TU.
static const char *shm_backing_path(const char *guest, char *buf, size_t n) {
    if (!guest || guest[0] != '/' || strncmp(guest, "/dev/shm/", 9)) return NULL;
    const char *name = guest + 9;
    int pfx = g_vfs_namespace.root_canonical[0]
                  ? snprintf(buf, n, "%s/dev/shm/", g_vfs_namespace.root_canonical)
                  : snprintf(buf, n, "/tmp/.hl-shm-%s-", g_namespace_key[0] ? g_namespace_key : "unscoped");
    if (pfx < 0 || pfx >= (int)n - 1) return NULL;
    int m = pfx + snprintf(buf + pfx, n - (size_t)pfx, "%s", name);
    if (m > (int)n - 1) m = (int)n - 1;
    for (int i = pfx; i < m; i++)
        if (buf[i] == '/') buf[i] = '_';
    return buf;
}

// Rewrite ABSOLUTE guest paths into the rootfs; relative paths pass through (resolved
// against the dir-fd by the *at syscall, e.g. ls stat-ing entries relative to a dir).
// nofollow=1 leaves the FINAL component unresolved (lstat/AT_SYMLINK_NOFOLLOW unlink), so a
// symlink is stat'd/removed as the link itself rather than its target.
static const char *atpath(int dirfd, const char *raw, char *buf, size_t n, int nofollow) {
    if (!raw) return raw;
    // POSIX shm + named semaphores: glibc backs both with files under /dev/shm (shm_open -> /dev/shm/<name>,
    // sem_open -> /dev/shm/sem.<name>). Route EVERY op (open/link/unlink/stat/rename) at the SAME host
    // backing the open(2) handler uses (case 56, via shm_hostpath -> shm_backing_path), so glibc's
    // multi-step named-sem create (temp file + link to the final name) and sem_unlink all resolve together.
    {
        const char *shp = shm_backing_path(raw, buf, n);
        if (shp) return shp;
    }
    // absolute -> rootfs-relative + confine (final component followed unless nofollow)
    if (raw[0] == '/') {
        // S2: serve the memoized host path (only when a rootfs is configured -- without one the
        // resolvers below return `raw` untouched and leave `buf` garbage, so there's nothing to cache).
        // Follow-path only: the rc_* cache memoizes followed results, so a nofollow lookup must bypass it.
        if (!nofollow && g_rootfs && rc_lookup(raw, buf, n)) return buf;
        if (g_nlower) {
            overlay_resolve(raw, buf, n, nofollow);
            if (!nofollow && g_rootfs) rc_store(raw, buf);
            return buf;
            // overlay: search upper+lowers
        }
        if (g_rootfs) {
            if (nofollow)
                xlate(raw, buf, n);
            else {
                xresolve_exec(raw, buf, n);
                rc_store(raw, buf);
            }
            return buf;
        }
        return nofollow ? xlate(raw, buf, n) : xresolve_exec(raw, buf, n);
    }
    if (!g_rootfs) return raw;
    // relative via a real dir-fd
    if (dirfd >= 0) {
        // untracked dir-fd (dup/inherited/high): FAIL CLOSED
        if (dirfd >= 1024 || !g_fdpath[dirfd][0]) {
            snprintf(buf, n, "%s/.jail-escape-denied", g_rootfs_canon);
            return buf;
        }
        // turn it into a confined absolute path
        const char *gdir = g_fdpath[dirfd];
        if (strncmp(gdir, g_rootfs_canon, g_rootfs_canon_len) == 0)
            // upper -> guest dir
            gdir += g_rootfs_canon_len;
        else
            for (int i = 0; i < g_nlower; i++)
                if (strncmp(gdir, g_lower[i].canon, g_lower[i].clen) == 0) {
                    gdir += g_lower[i].clen;
                    break;
                    // a lower -> guest dir
                }
        char combined[8400];
        snprintf(combined, sizeof combined, "/%s/%s", gdir, raw);
        if (g_nlower) {
            overlay_resolve(combined, buf, n, nofollow);
            return buf;
        }
        // openat then ignores dirfd (path absolute)
        return nofollow ? xlate(combined, buf, n) : xresolve(combined, buf, n);
    }
    {
        char j[8400];
        // AT_FDCWD-relative -> join the guest cwd, then confine
        snprintf(j, sizeof j, "%s/%s", g_cwd, raw);
        if (g_nlower) {
            overlay_resolve(j, buf, n, nofollow);
            return buf;
        }
        return nofollow ? xlate(j, buf, n) : xresolve_exec(j, buf, n);
    }
}
