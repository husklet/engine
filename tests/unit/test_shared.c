#include "test.h"

#include "../../src/linux_abi/shared.h"

#include <assert.h>
#include <string.h>
#include <sys/mman.h>

enum fault {
    FAULT_NONE,
    FAULT_MAP,
    FAULT_MAP_OWNED,
    FAULT_NO_HANDLE,
    FAULT_NO_ADDRESS,
    FAULT_SHORT,
    FAULT_DISCARD,
    FAULT_RELEASE,
};

typedef struct shared_host {
    enum fault fault;
    void *address;
    hl_host_handle handle;
    uint64_t size;
    uint32_t map_calls;
    uint32_t discard_calls;
    uint32_t release_calls;
    uint32_t expected_flags;
    int owned;
} shared_host;

static hl_host_result map_shared(void *opaque, uint64_t requested, uint64_t size, uint32_t protection, uint32_t flags,
                                 hl_host_memory_mapping *output) {
    shared_host *host = opaque;
    host->map_calls++;
    assert(requested == 0);
    assert(size == 4096);
    assert(protection == (HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE));
    assert(flags == host->expected_flags);
    assert(output != NULL && output->abi == HL_HOST_MEMORY_MAPPING_ABI && output->size >= sizeof(*output));
    if (host->fault == FAULT_MAP) return (hl_host_result){HL_STATUS_OUT_OF_MEMORY, 0, 0, 0};
    if (host->fault == FAULT_NO_HANDLE) return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
    host->address = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(host->address != MAP_FAILED);
    memset(host->address, 0xa5, (size_t)size);
    host->handle++;
    host->size = size;
    host->owned = 1;
    *output = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(*output), host->handle,
                                       (uint64_t)(uintptr_t)host->address, size, 0};
    if (host->fault == FAULT_MAP_OWNED) return (hl_host_result){HL_STATUS_OUT_OF_MEMORY, 0, 0, 0};
    if (host->fault == FAULT_NO_ADDRESS) output->address = 0;
    if (host->fault == FAULT_SHORT) output->mapped_size = size - 1;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result discard_shared(void *opaque, hl_host_handle handle) {
    shared_host *host = opaque;
    host->discard_calls++;
    assert(host->owned && handle == host->handle);
    if (host->fault == FAULT_DISCARD || host->fault == FAULT_RELEASE)
        return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    host->owned = 0;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result release_shared(void *opaque, hl_host_handle handle) {
    shared_host *host = opaque;
    host->release_calls++;
    assert(host->owned && handle == host->handle);
    if (host->fault == FAULT_RELEASE) return (hl_host_result){HL_STATUS_IO, 0, 0, 0};
    assert(munmap(host->address, (size_t)host->size) == 0);
    host->address = NULL;
    host->owned = 0;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static void force_cleanup(shared_host *host) {
    if (host->address != NULL) assert(munmap(host->address, (size_t)host->size) == 0);
    host->address = NULL;
    host->owned = 0;
}

static int run_fault(hl_host_services *services, shared_host *host, enum fault fault, uint32_t releases) {
    void *output = (void *)(uintptr_t)1;
    memset(host, 0, sizeof(*host));
    host->fault = fault;
    host->expected_flags = HL_HOST_MEMORY_SHARED;
    HL_CHECK(hl_linux_shared_create(services, 4096, &output) != HL_STATUS_OK);
    HL_CHECK(output == NULL && host->map_calls == 1 && host->discard_calls == (fault >= FAULT_DISCARD ? 1u : 0u));
    HL_CHECK(host->release_calls == releases);
    if (fault == FAULT_RELEASE) {
        HL_CHECK(host->owned);
        force_cleanup(host);
    } else {
        HL_CHECK(!host->owned);
    }
    return EXIT_SUCCESS;
}

int main(void) {
    shared_host host = {0};
    hl_host_memory_services memory = {.abi = HL_HOST_MEMORY_ABI,
                                      .size = sizeof(memory),
                                      .release = release_shared,
                                      .map_anonymous = map_shared,
                                      .discard = discard_shared};
    hl_host_services services = {.abi = HL_HOST_SERVICES_ABI,
                                 .size = sizeof(services),
                                 .capabilities = HL_HOST_CAP_MEMORY,
                                 .context = &host,
                                 .memory = &memory};
    void *output = NULL;

    host.expected_flags = HL_HOST_MEMORY_SHARED;
    HL_CHECK(hl_linux_shared_create(&services, 4096, &output) == HL_STATUS_OK);
    HL_CHECK(output == host.address && !host.owned && host.discard_calls == 1 && host.release_calls == 0);
    for (size_t index = 0; index < 4096; ++index) HL_CHECK(((unsigned char *)output)[index] == 0);
    ((unsigned char *)output)[0] = 42;
    HL_CHECK(((unsigned char *)host.address)[0] == 42);
    force_cleanup(&host);

    memset(&host, 0, sizeof(host));
    host.expected_flags = HL_HOST_MEMORY_PRIVATE;
    HL_CHECK(hl_linux_memory_create(&services, 4096, HL_HOST_MEMORY_PRIVATE, &output) == HL_STATUS_OK);
    HL_CHECK(output == host.address && !host.owned && host.discard_calls == 1);
    force_cleanup(&host);

    HL_CHECK(run_fault(&services, &host, FAULT_MAP, 0) == EXIT_SUCCESS);
    HL_CHECK(run_fault(&services, &host, FAULT_MAP_OWNED, 1) == EXIT_SUCCESS);
    HL_CHECK(run_fault(&services, &host, FAULT_NO_HANDLE, 0) == EXIT_SUCCESS);
    HL_CHECK(run_fault(&services, &host, FAULT_NO_ADDRESS, 1) == EXIT_SUCCESS);
    HL_CHECK(run_fault(&services, &host, FAULT_SHORT, 1) == EXIT_SUCCESS);
    HL_CHECK(run_fault(&services, &host, FAULT_DISCARD, 1) == EXIT_SUCCESS);
    HL_CHECK(run_fault(&services, &host, FAULT_RELEASE, 1) == EXIT_SUCCESS);

    output = (void *)(uintptr_t)1;
    HL_CHECK(hl_linux_shared_create(NULL, 4096, &output) == HL_STATUS_INVALID_ARGUMENT && output == NULL);
    HL_CHECK(hl_linux_shared_create(&services, 0, &output) == HL_STATUS_INVALID_ARGUMENT && output == NULL);
    HL_CHECK(hl_linux_shared_create(&services, 4096, NULL) == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(hl_linux_memory_create(&services, 4096, 0, &output) == HL_STATUS_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}
