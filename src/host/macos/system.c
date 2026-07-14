#include "../system.h"

#include <libproc.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/machine.h>
#include <mach/processor_info.h>
#include <mach/vm_map.h>
#include <mach/vm_statistics.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

static uint64_t hl_macos_boot_time(void) {
    struct timeval value = {0};
    size_t size = sizeof value;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &value, &size, NULL, 0) == 0 && value.tv_sec > 0) return (uint64_t)value.tv_sec;
    return (uint64_t)time(NULL);
}

static uint32_t hl_macos_online_cpus(void) {
    static const char *names[] = {"hw.activecpu", "hw.logicalcpu", "hw.ncpu"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; ++index) {
        int value = 0;
        size_t size = sizeof value;
        if (sysctlbyname(names[index], &value, &size, NULL, 0) == 0 && value > 0) return (uint32_t)value;
    }
    long value = sysconf(_SC_NPROCESSORS_ONLN);
    return value > 0 ? (uint32_t)value : 1;
}

int hl_host_system_read(hl_host_system_info *info, hl_host_cpu_ticks *cores, size_t core_capacity) {
    host_cpu_load_info_data_t aggregate;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    uint64_t memory_size = 0;
    size_t memory_size_size = sizeof memory_size;
    vm_size_t page_size = 4096;
    vm_statistics64_data_t memory;
    mach_msg_type_number_t memory_count = HOST_VM_INFO64_COUNT;
    processor_info_array_t values = NULL;
    mach_msg_type_number_t value_count = 0;
    natural_t cpu_count = 0;
    processor_cpu_load_info_t loads = NULL;
    if (info == NULL || (core_capacity != 0 && cores == NULL)) return 0;
    memset(info, 0, sizeof *info);
    info->boot_time_seconds = hl_macos_boot_time();
    info->online_cpus = hl_macos_online_cpus();
    info->reported_cores = info->online_cpus;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&aggregate, &count) == KERN_SUCCESS) {
        info->aggregate.user = (uint64_t)aggregate.cpu_ticks[CPU_STATE_USER];
        info->aggregate.nice = (uint64_t)aggregate.cpu_ticks[CPU_STATE_NICE];
        info->aggregate.system = (uint64_t)aggregate.cpu_ticks[CPU_STATE_SYSTEM];
        info->aggregate.idle = (uint64_t)aggregate.cpu_ticks[CPU_STATE_IDLE];
    }
    (void)sysctlbyname("hw.memsize", &memory_size, &memory_size_size, NULL, 0);
    info->memory_total = memory_size;
    (void)host_page_size(mach_host_self(), &page_size);
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info_t)&memory, &memory_count) == KERN_SUCCESS) {
        uint64_t free_bytes = ((uint64_t)memory.free_count + memory.speculative_count) * page_size;
        uint64_t cached_bytes = (uint64_t)memory.inactive_count * page_size;
        info->memory_free = free_bytes;
        info->memory_cached = cached_bytes;
        info->memory_available = free_bytes + cached_bytes;
    }
    if (core_capacity != 0 && host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpu_count, &values,
                                                  &value_count) == KERN_SUCCESS) {
        loads = (processor_cpu_load_info_t)values;
        info->reported_cores = (uint32_t)cpu_count;
        size_t limit = cpu_count < core_capacity ? cpu_count : core_capacity;
        for (size_t index = 0; index < limit; ++index) {
            cores[index].user = (uint64_t)loads[index].cpu_ticks[CPU_STATE_USER];
            cores[index].nice = (uint64_t)loads[index].cpu_ticks[CPU_STATE_NICE];
            cores[index].system = (uint64_t)loads[index].cpu_ticks[CPU_STATE_SYSTEM];
            cores[index].idle = (uint64_t)loads[index].cpu_ticks[CPU_STATE_IDLE];
        }
        vm_deallocate(mach_task_self(), (vm_address_t)values, value_count * sizeof(integer_t));
    }
    return 1;
}

int hl_host_process_read(int64_t pid, hl_host_process_info *info) {
    struct proc_bsdinfo bsd;
    struct proc_taskinfo task;
    if (info == NULL || pid <= 0 || pid > INT32_MAX ||
        proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof bsd) != (int)sizeof bsd)
        return 0;
    memset(info, 0, sizeof *info);
    info->parent_pid = bsd.pbi_ppid;
    info->process_group = bsd.pbi_pgid;
    info->session = getsid((pid_t)pid);
    info->start_time_seconds = bsd.pbi_start_tvsec;
    info->threads = 1;
    switch (bsd.pbi_status) {
    case 2: info->state = 'R'; break;
    case 4: info->state = 'T'; break;
    case 5: info->state = 'Z'; break;
    default: info->state = 'S'; break;
    }
    snprintf(info->name, sizeof info->name, "%s", bsd.pbi_comm);
    if (proc_pidinfo((int)pid, PROC_PIDTASKINFO, 0, &task, sizeof task) == (int)sizeof task) {
        info->resident_bytes = task.pti_resident_size;
        info->virtual_bytes = task.pti_virtual_size;
        info->user_time_ns = task.pti_total_user;
        info->system_time_ns = task.pti_total_system;
        if (task.pti_threadnum > 0) info->threads = (uint32_t)task.pti_threadnum;
    }
    return 1;
}
