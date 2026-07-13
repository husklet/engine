#include "test.h"

#include "hl/config.h"

#include <stddef.h>
#include <string.h>

typedef struct test_wire {
    hl_launch_config config;
    char pool[8];
} test_wire;

int main(void) {
    test_wire wire;
    hl_launch_config config;
    const char *pool;
    const char *value;
    size_t value_size;

    HL_CHECK(sizeof(hl_launch_config) == 128);
    HL_CHECK(offsetof(hl_launch_config, magic) == 0);
    HL_CHECK(offsetof(hl_launch_config, pool_size) == 4);
    HL_CHECK(offsetof(hl_launch_config, header_size) == 8);
    HL_CHECK(offsetof(hl_launch_config, abi) == 12);

    memset(&wire, 0, sizeof(wire));
    wire.config.magic = HL_CONFIG_MAGIC;
    wire.config.pool_size = sizeof(wire.pool);
    wire.config.header_size = sizeof(wire.config);
    wire.config.abi = HL_CONFIG_ABI;
    memcpy(wire.pool + 1, "guest", 6);
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), &config, &pool) == HL_STATUS_OK);
    HL_CHECK(hl_launch_config_string(&config, pool, 1, &value, &value_size) == HL_STATUS_OK);
    HL_CHECK(value_size == 5 && memcmp(value, "guest", 5) == 0);

    wire.config.pool_size++;
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    wire.config.pool_size--;
    wire.pool[0] = 'x';
    HL_CHECK(hl_launch_config_validate(&wire, sizeof(wire), NULL, NULL) == HL_STATUS_CORRUPT);
    return EXIT_SUCCESS;
}
