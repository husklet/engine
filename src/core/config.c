#include "hl/config.h"

#include <string.h>

hl_status hl_launch_config_validate(const void *wire, size_t wire_size, hl_launch_config *out_config,
                                    const char **out_pool) {
    hl_launch_config config;
    size_t complete_size;
    if (out_config != NULL) memset(out_config, 0, sizeof(*out_config));
    if (out_pool != NULL) *out_pool = NULL;
    if (wire == NULL || wire_size < sizeof(config)) return HL_STATUS_INVALID_ARGUMENT;
    memcpy(&config, wire, sizeof(config));
    if (config.magic != HL_CONFIG_MAGIC) return HL_STATUS_CORRUPT;
    if (config.abi != HL_CONFIG_ABI) return HL_STATUS_ABI_MISMATCH;
    if (config.header_size < sizeof(config) || config.header_size > wire_size) return HL_STATUS_CORRUPT;
    complete_size = (size_t)config.header_size + config.pool_size;
    if (complete_size < config.header_size || complete_size != wire_size) return HL_STATUS_CORRUPT;
    if (config.pool_size == 0 || ((const char *)wire)[config.header_size] != '\0') return HL_STATUS_CORRUPT;
    if (out_config != NULL) *out_config = config;
    if (out_pool != NULL) *out_pool = (const char *)wire + config.header_size;
    return HL_STATUS_OK;
}

hl_status hl_launch_config_string(const hl_launch_config *config, const char *pool, uint32_t offset,
                                  const char **out_string, size_t *out_size) {
    const char *end;
    if (out_string != NULL) *out_string = NULL;
    if (out_size != NULL) *out_size = 0;
    if (config == NULL || pool == NULL || offset >= config->pool_size) return HL_STATUS_INVALID_ARGUMENT;
    end = memchr(pool + offset, '\0', config->pool_size - offset);
    if (end == NULL) return HL_STATUS_CORRUPT;
    if (out_string != NULL) *out_string = pool + offset;
    if (out_size != NULL) *out_size = (size_t)(end - (pool + offset));
    return HL_STATUS_OK;
}
