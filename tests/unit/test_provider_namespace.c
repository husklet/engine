#include "../../src/core/provider_namespace.h"
#include "test.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static void put16(unsigned char **cursor, uint16_t value) {
    *(*cursor)++ = (unsigned char)value;
    *(*cursor)++ = (unsigned char)(value >> 8);
}

static void put32(unsigned char **cursor, uint32_t value) {
    put16(cursor, (uint16_t)value);
    put16(cursor, (uint16_t)(value >> 16));
}

static void put64(unsigned char **cursor, uint64_t value) {
    put32(cursor, (uint32_t)value);
    put32(cursor, (uint32_t)(value >> 32));
}

static void entry(unsigned char **cursor, uint64_t service, const char *path) {
    size_t size = strlen(path);
    put64(cursor, service);
    put32(cursor, 0660);
    put32(cursor, 10);
    put32(cursor, 20);
    put16(cursor, (uint16_t)size);
    memcpy(*cursor, path, size);
    *cursor += size;
}

static void entry_v2(unsigned char **cursor, uint8_t kind, uint64_t service, const char *path, const char *target) {
    size_t target_size = target == NULL ? 0 : strlen(target);
    *(*cursor)++ = kind;
    entry(cursor, service, path);
    put16(cursor, (uint16_t)target_size);
    if (target_size != 0) {
        memcpy(*cursor, target, target_size);
        *cursor += target_size;
    }
}

static void device_v3(unsigned char **cursor, uint8_t kind, uint64_t service, const char *path, uint32_t major,
                      uint32_t minor) {
    entry_v2(cursor, kind, service, path, NULL);
    put32(cursor, major);
    put32(cursor, minor);
}

int main(void) {
    hl_provider_namespace namespace = {0};
    unsigned char wire[256], invalid[256];
    unsigned char *cursor = wire;
    const hl_provider_node *node;
    uint64_t generation;
    put32(&cursor, 1);
    entry(&cursor, 9, "/run/provider");
    HL_CHECK(hl_provider_namespace_install(&namespace, wire, (size_t)(cursor - wire), 4, 128) == 0);
    node = hl_provider_namespace_resolve(&namespace, "/run/provider", 13);
    HL_CHECK(node != NULL && node->service == 9 && node->mode == 0660 && node->uid == 10 && node->gid == 20);
    generation = namespace.generation;

    cursor = invalid;
    put32(&cursor, 2);
    entry(&cursor, 1, "/run/provider");
    entry(&cursor, 2, "/run/provider/child");
    HL_CHECK(hl_provider_namespace_install(&namespace, invalid, (size_t)(cursor - invalid), 4, 128) == -EEXIST);
    HL_CHECK(namespace.generation == generation && namespace.count == 1 &&
             hl_provider_namespace_resolve(&namespace, "/run/provider", 13) != NULL);

    cursor = invalid;
    put32(&cursor, 1);
    entry(&cursor, 3, "/run/../escape");
    HL_CHECK(hl_provider_namespace_install(&namespace, invalid, (size_t)(cursor - invalid), 4, 128) == -EINVAL);
    HL_CHECK(namespace.generation == generation && namespace.count == 1);

    cursor = wire;
    put32(&cursor, UINT32_C(0x80000003));
    entry_v2(&cursor, HL_PROVIDER_NODE_DIRECTORY, 0, "/run/domain", NULL);
    entry_v2(&cursor, HL_PROVIDER_NODE_SERVICE, 44, "/run/domain/control", NULL);
    entry_v2(&cursor, HL_PROVIDER_NODE_SYMLINK, 0, "/run/domain/current", "control");
    HL_CHECK(hl_provider_namespace_install(&namespace, wire, (size_t)(cursor - wire), 4, 128) == 0);
    node = hl_provider_namespace_resolve(&namespace, "/run/domain", 11);
    HL_CHECK(node != NULL && node->kind == HL_PROVIDER_NODE_DIRECTORY && node->service == 0);
    node = hl_provider_namespace_resolve(&namespace, "/run/domain/control", 19);
    HL_CHECK(node != NULL && node->kind == HL_PROVIDER_NODE_SERVICE && node->service == 44);
    node = hl_provider_namespace_resolve(&namespace, "/run/domain/current", 19);
    HL_CHECK(node != NULL && node->kind == HL_PROVIDER_NODE_SYMLINK && node->target_size == 7 &&
             memcmp(node->target, "control", 7) == 0);

    cursor = wire;
    put32(&cursor, UINT32_C(0xc0000001));
    device_v3(&cursor, HL_PROVIDER_NODE_CHARACTER, 55, "/dev/provider", 226, 128);
    HL_CHECK(hl_provider_namespace_install(&namespace, wire, (size_t)(cursor - wire), 4, 128) == 0);
    node = hl_provider_namespace_resolve(&namespace, "/dev/provider", 13);
    HL_CHECK(node != NULL && node->kind == HL_PROVIDER_NODE_CHARACTER && node->service == 55 && node->major == 226 &&
             node->minor == 128);
    hl_provider_namespace_revoke(&namespace);
    HL_CHECK(namespace.count == 0 && namespace.generation != generation);
    return 0;
}
