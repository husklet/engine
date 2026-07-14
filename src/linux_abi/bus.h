#ifndef HL_LINUX_ABI_BUS_H
#define HL_LINUX_ABI_BUS_H

#include <stdint.h>

uint64_t hl_linux_bus_generation(void);
int hl_linux_bus_active(void);
int hl_linux_bus_hit(uint64_t address, uint64_t size);
uint64_t hl_linux_bus_fault(uint64_t address, uint64_t size);
typedef void (*hl_linux_bus_change_fn)(void *opaque, uint64_t generation, int active);
void hl_linux_bus_set_change_callback(hl_linux_bus_change_fn callback, void *opaque);

#endif
