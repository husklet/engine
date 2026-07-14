#ifndef HL_PRODUCTION_HOST_CLOCK_H
#define HL_PRODUCTION_HOST_CLOCK_H

#include "hl/host_services.h"

#include <time.h>

enum { HL_PRODUCTION_CLOCK_REALTIME = 1, HL_PRODUCTION_CLOCK_MONOTONIC = 2 };

int hl_production_clock_gettime(const hl_host_services *services, int clock_id, struct timespec *output);
int hl_production_clock_nanoseconds(const hl_host_services *services, int clock_id, uint64_t *output);

#endif
