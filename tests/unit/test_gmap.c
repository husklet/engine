#include "test.h"

#include "../../src/linux_abi/container/vfs/gmap.h"

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct mapping_probe {
    void *address;
    size_t size;
    unsigned maps;
    unsigned releases;
    unsigned discards;
    int fail_map;
    int malformed;
    uint64_t requested;
    uint64_t requested_size;
    uint32_t protection;
    uint32_t flags;
} mapping_probe;

#define FORMER_GMAP_CAPACITY 8192u

static hl_host_result probe_release(void *context, hl_host_handle handle) {
    mapping_probe *probe = context;
    if (handle != 1 || munmap(probe->address, probe->size) != 0)
        return (hl_host_result){HL_STATUS_PLATFORM_FAILURE, 0, 0, 0};
    probe->releases++;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result probe_discard(void *context, hl_host_handle handle) {
    mapping_probe *probe = context;
    if (handle != 1) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    probe->discards++;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result probe_map(void *context, uint64_t requested_address, uint64_t size, uint32_t protection,
                                uint32_t flags, hl_host_memory_mapping *output) {
    mapping_probe *probe = context;
    probe->requested = requested_address;
    probe->requested_size = size;
    probe->protection = protection;
    probe->flags = flags;
    if (probe->fail_map) return (hl_host_result){HL_STATUS_OUT_OF_MEMORY, 0, 0, 0};
    if (probe->malformed == 1) {
        output->handle = HL_HOST_HANDLE_INVALID;
        output->address = UINT64_C(0x1000);
        output->mapped_size = size;
        return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
    }
    probe->address = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe->address == MAP_FAILED) return (hl_host_result){HL_STATUS_OUT_OF_MEMORY, 0, 0, 0};
    probe->size = (size_t)size;
    probe->maps++;
    output->handle = 1;
    output->address = probe->malformed == 2 ? 0 : (uint64_t)(uintptr_t)probe->address;
    output->mapped_size = probe->malformed == 3 ? size + 1 : size;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

int main(void) {
    hl_limit_table limits;
    hl_gmap_entry first;
    hl_gmap_entry second;

    hl_limit_table_init(&limits);
    hl_gmap_bind_limits(&limits);
    HL_CHECK(hl_gmap_count() == 0);

    /* The registry is not an advertised guest limit. It must grow rather than
     * silently forgetting live VMAs after the historical fixed capacity. */
    for (size_t index = 0; index <= FORMER_GMAP_CAPACITY; ++index)
        hl_gmap_add(UINT64_C(0x100000000) + index * UINT64_C(0x2000), UINT64_C(0x1000));
    HL_CHECK(hl_gmap_count() == FORMER_GMAP_CAPACITY + 1u);
    HL_CHECK(hl_gmap_contains(UINT64_C(0x100000000) + FORMER_GMAP_CAPACITY * UINT64_C(0x2000), 1));
    for (size_t index = 0; index <= FORMER_GMAP_CAPACITY; ++index)
        hl_gmap_remove(UINT64_C(0x100000000) + index * UINT64_C(0x2000));
    HL_CHECK(hl_gmap_count() == 0);

    hl_gmap_add(0, 0x1000);
    hl_gmap_add(UINT64_MAX, 0x1000);
    hl_gmap_add(0x10000, 0x5000);
    HL_CHECK(hl_gmap_count() == 1);
    HL_CHECK(hl_gmap_find_length(0x10000) == 0x5000);
    HL_CHECK(hl_gmap_contains(0x11000, 0x2000));
    HL_CHECK(!hl_gmap_contains(0x14000, 0x2000));

    hl_gmap_set_guest_length(0x10000, 0x4000);
    HL_CHECK(hl_gmap_get(0, &first));
    HL_CHECK(first.address == 0x10000 && first.length == 0x5000 && first.guest_length == 0x4000);

    hl_gmap_unmap_range(0x12000, 0x13000);
    HL_CHECK(hl_gmap_count() == 2);
    HL_CHECK(hl_gmap_get(0, &first));
    HL_CHECK(hl_gmap_get(1, &second));
    HL_CHECK(first.address == 0x10000 && first.length == 0x2000 && first.guest_length == 0x2000);
    HL_CHECK(second.address == 0x13000 && second.length == 0x2000 && second.guest_length == 0x2000);
    HL_CHECK(!hl_gmap_contains(0x12000, 1));

    hl_gmap_lock_add(0x10011, 100);
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x1000);
    HL_CHECK(hl_gmap_lock_region_bytes(0x10000, 0x11000) == 0x1000);
    hl_gmap_lock_add(0x11000, 0x2000);
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x3000);
    hl_gmap_lock_remove(0x11800, 0x800);
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x2000);

    HL_CHECK(hl_limit_table_set(&limits, 8, 0x2000, 0x2000) == 0);
    HL_CHECK(hl_gmap_lock_limit_range(0x10000, 0x1000) == 0);
    HL_CHECK(hl_gmap_lock_limit_range(0x14000, 0x1000) == -ENOMEM);
    HL_CHECK(hl_gmap_lock_limit_all() == -ENOMEM);
    HL_CHECK(hl_limit_table_set(&limits, 8, 0, 0) == 0);
    HL_CHECK(hl_gmap_lock_limit_range(0x10000, 1) == -EPERM);

    hl_gmap_lock_all(1);
    HL_CHECK(hl_gmap_lock_future());
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x4000);
    hl_gmap_lock_reset();
    HL_CHECK(!hl_gmap_lock_future() && hl_gmap_lock_total_bytes() == 0);

    hl_gmap_remove(0x10000);
    hl_gmap_remove(0x13000);
    HL_CHECK(hl_gmap_count() == 0);

    {
        void *mapping = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        HL_CHECK(mapping != MAP_FAILED);
        hl_gmap_add((uint64_t)(uintptr_t)mapping, 0x1000);
        HL_CHECK(hl_gmap_count() == 1);
        (void)hl_gmap_lock_wire_current();
        hl_gmap_lock_unwire_all();
        hl_gmap_reset();
        HL_CHECK(hl_gmap_count() == 0);
    }
    {
        long page_value = sysconf(_SC_PAGESIZE);
        uint64_t page = page_value > 0 ? (uint64_t)page_value : UINT64_C(4096);
        void *physical = mmap(NULL, (size_t)(page * 2), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64_t physical_address = (uint64_t)(uintptr_t)physical;
        uint64_t logical_address = physical_address + page / 2;
        uint64_t found_address = 0, found_length = 0;
        HL_CHECK(physical != MAP_FAILED);
        hl_gmap_add_physical(logical_address, page, physical_address, page * 2);
        HL_CHECK(hl_gmap_find_physical(logical_address, &found_address, &found_length));
        HL_CHECK(found_address == physical_address && found_length == page * 2);
        hl_gmap_reset();
        errno = 0;
        HL_CHECK(mprotect(physical, (size_t)(page * 2), PROT_READ) == -1 && errno == ENOMEM);
    }
    {
        long page_value = sysconf(_SC_PAGESIZE);
        size_t page = page_value > 0 ? (size_t)page_value : 4096u;
        mapping_probe probe = {0};
        hl_host_memory_services memory = {0};
        hl_host_services host = {0};
        probe.size = page * 3;
        probe.address = mmap(NULL, probe.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        HL_CHECK(probe.address != MAP_FAILED);
        memory.release = probe_release;
        memory.discard = probe_discard;
        host.context = &probe;
        host.memory = &memory;
        hl_gmap_bind_host(&host);
        HL_CHECK(hl_exec_mapping_add((uint64_t)(uintptr_t)probe.address, probe.size, 1) == 0);
        HL_CHECK(munmap((char *)probe.address + page, page) == 0);
        hl_exec_mapping_discard_range((uint64_t)(uintptr_t)((char *)probe.address + page), page);
        HL_CHECK(probe.discards == 1 && probe.releases == 0);
        unsigned char *replacement = mmap((char *)probe.address + page, page, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        HL_CHECK(replacement == (unsigned char *)probe.address + page);
        replacement[0] = 0x5a;
        hl_exec_mapping_reset();
        HL_CHECK(probe.releases == 0 && replacement[0] == 0x5a);
        errno = 0;
        HL_CHECK(mprotect(probe.address, page, PROT_READ) == -1 && errno == ENOMEM);
        HL_CHECK(mprotect((char *)probe.address + page * 2, page, PROT_READ) == -1 && errno == ENOMEM);
        HL_CHECK(munmap(replacement, page) == 0);

        probe.address = mmap(NULL, probe.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        HL_CHECK(probe.address != MAP_FAILED);
        HL_CHECK(hl_exec_mapping_add((uint64_t)(uintptr_t)probe.address, probe.size, 1) == 0);
        hl_exec_mapping_reset();
        HL_CHECK(probe.releases == 1);
        errno = 0;
        HL_CHECK(mprotect(probe.address, probe.size, PROT_READ) == -1 && errno == ENOMEM);
        hl_gmap_bind_host(NULL);
    }
    {
        long page_value = sysconf(_SC_PAGESIZE);
        uint64_t page = page_value > 0 ? (uint64_t)page_value : UINT64_C(4096);
        mapping_probe probe = {0};
        hl_host_memory_services memory = {
            .map_anonymous = probe_map, .release = probe_release, .discard = probe_discard};
        hl_host_services host = {.context = &probe, .memory = &memory};
        uint64_t address = 0;
        hl_gmap_bind_host(&host);
        uint64_t requested = UINT64_C(0x40000000);
        HL_CHECK(hl_gmap_map_anonymous(requested, page, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                       HL_HOST_MEMORY_PRIVATE, &address) == HL_STATUS_OK);
        HL_CHECK(address == (uint64_t)(uintptr_t)probe.address && probe.maps == 1 && hl_gmap_count() == 1);
        HL_CHECK(probe.requested == requested && probe.requested_size == page);
        HL_CHECK(probe.protection == (HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE));
        HL_CHECK(probe.flags == HL_HOST_MEMORY_PRIVATE);
        hl_gmap_reset();
        HL_CHECK(probe.releases == 1 && hl_gmap_count() == 0);
        probe.fail_map = 1;
        address = 0;
        HL_CHECK(hl_gmap_map_anonymous(0, page, HL_HOST_MEMORY_READ, HL_HOST_MEMORY_PRIVATE, &address) ==
                 HL_STATUS_OUT_OF_MEMORY);
        HL_CHECK(address == 0 && probe.maps == 1 && probe.releases == 1 && hl_gmap_count() == 0);
        probe.fail_map = 0;
        for (int malformed = 1; malformed <= 3; ++malformed) {
            probe.malformed = malformed;
            address = UINT64_C(0xfeedface);
            HL_CHECK(hl_gmap_map_anonymous(0, page, HL_HOST_MEMORY_READ, HL_HOST_MEMORY_PRIVATE, &address) ==
                     HL_STATUS_PLATFORM_FAILURE);
            HL_CHECK(address == UINT64_C(0xfeedface) && hl_gmap_count() == 0);
        }
        HL_CHECK(probe.releases == 3);
        hl_gmap_bind_host(NULL);
    }
    return EXIT_SUCCESS;
}
