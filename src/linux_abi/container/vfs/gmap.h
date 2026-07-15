#ifndef HL_LINUX_GMAP_H
#define HL_LINUX_GMAP_H

#include "../../limits.h"
#include "hl/host_services.h"

#include <stddef.h>
#include <stdint.h>

typedef struct hl_gmap_entry {
    uint64_t address;
    uint64_t length;
    uint64_t guest_length;
} hl_gmap_entry;

void hl_gmap_bind_limits(const hl_limit_table *limits);
void hl_gmap_bind_host(const hl_host_services *host);
size_t hl_gmap_count(void);
int hl_gmap_get(size_t index, hl_gmap_entry *entry);
void hl_gmap_add(uint64_t address, uint64_t length);
void hl_gmap_set_guest_length(uint64_t address, uint64_t guest_length);
void hl_gmap_remove(uint64_t address);
uint64_t hl_gmap_find_length(uint64_t address);
int hl_gmap_contains(uint64_t address, uint64_t length);
void hl_gmap_unmap_range(uint64_t start, uint64_t end);
void hl_gmap_reset(void);

/* Loader allocations have opaque host ownership independent of guest VMA splits. */
int hl_exec_mapping_add(uint64_t address, uint64_t length, hl_host_handle host_mapping);
void hl_exec_mapping_discard_range(uint64_t address, uint64_t length);
void hl_exec_mapping_reset(void);

void hl_gmap_lock_remove(uint64_t address, uint64_t length);
void hl_gmap_lock_add(uint64_t address, uint64_t length);
void hl_gmap_lock_reset(void);
int hl_gmap_lock_wire_current(void);
void hl_gmap_lock_unwire_all(void);
uint64_t hl_gmap_lock_region_bytes(uint64_t low, uint64_t high);
uint64_t hl_gmap_lock_total_bytes(void);
int hl_gmap_lock_limit_range(uint64_t address, uint64_t length);
int hl_gmap_lock_limit_all(void);
int hl_gmap_lock_future(void);
void hl_gmap_lock_all(int future);

/* Temporary call-site aliases while the surrounding unity roots are decomposed. */
#define mlk_del hl_gmap_lock_remove
#define mlk_add hl_gmap_lock_add
#define mlk_reset hl_gmap_lock_reset
#define mlk_wire_current hl_gmap_lock_wire_current
#define mlk_unwire_all hl_gmap_lock_unwire_all
#define mlk_region_locked hl_gmap_lock_region_bytes
#define mlk_total_locked hl_gmap_lock_total_bytes
#define mlk_rlimit_gate hl_gmap_lock_limit_range
#define mlk_rlimit_gate_all hl_gmap_lock_limit_all

#endif
