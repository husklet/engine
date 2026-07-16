#include "test.h"

#include "fdcache.h"
#include "hl/fake.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>

enum generation_fault {
    GENERATION_OK,
    GENERATION_OPEN_FAIL,
    GENERATION_MAP_FAIL,
    GENERATION_MAP_FAIL_OWNED,
    GENERATION_MAP_NO_HANDLE,
    GENERATION_MAP_NO_ADDRESS,
    GENERATION_MAP_SHORT,
    GENERATION_CLOSE_FAIL,
};

struct generation_mapping {
    hl_host_handle handle;
    _Atomic uint32_t *address;
    int live;
};

struct generation_host {
    hl_fake_host fake;
    enum generation_fault fault;
    struct generation_mapping mappings[16];
    uint32_t open_calls;
    uint32_t map_calls;
    uint32_t close_calls;
    uint32_t release_calls;
    hl_host_handle fail_release;
};

static hl_host_result generation_open(void *opaque, hl_host_handle directory, const char *path, size_t path_size,
                                      uint32_t access, uint32_t creation, uint32_t permissions) {
    struct generation_host *host = opaque;
    host->open_calls++;
    assert(directory == HL_HOST_HANDLE_CWD);
    assert(path_size == strlen("/run/fsgen") && memcmp(path, "/run/fsgen", path_size) == 0);
    assert(access == HL_HOST_FILE_READ && creation == 0 && permissions == 0);
    if (host->fault == GENERATION_OPEN_FAIL) return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    return (hl_host_result){HL_STATUS_OK, 0, 41, 0};
}

static hl_host_result generation_map(void *opaque, hl_host_handle file, uint64_t requested_address, uint64_t offset,
                                     uint64_t size, uint32_t protection, uint32_t flags,
                                     hl_host_file_mapping *output) {
    struct generation_host *host = opaque;
    struct generation_mapping *mapping = NULL;
    void *address;
    host->map_calls++;
    assert(file == 41 && requested_address == 0 && offset == 0 && size == sizeof(uint32_t));
    assert(protection == HL_HOST_MEMORY_READ && flags == HL_HOST_MEMORY_SHARED);
    assert(output->abi == HL_HOST_FILE_MAPPING_ABI && output->size >= sizeof(*output));
    if (host->fault == GENERATION_MAP_FAIL) return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    if (host->fault == GENERATION_MAP_NO_HANDLE) return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
    for (size_t index = 0; index < sizeof(host->mappings) / sizeof(host->mappings[0]); ++index) {
        if (!host->mappings[index].live) {
            mapping = &host->mappings[index];
            break;
        }
    }
    assert(mapping != NULL);
    address = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(address != MAP_FAILED);
    mapping->handle = ++host->fake.next_handle;
    mapping->address = address;
    mapping->live = 1;
    atomic_store(mapping->address, 7);
    output->handle = mapping->handle;
    output->address = (uint64_t)(uintptr_t)mapping->address;
    output->mapped_size = sizeof(uint32_t);
    if (host->fault == GENERATION_MAP_FAIL_OWNED) return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    if (host->fault == GENERATION_MAP_NO_ADDRESS) output->address = 0;
    if (host->fault == GENERATION_MAP_SHORT) output->mapped_size = sizeof(uint32_t) - 1;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result generation_close(void *opaque, hl_host_handle file) {
    struct generation_host *host = opaque;
    host->close_calls++;
    assert(file == 41);
    if (host->fault == GENERATION_CLOSE_FAIL) return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result generation_release(void *opaque, hl_host_handle handle) {
    struct generation_host *host = opaque;
    host->release_calls++;
    if (host->fail_release == handle) {
        host->fail_release = HL_HOST_HANDLE_INVALID;
        return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    }
    for (size_t index = 0; index < sizeof(host->mappings) / sizeof(host->mappings[0]); ++index) {
        struct generation_mapping *mapping = &host->mappings[index];
        if (mapping->live && mapping->handle == handle) {
            assert(munmap(mapping->address, 4096) == 0);
            mapping->live = 0;
            mapping->address = NULL;
            return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
        }
    }
    return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
}

static struct generation_mapping *generation_live(struct generation_host *host, hl_host_handle except) {
    for (size_t index = 0; index < sizeof(host->mappings) / sizeof(host->mappings[0]); ++index)
        if (host->mappings[index].live && host->mappings[index].handle != except) return &host->mappings[index];
    return NULL;
}

static uint32_t generation_live_count(const struct generation_host *host) {
    uint32_t count = 0;
    for (size_t index = 0; index < sizeof(host->mappings) / sizeof(host->mappings[0]); ++index)
        if (host->mappings[index].live) count++;
    return count;
}

static int generation_expect_coherence(struct generation_host *host, struct generation_mapping *mapping,
                                       uint32_t value) {
    struct stat metadata = {0};
    struct stat found = {0};
    int result = 0;
    metadata.st_ino = value;
    hl_fdcache_metadata_store("/root/generation", -2, &metadata);
    HL_CHECK(hl_fdcache_metadata_lookup("/root/generation", &result, &found) == 1);
    atomic_store(mapping->address, value);
    hl_fdcache_generation_poll();
    HL_CHECK(hl_fdcache_metadata_lookup("/root/generation", &result, &found) == 0);
    (void)host;
    return EXIT_SUCCESS;
}

int main(void) {
    struct generation_host generation = {0};
    hl_fake_host *fake = &generation.fake;
    hl_host_services host;
    hl_host_file_services files;
    hl_host_memory_services memory;
    char root[] = "/root";
    size_t root_length = strlen(root);
    struct hl_linux_vfs_lower lowers[1] = {{"/lower", 6}};
    int lower_count = 1;
    int volume_count = 0;
    int threaded = 1;
    char fd_paths[4][HL_FDCACHE_PATH_CAPACITY] = {{0}};
    struct hl_linux_vfs_namespace vfs = {root, &root_length, lowers, &lower_count};
    hl_fdcache_binding binding = {
        &host, &vfs, &volume_count, root, &root_length, fd_paths, 4, &threaded, NULL,
    };
    struct stat stored = {0};
    struct stat found = {0};
    int result = 0;
    char path[64];

    hl_fake_host_init(fake, &host);
    HL_CHECK(hl_fdcache_bind(&binding) == 0);

    stored.st_ino = 42;
    hl_fdcache_metadata_store("/root/file", 0, &stored);
    HL_CHECK(hl_fdcache_metadata_lookup("/root/file", &result, &found) == 1);
    HL_CHECK(result == 0 && found.st_ino == 42);
    hl_fdcache_resolution_bump();
    HL_CHECK(hl_fdcache_metadata_lookup("/root/file", &result, &found) == 0);

    hl_fdcache_metadata_store("/root/missing", -2, &stored);
    HL_CHECK(hl_fdcache_metadata_lookup("/root/missing", &result, &found) == 1 && result == -2);
    hl_fdcache_resolution_bump();
    HL_CHECK(hl_fdcache_metadata_lookup("/root/missing", &result, &found) == 0);

    hl_fdcache_resolution_store("/guest", "/root/guest");
    HL_CHECK(hl_fdcache_resolution_lookup("/guest", path, sizeof path) == 1);
    HL_CHECK(strcmp(path, "/root/guest") == 0);
    hl_fdcache_reset();
    HL_CHECK(hl_fdcache_resolution_lookup("/guest", path, sizeof path) == 0);

    HL_CHECK(hl_fdcache_dentry_cacheable(root) == 1);
    HL_CHECK(hl_fdcache_dentry_cacheable(lowers[0].canon) == 1);
    HL_CHECK(hl_fdcache_dentry_cacheable("/other") == 0);

    hl_fdcache_fd_setpath(2, "/root/file");
    HL_CHECK(strcmp(fd_paths[2], "/root/file") == 0);
    hl_fdcache_fd_clear(2);
    HL_CHECK(fd_paths[2][0] == 0);

    files = *host.file;
    memory = *host.memory;
    files.open_relative = generation_open;
    files.close = generation_close;
    memory.map_file = generation_map;
    memory.release = generation_release;
    host.file = &files;
    host.memory = &memory;
    binding.generation_file = "/run/fsgen";

    /* A valid mapping is adopted only after the file is closed, and its initial value causes no flush. */
    HL_CHECK(hl_fdcache_bind(&binding) == 0);
    HL_CHECK(generation.open_calls == 1 && generation.map_calls == 1 && generation.close_calls == 1);
    struct generation_mapping *current = generation_live(&generation, HL_HOST_HANDLE_INVALID);
    HL_CHECK(current != NULL && generation.release_calls == 0 && generation_live_count(&generation) == 1);
    hl_fdcache_metadata_store("/root/steady", 0, &stored);
    hl_fdcache_generation_poll();
    HL_CHECK(hl_fdcache_metadata_lookup("/root/steady", &result, &found) == 1);
    HL_CHECK(generation_expect_coherence(&generation, current, 8) == EXIT_SUCCESS);

    /* Every preparation failure leaves the active mapping authoritative. */
    const enum generation_fault faults[] = {GENERATION_OPEN_FAIL,       GENERATION_MAP_FAIL,
                                            GENERATION_MAP_FAIL_OWNED,  GENERATION_MAP_NO_HANDLE,
                                            GENERATION_MAP_NO_ADDRESS, GENERATION_MAP_SHORT,
                                            GENERATION_CLOSE_FAIL};
    for (size_t index = 0; index < sizeof(faults) / sizeof(faults[0]); ++index) {
        uint32_t releases = generation.release_calls;
        generation.fault = faults[index];
        HL_CHECK(hl_fdcache_bind(&binding) == 0);
        if (faults[index] == GENERATION_MAP_FAIL_OWNED || faults[index] == GENERATION_MAP_NO_ADDRESS ||
            faults[index] == GENERATION_MAP_SHORT || faults[index] == GENERATION_CLOSE_FAIL)
            HL_CHECK(generation.release_calls == releases + 1);
        else
            HL_CHECK(generation.release_calls == releases);
        HL_CHECK(generation_live_count(&generation) == 1);
        HL_CHECK(generation_expect_coherence(&generation, current, 9 + (uint32_t)index) == EXIT_SUCCESS);
    }
    generation.fault = GENERATION_OK;

    /* Replacement releases the old mapping exactly once and switches coherence to the new page. */
    uint32_t releases = generation.release_calls;
    hl_host_handle old_handle = current->handle;
    HL_CHECK(hl_fdcache_bind(&binding) == 0);
    HL_CHECK(generation.release_calls == releases + 1 && !current->live && generation_live_count(&generation) == 1);
    current = generation_live(&generation, old_handle);
    HL_CHECK(current != NULL);
    HL_CHECK(generation_expect_coherence(&generation, current, 32) == EXIT_SUCCESS);

    /* If retiring the old page fails, the prepared replacement is rolled back and the old page remains live. */
    releases = generation.release_calls;
    generation.fail_release = current->handle;
    HL_CHECK(hl_fdcache_bind(&binding) == 0);
    HL_CHECK(generation.release_calls == releases + 2 && current->live);
    HL_CHECK(generation_live(&generation, current->handle) == NULL && generation_live_count(&generation) == 1);
    HL_CHECK(generation_expect_coherence(&generation, current, 33) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
