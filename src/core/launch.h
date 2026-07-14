#ifndef HL_CORE_LAUNCH_H
#define HL_CORE_LAUNCH_H

#include <stdint.h>

int hl_run_config_file(const char *path);
typedef int (*hl_launch_runner)(const char *rootfs, uint32_t argc, char *const argv[]);
int hl_run_config_file_with(const char *path, hl_launch_runner runner);

#endif
