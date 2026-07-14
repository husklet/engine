#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/host/system.h"

#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
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
    char temporary[] = "/tmp/hl-system-XXXXXX";
    int file = mkstemp(temporary);
    int pipes[2];
    HL_CHECK(file >= 0 && pipe(pipes) == 0);
    hl_host_process_fd descriptor;
    char target[4096] = {0};
    size_t target_size = 0;
    HL_CHECK(hl_host_process_fd_read(getpid(), file, &descriptor, target, sizeof target - 1, &target_size));
    HL_CHECK(descriptor.descriptor == file && descriptor.kind == HL_HOST_FD_FILE && target_size > 0);
    target[target_size] = '\0';
    HL_CHECK(strstr(target, strrchr(temporary, '/') + 1) != NULL);
    HL_CHECK(hl_host_process_fd_read(getpid(), pipes[0], &descriptor, NULL, 0, &target_size));
    HL_CHECK(descriptor.kind == HL_HOST_FD_PIPE && target_size == 0);
    hl_host_process_fd_private_add(pipes[0]);
    size_t descriptor_count = 0;
    HL_CHECK(hl_host_process_fds(getpid(), NULL, 0, &descriptor_count) && descriptor_count >= 3);
    hl_host_process_fd one_descriptor;
    size_t observed_count = 0;
    HL_CHECK(hl_host_process_fds(getpid(), &one_descriptor, 1, &observed_count) && observed_count >= 1);
    hl_host_process_fd *descriptors = malloc(descriptor_count * sizeof *descriptors);
    HL_CHECK(descriptors != NULL);
    HL_CHECK(hl_host_process_fds(getpid(), descriptors, descriptor_count, &observed_count));
    size_t visible = observed_count < descriptor_count ? observed_count : descriptor_count;
    int saw_file = 0, saw_pipe = 0, saw_private = 0;
    for (size_t index = 0; index < visible; ++index) {
        if (descriptors[index].descriptor == file) saw_file = 1;
        if (descriptors[index].descriptor == pipes[0]) {
            saw_pipe = 1;
            saw_private = (descriptors[index].flags & HL_HOST_PROCESS_FD_ENGINE_PRIVATE) != 0;
        }
    }
    free(descriptors);
    HL_CHECK(saw_file && saw_pipe && saw_private);
    hl_host_process_fd_private_remove(pipes[0]);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        for (;;)
            pause();
    }
    HL_CHECK(hl_host_process_read(child, &process));
    HL_CHECK(process.parent_pid == getpid() && process.threads > 0);
    HL_CHECK(hl_host_process_fd_read(child, file, &descriptor, target, sizeof target, &target_size));
    HL_CHECK(descriptor.kind == HL_HOST_FD_FILE && target_size > 0);
    HL_CHECK(hl_host_process_fd_read(child, pipes[1], &descriptor, NULL, 0, &target_size));
    HL_CHECK(descriptor.kind == HL_HOST_FD_PIPE);
    {
        size_t peer_count = 0;
        int found = 0;
        HL_CHECK(hl_host_process_peers(NULL, 0, &peer_count) && peer_count > 0);
        hl_host_process_peer *peers = malloc(peer_count * sizeof *peers);
        HL_CHECK(peers != NULL && hl_host_process_peers(peers, peer_count, &peer_count));
        for (size_t index = 0; index < peer_count; ++index)
            if (peers[index].identity == child) {
                found = 1;
                break;
            }
        free(peers);
        HL_CHECK(found);
        HL_CHECK(!hl_host_process_interrupt((hl_host_process_peer){-1}));
    }
    HL_CHECK(kill(child, SIGKILL) == 0);
    HL_CHECK(waitpid(child, NULL, 0) == child);
    HL_CHECK(!hl_host_process_read(child, &process));
    HL_CHECK(!hl_host_process_fd_read(child, file, &descriptor, target, sizeof target, &target_size));
    close(file);
    close(pipes[0]);
    close(pipes[1]);
    unlink(temporary);
    return 0;
}
