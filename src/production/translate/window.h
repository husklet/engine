#ifndef HL_PRODUCTION_WINDOW_H
#define HL_PRODUCTION_WINDOW_H

#include <stdint.h>

int hl_window_contains(uint64_t extent, uint64_t offset, uint64_t width, uint64_t alignment);

#endif
