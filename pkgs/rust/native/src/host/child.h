#ifndef HL_HOST_CHILD_H
#define HL_HOST_CHILD_H

#include <signal.h>

typedef struct hl_host_child_watch {
    int read_descriptor;
    int write_descriptor;
    int active;
    struct sigaction previous;
} hl_host_child_watch;

int hl_host_child_watch_init(hl_host_child_watch *watch);
int hl_host_child_watch_descriptor(const hl_host_child_watch *watch);
void hl_host_child_watch_notify(const hl_host_child_watch *watch);
void hl_host_child_watch_drain(const hl_host_child_watch *watch);
void hl_host_child_watch_close(hl_host_child_watch *watch);

#endif
