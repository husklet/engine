#ifndef HL_HOST_SYSTEM_H
#define HL_HOST_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct hl_host_cpu_ticks {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
} hl_host_cpu_ticks;

typedef struct hl_host_system_info {
    uint64_t boot_time_seconds;
    uint64_t memory_total;
    uint64_t memory_free;
    uint64_t memory_available;
    uint64_t memory_cached;
    uint32_t online_cpus;
    uint32_t reported_cores;
    hl_host_cpu_ticks aggregate;
} hl_host_system_info;

typedef struct hl_host_process_info {
    int64_t parent_pid;
    int64_t process_group;
    int64_t session;
    uint64_t resident_bytes;
    uint64_t virtual_bytes;
    uint64_t user_time_ns;
    uint64_t system_time_ns;
    uint64_t start_time_seconds;
    uint32_t threads;
    char state;
    char name[32];
} hl_host_process_info;

enum {
    HL_HOST_FD_OTHER = 0,
    HL_HOST_FD_FILE = 1,
    HL_HOST_FD_PIPE = 2,
    HL_HOST_FD_SOCKET = 3,
};

typedef struct hl_host_process_fd {
    int32_t descriptor;
    uint32_t kind;
} hl_host_process_fd;

/* Snapshot host-wide values and up to core_capacity per-core counters. */
int hl_host_system_read(hl_host_system_info *info, hl_host_cpu_ticks *cores, size_t core_capacity);

/* Snapshot one live native process. Returns zero when the pid is absent or inaccessible. */
int hl_host_process_read(int64_t pid, hl_host_process_info *info);

/* Enumerate a process descriptor table. count receives the total observed, including truncated entries. */
int hl_host_process_fds(int64_t pid, hl_host_process_fd *entries, size_t capacity, size_t *count);

/* Query one open descriptor and, for files, copy its native absolute path without a trailing NUL. */
int hl_host_process_fd_read(int64_t pid, int32_t descriptor, hl_host_process_fd *entry, char *path,
                            size_t path_capacity, size_t *path_size);

#endif
