#ifndef HL_HOST_RANGE_H
#define HL_HOST_RANGE_H

#include <stddef.h>
#include <stdint.h>

enum {
    HL_HOST_REGION_READ = 1u << 0,
    HL_HOST_REGION_WRITE = 1u << 1,
    HL_HOST_REGION_EXECUTE = 1u << 2,
};

typedef struct hl_host_region {
    uintptr_t address;
    size_t size;
    uint32_t protection;
} hl_host_region;

/* Native host VM page size, or zero when the host cannot provide a valid value. */
size_t hl_host_page_size(void);

/* True when the host page containing address is part of the process address space. */
int hl_host_address_mapped(uintptr_t address);

/* True when every host page touched by [address, address + size) is mapped. */
int hl_host_range_mapped(uintptr_t address, size_t size);

/* True when the aligned host page immediately below or above page_address is mapped. */
int hl_host_page_neighbor_mapped(uintptr_t page_address);

/* First mapped region containing address or beginning above it. Returns zero when none exists. */
int hl_host_region_query(uintptr_t address, hl_host_region *region);

#endif
