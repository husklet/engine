#ifndef HL_CONFIG_H
#define HL_CONFIG_H

#include "hl/base.h"

HL_EXTERN_C_BEGIN

#define HL_CONFIG_MAGIC UINT32_C(0x484c4346)
#define HL_CONFIG_ABI 1u

typedef struct hl_launch_config {
    uint32_t magic;
    uint32_t pool_size;
    uint32_t header_size;
    uint32_t abi;
    uint64_t memory_limit;
    uint32_t pid_limit;
    uint32_t cpu_limit;
    int32_t uid;
    int32_t gid;
    uint32_t rootfs_read_only;
    uint32_t sandbox;
    uint32_t network_isolated;
    uint32_t publish_external;
    uint32_t rootfs_offset;
    uint32_t lower_layers_offset;
    uint32_t hostname_offset;
    uint32_t network_namespace_offset;
    uint32_t publish_offset;
    uint32_t volumes_offset;
    uint32_t limits_offset;
    uint32_t working_directory_offset;
    uint32_t environment_offset;
    uint32_t translation_cache_offset;
    uint32_t network_bridge_offset;
    uint32_t ip_offset;
    uint32_t filesystem_generation_offset;
    uint32_t arguments_offset;
    uint32_t gpu_enabled;
    uint32_t translation_cache_disabled;
    uint32_t egress_proxy_offset;
    uint32_t reserved;
} hl_launch_config;

HL_API hl_status hl_launch_config_validate(const void *wire, size_t wire_size, hl_launch_config *out_config,
                                           const char **out_pool);
HL_API hl_status hl_launch_config_string(const hl_launch_config *config, const char *pool, uint32_t offset,
                                         const char **out_string, size_t *out_size);

HL_EXTERN_C_END

#endif
