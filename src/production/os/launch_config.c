// Production launch bridge. Wire parsing is owned by the portable hl config library; this adapter only
// maps a validated launch model onto the legacy environment-backed container initialization path.
//
// A host launcher serializes the container into the position-independent hl_launch_config wire buffer
// and spawns the architecture-matching Linux engine as `<engine> --configfd <fd>`,
// streaming that buffer over `fd`. This is the ENGINE side: read + validate the buffer, then translate
// every populated field back into the exact `DD_*`/`DDJIT_*` environment variable the existing env-driven
// setup (container_init() in targets/*.c, the guest-env reader in os/linux/elf.c, the pcache/sentry
// readers) already consumes, rebuild the guest argv, and hand off to dd_run() -- the identical call the
// normal env/flag launch makes. Reusing the env path means ZERO duplication of container setup logic.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../include/hl/config.h"

// dd_run() is the remaining legacy internal entry defined by each Linux guest target TU.
int dd_run(const char *rootfs, int argc, char *const argv[]);

// Read exactly `n` bytes from `fd` into `buf`, looping over short reads. Returns 0 on success, -1 on
// EOF/error -- a truncated buffer is a hard failure (a partial config must never launch a container).
static int cfd_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (getenv("DD_CONFIGFD_DEBUG"))
                fprintf(stderr, "[DDCONFIGFD] fd=%d read error got=%zu want=%zu errno=%d\n", fd, got, n, errno);
            return -1;
        }
        if (r == 0) {
            if (getenv("DD_CONFIGFD_DEBUG")) {
                int fl = fcntl(fd, F_GETFD, 0);
                fprintf(stderr, "[DDCONFIGFD] fd=%d eof got=%zu want=%zu fdflags=%d errno=%d\n", fd, got, n, fl, errno);
            }
            return -1;
        } // premature EOF
        got += (size_t)r;
    }
    return 0;
}

static const char *launch_string(const hl_launch_config *config, const char *pool, uint32_t offset) {
    const char *value = "";
    if (offset != 0 && hl_launch_config_string(config, pool, offset, &value, NULL) != HL_STATUS_OK) return NULL;
    return value;
}

static int launch_strings_valid(const hl_launch_config *config, const char *pool) {
    const uint32_t offsets[] = {
        config->rootfs_offset,
        config->lower_layers_offset,
        config->hostname_offset,
        config->network_namespace_offset,
        config->publish_offset,
        config->volumes_offset,
        config->limits_offset,
        config->working_directory_offset,
        config->environment_offset,
        config->translation_cache_offset,
        config->network_bridge_offset,
        config->ip_offset,
        config->filesystem_generation_offset,
        config->egress_proxy_offset,
    };
    size_t i;
    for (i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        if (offsets[i] != 0 && hl_launch_config_string(config, pool, offsets[i], NULL, NULL) != HL_STATUS_OK) return 0;
    }
    return 1;
}

// Read an hl launch config (+ trailing string pool) from `fd`, re-hydrate the engine's legacy environment,
// rebuild the guest argv, and dispatch to dd_run(). The spawn shim normally enters through
// hl_run_config_file() below; --configfd remains supported for direct/debug launches. Returns dd_run()'s
// exit code, or a nonzero code on any read/validation failure. Single-shot per process.
int hl_run_config_fd(int fd) {
    enum { HL_LAUNCH_HEADER_LIMIT = 4096, HL_LAUNCH_POOL_LIMIT = 64 * 1024 * 1024 };

    hl_launch_config cfg;
    uint8_t prefix[16];
    uint8_t *wire = NULL;
    const char *pool = NULL;
    uint32_t magic;
    uint32_t pool_size;
    uint32_t header_size;
    uint32_t abi;
    size_t wire_size;

    if (cfd_read_full(fd, prefix, sizeof prefix) != 0) {
        fprintf(stderr, "hl-engine: --configfd: short config prefix\n");
        return 78;
    }
    memcpy(&magic, prefix + 0, 4);
    memcpy(&pool_size, prefix + 4, 4);
    memcpy(&header_size, prefix + 8, 4);
    memcpy(&abi, prefix + 12, 4);
    if ((magic != HL_CONFIG_MAGIC && magic != HL_CONFIG_LEGACY_MAGIC) || abi != HL_CONFIG_ABI ||
        header_size < sizeof(hl_launch_config) || header_size > HL_LAUNCH_HEADER_LIMIT || pool_size == 0 ||
        pool_size > HL_LAUNCH_POOL_LIMIT) {
        fprintf(stderr, "hl-engine: --configfd: invalid config prefix\n");
        return 78;
    }
    wire_size = (size_t)header_size + (size_t)pool_size;
    if (wire_size < header_size) return 78;
    wire = (uint8_t *)malloc(wire_size);
    if (wire == NULL) return 78;
    memcpy(wire, prefix, sizeof prefix);
    if (cfd_read_full(fd, wire + sizeof prefix, wire_size - sizeof prefix) != 0) {
        fprintf(stderr, "hl-engine: --configfd: truncated config\n");
        free(wire);
        return 78;
    }
    if (hl_launch_config_validate(wire, wire_size, &cfg, &pool) != HL_STATUS_OK || !launch_strings_valid(&cfg, pool) ||
        hl_launch_config_arguments_validate(&cfg, pool, NULL) != HL_STATUS_OK) {
        fprintf(stderr, "hl-engine: --configfd: malformed config\n");
        free(wire);
        return 78;
    }

    char num[32];
    const char *s;

    // scalars -> the same env vars container_init()/container_read_resource_env() read.
    if (cfg.memory_limit) {
        snprintf(num, sizeof num, "%llu", (unsigned long long)cfg.memory_limit);
        setenv("DD_MEM_MAX", num, 1);
    }
    if (cfg.pid_limit) {
        snprintf(num, sizeof num, "%u", cfg.pid_limit);
        setenv("DD_PIDS_MAX", num, 1);
    }
    if (cfg.cpu_limit) {
        snprintf(num, sizeof num, "%u", cfg.cpu_limit);
        setenv("DD_CPUS", num, 1);
    }
    if (cfg.rootfs_read_only) setenv("DD_ROOTFS_RO", "1", 1);
    if (cfg.network_isolated) setenv("DD_NET_ISOLATE", "1", 1);
    if (cfg.publish_external) setenv("DD_PUBLISH_DAEMON", "1", 1);
    // GPU rung 2/3 (--gui): opt-in the host-IOSurface path. The engine getenv()s this (vfs.c
    // gpu_iosurface_on()); carrying it in the typed config — not the ambient host env — is what makes it
    // reach the engine reliably (the FFI/bridge does not forward the launcher's ambient environment).
    if (cfg.gpu_enabled) setenv("DD_GPU_IOSURFACE", "1", 1);
    if (cfg.uid >= 0) {
        snprintf(num, sizeof num, "%d", cfg.uid);
        setenv("DD_UID", num, 1);
    }
    if (cfg.gid >= 0) {
        snprintf(num, sizeof num, "%d", cfg.gid);
        setenv("DD_GID", num, 1);
    }

    // pooled strings -> the same env vars (decode via offsets; "" means unset -> leave the env untouched).
    s = launch_string(&cfg, pool, cfg.hostname_offset);
    if (s[0]) setenv("DD_HOSTNAME", s, 1);
    s = launch_string(&cfg, pool, cfg.limits_offset);
    if (s[0]) setenv("DD_ULIMITS", s, 1);
    s = launch_string(&cfg, pool, cfg.publish_offset);
    if (s[0]) setenv("DD_PUBLISH", s, 1);
    s = launch_string(&cfg, pool, cfg.lower_layers_offset);
    if (s[0]) setenv("DD_LOWER", s, 1);
    s = launch_string(&cfg, pool, cfg.network_namespace_offset);
    if (s[0]) setenv("DD_NETNS", s, 1);
    s = launch_string(&cfg, pool, cfg.volumes_offset);
    if (s[0]) setenv("DDVOL", s, 1);
    s = launch_string(&cfg, pool, cfg.working_directory_offset);
    if (s[0]) setenv("DD_CWD", s, 1);
    s = launch_string(&cfg, pool, cfg.environment_offset);
    if (s[0]) setenv("DD_GUEST_ENV", s, 1);
    s = launch_string(&cfg, pool, cfg.network_bridge_offset);
    if (s[0]) setenv("DD_NETBR", s, 1);
    s = launch_string(&cfg, pool, cfg.ip_offset);
    if (s[0]) setenv("DD_IP", s, 1);
    s = launch_string(&cfg, pool, cfg.filesystem_generation_offset);
    if (s[0]) setenv("DD_FSGEN_FILE", s, 1);
    // Per-workspace VPN egress: the engine's netns.c egress_socks() getenv()s DD_EGRESS_SOCKS to funnel the
    // guest's genuine external TCP connects through this SOCKS5 proxy. Carried in the typed wire (not the
    // ambient host env, which the FFI spawn never forwards) — "" leaves it unset so direct egress is unchanged.
    s = launch_string(&cfg, pool, cfg.egress_proxy_offset);
    if (s[0]) setenv("DD_EGRESS_SOCKS", s, 1);

    // persistent translated-code cache: presence of a dir enables it (DDJIT_PCACHE gate + dir).
    s = launch_string(&cfg, pool, cfg.translation_cache_offset);
    if (s[0]) {
        setenv("DDJIT_PCACHE", "1", 1);
        setenv("DDJIT_PCACHE_DIR", s, 1);
    }
    // per-container persistent-cache kill switch: carried through typed launch so a single container can opt
    // out even when the runtime enables pcache defaults globally (DDJIT_NOPCACHE wins over DDJIT_PCACHE).
    if (cfg.translation_cache_disabled) setenv("DDJIT_NOPCACHE", "1", 1);
    // untrusted-guest sentry: both gates as the engine reads them.
    if (cfg.sandbox) {
        setenv("DDJIT_UNTRUSTED", "1", 1);
        setenv("DDJIT_SANDBOX", "1", 1);
    }

    // guest argv: NUL-separated, double-NUL terminated, at argv_off. Count, then point argv2[] into the pool.
    size_t argument_count = 0;
    (void)hl_launch_config_arguments_validate(&cfg, pool, &argument_count);
    char **argv2 = (char **)calloc(argument_count + 1, sizeof(char *));
    if (!argv2) {
        free(wire);
        return 78;
    }
    for (size_t i = 0; i < argument_count; i++) {
        const char *argument = NULL;
        if (hl_launch_config_argument(&cfg, pool, i, &argument, NULL) != HL_STATUS_OK) {
            free(argv2);
            free(wire);
            return 78;
        }
        argv2[i] = (char *)argument;
    }

    // rootfs: "" (bare launch) maps to NULL, matching the flag path's `rootfs = NULL` default.
    const char *rootfs = launch_string(&cfg, pool, cfg.rootfs_offset);
    int rc = dd_run(rootfs[0] ? rootfs : NULL, (int)argument_count, argv2);
    // Single-shot process: dd_run typically _exit()s the worker and never returns; if it does, release.
    free(argv2);
    free(wire);
    return rc;
}

int hl_run_config_file(const char *path) {
    if (!path || !path[0]) return 78;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "dd: --configfile: open failed: %s\n", path);
        return 78;
    }
    unlink(path);
    int rc = hl_run_config_fd(fd);
    close(fd);
    return rc;
}
