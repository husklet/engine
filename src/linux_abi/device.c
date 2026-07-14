#include "device.h"

uint32_t hl_linux_device_major(uint64_t device) {
    return (uint32_t)(((device >> 8) & 0xfffu) | ((uint32_t)(device >> 32) & ~0xfffu));
}

uint32_t hl_linux_device_minor(uint64_t device) {
    return (uint32_t)((device & 0xffu) | ((uint32_t)(device >> 12) & ~0xffu));
}
