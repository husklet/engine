// Production launch bridge. Wire parsing is owned by the portable hl config library. This file maps the
// validated launch model onto the current engine initialization state until that state becomes instance-owned.
//
// A host launcher serializes the container into the position-independent hl_launch_config wire buffer
// and spawns the architecture-matching Linux engine as `<engine> --configfile <path>`. This is the engine
// side: open, unlink, read, and validate the buffer, then translate
// every populated field into the process-local option store, rebuilds the guest argv, and enters
// the Linux guest engine.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/hl/config.h"
#include "../../include/hl/host_services.h"
#include "../../include/hl/linux_abi.h"
#include "launch.h"
#include "options.h"

// hl_run_linux_guest() is the internal Linux guest entry defined by each target translation unit.
int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]);

// Read exactly `n` bytes from `fd` into `buf`, looping over short reads. Returns 0 on success, -1 on
// EOF/error -- a truncated buffer is a hard failure (a partial config must never launch a container).
static int cfd_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { return -1; } // premature EOF
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
        config->debug_log_offset,
        config->checkpoint_directory_offset,
        config->restore_directory_offset,
    };
    size_t i;
    for (i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        if (offsets[i] != 0 && hl_launch_config_string(config, pool, offsets[i], NULL, NULL) != HL_STATUS_OK) return 0;
    }
    return 1;
}

// Read an hl launch config from an already-open file, populate launch state, rebuild the guest argv, and
// enter the Linux guest. Private: hl_run_config_file() is the sole launch protocol.
static int hl_read_config_file(int fd) {
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
        fprintf(stderr, "hl-engine: launch config has a short prefix\n");
        return 78;
    }
    memcpy(&magic, prefix + 0, 4);
    memcpy(&pool_size, prefix + 4, 4);
    memcpy(&header_size, prefix + 8, 4);
    memcpy(&abi, prefix + 12, 4);
    if (magic != HL_CONFIG_MAGIC || abi != HL_CONFIG_ABI || header_size < sizeof(hl_launch_config) ||
        header_size > HL_LAUNCH_HEADER_LIMIT || pool_size == 0 || pool_size > HL_LAUNCH_POOL_LIMIT) {
        fprintf(stderr, "hl-engine: launch config has an invalid prefix\n");
        return 78;
    }
    wire_size = (size_t)header_size + (size_t)pool_size;
    if (wire_size < header_size) return 78;
    wire = (uint8_t *)malloc(wire_size);
    if (wire == NULL) return 78;
    memcpy(wire, prefix, sizeof prefix);
    if (cfd_read_full(fd, wire + sizeof prefix, wire_size - sizeof prefix) != 0) {
        fprintf(stderr, "hl-engine: launch config is truncated\n");
        free(wire);
        return 78;
    }
    if (hl_launch_config_validate(wire, wire_size, &cfg, &pool) != HL_STATUS_OK || !launch_strings_valid(&cfg, pool) ||
        hl_launch_config_arguments_validate(&cfg, pool, NULL) != HL_STATUS_OK) {
        fprintf(stderr, "hl-engine: launch config is malformed\n");
        free(wire);
        return 78;
    }

    char num[32];
    const char *s;

    // Scalars populate the same process-local values container initialization reads.
    if (cfg.memory_limit) {
        snprintf(num, sizeof num, "%llu", (unsigned long long)cfg.memory_limit);
        hl_option_set("HL_MEM_MAX", num, 1);
    }
    if (cfg.pid_limit) {
        snprintf(num, sizeof num, "%u", cfg.pid_limit);
        hl_option_set("HL_PIDS_MAX", num, 1);
    }
    if (cfg.cpu_limit) {
        snprintf(num, sizeof num, "%u", cfg.cpu_limit);
        hl_option_set("HL_CPUS", num, 1);
    }
    if (cfg.rootfs_read_only) hl_option_set("HL_ROOTFS_RO", "1", 1);
    if (cfg.network_isolated) hl_option_set("HL_NET_ISOLATE", "1", 1);
    if (cfg.publish_external) hl_option_set("HL_PUBLISH_DAEMON", "1", 1);
    if (cfg.uid >= 0) {
        snprintf(num, sizeof num, "%d", cfg.uid);
        hl_option_set("HL_UID", num, 1);
    }
    if (cfg.gid >= 0) {
        snprintf(num, sizeof num, "%d", cfg.gid);
        hl_option_set("HL_GID", num, 1);
    }

    // Pooled strings are copied by hl_option_set; an empty field leaves the option unset.
    s = launch_string(&cfg, pool, cfg.hostname_offset);
    if (s[0]) hl_option_set("HL_HOSTNAME", s, 1);
    s = launch_string(&cfg, pool, cfg.limits_offset);
    if (s[0]) hl_option_set("HL_ULIMITS", s, 1);
    s = launch_string(&cfg, pool, cfg.publish_offset);
    if (s[0]) hl_option_set("HL_PUBLISH", s, 1);
    s = launch_string(&cfg, pool, cfg.lower_layers_offset);
    if (s[0]) hl_option_set("HL_LOWER", s, 1);
    s = launch_string(&cfg, pool, cfg.network_namespace_offset);
    if (s[0]) hl_option_set("HL_NETNS", s, 1);
    s = launch_string(&cfg, pool, cfg.volumes_offset);
    if (s[0]) hl_option_set("HL_VOLUMES", s, 1);
    s = launch_string(&cfg, pool, cfg.working_directory_offset);
    if (s[0]) hl_option_set("HL_CWD", s, 1);
    s = launch_string(&cfg, pool, cfg.environment_offset);
    if (s[0]) hl_option_set("HL_GUEST_ENV", s, 1);
    s = launch_string(&cfg, pool, cfg.network_bridge_offset);
    if (s[0]) hl_option_set("HL_NETBR", s, 1);
    s = launch_string(&cfg, pool, cfg.ip_offset);
    if (s[0]) hl_option_set("HL_IP", s, 1);
    s = launch_string(&cfg, pool, cfg.filesystem_generation_offset);
    if (s[0]) hl_option_set("HL_FSGEN_FILE", s, 1);
    // Per-workspace VPN egress: netns.c reads HL_EGRESS_SOCKS to funnel the
    // guest's genuine external TCP connects through this SOCKS5 proxy. Carried in the typed wire (not the
    // ambient host env, which the FFI spawn never forwards) — "" leaves it unset so direct egress is unchanged.
    s = launch_string(&cfg, pool, cfg.egress_proxy_offset);
    if (s[0]) hl_option_set("HL_EGRESS_SOCKS", s, 1);
    s = launch_string(&cfg, pool, cfg.checkpoint_directory_offset);
    if (s[0]) hl_option_set("HL_CHECKPOINT_DIR", s, 1);
    s = launch_string(&cfg, pool, cfg.restore_directory_offset);
    if (s[0]) hl_option_set("HL_RESTORE_DIR", s, 1);
#if defined(HL_ENABLE_LOGGING) && HL_ENABLE_LOGGING
    s = launch_string(&cfg, pool, cfg.debug_log_offset);
    if (s[0]) hl_option_set("HL_LOG", s, 1);
#endif

    // Persistent translated-code cache: presence of a directory enables it.
    s = launch_string(&cfg, pool, cfg.translation_cache_offset);
    if (s[0]) {
        hl_option_set("HL_PCACHE", "1", 1);
        hl_option_set("HL_PCACHE_DIR", s, 1);
    }
    /* A typed per-container disable removes cache activation instead of creating a second kill-switch contract. */
    if (cfg.translation_cache_disabled) {
        hl_option_unset("HL_PCACHE");
        hl_option_unset("HL_PCACHE_DIR");
    }
    // untrusted-guest sentry: both gates as the engine reads them.
    if (cfg.sandbox) {
        hl_option_set("HL_UNTRUSTED", "1", 1);
        hl_option_set("HL_SANDBOX", "1", 1);
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
    int rc = hl_run_linux_guest(NULL, NULL, rootfs[0] ? rootfs : NULL, (uint32_t)argument_count, argv2);
    // Single-shot process: the guest usually exits the worker; if it returns, release temporary storage.
    free(argv2);
    free(wire);
    return rc;
}

int hl_run_config_file(const char *path) {
    if (!path || !path[0]) return 78;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "hl-engine: --configfile: open failed: %s\n", path);
        return 78;
    }
    unlink(path);
    int rc = hl_read_config_file(fd);
    close(fd);
    return rc;
}
