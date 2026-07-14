#include "x86_64_codegen.h"

#include <stdint.h>

/* rdi carries hl_ir_exit *; avoid rsp/rbp and callee-saved registers. */
static const uint8_t hl_x86_64_registers[] = {0, 1, 2, 6, 8, 9, 10, 11};

static uint8_t hl_x86_64_value_register(hl_ir_value value) {
    return hl_x86_64_registers[value.id - 1u];
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
    hl_status status = hl_x86_64_emit_rex(output, source, destination, wide);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, opcode);
    if (status != HL_STATUS_OK) return status;
    return hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0xc0) | (uint8_t)(source & 7u) << 3 | (destination & 7u)));
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

static hl_status hl_x86_64_store_register(hl_code_buffer *output, uint8_t source, uint8_t displacement) {
    hl_status status = hl_x86_64_emit_rex(output, source, 7, 1);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x89));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, (uint8_t)(UINT8_C(0x40) | (source & 7u) << 3 | 7u));
    if (status != HL_STATUS_OK) return status;
    return hl_x86_64_emit_byte(output, displacement);
}

static hl_status hl_x86_64_exit(hl_code_buffer *output, const hl_ir_instruction *instruction, uint32_t kind) {
    const uint8_t scratch = 11;
    hl_status status = hl_x86_64_emit_byte(output, UINT8_C(0xc7));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x07));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_u32(output, kind);
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0xc7));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x47));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x04));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_u32(output, 0);
    if (status != HL_STATUS_OK) return status;
    if (instruction->operand_count == 0) {
        status = hl_x86_64_emit_constant(output, scratch, instruction->immediate, 1);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_store_register(output, scratch, 8);
    } else {
        status = hl_x86_64_store_register(output, hl_x86_64_value_register(instruction->operands[0]), 8);
    }
    if (status != HL_STATUS_OK) return status;
    if (instruction->operand_count < 2) {
        status = hl_x86_64_emit_constant(output, scratch, 0, 1);
        if (status != HL_STATUS_OK) return status;
        status = hl_x86_64_store_register(output, scratch, 16);
    } else {
        status = hl_x86_64_store_register(output, hl_x86_64_value_register(instruction->operands[1]), 16);
    }
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0x31));
    if (status != HL_STATUS_OK) return status;
    status = hl_x86_64_emit_byte(output, UINT8_C(0xc0));
    if (status != HL_STATUS_OK) return status;
    return hl_x86_64_emit_byte(output, UINT8_C(0xc3));
}

static int hl_x86_64_type_supported(uint16_t type) {
    return type == HL_IR_TYPE_NONE || type == HL_IR_TYPE_I32 || type == HL_IR_TYPE_I64 ||
           type == HL_IR_TYPE_GUEST_ADDRESS;
}

hl_status hl_codegen_x86_64(const hl_ir_block *block, hl_code_buffer *output) {
    uint32_t i;
    for (i = 0; i < block->instruction_count; ++i) {
        const hl_ir_instruction *instruction = &block->instructions[i];
        uint8_t destination = 0;
        int wide = instruction->result.type != HL_IR_TYPE_I32;
        uint32_t operand;
        hl_status status;
        if (!hl_x86_64_type_supported(instruction->result.type)) return HL_STATUS_NOT_SUPPORTED;
        if (instruction->result.id > HL_ARRAY_COUNT(hl_x86_64_registers)) return HL_STATUS_RESOURCE_LIMIT;
        for (operand = 0; operand < instruction->operand_count; ++operand) {
            if (!hl_x86_64_type_supported(instruction->operands[operand].type)) return HL_STATUS_NOT_SUPPORTED;
            if (instruction->operands[operand].id > HL_ARRAY_COUNT(hl_x86_64_registers))
                return HL_STATUS_RESOURCE_LIMIT;
        }
        if (instruction->result.id != 0) destination = hl_x86_64_value_register(instruction->result);
        switch (instruction->opcode) {
        case HL_IR_OP_CONSTANT:
            status = hl_x86_64_emit_constant(output, destination, instruction->immediate, wide);
            break;
        case HL_IR_OP_COPY:
            status = hl_x86_64_emit_binary(output, UINT8_C(0x89), destination,
                                           hl_x86_64_value_register(instruction->operands[0]), wide);
            break;
        case HL_IR_OP_ADD:
        case HL_IR_OP_SUB:
        case HL_IR_OP_AND:
        case HL_IR_OP_OR:
        case HL_IR_OP_XOR: {
            static const uint8_t opcodes[] = {UINT8_C(0x01), UINT8_C(0x29), UINT8_C(0x21), UINT8_C(0x09),
                                              UINT8_C(0x31)};
            status = hl_x86_64_emit_binary(output, UINT8_C(0x89), destination,
                                           hl_x86_64_value_register(instruction->operands[0]), wide);
            if (status != HL_STATUS_OK) return status;
            status = hl_x86_64_emit_binary(output, opcodes[instruction->opcode - HL_IR_OP_ADD], destination,
                                           hl_x86_64_value_register(instruction->operands[1]), wide);
            break;
        }
        case HL_IR_OP_SAFEPOINT: status = HL_STATUS_OK; break;
        case HL_IR_OP_GUEST_RETURN: status = hl_x86_64_exit(output, instruction, HL_IR_EXIT_RETURN); break;
        case HL_IR_OP_SYSCALL_EXIT: status = hl_x86_64_exit(output, instruction, HL_IR_EXIT_SYSCALL); break;
        case HL_IR_OP_FAULT_EXIT: status = hl_x86_64_exit(output, instruction, HL_IR_EXIT_FAULT); break;
        default: return HL_STATUS_NOT_SUPPORTED;
        }
        if (status != HL_STATUS_OK) return status;
    }
    return HL_STATUS_OK;
}
