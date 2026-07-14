#ifndef HL_TRANSLATOR_IDENTITY_H
#define HL_TRANSLATOR_IDENTITY_H

#include <stdint.h>

uint64_t hl_identity_name(const char *name);
uint64_t hl_identity_mix(uint64_t program, uint64_t interpreter, uint64_t engine, uint64_t name);

#endif
