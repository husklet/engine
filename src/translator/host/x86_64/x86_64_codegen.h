#ifndef HL_X86_64_CODEGEN_H
#define HL_X86_64_CODEGEN_H

#include "hl/codegen.h"

hl_status hl_codegen_x86_64(const hl_ir_block *block, hl_code_buffer *output);
hl_status hl_codegen_x86_64_function(const hl_ir_function *function, hl_code_buffer *output);

#endif
