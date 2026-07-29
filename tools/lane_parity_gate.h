#ifndef HL_TOOLS_LANE_PARITY_GATE_H
#define HL_TOOLS_LANE_PARITY_GATE_H

#include <stdio.h>

int hl_lane_parity_check(const char *manifest_path, const char *host_os, const char *host_cpu, FILE *diagnostics);

#endif
