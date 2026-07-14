// Authoritative engine option registry.
//
#include "options.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct hl_option_definition {
    const char *name;
    const char *purpose;
    uint8_t ownership;
    uint8_t shape;
    char *value;
    size_t value_size;
} hl_option_definition;

enum hl_option_ownership {
    HL_OPTION_LAUNCH_INPUT = 1,
    HL_OPTION_INTERNAL_STATE = 2,
    HL_OPTION_DEBUG_ONLY = 3,
};

enum hl_option_shape {
    HL_OPTION_TEXT = 1,
    HL_OPTION_PATH = 2,
    HL_OPTION_INTEGER = 3,
    HL_OPTION_FLAG = 4,
    HL_OPTION_RECORDS = 5,
};

#define HL_LAUNCH_OPTION(name, purpose, shape) {name, purpose, HL_OPTION_LAUNCH_INPUT, shape, NULL, 0}
#define HL_INTERNAL_OPTION(name, purpose, shape) {name, purpose, HL_OPTION_INTERNAL_STATE, shape, NULL, 0}
#define HL_DEBUG_OPTION(name, purpose, shape) {name, purpose, HL_OPTION_DEBUG_ONLY, shape, NULL, 0}

enum { HL_OPTION_STORE_LIMIT = 64 * 1024 * 1024 };

static size_t hl_option_store_size;

static hl_option_definition hl_option_definitions[] = {
    /* Public launch-config inputs and their process-local representation. */
    HL_LAUNCH_OPTION("HL_CHECKPOINT_DIR", "aarch64 checkpoint output directory", HL_OPTION_PATH),
    HL_LAUNCH_OPTION("HL_CPUS", "guest-visible CPU quota", HL_OPTION_INTEGER),
    HL_LAUNCH_OPTION("HL_CWD", "initial guest working directory", HL_OPTION_PATH),
    HL_LAUNCH_OPTION("HL_EGRESS_SOCKS", "SOCKS5 endpoint for external TCP egress", HL_OPTION_TEXT),
    HL_LAUNCH_OPTION("HL_FSGEN_FILE", "shared overlay filesystem-generation file", HL_OPTION_PATH),
    HL_LAUNCH_OPTION("HL_GID", "initial guest group identity", HL_OPTION_INTEGER),
    HL_LAUNCH_OPTION("HL_GUEST_ENV", "serialized Linux guest environment", HL_OPTION_RECORDS),
    HL_LAUNCH_OPTION("HL_HOSTNAME", "Linux guest hostname", HL_OPTION_TEXT),
    HL_LAUNCH_OPTION("HL_IP", "guest virtual IPv4 address paired with HL_NETBR", HL_OPTION_TEXT),
    HL_LAUNCH_OPTION("HL_LOWER", "ordered root filesystem lower layers", HL_OPTION_RECORDS),
    HL_LAUNCH_OPTION("HL_MEM_MAX", "guest memory limit", HL_OPTION_INTEGER),
    HL_LAUNCH_OPTION("HL_NETBR", "shared virtual-network bridge identity", HL_OPTION_TEXT),
    HL_LAUNCH_OPTION("HL_NETNS", "guest network and IPC namespace identity", HL_OPTION_TEXT),
    HL_LAUNCH_OPTION("HL_NET_ISOLATE", "disable guest external networking", HL_OPTION_FLAG),
    HL_LAUNCH_OPTION("HL_PCACHE", "enable persistent translated-code caching", HL_OPTION_FLAG),
    HL_LAUNCH_OPTION("HL_PCACHE_DIR", "persistent translated-code cache storage", HL_OPTION_PATH),
    HL_LAUNCH_OPTION("HL_PIDS_MAX", "guest process limit", HL_OPTION_INTEGER),
    HL_LAUNCH_OPTION("HL_PUBLISH", "guest-to-host port publication rules", HL_OPTION_RECORDS),
    HL_LAUNCH_OPTION("HL_PUBLISH_DAEMON", "suppress engine listeners because the host daemon publishes ports",
                     HL_OPTION_FLAG),
    HL_LAUNCH_OPTION("HL_RESTORE_DIR", "aarch64 checkpoint directory selected for restore", HL_OPTION_PATH),
    HL_LAUNCH_OPTION("HL_ROOTFS_RO", "mount the guest root filesystem read-only", HL_OPTION_FLAG),
    HL_LAUNCH_OPTION("HL_SANDBOX", "apply host confinement to the untrusted worker", HL_OPTION_FLAG),
    HL_LAUNCH_OPTION("HL_UID", "initial guest user identity", HL_OPTION_INTEGER),
    HL_LAUNCH_OPTION("HL_ULIMITS", "serialized Linux resource limits", HL_OPTION_RECORDS),
    HL_LAUNCH_OPTION("HL_UNTRUSTED", "route host-authority operations through the sentry", HL_OPTION_FLAG),
    HL_LAUNCH_OPTION("HL_VOLUMES", "guest volume mount specification", HL_OPTION_RECORDS),

    /* State produced by guest exec/launch translation, never an independent public input. */
    HL_INTERNAL_OPTION("HL_GUEST_ENV_ESC", "guest environment uses escaped record encoding", HL_OPTION_FLAG),
    HL_INTERNAL_OPTION("HL_GUEST_ENV_EXACT", "guest exec environment suppresses engine defaults", HL_OPTION_FLAG),

    /* Compile-time-disabled in production; ambient ingestion is guarded by HL_ENABLE_LOGGING. */
    HL_DEBUG_OPTION("HL_LOG", "debug-build logging tag selector", HL_OPTION_TEXT),
};

static hl_option_definition *hl_option_find(const char *name) {
    size_t index;
    if (name == NULL) return NULL;
    for (index = 0; index < sizeof hl_option_definitions / sizeof hl_option_definitions[0]; index++)
        if (strcmp(name, hl_option_definitions[index].name) == 0) return &hl_option_definitions[index];
    return NULL;
}

const char *hl_option_get(const char *name) {
    hl_option_definition *definition = hl_option_find(name);
    if (definition == NULL) return NULL;
    return definition->value;
}

static size_t hl_option_value_size(const char *value) {
    size_t length;
    for (length = 0; length < HL_OPTION_STORE_LIMIT; length++)
        if (value[length] == 0) return length + 1;
    return 0;
}

int hl_option_set(const char *name, const char *value, int overwrite) {
    hl_option_definition *definition = hl_option_find(name);
    size_t value_size;
    char *copy;
    if (definition == NULL || value == NULL) return -1;
    if (!overwrite && definition->value != NULL) return 0;
    value_size = hl_option_value_size(value);
    if (value_size == 0 || hl_option_store_size - definition->value_size > HL_OPTION_STORE_LIMIT - value_size)
        return -1;
    copy = (char *)malloc(value_size);
    if (copy == NULL) return -1;
    memcpy(copy, value, value_size);
    free(definition->value);
    definition->value = copy;
    hl_option_store_size = hl_option_store_size - definition->value_size + value_size;
    definition->value_size = value_size;
    return 0;
}

int hl_option_unset(const char *name) {
    hl_option_definition *definition = hl_option_find(name);
    if (definition == NULL) return -1;
    free(definition->value);
    definition->value = NULL;
    hl_option_store_size -= definition->value_size;
    definition->value_size = 0;
    return 0;
}

void hl_option_reset(void) {
    size_t index;
    for (index = 0; index < sizeof hl_option_definitions / sizeof hl_option_definitions[0]; index++) {
        free(hl_option_definitions[index].value);
        hl_option_definitions[index].value = NULL;
        hl_option_definitions[index].value_size = 0;
    }
    hl_option_store_size = 0;
#if defined(HL_ENABLE_LOGGING) && HL_ENABLE_LOGGING
    /* Debug builds alone may opt into diagnostics from the process environment. */
    {
        const char *selector = getenv("HL_LOG");
        if (selector != NULL) (void)hl_option_set("HL_LOG", selector, 1);
    }
#endif
}
