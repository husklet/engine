#ifndef HL_AARCH64_CODEGEN_H
#define HL_AARCH64_CODEGEN_H

#include "hl/codegen.h"

hl_status hl_codegen_aarch64(const hl_ir_block *block, hl_code_buffer *output);
hl_status hl_codegen_aarch64_function(const hl_ir_function *function, hl_code_buffer *output);

#endif
