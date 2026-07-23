#include "test.h"

#include "hl/config.h"

#include <stddef.h>
#include <string.h>

typedef struct test_wire {
    hl_launch_config config;
    char pool[32];
} test_wire;

int main(void) {
    test_wire wire;
    hl_launch_config config;
    const char *pool;
    const char *value;
    size_t value_size;
    size_t argument_count;
    hl_engine_publish_rule rule = {UINT32_C(0x0100007f), 8080, 80};
    const hl_engine_publish_rule *rules;

    HL_CHECK(sizeof(hl_engine_publish_rule) == 8);
    HL_CHECK(offsetof(hl_engine_publish_rule, host_ipv4_be) == 0);
    HL_CHECK(offsetof(hl_engine_publish_rule, host_port) == 4);
    HL_CHECK(offsetof(hl_engine_publish_rule, guest_port) == 6);
    HL_CHECK(offsetof(hl_launch_config, magic) == 0);
    HL_CHECK(offsetof(hl_launch_config, pool_size) == 4);
    HL_CHECK(offsetof(hl_launch_config, header_size) == 8);
    HL_CHECK(offsetof(hl_launch_config, abi) == 12);
    HL_CHECK(offsetof(hl_launch_config, restore_directory_offset) == 128);
    HL_CHECK(offsetof(hl_launch_config, result_path_offset) == 132);
    HL_CHECK(offsetof(hl_launch_config, publish_count) == 136);
    HL_CHECK(offsetof(hl_launch_config, network_interfaces_offset) == 140);
    HL_CHECK(offsetof(hl_launch_config, file_owners_offset) == 144);
    HL_CHECK(offsetof(hl_launch_config, process_domain) == 152);
    HL_CHECK(offsetof(hl_launch_config, network_transport) == 176);
    HL_CHECK(offsetof(hl_launch_config, lower_layer_count) == 184);
    HL_CHECK(offsetof(hl_launch_config, overlay_work_offset) == 188);
    HL_CHECK(sizeof(hl_launch_config) == 200);
    HL_CHECK(sizeof(hl_launch_result) == 32);

    memset(&wire, 0, sizeof(wire));
    wire.config.magic = HL_CONFIG_MAGIC;
    wire.config.abi = HL_CONFIG_ABI + 1;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_ABI_MISMATCH);
    wire.config.abi = HL_CONFIG_ABI;
    wire.config.pool_size = sizeof(wire.pool);
    wire.config.header_size = sizeof(wire.config);
    wire.config.abi = HL_CONFIG_ABI;
    wire.config.process_domain[0] = 1;
    wire.config.network_transport = HL_CONFIG_NETWORK_VIRTUAL;
    memcpy(wire.pool + 1, "guest", 6);
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), &config, &pool) == HL_STATUS_OK);
    HL_CHECK(hl_launch_config_string(&config, pool, 1, &value, &value_size) == HL_STATUS_OK);
    HL_CHECK(value_size == 5 && memcmp(value, "guest", 5) == 0);

    wire.config.arguments_offset = 8;
    memcpy(wire.pool + 8, "guest\0--flag\0\0", 15);
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), &config, &pool) == HL_STATUS_OK);
    HL_CHECK(hl_launch_config_arguments_validate(&config, pool, &argument_count) == HL_STATUS_OK);
    HL_CHECK(argument_count == 2);
    HL_CHECK(hl_launch_config_argument(&config, pool, 0, &value, &value_size) == HL_STATUS_OK);
    HL_CHECK(value_size == 5 && memcmp(value, "guest", 5) == 0);
    HL_CHECK(hl_launch_config_argument(&config, pool, 1, &value, &value_size) == HL_STATUS_OK);
    HL_CHECK(value_size == 6 && memcmp(value, "--flag", 6) == 0);
    HL_CHECK(hl_launch_config_argument(&config, pool, 2, NULL, NULL) == HL_STATUS_NOT_FOUND);

    wire.config.process_domain[0] = 0;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_INVALID_ARGUMENT);
    wire.config.process_domain[1] = 1;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_OK);

    wire.config.magic = UINT32_C(0x44434647);
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.magic = HL_CONFIG_MAGIC;

    wire.config.pool_size++;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.pool_size--;
    wire.pool[0] = 'x';
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.pool[0] = '\0';
    wire.config.publish_offset = 16;
    wire.config.publish_count = 1;
    memcpy(wire.pool + 16, &rule, sizeof rule);
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), &config, &pool) == HL_STATUS_OK);
    HL_CHECK(hl_launch_config_publish(&config, pool, &rules) == HL_STATUS_OK);
    HL_CHECK(rules[0].host_ipv4_be == rule.host_ipv4_be && rules[0].host_port == 8080 && rules[0].guest_port == 80);
    wire.config.publish_offset = 0;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.publish_count = 0;

    wire.config.network_transport = 3;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.network_transport = HL_CONFIG_NETWORK_VIRTUAL;
    wire.config.reserved = 1;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.reserved = 0;
    wire.config.reserved_abi11 = 1;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.reserved_abi11 = 0;
    wire.config.lower_layers_offset = 1;
    wire.config.lower_layer_count = 1;
    wire.config.overlay_work_offset = 24;
    memcpy(wire.pool + 1, "/lower", 7);
    memcpy(wire.pool + 24, "/work", 6);
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_OK);
    wire.config.lower_layer_count = 9;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.lower_layer_count = 1;
    wire.config.overlay_work_offset = 0;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.lower_layers_offset = 0;
    wire.config.lower_layer_count = 0;

    {
        unsigned char legacy[176 + 4] = {0};
        uint32_t word;
        uint64_t identity = 1;
        word = HL_CONFIG_MAGIC;
        memcpy(legacy + 0, &word, sizeof word);
        word = 4;
        memcpy(legacy + 4, &word, sizeof word);
        word = 176;
        memcpy(legacy + 8, &word, sizeof word);
        word = HL_CONFIG_ABI_LEGACY;
        memcpy(legacy + 12, &word, sizeof word);
        memcpy(legacy + offsetof(hl_launch_config, process_domain), &identity, sizeof identity);
        legacy[176] = '\0';
        HL_CHECK(hl_launch_config_validate(legacy, sizeof legacy, &config, &pool) == HL_STATUS_OK);
        HL_CHECK(config.network_transport == HL_CONFIG_NETWORK_VIRTUAL && pool == (const char *)legacy + 176);
        word = 1;
        memcpy(legacy + offsetof(hl_launch_config, network_isolated), &word, sizeof word);
        HL_CHECK(hl_launch_config_validate(legacy, sizeof legacy, &config, NULL) == HL_STATUS_OK);
        HL_CHECK(config.network_transport == HL_CONFIG_NETWORK_ISOLATED);
        memcpy(legacy + offsetof(hl_launch_config, reserved), &word, sizeof word);
        HL_CHECK(hl_launch_config_validate(legacy, sizeof legacy, NULL, NULL) == HL_STATUS_CORRUPT);
    }
    return EXIT_SUCCESS;
}
