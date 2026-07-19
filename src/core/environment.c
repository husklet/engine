#define _POSIX_C_SOURCE 200809L

#include "environment.h"

#include <errno.h>
#include <stdlib.h>

const char *hl_environment_debug_log(void) { return getenv("HL_LOG"); }

int hl_environment_take_activation_descriptor(long *descriptor) {
    const char *value = getenv("HL_ACTIVATION_FD");
    char *end = NULL;
    long parsed;
    if (descriptor == NULL) return -1;
    if (value == NULL) return 0;
    errno = 0;
    parsed = strtol(value, &end, 10);
    (void)unsetenv("HL_ACTIVATION_FD");
    if (errno != 0 || end == value || *end != 0) return -1;
    *descriptor = parsed;
    return 1;
}
