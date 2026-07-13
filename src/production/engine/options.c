// Authoritative production option registry.
//
typedef struct hl_option_definition {
    const char *name;
} hl_option_definition;

#define HL_RUNTIME_OPTION(name) {name}

static const hl_option_definition hl_option_definitions[] = {
    // Typed container/launch state and persistent operational choices.
    HL_RUNTIME_OPTION("HL_CHECKPOINT_DIR"),
    HL_RUNTIME_OPTION("HL_CPUS"),
    HL_RUNTIME_OPTION("HL_CWD"),
    HL_RUNTIME_OPTION("HL_EGRESS_SOCKS"),
    HL_RUNTIME_OPTION("HL_FSGEN_FILE"),
    HL_RUNTIME_OPTION("HL_GID"),
    HL_RUNTIME_OPTION("HL_CHROME_WINDOW_SIZE"),
    HL_RUNTIME_OPTION("HL_GPU_BRIDGE_NAME"),
    HL_RUNTIME_OPTION("HL_GPU_IOSURFACE"),
    HL_RUNTIME_OPTION("HL_GPU_POOL"),
    HL_RUNTIME_OPTION("HL_GPU_POOL_N"),
    HL_RUNTIME_OPTION("HL_GUEST_ENV"),
    HL_RUNTIME_OPTION("HL_GUEST_ENV_ESC"),
    HL_RUNTIME_OPTION("HL_GUEST_ENV_EXACT"),
    HL_RUNTIME_OPTION("HL_HOSTNAME"),
    HL_RUNTIME_OPTION("HL_IP"),
    HL_RUNTIME_OPTION("HL_LOG"),
    HL_RUNTIME_OPTION("HL_LOWER"),
    HL_RUNTIME_OPTION("HL_MEM_MAX"),
    HL_RUNTIME_OPTION("HL_NETBR"),
    HL_RUNTIME_OPTION("HL_NETNS"),
    HL_RUNTIME_OPTION("HL_NET_ISOLATE"),
    HL_RUNTIME_OPTION("HL_NOPCACHE"),
    HL_RUNTIME_OPTION("HL_PCACHE"),
    HL_RUNTIME_OPTION("HL_PCACHE_DIR"),
    HL_RUNTIME_OPTION("HL_PIDS_MAX"),
    HL_RUNTIME_OPTION("HL_PUBLISH"),
    HL_RUNTIME_OPTION("HL_PUBLISH_DAEMON"),
    HL_RUNTIME_OPTION("HL_RESTORE_DIR"),
    HL_RUNTIME_OPTION("HL_ROOTFS_RO"),
    HL_RUNTIME_OPTION("HL_SANDBOX"),
    HL_RUNTIME_OPTION("HL_UID"),
    HL_RUNTIME_OPTION("HL_ULIMITS"),
    HL_RUNTIME_OPTION("HL_UNTRUSTED"),
    HL_RUNTIME_OPTION("HL_VOLUMES"),
};

static const hl_option_definition *hl_option_find(const char *name) {
    size_t index;
    if (name == NULL) return NULL;
    for (index = 0; index < sizeof hl_option_definitions / sizeof hl_option_definitions[0]; index++)
        if (strcmp(name, hl_option_definitions[index].name) == 0) return &hl_option_definitions[index];
    return NULL;
}

static const char *hl_option_get(const char *name) {
    const hl_option_definition *definition = hl_option_find(name);
    if (definition == NULL) return NULL;
    return getenv(definition->name);
}

static int hl_option_set(const char *name, const char *value, int overwrite) {
    const hl_option_definition *definition = hl_option_find(name);
    if (definition == NULL) return -1;
    return setenv(definition->name, value, overwrite);
}
