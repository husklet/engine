#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/host/system.h"

#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t cpu_total(const hl_host_cpu_ticks *ticks) {
    return ticks->user + ticks->nice + ticks->system + ticks->idle;
}

int main(void) {
    hl_host_system_info first;
    hl_host_system_info second;
    hl_host_cpu_ticks cores[256];
    hl_host_process_info process;
    memset(cores, 0, sizeof cores);
    HL_CHECK(hl_host_system_read(&first, cores, sizeof cores / sizeof cores[0]));
    HL_CHECK(first.boot_time_seconds > 0 && first.boot_time_seconds <= (uint64_t)time(NULL));
    HL_CHECK(first.online_cpus > 0 && first.reported_cores > 0);
    HL_CHECK(first.memory_total > 0 && first.memory_available <= first.memory_total);
    HL_CHECK(cpu_total(&first.aggregate) > 0);
    HL_CHECK(cpu_total(&cores[0]) > 0);
    HL_CHECK(!hl_host_system_read(NULL, cores, 1));
    HL_CHECK(!hl_host_system_read(&second, NULL, 1));
    HL_CHECK(hl_host_system_read(&second, NULL, 0));
    HL_CHECK(cpu_total(&second.aggregate) >= cpu_total(&first.aggregate));

    HL_CHECK(hl_host_process_read((int64_t)getpid(), &process));
    HL_CHECK(process.parent_pid > 0 && process.process_group > 0 && process.session > 0);
    HL_CHECK(process.threads > 0 && process.resident_bytes > 0 && process.virtual_bytes >= process.resident_bytes);
    HL_CHECK(process.start_time_seconds >= first.boot_time_seconds &&
             process.start_time_seconds <= (uint64_t)time(NULL));
    HL_CHECK(process.state != '\0' && process.name[0] != '\0');
    HL_CHECK(!hl_host_process_read(-1, &process));
    HL_CHECK(!hl_host_process_read((int64_t)getpid(), NULL));
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        for (;;)
            pause();
    }
    HL_CHECK(hl_host_process_read(child, &process));
    HL_CHECK(process.parent_pid == getpid() && process.threads > 0);
    HL_CHECK(kill(child, SIGKILL) == 0);
    HL_CHECK(waitpid(child, NULL, 0) == child);
    HL_CHECK(!hl_host_process_read(child, &process));
    return 0;
}
