#ifndef HL_TRANSLATOR_GUEST_X86_64_X87STATE_H
#define HL_TRANSLATOR_GUEST_X86_64_X87STATE_H

#include <stdint.h>

double hl_x86_ext80_load(const uint8_t image[10]);
void hl_x86_ext80_store(double value, uint8_t image[10]);

#endif
