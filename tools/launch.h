#ifndef HL_TOOLS_LAUNCH_H
#define HL_TOOLS_LAUNCH_H

#include <stdlib.h>
#include <unistd.h>

// Remote launch children inherit only stdio. GNU make's jobserver descriptors and flags are host-local
// coordination state; leaking either through the bridge changes firewall admission and can hold pipes open.
static inline void hl_launch_hygiene(void) {
    long maximum;
    (void)unsetenv("MAKEFLAGS");
    (void)unsetenv("MFLAGS");
    maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 4096) maximum = 4096;
    for (int descriptor = 3; descriptor < maximum; ++descriptor) (void)close(descriptor);
}

#endif
