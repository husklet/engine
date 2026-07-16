#include "test.h"

#include "hl/engine.h"
#include "hl/fake.h"
#include "../../src/core/engine_backend.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static int32_t fake_entry(void *context) {
    (void)context;
    return 0;
}

static uint32_t fake_box_seen;
static hl_engine_config fake_config_seen;
static const char *expected_cwd;
static const char *expected_hostname;
static const char *expected_environment;
static const char *expected_uid;
static const char *expected_gid;
static uint32_t expected_box_flags;
static const char *expected_rootfs;
static const char *expected_seed_cpu;
static const char *expected_seed_volume;
static uint32_t expected_extended;
static const char *expected_extended_values[16];
static const hl_host_file_services *rollback_file;
static uint32_t rollback_clone_calls;
static uint32_t rollback_invalid_result;

static hl_host_result rollback_clone(void *context, hl_host_handle handle) {
    if (++rollback_clone_calls == 2u) {
        if (rollback_invalid_result) return (hl_host_result){HL_STATUS_OK, 0, HL_HOST_HANDLE_INVALID, 0};
        return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    }
    return rollback_file->clone_for_fork(context, handle);
}

static int check_fd_import_rollback(uint32_t invalid_result) {
    hl_fake_host fake;
    hl_host_services services;
    hl_host_file_services file;
    hl_engine_config config = {0};
    hl_engine_fd_binding bindings[2] = {0};
    hl_host_result originals[2];
    hl_engine *engine = NULL;
    uint32_t index;
    hl_fake_host_init(&fake, &services);
    file = *services.file;
    rollback_file = services.file;
    rollback_clone_calls = 0;
    rollback_invalid_result = invalid_result;
    file.clone_for_fork = rollback_clone;
    services.file = &file;
    for (index = 0; index < 2; ++index) {
        originals[index] = hl_fake_host_file_create(&fake);
        HL_CHECK(originals[index].status == HL_STATUS_OK);
        bindings[index].abi = HL_ENGINE_ABI;
        bindings[index].size = sizeof(bindings[index]);
        bindings[index].guest_fd = 3u + index;
        bindings[index].ownership = HL_ENGINE_FD_BORROW;
        bindings[index].host_handle = originals[index].value;
    }
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_GUEST_ISA_AARCH64;
    config.fd_bindings = bindings;
    config.fd_binding_count = 2;
    HL_CHECK(hl_engine_create(&config, &services, &engine) ==
             (invalid_result ? HL_STATUS_PLATFORM_FAILURE : HL_STATUS_IO));
    HL_CHECK(engine == NULL && rollback_clone_calls == 2u);
    HL_CHECK(fake.live_files == 2u && fake.live_file_clones == 0u && fake.file_close_count == 1u);
    HL_CHECK(services.file->close(services.context, originals[0].value).status == HL_STATUS_OK);
    HL_CHECK(services.file->close(services.context, originals[1].value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_files == 0u && fake.file_close_count == 3u);
    return EXIT_SUCCESS;
}

static const char *const extended_option_names[16] = {
    "HL_LOWER", "HL_PUBLISH", "HL_VOLUMES", "HL_ULIMITS", "HL_NETNS", "HL_PCACHE_DIR", "HL_NETBR", "HL_IP",
    "HL_FSGEN_FILE", "HL_EGRESS_SOCKS", "HL_CHECKPOINT_DIR", "HL_RESTORE_DIR", "HL_PCACHE", "HL_PUBLISH_DAEMON",
    "HL_UNTRUSTED", "HL_SANDBOX"
};

static int option_matches(const hl_options *options, const char *name, const char *expected) {
    const char *actual = hl_options_get(options, name);
    return expected == NULL ? actual == NULL : actual != NULL && strcmp(actual, expected) == 0;
}

static int option_matches_u64(const hl_options *options, const char *name, uint64_t expected) {
    char value[32];
    const char *actual = hl_options_get(options, name);
    if (expected == 0) return actual == NULL;
    snprintf(value, sizeof(value), "%llu", (unsigned long long)expected);
    return actual != NULL && strcmp(actual, value) == 0;
}

static uint32_t fake_backend_seen;

static hl_status fake_start_common(const hl_host_services *host, hl_linux_abi *box, hl_options *options,
                                   const hl_engine_config *config, uint32_t argc, const char *const argv[],
                                   hl_host_handle *process, hl_host_handle *result_stream) {
    hl_host_result spawned;
    (void)argc;
    (void)argv;
    if (!option_matches_u64(options, "HL_MEM_MAX", config->memory_limit) ||
        !option_matches_u64(options, "HL_PIDS_MAX", config->pid_limit) ||
        (expected_seed_cpu == NULL && !option_matches_u64(options, "HL_CPUS", config->cpu_limit)) ||
        (expected_seed_cpu != NULL && !option_matches(options, "HL_CPUS", expected_seed_cpu)) ||
        (expected_seed_volume != NULL && !option_matches(options, "HL_VOLUMES", expected_seed_volume)))
        return HL_STATUS_CORRUPT;
    if (expected_cwd != NULL &&
        (!option_matches(options, "HL_CWD", expected_cwd) ||
         !option_matches(options, "HL_HOSTNAME", expected_hostname) ||
         !option_matches(options, "HL_GUEST_ENV", expected_environment) ||
         !option_matches(options, "HL_UID", expected_uid) || !option_matches(options, "HL_GID", expected_gid) ||
         !option_matches(options, "HL_ROOTFS_RO", (expected_box_flags & HL_ENGINE_BOX_ROOTFS_READ_ONLY) ? "1" : NULL) ||
         !option_matches(options, "HL_SANDBOX", (expected_box_flags & HL_ENGINE_BOX_SANDBOX) ? "1" : NULL) ||
         !option_matches(options, "HL_UNTRUSTED", (expected_box_flags & HL_ENGINE_BOX_SANDBOX) ? "1" : NULL) ||
         !option_matches(options, "HL_NET_ISOLATE", (expected_box_flags & HL_ENGINE_BOX_NETWORK_ISOLATED) ? "1" : NULL)))
        return HL_STATUS_CORRUPT;
    if (expected_rootfs != NULL && (config->rootfs == NULL || strcmp(config->rootfs, expected_rootfs) != 0))
        return HL_STATUS_CORRUPT;
    if (expected_extended) {
        size_t index;
        for (index = 0; index < sizeof(extended_option_names) / sizeof(extended_option_names[0]); ++index)
            if (!option_matches(options, extended_option_names[index], expected_extended_values[index]))
                return HL_STATUS_CORRUPT;
    }
    if (box == NULL || hl_linux_abi_validate_fds(box) != HL_STATUS_OK) return HL_STATUS_CORRUPT;
    fake_box_seen++;
    fake_config_seen = *config;
    spawned = host->process->spawn_cloned(host->context, fake_entry, NULL);
    if (spawned.status != HL_STATUS_OK) return (hl_status)spawned.status;
    *process = spawned.value;
    *result_stream = HL_HOST_HANDLE_INVALID;
    return HL_STATUS_OK;
}

static hl_status fake_start_aarch64(const hl_host_services *host, hl_linux_abi *box, hl_options *options,
                                    const hl_engine_config *config, uint32_t argc, const char *const argv[],
                                    hl_host_handle *process, hl_host_handle *result_stream) {
    fake_backend_seen = HL_GUEST_ISA_AARCH64;
    return fake_start_common(host, box, options, config, argc, argv, process, result_stream);
}

static hl_status fake_start_x86_64(const hl_host_services *host, hl_linux_abi *box, hl_options *options,
                                   const hl_engine_config *config, uint32_t argc, const char *const argv[],
                                   hl_host_handle *process, hl_host_handle *result_stream) {
    fake_backend_seen = HL_GUEST_ISA_X86_64;
    return fake_start_common(host, box, options, config, argc, argv, process, result_stream);
}

static const hl_engine_backend fake_backend = {HL_GUEST_ISA_AARCH64, fake_start_aarch64, NULL, NULL};
static const hl_engine_backend fake_x86_backend = {HL_GUEST_ISA_X86_64, fake_start_x86_64, NULL, NULL};

typedef struct run_context {
    hl_engine *engine;
    hl_engine_exit result;
    hl_status status;
} run_context;

typedef struct box_abi1 {
    HL_ABI_HEADER;
    uint32_t flags;
    int32_t uid;
    int32_t gid;
    uint32_t reserved;
    const char *working_directory;
    const char *hostname;
    const char *environment;
} box_abi1;

static void *run_engine(void *opaque) {
    run_context *context = opaque;
    context->status = hl_engine_run(context->engine, 0, NULL, &context->result);
    return NULL;
}

static int check_concurrent_stop(hl_fake_host *fake, hl_host_services *services, hl_engine_config *config,
                                 uint32_t request, int32_t expected_signal) {
    run_context context;
    pthread_t thread;
    memset(&context, 0, sizeof(context));
    context.result.abi = HL_ENGINE_ABI;
    context.result.size = sizeof(context.result);
    hl_engine_backend_register(&fake_backend);
    HL_CHECK(hl_engine_create(config, services, &context.engine) == HL_STATUS_OK);
    HL_CHECK(hl_engine_request(context.engine, request, NULL, 0) == HL_STATUS_BUSY);
    hl_fake_host_block_process_wait(fake, 1);
    HL_CHECK(pthread_create(&thread, NULL, run_engine, &context) == 0);
    while (__atomic_load_n(&fake->live_processes, __ATOMIC_ACQUIRE) == 0)
        sched_yield();
    HL_CHECK(hl_engine_request(context.engine, request, NULL, 0) == HL_STATUS_OK);
    HL_CHECK(pthread_join(thread, NULL) == 0);
    HL_CHECK(context.status == HL_STATUS_OK && context.result.kind == HL_ENGINE_EXIT_SIGNAL &&
             context.result.guest_status == expected_signal);
    HL_CHECK(hl_engine_request(context.engine, request, NULL, 0) == HL_STATUS_BUSY);
    HL_CHECK(hl_engine_run(context.engine, 0, NULL, &context.result) == HL_STATUS_BUSY);
    hl_engine_destroy(context.engine);
    return EXIT_SUCCESS;
}

static int check_concurrent_destroy(hl_fake_host *fake, hl_host_services *services, hl_engine_config *config) {
    enum { REPEATS = 100 };
    uint32_t iteration;
    for (iteration = 0; iteration < REPEATS; ++iteration) {
        run_context context;
        pthread_t thread;
        memset(&context, 0, sizeof(context));
        context.result.abi = HL_ENGINE_ABI;
        context.result.size = sizeof(context.result);
        hl_fake_host_block_process_wait(fake, 1);
        HL_CHECK(hl_engine_create(config, services, &context.engine) == HL_STATUS_OK);
        HL_CHECK(pthread_create(&thread, NULL, run_engine, &context) == 0);
        while (__atomic_load_n(&fake->live_processes, __ATOMIC_ACQUIRE) == 0)
            sched_yield();
        hl_engine_destroy(context.engine);
        HL_CHECK(pthread_join(thread, NULL) == 0);
        HL_CHECK(context.status == HL_STATUS_OK && context.result.kind == HL_ENGINE_EXIT_SIGNAL &&
                 context.result.guest_status == 9 && __atomic_load_n(&fake->live_processes, __ATOMIC_ACQUIRE) == 0);
    }
    return EXIT_SUCCESS;
}

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_engine_config config;
    hl_engine_exit engine_exit;
    hl_engine *engine = NULL;
    hl_engine *second_engine = NULL;
    hl_engine_box_config box;
    hl_engine_box_config second_box;
    box_abi1 old_box;
    hl_options source_options;
    char first_cwd[] = "/first";
    char first_host[] = "first-box";
    char first_env[] = "MODE=first\nTERM=xterm";
    char second_cwd[] = "/second";
    char second_host[] = "second-box";
    char second_env[] = "MODE=second";
    char first_root[] = "/root/first";
    char second_root[] = "/root/second";
    char lower[] = "/layers/base:/layers/app";
    hl_engine_publish_rule publish[] = {{0, 8080, 80}};
    char volumes[] = "ro:/guest:/host";
    char limits[] = "nofile=1024:2048";
    char netns[] = "box-alpha";
    char cache[] = "/cache/translated";
    char bridge[] = "bridge-alpha";
    char ip[] = "10.0.0.2";
    char fsgen[] = "/run/fs-generation";
    char proxy[] = "127.0.0.1:1080";
    char checkpoint[] = "/checkpoints/out";

    HL_CHECK(check_fd_import_rollback(0) == EXIT_SUCCESS);
    HL_CHECK(check_fd_import_rollback(1) == EXIT_SUCCESS);

    hl_fake_host_init(&fake, &services);
    memset(&config, 0, sizeof(config));
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_GUEST_ISA_AARCH64;
    config.flags = 1;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    config.flags = 0;
    config.reserved = 1;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    config.reserved = 0;
    config.payload = "program";
    config.payload_size = 7;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_NOT_SUPPORTED && engine == NULL);
    config.payload = NULL;
    config.payload_size = 0;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK && engine != NULL);
    HL_CHECK(hl_engine_request(engine, UINT32_MAX, NULL, 0) == HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(hl_engine_request(engine, HL_ENGINE_REQUEST_FORCE_STOP, "x", 1) == HL_STATUS_INVALID_ARGUMENT);
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(engine_exit.kind == HL_ENGINE_EXIT_ENGINE_ERROR);
    hl_engine_destroy(engine);

    config.memory_limit = UINT64_C(536870912);
    config.pid_limit = 37;
    config.cpu_limit = 3;
    hl_engine_backend_register(&fake_backend);
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK);
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    HL_CHECK(fake_config_seen.memory_limit == config.memory_limit && fake_config_seen.pid_limit == config.pid_limit &&
             fake_config_seen.cpu_limit == config.cpu_limit);
    hl_engine_destroy(engine);
    config.memory_limit = 0;
    config.pid_limit = 0;
    config.cpu_limit = 0;

    /* Registering the second production ISA must not replace the first one. */
    hl_engine_backend_register(&fake_x86_backend);
    config.guest_isa = HL_GUEST_ISA_AARCH64;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK);
    fake_backend_seen = 0;
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    HL_CHECK(fake_backend_seen == HL_GUEST_ISA_AARCH64);
    hl_engine_destroy(engine);
    config.guest_isa = HL_GUEST_ISA_X86_64;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK);
    fake_backend_seen = 0;
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    HL_CHECK(fake_backend_seen == HL_GUEST_ISA_X86_64);
    hl_engine_destroy(engine);
    config.guest_isa = HL_GUEST_ISA_AARCH64;

    /* The public box input is validated before effects and deeply owned by each instance. */
    memset(&box, 0, sizeof(box));
    box.abi = HL_ENGINE_BOX_ABI;
    box.size = sizeof(box);
    box.uid = 1001;
    box.gid = 1002;
    box.working_directory = first_cwd;
    box.hostname = first_host;
    box.environment = first_env;
    box.flags = HL_ENGINE_BOX_ROOTFS_READ_ONLY | HL_ENGINE_BOX_SANDBOX;
    config.box = &box;
    config.rootfs = first_root;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK);
    box.uid = 2001;
    box.gid = 2002;
    box.working_directory = second_cwd;
    box.hostname = second_host;
    box.environment = second_env;
    box.flags = HL_ENGINE_BOX_NETWORK_ISOLATED;
    config.rootfs = second_root;
    HL_CHECK(hl_engine_create(&config, &services, &second_engine) == HL_STATUS_OK);
    memset(first_cwd, 'x', sizeof(first_cwd) - 1);
    memset(first_host, 'x', sizeof(first_host) - 1);
    memset(first_env, 'x', sizeof(first_env) - 1);
    memset(first_root, 'x', sizeof(first_root) - 1);
    expected_cwd = "/first";
    expected_hostname = "first-box";
    expected_environment = "MODE=first\nTERM=xterm";
    expected_uid = "1001";
    expected_gid = "1002";
    expected_box_flags = HL_ENGINE_BOX_ROOTFS_READ_ONLY | HL_ENGINE_BOX_SANDBOX;
    expected_rootfs = "/root/first";
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    expected_cwd = "/second";
    expected_hostname = "second-box";
    expected_environment = "MODE=second";
    expected_uid = "2001";
    expected_gid = "2002";
    expected_box_flags = HL_ENGINE_BOX_NETWORK_ISOLATED;
    expected_rootfs = "/root/second";
    HL_CHECK(hl_engine_run(second_engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    hl_engine_destroy(engine);
    hl_engine_destroy(second_engine);
    expected_cwd = NULL;
    expected_rootfs = NULL;
    config.box = NULL;
    config.rootfs = NULL;

    /* ABI 2 owns every extended setting and applies it only to its engine instance. */
    memset(&box, 0, sizeof(box));
    box.abi = HL_ENGINE_BOX_ABI;
    box.size = sizeof(box);
    box.uid = -1;
    box.gid = -1;
    box.lower_layers = lower;
    box.publish = publish;
    box.publish_count = 1;
    box.volumes = volumes;
    box.limits = limits;
    box.network_namespace = netns;
    box.translation_cache = cache;
    box.network_bridge = bridge;
    box.ip = ip;
    box.filesystem_generation = fsgen;
    box.egress_proxy = proxy;
    box.checkpoint_directory = checkpoint;
    box.flags = HL_ENGINE_BOX_PUBLISH_EXTERNAL | HL_ENGINE_BOX_SENTRY_ONLY;
    config.box = &box;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK);
    memset(&second_box, 0, sizeof(second_box));
    second_box.abi = HL_ENGINE_BOX_ABI;
    second_box.size = sizeof(second_box);
    second_box.uid = -1;
    second_box.gid = -1;
    second_box.lower_layers = "/layers/second";
    second_box.translation_cache = "/cache/second";
    config.box = &second_box;
    HL_CHECK(hl_engine_create(&config, &services, &second_engine) == HL_STATUS_OK);
    memset(lower, 'x', sizeof(lower) - 1);
    memset(publish, 'x', sizeof(publish));
    memset(volumes, 'x', sizeof(volumes) - 1);
    memset(limits, 'x', sizeof(limits) - 1);
    memset(netns, 'x', sizeof(netns) - 1);
    memset(cache, 'x', sizeof(cache) - 1);
    memset(bridge, 'x', sizeof(bridge) - 1);
    memset(ip, 'x', sizeof(ip) - 1);
    memset(fsgen, 'x', sizeof(fsgen) - 1);
    memset(proxy, 'x', sizeof(proxy) - 1);
    memset(checkpoint, 'x', sizeof(checkpoint) - 1);
    expected_extended_values[0] = "/layers/base:/layers/app";
    expected_extended_values[1] = "8080:80";
    expected_extended_values[2] = "ro:/guest:/host";
    expected_extended_values[3] = "nofile=1024:2048";
    expected_extended_values[4] = "box-alpha";
    expected_extended_values[5] = "/cache/translated";
    expected_extended_values[6] = "bridge-alpha";
    expected_extended_values[7] = "10.0.0.2";
    expected_extended_values[8] = "/run/fs-generation";
    expected_extended_values[9] = "127.0.0.1:1080";
    expected_extended_values[10] = "/checkpoints/out";
    expected_extended_values[12] = "1";
    expected_extended_values[13] = "1";
    expected_extended_values[14] = "1";
    expected_extended = 1;
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    memset(expected_extended_values, 0, sizeof(expected_extended_values));
    expected_extended_values[0] = "/layers/second";
    expected_extended_values[5] = "/cache/second";
    expected_extended_values[12] = "1";
    HL_CHECK(hl_engine_run(second_engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    hl_engine_destroy(engine);
    hl_engine_destroy(second_engine);
    expected_extended = 0;
    memset(expected_extended_values, 0, sizeof(expected_extended_values));

    /* ABI 1's exact historical prefix remains accepted without reading ABI 2 storage. */
    memset(&old_box, 0, sizeof(old_box));
    old_box.abi = HL_ENGINE_BOX_ABI_1;
    old_box.size = sizeof(old_box);
    old_box.uid = -1;
    old_box.gid = -1;
    old_box.hostname = "old-abi";
    HL_CHECK(sizeof(old_box) == offsetof(hl_engine_box_config, lower_layers));
    config.box = (const hl_engine_box_config *)(const void *)&old_box;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK);
    hl_engine_destroy(engine);

    /* Invalid cross-field combinations fail transactionally and never return an engine. */
    memset(&box, 0, sizeof(box));
    box.abi = HL_ENGINE_BOX_ABI;
    box.size = sizeof(box);
    box.uid = -1;
    box.gid = -1;
    config.box = &box;
    box.translation_cache = "/cache";
    box.flags = HL_ENGINE_BOX_TRANSLATION_CACHE_DISABLED;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.translation_cache = NULL;
    box.flags = HL_ENGINE_BOX_SANDBOX | HL_ENGINE_BOX_SENTRY_ONLY;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.flags = 0;
    box.checkpoint_directory = "/checkpoint";
    box.restore_directory = "/restore";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.checkpoint_directory = NULL;
    box.restore_directory = NULL;
    box.ip = "10.0.0.2";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.ip = NULL;
    box.flags = HL_ENGINE_BOX_PUBLISH_EXTERNAL;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.flags = HL_ENGINE_BOX_NETWORK_ISOLATED;
    box.egress_proxy = "127.0.0.1:1080";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.egress_proxy = NULL;
    box.flags = 0;
    box.translation_cache = "relative";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.translation_cache = NULL;
    box.lower_layers = "";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.lower_layers = "relative:/lower";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.lower_layers = NULL;
    publish[0] = (hl_engine_publish_rule){UINT32_C(0x0100007f), 8080, 80};
    box.publish = publish;
    box.publish_count = 1;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK && engine != NULL);
    hl_engine_destroy(engine);
    engine = NULL;
    publish[0].host_port = 0;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    publish[0].host_port = 8080;
    box.publish_count = 0;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.publish_count = 1;
    box.publish = NULL;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.publish = NULL;
    box.publish_count = 0;
    box.volumes = "guest:/host";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.volumes = NULL;
    box.limits = "nofile=abc";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.limits = NULL;
    box.network_namespace = "bad/name";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.network_namespace = NULL;
    box.network_bridge = "bridge";
    box.ip = "999.0.0.1";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.ip = NULL;
    box.network_bridge = NULL;
    box.egress_proxy = "proxy:not-a-port";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    config.box = NULL;

    memset(&box, 0, sizeof(box));
    box.uid = -1;
    box.gid = -1;
    box.size = sizeof(box);
    box.abi = HL_ENGINE_BOX_ABI + 1;
    config.box = &box;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_ABI_MISMATCH && engine == NULL);
    box.abi = HL_ENGINE_BOX_ABI;
    box.flags = UINT32_MAX;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.flags = 0;
    box.uid = -2;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.uid = -1;
    box.gid = -1;
    box.working_directory = "relative";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.working_directory = "/";
    box.hostname = "hostname-that-is-deliberately-longer-than-the-linux-uts-name-limit-of-sixty-four-bytes";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.hostname = "bad/name";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.hostname = "-bad";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.hostname = "valid-box";
    box.environment = "NO_EQUALS";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.environment = "9BAD=value";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.environment = "GOOD=value\n";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_INVALID_ARGUMENT && engine == NULL);
    box.environment = "GOOD=value\nEMPTY=";
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK && engine != NULL);
    hl_engine_destroy(engine);
    engine = NULL;
    config.box = NULL;

    /* A validated launcher snapshot becomes private engine state; later source mutation cannot mask it. */
    HL_CHECK(hl_options_init(&source_options) == 0);
    HL_CHECK(hl_options_set(&source_options, "HL_CPUS", "2", 1) == 0);
    HL_CHECK(hl_options_set(&source_options, "HL_HOSTNAME", "wire-host", 1) == 0);
    HL_CHECK(hl_options_set(&source_options, "HL_VOLUMES", "/host:/guest:ro", 1) == 0);
    HL_CHECK(hl_engine_create_with_options(&config, &services, &source_options, &engine) == HL_STATUS_OK);
    HL_CHECK(hl_options_set(&source_options, "HL_CPUS", "99", 1) == 0);
    HL_CHECK(hl_options_unset(&source_options, "HL_VOLUMES") == 0);
    expected_seed_cpu = "2";
    expected_seed_volume = "/host:/guest:ro";
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_OK);
    hl_engine_destroy(engine);
    hl_options_destroy(&source_options);
    expected_seed_cpu = NULL;
    expected_seed_volume = NULL;

    HL_CHECK(check_concurrent_stop(&fake, &services, &config, HL_ENGINE_REQUEST_INTERRUPT, 2) == EXIT_SUCCESS);
    HL_CHECK(check_concurrent_stop(&fake, &services, &config, HL_ENGINE_REQUEST_FORCE_STOP, 9) == EXIT_SUCCESS);
    HL_CHECK(fake_box_seen == 10);
    HL_CHECK(check_concurrent_destroy(&fake, &services, &config) == EXIT_SUCCESS);

    config.abi++;
    engine = (hl_engine *)(uintptr_t)1;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_ABI_MISMATCH);
    HL_CHECK(engine == NULL);
    return EXIT_SUCCESS;
}
