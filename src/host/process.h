#ifndef HL_HOST_PROCESS_H
#define HL_HOST_PROCESS_H

#include <sys/types.h>

// Return a close-on-exec descriptor that becomes persistently readable when pid exits.
// The caller owns the descriptor. Returns -1 with errno set when the host cannot watch pid.
int hl_host_process_open(pid_t pid);

#endif
