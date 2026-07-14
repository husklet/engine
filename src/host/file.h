#ifndef HL_HOST_FILE_H
#define HL_HOST_FILE_H

#include "hl/host_services.h"

int hl_host_file_create(const hl_host_services *services, const char *path, uint32_t permissions);
int hl_host_file_exclusive(const hl_host_services *services, const char *path, uint32_t permissions);
int hl_host_file_reset(const hl_host_services *services, const char *path, uint32_t permissions);

#endif
