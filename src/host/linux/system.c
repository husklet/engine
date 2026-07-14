#define _GNU_SOURCE
#include "../system.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int hl_linux_cpu_line(const char *line, hl_host_cpu_ticks *ticks) {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long wait = 0;
    if (sscanf(line, "%*s %llu %llu %llu %llu %llu", &user, &nice, &system, &idle, &wait) < 4) return 0;
    ticks->user = user;
    ticks->nice = nice;
    ticks->system = system;
    ticks->idle = idle + wait;
    return 1;
}

static void hl_linux_memory(hl_host_system_info *info) {
    FILE *file = fopen("/proc/meminfo", "r");
    char line[256];
    if (file == NULL) return;
    while (fgets(line, sizeof line, file) != NULL) {
        unsigned long long kib = 0;
        if (sscanf(line, "MemTotal: %llu kB", &kib) == 1)
            info->memory_total = kib * 1024;
        else if (sscanf(line, "MemFree: %llu kB", &kib) == 1)
            info->memory_free = kib * 1024;
        else if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1)
            info->memory_available = kib * 1024;
        else if (sscanf(line, "Cached: %llu kB", &kib) == 1)
            info->memory_cached = kib * 1024;
    }
    fclose(file);
}

int hl_host_system_read(hl_host_system_info *info, hl_host_cpu_ticks *cores, size_t core_capacity) {
    FILE *file;
    char line[512];
    uint32_t core_count = 0;
    if (info == NULL || (core_capacity != 0 && cores == NULL) || (file = fopen("/proc/stat", "r")) == NULL) return 0;
    memset(info, 0, sizeof *info);
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, "cpu ", 4) == 0) {
            (void)hl_linux_cpu_line(line, &info->aggregate);
        } else if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
            if (core_count < core_capacity) (void)hl_linux_cpu_line(line, &cores[core_count]);
            core_count++;
        } else {
            unsigned long long boot;
            if (sscanf(line, "btime %llu", &boot) == 1) info->boot_time_seconds = boot;
        }
    }
    fclose(file);
    info->reported_cores = core_count;
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    info->online_cpus = online > 0 ? (uint32_t)online : (core_count != 0 ? core_count : 1);
    if (info->boot_time_seconds == 0) info->boot_time_seconds = (uint64_t)time(NULL);
    hl_linux_memory(info);
    return 1;
}

int hl_host_process_read(int64_t pid, hl_host_process_info *info) {
    char path[64];
    char line[8192];
    FILE *file;
    char *left;
    char *right;
    char *cursor;
    char *save = NULL;
    uint64_t fields[25] = {0};
    long ticks;
    long page_size;
    hl_host_system_info system;
    if (info == NULL || pid <= 0) return 0;
    snprintf(path, sizeof path, "/proc/%lld/stat", (long long)pid);
    if ((file = fopen(path, "r")) == NULL) return 0;
    if (fgets(line, sizeof line, file) == NULL) {
        fclose(file);
        return 0;
    }
    fclose(file);
    left = strchr(line, '(');
    right = strrchr(line, ')');
    if (left == NULL || right == NULL || right <= left || right[1] != ' ' || right[2] == '\0') return 0;
    memset(info, 0, sizeof *info);
    size_t name_size = (size_t)(right - left - 1);
    if (name_size >= sizeof info->name) name_size = sizeof info->name - 1;
    memcpy(info->name, left + 1, name_size);
    info->name[name_size] = '\0';
    info->state = right[2];
    cursor = right + 3;
    for (size_t field = 4; field <= 24; ++field) {
        char *token = strtok_r(field == 4 ? cursor : NULL, " ", &save);
        char *end = NULL;
        if (token == NULL) return 0;
        errno = 0;
        fields[field] = strtoull(token, &end, 10);
        if (errno != 0 || end == token) return 0;
    }
    info->parent_pid = (int64_t)fields[4];
    info->process_group = (int64_t)fields[5];
    info->session = (int64_t)fields[6];
    info->threads = fields[20] > UINT32_MAX ? UINT32_MAX : (uint32_t)fields[20];
    ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) ticks = 100;
    info->user_time_ns = fields[14] * UINT64_C(1000000000) / (uint64_t)ticks;
    info->system_time_ns = fields[15] * UINT64_C(1000000000) / (uint64_t)ticks;
    if (hl_host_system_read(&system, NULL, 0))
        info->start_time_seconds = system.boot_time_seconds + fields[22] / (uint64_t)ticks;
    info->virtual_bytes = fields[23];
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    info->resident_bytes = fields[24] * (uint64_t)page_size;
    return 1;
}
