// Authoritative engine option registry.
//
typedef struct hl_option_definition {
    const char *name;
    const char *purpose;
    char *value;
    size_t value_size;
} hl_option_definition;

#define HL_RUNTIME_OPTION(name, purpose) {name, purpose, NULL, 0}

enum { HL_OPTION_STORE_LIMIT = 64 * 1024 * 1024 };

static size_t hl_option_store_size;

static hl_option_definition hl_option_definitions[] = {
    /* Typed launch state and operational choices consumed by production code. */
    HL_RUNTIME_OPTION("HL_CHECKPOINT_DIR", "directory receiving an explicitly requested checkpoint"),
    HL_RUNTIME_OPTION("HL_CPUS", "guest-visible CPU quota"),
    HL_RUNTIME_OPTION("HL_CWD", "initial guest working directory"),
    HL_RUNTIME_OPTION("HL_EGRESS_SOCKS", "configured external network egress proxy"),
    HL_RUNTIME_OPTION("HL_FSGEN_FILE", "shared filesystem-generation state"),
    HL_RUNTIME_OPTION("HL_GID", "initial guest group identity"),
    HL_RUNTIME_OPTION("HL_CHROME_WINDOW_SIZE", "initial Chrome window dimensions"),
    HL_RUNTIME_OPTION("HL_GPU_BRIDGE_NAME", "compositor bridge endpoint"),
    HL_RUNTIME_OPTION("HL_GPU_IOSURFACE", "enable the configured macOS GPU presentation path"),
    HL_RUNTIME_OPTION("HL_GPU_POOL", "shared GPU buffer-pool descriptor"),
    HL_RUNTIME_OPTION("HL_GPU_POOL_N", "shared GPU buffer-pool capacity"),
    HL_RUNTIME_OPTION("HL_GUEST_ENV", "serialized Linux guest environment"),
    HL_RUNTIME_OPTION("HL_GUEST_ENV_ESC", "guest environment uses escaped record encoding"),
    HL_RUNTIME_OPTION("HL_GUEST_ENV_EXACT", "guest environment suppresses engine defaults"),
    HL_RUNTIME_OPTION("HL_HOSTNAME", "Linux guest hostname"),
    HL_RUNTIME_OPTION("HL_IP", "guest virtual IPv4 address"),
    HL_RUNTIME_OPTION("HL_LOG", "debug-build logging tag selector"),
    HL_RUNTIME_OPTION("HL_LOWER", "ordered root filesystem lower layers"),
    HL_RUNTIME_OPTION("HL_MEM_MAX", "guest memory limit"),
    HL_RUNTIME_OPTION("HL_NETBR", "virtual network bridge identity"),
    HL_RUNTIME_OPTION("HL_NETNS", "guest network and IPC namespace identity"),
    HL_RUNTIME_OPTION("HL_NET_ISOLATE", "disable guest external networking"),
    HL_RUNTIME_OPTION("HL_PCACHE", "enable persistent translated-code cache"),
    HL_RUNTIME_OPTION("HL_PCACHE_DIR", "persistent translated-code cache directory"),
    HL_RUNTIME_OPTION("HL_PIDS_MAX", "guest process limit"),
    HL_RUNTIME_OPTION("HL_PUBLISH", "guest-to-host port publication rules"),
    HL_RUNTIME_OPTION("HL_PUBLISH_DAEMON", "port publication is owned by the host daemon"),
    HL_RUNTIME_OPTION("HL_RESTORE_DIR", "checkpoint directory selected for restore"),
    HL_RUNTIME_OPTION("HL_ROOTFS_RO", "mount the guest root filesystem read-only"),
    HL_RUNTIME_OPTION("HL_SANDBOX", "confine the untrusted guest worker"),
    HL_RUNTIME_OPTION("HL_UID", "initial guest user identity"),
    HL_RUNTIME_OPTION("HL_ULIMITS", "serialized Linux resource limits"),
    HL_RUNTIME_OPTION("HL_UNTRUSTED", "route host-authority operations through the sentry"),
    HL_RUNTIME_OPTION("HL_VOLUMES", "guest volume mount specification"),
};

static hl_option_definition *hl_option_find(const char *name) {
    size_t index;
    if (name == NULL) return NULL;
    for (index = 0; index < sizeof hl_option_definitions / sizeof hl_option_definitions[0]; index++)
        if (strcmp(name, hl_option_definitions[index].name) == 0) return &hl_option_definitions[index];
    return NULL;
}

static const char *hl_option_get(const char *name) {
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

static int hl_option_set(const char *name, const char *value, int overwrite) {
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

static int hl_option_unset(const char *name) {
    hl_option_definition *definition = hl_option_find(name);
    if (definition == NULL) return -1;
    free(definition->value);
    definition->value = NULL;
    hl_option_store_size -= definition->value_size;
    definition->value_size = 0;
    return 0;
}

static void hl_option_reset(void) {
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
