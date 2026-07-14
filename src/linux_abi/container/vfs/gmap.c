// hl/linux_abi -- guest mapping and Linux memory-lock state.
#include "gmap.h"

#include <errno.h>
#include <sys/mman.h>

#define MLK_N 1024
#define HL_GUEST_RLIMIT_MEMLOCK 8

struct hl_gmap_lock_range {
    uint64_t low;
    uint64_t high;
};

struct hl_gmap_context {
    hl_gmap_entry mappings[HL_GMAP_CAPACITY];
    size_t mapping_count;
    struct hl_gmap_lock_range locks[MLK_N];
    size_t lock_count;
    int lock_all;
    int lock_future;
    const hl_limit_table *limits;
};

static struct hl_gmap_context g_gmap;

void hl_gmap_bind_limits(const hl_limit_table *limits) {
    g_gmap.limits = limits;
}

size_t hl_gmap_count(void) {
    return g_gmap.mapping_count;
}

int hl_gmap_get(size_t index, hl_gmap_entry *entry) {
    if (!entry || index >= g_gmap.mapping_count) return 0;
    *entry = g_gmap.mappings[index];
    return 1;
}

void hl_gmap_add(uint64_t address, uint64_t length) {
    hl_gmap_entry *entry;
    if (!address || address == UINT64_MAX || !length || g_gmap.mapping_count >= HL_GMAP_CAPACITY) return;
    entry = &g_gmap.mappings[g_gmap.mapping_count++];
    entry->address = address;
    entry->length = length;
    entry->guest_length = length;
}

void hl_gmap_set_guest_length(uint64_t address, uint64_t guest_length) {
    for (size_t index = 0; index < g_gmap.mapping_count; index++)
        if (g_gmap.mappings[index].address == address) {
            g_gmap.mappings[index].guest_length = guest_length;
            return;
        }
}

void hl_gmap_remove(uint64_t address) {
    for (size_t index = 0; index < g_gmap.mapping_count; index++)
        if (g_gmap.mappings[index].address == address) {
            g_gmap.mappings[index] = g_gmap.mappings[--g_gmap.mapping_count];
            return;
        }
}

uint64_t hl_gmap_find_length(uint64_t address) {
    for (size_t index = 0; index < g_gmap.mapping_count; index++)
        if (g_gmap.mappings[index].address == address) return g_gmap.mappings[index].length;
    return 0;
}

int hl_gmap_contains(uint64_t address, uint64_t length) {
    uint64_t end = address + length;
    if (end < address) return 0;
    for (size_t index = 0; index < g_gmap.mapping_count; index++) {
        const hl_gmap_entry *entry = &g_gmap.mappings[index];
        if (entry->address <= address && end <= entry->address + entry->length) return 1;
    }
    return 0;
}

void hl_gmap_unmap_range(uint64_t start, uint64_t end) {
    for (size_t index = 0; index < g_gmap.mapping_count;) {
        hl_gmap_entry *entry = &g_gmap.mappings[index];
        uint64_t base = entry->address;
        uint64_t mapped_end = base + entry->length;
        if (end <= base || start >= mapped_end) {
            index++;
            continue;
        }
        int keep_head = base < start;
        int keep_tail = end < mapped_end;
        if (!keep_head && !keep_tail) {
            *entry = g_gmap.mappings[--g_gmap.mapping_count];
            continue;
        }
        if (keep_head)
            entry->length = entry->guest_length = start - base;
        else
            entry->address = end, entry->length = entry->guest_length = mapped_end - end;
        if (keep_head && keep_tail) hl_gmap_add(end, mapped_end - end);
        index++;
    }
}

void hl_gmap_reset(void) {
    for (size_t index = 0; index < g_gmap.mapping_count; index++)
        munmap((void *)(uintptr_t)g_gmap.mappings[index].address, (size_t)g_gmap.mappings[index].length);
    g_gmap.mapping_count = 0;
}

void hl_gmap_lock_remove(uint64_t address, uint64_t length) {
    if (!length || !g_gmap.lock_count) return;
    uint64_t remove_low = address & ~UINT64_C(0xfff);
    uint64_t remove_high = (address + length + UINT64_C(0xfff)) & ~UINT64_C(0xfff);
    for (size_t index = 0; index < g_gmap.lock_count;) {
        uint64_t low = g_gmap.locks[index].low;
        uint64_t high = g_gmap.locks[index].high;
        if (remove_high <= low || remove_low >= high) {
            index++;
            continue;
        }
        int keep_head = low < remove_low;
        int keep_tail = remove_high < high;
        if (!keep_head && !keep_tail) {
            g_gmap.locks[index] = g_gmap.locks[--g_gmap.lock_count];
            continue;
        }
        if (keep_head)
            g_gmap.locks[index].high = remove_low;
        else
            g_gmap.locks[index].low = remove_high;
        if (keep_head && keep_tail && g_gmap.lock_count < MLK_N) {
            g_gmap.locks[g_gmap.lock_count].low = remove_high;
            g_gmap.locks[g_gmap.lock_count].high = high;
            g_gmap.lock_count++;
        }
        index++;
    }
}

void hl_gmap_lock_add(uint64_t address, uint64_t length) {
    if (!length) return;
    uint64_t low = address & ~UINT64_C(0xfff);
    uint64_t high = (address + length + UINT64_C(0xfff)) & ~UINT64_C(0xfff);
    if (high <= low) return;
    hl_gmap_lock_remove(low, high - low);
    if (g_gmap.lock_count >= MLK_N) return;
    g_gmap.locks[g_gmap.lock_count].low = low;
    g_gmap.locks[g_gmap.lock_count].high = high;
    g_gmap.lock_count++;
}

void hl_gmap_lock_reset(void) {
    g_gmap.lock_count = 0;
    g_gmap.lock_all = 0;
    g_gmap.lock_future = 0;
}

int hl_gmap_lock_wire_current(void) {
    int failed = 0;
    for (size_t index = 0; index < g_gmap.mapping_count; index++) {
        hl_gmap_entry *entry = &g_gmap.mappings[index];
        if (!entry->address || !entry->length) continue;
        if (mlock((void *)(uintptr_t)entry->address, (size_t)entry->length) != 0) failed++;
    }
    return failed;
}

void hl_gmap_lock_unwire_all(void) {
    for (size_t index = 0; index < g_gmap.mapping_count; index++) {
        hl_gmap_entry *entry = &g_gmap.mappings[index];
        if (!entry->address || !entry->length) continue;
        munlock((void *)(uintptr_t)entry->address, (size_t)entry->length);
    }
}

uint64_t hl_gmap_lock_region_bytes(uint64_t low, uint64_t high) {
    if (high <= low) return 0;
    if (g_gmap.lock_all) return high - low;
    uint64_t sum = 0;
    for (size_t index = 0; index < g_gmap.lock_count; index++) {
        uint64_t start = g_gmap.locks[index].low > low ? g_gmap.locks[index].low : low;
        uint64_t end = g_gmap.locks[index].high < high ? g_gmap.locks[index].high : high;
        if (end > start) sum += end - start;
    }
    return sum;
}

uint64_t hl_gmap_lock_total_bytes(void) {
    uint64_t sum = 0;
    if (g_gmap.lock_all) {
        for (size_t index = 0; index < g_gmap.mapping_count; index++)
            sum += g_gmap.mappings[index].guest_length;
        return sum;
    }
    for (size_t index = 0; index < g_gmap.lock_count; index++)
        sum += g_gmap.locks[index].high - g_gmap.locks[index].low;
    return sum;
}

static uint64_t hl_gmap_memlock_limit(void) {
    uint64_t current = UINT64_MAX;
    if (g_gmap.limits) hl_limit_table_get(g_gmap.limits, HL_GUEST_RLIMIT_MEMLOCK, &current, NULL);
    return current;
}

static uint64_t hl_gmap_locked_bytes(void) {
    uint64_t sum = 0;
    for (size_t index = 0; index < g_gmap.lock_count; index++)
        sum += g_gmap.locks[index].high - g_gmap.locks[index].low;
    return sum;
}

static uint64_t hl_gmap_uncounted_bytes(uint64_t address, uint64_t length) {
    if (!length) return 0;
    uint64_t low = address & ~UINT64_C(0xfff);
    uint64_t high = (address + length + UINT64_C(0xfff)) & ~UINT64_C(0xfff);
    if (high <= low) return 0;
    uint64_t already = 0;
    for (size_t index = 0; index < g_gmap.lock_count; index++) {
        uint64_t start = g_gmap.locks[index].low > low ? g_gmap.locks[index].low : low;
        uint64_t end = g_gmap.locks[index].high < high ? g_gmap.locks[index].high : high;
        if (end > start) already += end - start;
    }
    return (high - low) - already;
}

int hl_gmap_lock_limit_range(uint64_t address, uint64_t length) {
    uint64_t limit = hl_gmap_memlock_limit();
    if (limit == UINT64_MAX) return 0;
    if (!limit) return -EPERM;
    if (!length) return 0;
    if (hl_gmap_locked_bytes() + hl_gmap_uncounted_bytes(address, length) > limit) return -ENOMEM;
    return 0;
}

int hl_gmap_lock_limit_all(void) {
    uint64_t limit = hl_gmap_memlock_limit();
    if (limit == UINT64_MAX) return 0;
    if (!limit) return -EPERM;
    uint64_t total = 0;
    for (size_t index = 0; index < g_gmap.mapping_count; index++)
        total += g_gmap.mappings[index].guest_length;
    return total > limit ? -ENOMEM : 0;
}

int hl_gmap_lock_future(void) {
    return g_gmap.lock_future;
}

void hl_gmap_lock_all(int future) {
    if (future) g_gmap.lock_future = 1;
    g_gmap.lock_all = 1;
}
