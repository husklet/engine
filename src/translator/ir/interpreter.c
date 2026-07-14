#include "hl/ir.h"

#include <stdlib.h>
#include <string.h>

typedef struct hl_ir_runtime_value {
    uint64_t bits;
    uint16_t type;
} hl_ir_runtime_value;

hl_status hl_ir_interpret(const hl_ir_block *block, hl_ir_execution *execution) {
    hl_ir_runtime_value *values;
    uint32_t i;
    hl_status status;
    hl_ir_exit *out_exit;
    if (execution == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (execution->abi != HL_IR_EXECUTION_ABI || execution->size < sizeof(*execution)) return HL_STATUS_ABI_MISMATCH;
    if (execution->memory_size != 0 && execution->memory == NULL) return HL_STATUS_INVALID_ARGUMENT;
    out_exit = &execution->exit;
    memset(out_exit, 0, sizeof(*out_exit));
    status = hl_ir_validate(block, NULL);
    if (status != HL_STATUS_OK) return status;
    values = calloc(block->next_value_id, sizeof(*values));
    if (values == NULL) return HL_STATUS_OUT_OF_MEMORY;
    for (i = 0; i < block->instruction_count; ++i) {
        const hl_ir_instruction *instruction = &block->instructions[i];
        uint64_t result = 0;
        switch (instruction->opcode) {
        case HL_IR_OP_CONSTANT: result = instruction->immediate; break;
        case HL_IR_OP_COPY: result = values[instruction->operands[0].id].bits; break;
        case HL_IR_OP_ADD:
            result = values[instruction->operands[0].id].bits + values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_SUB:
            result = values[instruction->operands[0].id].bits - values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_AND:
            result = values[instruction->operands[0].id].bits & values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_OR:
            result = values[instruction->operands[0].id].bits | values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_XOR:
            result = values[instruction->operands[0].id].bits ^ values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_LOAD:
        case HL_IR_OP_STORE: {
            uint64_t address = values[instruction->operands[0].id].bits;
            uint64_t effective = address + instruction->immediate;
            uint16_t value_type =
                instruction->opcode == HL_IR_OP_LOAD ? instruction->result.type : instruction->operands[1].type;
            uint64_t width = value_type == HL_IR_TYPE_I32 ? 4u : 8u;
            if (effective < address || effective > execution->memory_size ||
                width > execution->memory_size - effective) {
                out_exit->kind = HL_IR_EXIT_FAULT;
                out_exit->value = HL_IR_FAULT_MEMORY;
                out_exit->detail = effective;
                free(values);
                return HL_STATUS_OK;
            }
            if (instruction->opcode == HL_IR_OP_LOAD) {
                uint64_t loaded = 0;
                uint64_t byte;
                for (byte = 0; byte < width; ++byte)
                    loaded |= (uint64_t)execution->memory[effective + byte] << (byte * 8u);
                result = loaded;
            } else {
                uint64_t stored = values[instruction->operands[1].id].bits;
                uint64_t byte;
                for (byte = 0; byte < width; ++byte)
                    execution->memory[effective + byte] = (uint8_t)(stored >> (byte * 8u));
            }
            break;
        }
        case HL_IR_OP_SAFEPOINT: break;
        case HL_IR_OP_GUEST_RETURN:
            out_exit->kind = HL_IR_EXIT_RETURN;
            out_exit->value =
                instruction->operand_count == 0 ? instruction->immediate : values[instruction->operands[0].id].bits;
            free(values);
            return HL_STATUS_OK;
        case HL_IR_OP_SYSCALL_EXIT:
            out_exit->kind = HL_IR_EXIT_SYSCALL;
            out_exit->value =
                instruction->operand_count == 0 ? instruction->immediate : values[instruction->operands[0].id].bits;
            out_exit->detail = instruction->operand_count < 2 ? 0 : values[instruction->operands[1].id].bits;
            free(values);
            return HL_STATUS_OK;
        case HL_IR_OP_FAULT_EXIT:
            out_exit->kind = HL_IR_EXIT_FAULT;
            out_exit->value =
                instruction->operand_count == 0 ? instruction->immediate : values[instruction->operands[0].id].bits;
            out_exit->detail = instruction->operand_count < 2 ? 0 : values[instruction->operands[1].id].bits;
            free(values);
            return HL_STATUS_OK;
        default: free(values); return HL_STATUS_NOT_SUPPORTED;
        }
        if (instruction->result.type == HL_IR_TYPE_I32) result &= UINT32_MAX;
        if (instruction->result.id != 0) {
            values[instruction->result.id].bits = result;
            values[instruction->result.id].type = instruction->result.type;
        }
    }
    free(values);
    return HL_STATUS_CORRUPT;
}
