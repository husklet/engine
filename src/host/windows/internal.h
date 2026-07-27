#ifndef HL_HOST_WINDOWS_INTERNAL_H
#define HL_HOST_WINDOWS_INTERNAL_H

#include "hl/windows.h"
#include "../sync.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* Reservation granularity, not page size. SYSTEM_INFO.dwAllocationGranularity is
 * 64 KiB on every shipping x86-64 Windows and the value is baked into the ABI of
 * the placeholder calls; the backend still reads it back at create time and
 * refuses to run if the machine disagrees. */
#define HL_WINDOWS_ALLOCATION_GRANULARITY UINT64_C(65536)
#define HL_WINDOWS_PAGE_SIZE UINT64_C(4096)
#define HL_WINDOWS_HANDLE_CAPACITY 4096u

/* hl_host_result.detail_domain. 1 is errno (the POSIX backends), 0 is "no
 * domain" (src/host/sync.c); 2 and 3 are reserved here for Win32 and NTSTATUS
 * so a reader can tell which numbering a detail came from. */
enum { HL_WINDOWS_DETAIL_WIN32 = 2u };

typedef enum hl_windows_handle_kind {
    HL_WINDOWS_HANDLE_NONE = 0,
    HL_WINDOWS_HANDLE_MAPPING = 1,
    HL_WINDOWS_HANDLE_FILE = 2,
    HL_WINDOWS_HANDLE_SOCKET = 3,
    HL_WINDOWS_HANDLE_POLLSET = 4,
    HL_WINDOWS_HANDLE_SHARED_MEMORY = 5,
    HL_WINDOWS_HANDLE_PROCESS = 6,
    HL_WINDOWS_HANDLE_COUNTER = 7,
    HL_WINDOWS_HANDLE_TRANSFER = 8,
    HL_WINDOWS_HANDLE_DIRECTORY = 9,
    HL_WINDOWS_HANDLE_WATCH = 10,
    HL_WINDOWS_HANDLE_STREAM = 11
} hl_windows_handle_kind;

/*
 * One record per NT allocation inside a mapping, kept in ascending offset order
 * and always 1:1 with a distinct AllocationBase. That correspondence is the
 * invariant the whole memory group rests on: every placeholder-replacing call
 * demands the *exact* bounds of one placeholder, so a region record is what
 * lets the backend name those bounds later. A range with no record is address
 * space the process no longer owns.
 */
typedef enum hl_windows_region_state {
    HL_WINDOWS_REGION_COMMIT = 1, /* private committed pages (VirtualAlloc2) */
    HL_WINDOWS_REGION_VIEW = 2    /* a section view (MapViewOfFile3) */
} hl_windows_region_state;

typedef struct hl_windows_region {
    uint64_t offset; /* from the mapping base */
    uint64_t size;
    uint64_t section_offset; /* byte offset into the section, VIEW only */
    uint32_t state;
    uint32_t protection; /* HL_HOST_MEMORY_* as requested, not the Win32 spelling */
} hl_windows_region;

typedef struct hl_windows_handle_entry {
    uint32_t generation;
    uint16_t kind;
    uint16_t reserved;
    HANDLE object;       /* file, socket, process, ... for non-mapping kinds */
    HANDLE section;      /* section backing this mapping's views, NULL if private */
    HANDLE section_file; /* file the section was created over, borrowed not owned */
    uint32_t section_owned;
    void *address;
    void *executable_address; /* second alias of a dual-alias code mapping */
    uint64_t size;
    hl_windows_region *regions;
    uint32_t region_count;
    uint32_t region_capacity;
} hl_windows_handle_entry;

/*
 * VirtualAlloc2, MapViewOfFile3 and UnmapViewOfFile2 are exported by
 * KernelBase.dll and *not* by kernel32.dll, and mingw-w64's import libraries
 * carry no thunk for any of them: taking their address links only against
 * -lmincore, which would force a link flag on every consumer of this archive.
 * They are resolved by name instead, once, at create time. Same story for
 * QueryUnbiasedInterruptTimePrecise.
 */
typedef struct hl_windows_kernelbase {
    PVOID(WINAPI *virtual_alloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
    PVOID(WINAPI *map_view_of_file3)
    (HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
    BOOL(WINAPI *unmap_view_of_file2)(HANDLE, PVOID, ULONG);
    VOID(WINAPI *query_unbiased_interrupt_time_precise)(PULONGLONG);
    BOOLEAN(WINAPI *query_unbiased_interrupt_time)(PULONGLONG);
} hl_windows_kernelbase;

struct hl_host_windows {
    SRWLOCK lock;
    hl_windows_kernelbase api;
    uint32_t destroying;
    hl_host_sync_registry *sync;
    hl_windows_handle_entry *handles;
    uint32_t handle_capacity;
};

/* --- shared plumbing (host.c) --------------------------------------------- */

hl_host_result hl_windows_result(hl_status status, uint64_t value, uint64_t detail);
hl_status hl_windows_status_from_error(DWORD error);
hl_host_result hl_windows_last_error_result(void);

void hl_windows_lock(hl_host_windows *host);
void hl_windows_unlock(hl_host_windows *host);

hl_host_handle hl_windows_encode_handle(uint32_t index, uint32_t generation);
hl_windows_handle_entry *hl_windows_lookup_locked(hl_host_windows *host, hl_host_handle handle,
                                                  hl_windows_handle_kind kind);
/* Reserves a slot for `kind` and returns its opaque handle in `value`. The
 * caller fills the payload under the host lock once the native object exists. */
hl_host_result hl_windows_allocate_handle(hl_host_windows *host, hl_windows_handle_kind kind);
void hl_windows_clear_entry_locked(hl_windows_handle_entry *entry);

/* --- groups --------------------------------------------------------------- */

extern const hl_host_memory_services hl_windows_memory_services;
extern const hl_host_clock_services hl_windows_clock_services;

/* Tear down every mapping a destroyed host still owns. */
void hl_windows_memory_destroy_entry(hl_host_windows *host, hl_windows_handle_entry *entry);

#endif
