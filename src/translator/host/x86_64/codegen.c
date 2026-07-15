#include "x86_64_codegen.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* rdi carries hl_ir_exit *; avoid rsp/rbp and callee-saved registers. */
static const uint8_t hl_x86_64_registers[] = {0, 1, 2, 6, 8, 9, 10, 11};

static uint8_t hl_x86_64_value_register(hl_ir_value value) {
    return hl_x86_64_registers[value.id - 1u];
}

static uint32_t hl_x86_64_spill_offset(hl_ir_value value) {
    return (value.id - (uint32_t)HL_ARRAY_COUNT(hl_x86_64_registers) - 1u) * 8u;
}

static hl_status hl_x86_64_emit_byte(hl_code_buffer *output, uint8_t byte) {
    if (output->code_size >= output->capacity) return HL_STATUS_RESOURCE_LIMIT;
    output->data[output->code_size++] = byte;
    return HL_STATUS_OK;
}

static hl_status hl_x86_64_emit_rex(hl_code_buffer *output, uint8_t reg, uint8_t rm, int wide) {
    uint8_t rex = (uint8_t)(UINT8_C(0x40) | (wide ? 8u : 0u) | (reg >= 8 ? 4u : 0u) | (rm >= 8 ? 1u : 0u));
    return rex == UINT8_C(0x40) ? HL_STATUS_OK : hl_x86_64_emit_byte(output, rex);
}

static hl_status hl_x86_64_emit_binary(hl_code_buffer *output, uint8_t opcode, uint8_t destination, uint8_t source,
                                       int wide) {
    uint8_t modrm;
    hl_status status = hl_x86_64_emit_rex(output, source, destination, wide);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, opcode);
    if (status != HL_STATUS_OK) return status;
    modrm = (uint8_t)(UINT8_C(0xc0) | (uint8_t)((uint8_t)(source & UINT8_C(7)) << 3) |
                      (uint8_t)(destination & UINT8_C(7)));
    return hl_x86_64_emit_byte(output, modrm);
}

static hl_status hl_x86_64_emit_constant(hl_code_buffer *output, uint8_t destination, uint64_t value, int wide) {
    uint32_t byte;
    hl_status status;
    if (wide || destination >= 8) {
        status =
            hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0x40) | (wide ? 8u : 0u) | (destination >= 8 ? 1u : 0u)));
        if (status != HL_STATUS_OK) return status;
    }
    status = hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0xb8) | (destination & 7u)));
    if (status != HL_STATUS_OK) return status;
    for (byte = 0; byte < (wide ? 8u : 4u); ++byte) {
        status = hl_x86_64_emit_byte(output, (uint8_t)(value >> (byte * 8u)));
        if (status != HL_STATUS_OK) return status;
    }
    return HL_STATUS_OK;
}

static hl_status hl_x86_64_emit_u32(hl_code_buffer *output, uint32_t value) {
    uint32_t byte;
    hl_status status;
    for (byte = 0; byte < 4; ++byte) {
        status = hl_x86_64_emit_byte(output, (uint8_t)(value >> (byte * 8u)));
        if (status != HL_STATUS_OK) return status;
    }
    return HL_STATUS_OK;
}

static hl_status hl_x86_64_spill_access(hl_code_buffer *output, uint8_t opcode, uint8_t reg, uint32_t offset) {
    hl_status status = hl_x86_64_emit_rex(output, reg, 4, 1);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, opcode);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0x84) | (reg & 7u) << 3));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x24));
    if (status != HL_STATUS_OK) return status;
    return hl_x86_64_emit_u32(output, offset);
}

static hl_status hl_x86_64_load_value(hl_code_buffer *output, hl_ir_value value, uint8_t scratch,
                                      uint8_t *out_register) {
    if (value.id <= HL_ARRAY_COUNT(hl_x86_64_registers)) {
        *out_register = hl_x86_64_value_register(value);
        return HL_STATUS_OK;
    }
    *out_register = scratch;
    return hl_x86_64_spill_access(output, UINT8_C(0x8b), scratch, hl_x86_64_spill_offset(value));
}

static hl_status hl_x86_64_store_register(hl_code_buffer *output, uint8_t source, uint8_t displacement) {
    hl_status status = hl_x86_64_emit_rex(output, source, 7, 1);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x89));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0x40) | (source & 7u) << 3 | 7u));
    if (status != HL_STATUS_OK) return status;
    return hl_x86_64_emit_byte(output, displacement);
}

static hl_status hl_x86_64_emit_memory_fault(hl_code_buffer *output, uint32_t frame_size) {
    hl_status status;
    /* kind=fault, reserved=0, value=memory, detail=r12 */
    static const uint8_t prefix[] = {0xc7, 0x47, 24,   3,  0, 0, 0, 0xc7, 0x47, 28,   0,    0,  0,    0,
                                     0x48, 0xc7, 0x47, 32, 1, 0, 0, 0,    0x4c, 0x89, 0x67, 40, 0x31, 0xc0};
    size_t i;
    for (i = 0; i < sizeof(prefix); ++i) {
        status = hl_x86_64_emit_byte(output, prefix[i]);
        if (status != HL_STATUS_OK) return status;
    }
    if (frame_size != 0) {
        static const uint8_t restore[] = {0x48, 0x81, 0xc4};
        for (i = 0; i < sizeof(restore); ++i) {
            status = hl_x86_64_emit_byte(output, restore[i]);
            if (status != HL_STATUS_OK) return status;
        }
        status = hl_x86_64_emit_u32(output, frame_size);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, 0x41);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, 0x5d);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, 0x41);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, 0x5c);
        if (status != HL_STATUS_OK) return status;
    }
    return hl_x86_64_emit_byte(output, 0xc3);
}

static hl_status hl_x86_64_success_or_fault(hl_code_buffer *output, uint8_t condition, uint32_t frame_size) {
    size_t displacement_at;
    hl_status status = hl_x86_64_emit_byte(output, 0x0f);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, condition);
    if (status != HL_STATUS_OK) return status;
    displacement_at = output->code_size;
    status = hl_x86_64_emit_u32(output, 0);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_memory_fault(output, frame_size);
    if (status != HL_STATUS_OK) return status;
    {
        uint32_t displacement = (uint32_t)(output->code_size - displacement_at - 4u);
        memcpy(output->data + displacement_at, &displacement, sizeof(displacement));
    }
    return HL_STATUS_OK;
}

static hl_status hl_x86_64_exit(hl_code_buffer *output, const hl_ir_instruction *instruction, uint32_t kind,
                                uint32_t frame_size) {
    const uint8_t scratch = 11;
    uint8_t value_register;
    hl_status status = hl_x86_64_emit_byte(output, UINT8_C(0xc7));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x47));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(24));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_u32(output, kind);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0xc7));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x47));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(28));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_u32(output, 0);
    if (status != HL_STATUS_OK) return status;
    if (instruction->operand_count == 0) {
        status = hl_x86_64_emit_constant(output, scratch, instruction->immediate, 1);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_store_register(output, scratch, 32);
    } else {
        status = hl_x86_64_load_value(output, instruction->operands[0], 12, &value_register);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_store_register(output, value_register, 32);
    }
    if (status != HL_STATUS_OK) return status;
    if (instruction->operand_count < 2) {
        status = hl_x86_64_emit_constant(output, scratch, 0, 1);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_store_register(output, scratch, 40);
    } else {
        status = hl_x86_64_load_value(output, instruction->operands[1], 12, &value_register);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_store_register(output, value_register, 40);
    }
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x31));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0xc0));
    if (status != HL_STATUS_OK) return status;
    if (frame_size != 0) {
        status = hl_x86_64_emit_byte(output, UINT8_C(0x48));
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, UINT8_C(0x81));
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, UINT8_C(0xc4));
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_u32(output, frame_size);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, UINT8_C(0x41));
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, UINT8_C(0x5d));
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, UINT8_C(0x41));
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_emit_byte(output, UINT8_C(0x5c));
        if (status != HL_STATUS_OK) return status;
    }
    return hl_x86_64_emit_byte(output, UINT8_C(0xc3));
}

static int hl_x86_64_type_supported(uint16_t type) {
    return type == HL_IR_TYPE_NONE || type == HL_IR_TYPE_I32 || type == HL_IR_TYPE_I64 ||
           type == HL_IR_TYPE_GUEST_ADDRESS;
}

typedef struct hl_x86_64_fixup {
    size_t displacement;
    uint32_t label;
} hl_x86_64_fixup;

hl_status hl_codegen_x86_64_function(const hl_ir_function *function, hl_code_buffer *output) {
    uint32_t i, block_index;
    int has_memory = 0;
    uint32_t spill_count = function->next_value_id > HL_ARRAY_COUNT(hl_x86_64_registers) + 1u
                               ? function->next_value_id - (uint32_t)HL_ARRAY_COUNT(hl_x86_64_registers) - 1u
                               : 0;
    uint32_t frame_size = (spill_count * 8u + 15u) & ~15u;
    size_t *label_offsets = calloc(function->block_count, sizeof(*label_offsets));
    size_t fixup_capacity = (size_t)function->block_count * 2u + 1u;
    size_t fixup_count = 0;
    hl_x86_64_fixup *fixups = calloc(fixup_capacity, sizeof(*fixups));
    hl_status final_status = HL_STATUS_OK;
    if (label_offsets == NULL || fixups == NULL) {
        free(label_offsets);
        free(fixups);
        return HL_STATUS_OUT_OF_MEMORY;
    }
    for (block_index = 0; block_index < function->block_count; ++block_index)
        for (i = 0; i < function->blocks[block_index].instruction_count; ++i)
            if (function->blocks[block_index].instructions[i].opcode == HL_IR_OP_LOAD ||
                function->blocks[block_index].instructions[i].opcode == HL_IR_OP_STORE)
                has_memory = 1;
    if (has_memory) frame_size += 16;
    if (frame_size != 0) {
        static const uint8_t prologue[] = {UINT8_C(0x41), UINT8_C(0x54), UINT8_C(0x41), UINT8_C(0x55),
                                           UINT8_C(0x48), UINT8_C(0x81), UINT8_C(0xec)};
        size_t byte;
        hl_status prologue_status;
        for (byte = 0; byte < sizeof(prologue); ++byte) {
            prologue_status = hl_x86_64_emit_byte(output, prologue[byte]);
            if (prologue_status != HL_STATUS_OK) {
                final_status = prologue_status;
                goto done;
            }
        }
        prologue_status = hl_x86_64_emit_u32(output, frame_size);
        if (prologue_status != HL_STATUS_OK) {
            final_status = prologue_status;
            goto done;
        }
    }
    if (function->blocks[0].label != function->entry_label) {
        hl_status status = hl_x86_64_emit_byte(output, 0xe9);
        if (status != HL_STATUS_OK) {
            final_status = status;
            goto done;
        }
        fixups[fixup_count].displacement = output->code_size;
        fixups[fixup_count++].label = function->entry_label;
        status = hl_x86_64_emit_u32(output, 0);
        if (status != HL_STATUS_OK) {
            final_status = status;
            goto done;
        }
    }
    for (block_index = 0; block_index < function->block_count; ++block_index) {
        const hl_ir_block *block = &function->blocks[block_index];
        label_offsets[block_index] = output->code_size;
        for (i = 0; i < block->instruction_count; ++i) {
            const hl_ir_instruction *instruction = &block->instructions[i];
            uint8_t destination = 0;
            int wide = instruction->result.type != HL_IR_TYPE_I32;
            uint32_t operand;
            uint8_t operand_registers[HL_IR_MAX_OPERANDS];
            hl_status status;
            if (!hl_x86_64_type_supported(instruction->result.type) &&
                instruction->result.type != HL_IR_TYPE_CONDITION) {
                final_status = HL_STATUS_NOT_SUPPORTED;
                goto done;
            }
            for (operand = 0; operand < instruction->operand_count; ++operand) {
                if (!hl_x86_64_type_supported(instruction->operands[operand].type) &&
                    instruction->operands[operand].type != HL_IR_TYPE_CONDITION) {
                    final_status = HL_STATUS_NOT_SUPPORTED;
                    goto done;
                }
                status = hl_x86_64_load_value(output, instruction->operands[operand], (uint8_t)(12u + operand),
                                              &operand_registers[operand]);
                if (status != HL_STATUS_OK) return status;
            }
            if (instruction->result.id != 0)
                destination = instruction->result.id <= HL_ARRAY_COUNT(hl_x86_64_registers)
                                  ? hl_x86_64_value_register(instruction->result)
                                  : 12;
            switch (instruction->opcode) {
            case HL_IR_OP_CONSTANT:
                status = hl_x86_64_emit_constant(output, destination, instruction->immediate, wide);
                break;
            case HL_IR_OP_COPY:
                status = hl_x86_64_emit_binary(output, UINT8_C(0x89), destination, operand_registers[0], wide);
                break;
            case HL_IR_OP_ADD:
            case HL_IR_OP_SUB:
            case HL_IR_OP_AND:
            case HL_IR_OP_OR:
            case HL_IR_OP_XOR: {
                static const uint8_t opcodes[] = {UINT8_C(0x01), UINT8_C(0x29), UINT8_C(0x21), UINT8_C(0x09),
                                                  UINT8_C(0x31)};
                status = hl_x86_64_emit_binary(output, UINT8_C(0x89), destination, operand_registers[0], wide);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_binary(output, opcodes[instruction->opcode - HL_IR_OP_ADD], destination,
                                               operand_registers[1], wide);
                break;
            }
            case HL_IR_OP_COMPARE: {
                static const uint8_t condition_codes[] = {0,    0x94, 0x95, 0x9c, 0x9e, 0x9f,
                                                          0x9d, 0x92, 0x96, 0x97, 0x93};
                int operand_wide = instruction->operands[0].type != HL_IR_TYPE_I32;
                status = hl_x86_64_emit_binary(output, 0x39, operand_registers[0], operand_registers[1], operand_wide);
                if (status != HL_STATUS_OK) break;
                if (destination >= 4) {
                    status = hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0x40) | (destination >= 8 ? 1u : 0u)));
                    if (status != HL_STATUS_OK) break;
                }
                status = hl_x86_64_emit_byte(output, 0x0f);
                if (status != HL_STATUS_OK) break;
                status = hl_x86_64_emit_byte(output, condition_codes[instruction->flags]);
                if (status != HL_STATUS_OK) break;
                status = hl_x86_64_emit_byte(output, (uint8_t)(0xc0u | (destination & 7u)));
                if (status != HL_STATUS_OK) break;
                status = hl_x86_64_emit_rex(output, destination, destination, 1);
                if (status != HL_STATUS_OK) break;
                status = hl_x86_64_emit_byte(output, 0x0f);
                if (status != HL_STATUS_OK) break;
                status = hl_x86_64_emit_byte(output, 0xb6);
                if (status != HL_STATUS_OK) break;
                status = hl_x86_64_emit_byte(output, (uint8_t)(0xc0u | (destination & 7u) << 3 | (destination & 7u)));
                break;
            }
            case HL_IR_OP_BRANCH:
            case HL_IR_OP_BRANCH_CONDITIONAL: {
                uint32_t labels[2] = {instruction->true_label, instruction->false_label};
                uint32_t count = instruction->opcode == HL_IR_OP_BRANCH ? 1u : 2u;
                uint32_t branch;
                if (instruction->opcode == HL_IR_OP_BRANCH_CONDITIONAL) {
                    status = hl_x86_64_emit_binary(output, 0x85, operand_registers[0], operand_registers[0], 1);
                    if (status != HL_STATUS_OK) break;
                    status = hl_x86_64_emit_byte(output, 0x0f);
                    if (status != HL_STATUS_OK) break;
                    status = hl_x86_64_emit_byte(output, 0x85); /* jne true */
                    if (status != HL_STATUS_OK) break;
                } else {
                    status = hl_x86_64_emit_byte(output, 0xe9);
                    if (status != HL_STATUS_OK) break;
                }
                for (branch = 0; branch < count; ++branch) {
                    if (branch != 0) {
                        status = hl_x86_64_emit_byte(output, 0xe9);
                        if (status != HL_STATUS_OK) break;
                    }
                    fixups[fixup_count].displacement = output->code_size;
                    fixups[fixup_count++].label = labels[branch];
                    status = hl_x86_64_emit_u32(output, 0);
                    if (status != HL_STATUS_OK) break;
                }
                break;
            }
            case HL_IR_OP_SAFEPOINT: status = HL_STATUS_OK; break;
            case HL_IR_OP_LOAD:
            case HL_IR_OP_STORE: {
                uint64_t width =
                    (instruction->opcode == HL_IR_OP_LOAD ? instruction->result.type : instruction->operands[1].type) ==
                            HL_IR_TYPE_I32
                        ? 4u
                        : 8u;
                if (instruction->opcode == HL_IR_OP_STORE) {
                    status = hl_x86_64_spill_access(output, 0x89, operand_registers[1], frame_size - 8u);
                    if (status != HL_STATUS_OK) return status;
                }
                /* r13=original, r12=effective */
                status = hl_x86_64_emit_binary(output, 0x89, 13, operand_registers[0], 1);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_binary(output, 0x89, 12, 13, 1);
                if (status != HL_STATUS_OK) return status;
                if (instruction->immediate != 0) {
                    status = hl_x86_64_emit_constant(output, 11, instruction->immediate, 1);
                    if (status != HL_STATUS_OK) return status;
                    status = hl_x86_64_emit_binary(output, 0x01, 12, 11, 1);
                    if (status != HL_STATUS_OK) return status;
                    status = hl_x86_64_emit_binary(output, 0x39, 12, 13, 1);
                    if (status != HL_STATUS_OK) return status;
                    status = hl_x86_64_success_or_fault(output, 0x83, frame_size); /* jae */
                    if (status != HL_STATUS_OK) return status;
                }
                /* r13=size; require size>=width and effective<=size-width. */
                status = hl_x86_64_emit_rex(output, 13, 7, 1);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x8b);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x6f); /* r13,[rdi+16] */
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 16);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x49);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x83);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0xfd); /* cmp r13,imm8 */
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, (uint8_t)width);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_success_or_fault(output, 0x83, frame_size);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x49);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x83);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0xed); /* sub r13,imm8 */
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, (uint8_t)width);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_binary(output, 0x39, 12, 13, 1); /* cmp effective,size-width */
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_success_or_fault(output, 0x86, frame_size); /* jbe */
                if (status != HL_STATUS_OK) return status;
                /* r13=memory base, [r13+r12]. */
                status = hl_x86_64_emit_rex(output, 13, 7, 1);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x8b);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x6f);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 8);
                if (status != HL_STATUS_OK) return status;
                if (instruction->opcode == HL_IR_OP_STORE) {
                    status = hl_x86_64_spill_access(output, 0x8b, 11, frame_size - 8u);
                    if (status != HL_STATUS_OK) return status;
                }
                {
                    uint8_t data_register = instruction->opcode == HL_IR_OP_LOAD ? destination : 11;
                    status = hl_x86_64_emit_byte(
                        output, (uint8_t)(UINT8_C(0x43) | (width == 8 ? 8u : 0u) | (data_register >= 8 ? 4u : 0u)));
                }
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, instruction->opcode == HL_IR_OP_LOAD ? 0x8b : 0x89);
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(
                    output,
                    (uint8_t)(UINT8_C(0x44) | ((instruction->opcode == HL_IR_OP_LOAD ? destination : 11) & 7u) << 3));
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0x25); /* base=r13,index=r12 */
                if (status != HL_STATUS_OK) return status;
                status = hl_x86_64_emit_byte(output, 0); /* disp8 */
                break;
            }
            case HL_IR_OP_GUEST_RETURN:
                status = hl_x86_64_exit(output, instruction, HL_IR_EXIT_RETURN, frame_size);
                break;
            case HL_IR_OP_SYSCALL_EXIT:
                status = hl_x86_64_exit(output, instruction, HL_IR_EXIT_SYSCALL, frame_size);
                break;
            case HL_IR_OP_FAULT_EXIT: status = hl_x86_64_exit(output, instruction, HL_IR_EXIT_FAULT, frame_size); break;
            default: final_status = HL_STATUS_NOT_SUPPORTED; goto done;
            }
            if (status != HL_STATUS_OK) {
                final_status = status;
                goto done;
            }
            if (instruction->result.id > HL_ARRAY_COUNT(hl_x86_64_registers)) {
                status = hl_x86_64_spill_access(output, UINT8_C(0x89), destination,
                                                hl_x86_64_spill_offset(instruction->result));
                if (status != HL_STATUS_OK) {
                    final_status = status;
                    goto done;
                }
            }
        }
    }
    for (i = 0; i < fixup_count; ++i) {
        uint32_t target;
        int64_t displacement;
        for (target = 0; target < function->block_count; ++target)
            if (function->blocks[target].label == fixups[i].label) break;
        displacement = (int64_t)label_offsets[target] - (int64_t)(fixups[i].displacement + 4u);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            final_status = HL_STATUS_RESOURCE_LIMIT;
            goto done;
        }
        {
            int32_t encoded = (int32_t)displacement;
            memcpy(output->data + fixups[i].displacement, &encoded, 4);
        }
    }
done:
    free(fixups);
    free(label_offsets);
    return final_status;
}

hl_status hl_codegen_x86_64(const hl_ir_block *block, hl_code_buffer *output) {
    hl_ir_function function;
    hl_ir_block wrapper = *block;
    memset(&function, 0, sizeof(function));
    wrapper.label = 1;
    function.abi = HL_IR_ABI;
    function.size = sizeof(function);
    function.blocks = &wrapper;
    function.block_count = function.block_capacity = 1;
    function.next_value_id = block->next_value_id;
    function.entry_label = 1;
    return hl_codegen_x86_64_function(&function, output);
}
