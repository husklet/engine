#ifndef HL_HOST_RANGE_H
#define HL_HOST_RANGE_H

#include <stddef.h>
#include <stdint.h>

/* Native host VM page size, or zero when the host cannot provide a valid value. */
size_t hl_host_page_size(void);

/* True when the host page containing address is part of the process address space. */
int hl_host_address_mapped(uintptr_t address);

/* True when every host page touched by [address, address + size) is mapped. */
int hl_host_range_mapped(uintptr_t address, size_t size);

/* True when the aligned host page immediately below or above page_address is mapped. */
int hl_host_page_neighbor_mapped(uintptr_t page_address);

#endif
