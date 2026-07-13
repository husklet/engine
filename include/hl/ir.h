#ifndef HL_IR_H
#define HL_IR_H

#include "hl/base.h"

HL_EXTERN_C_BEGIN

#define HL_IR_ABI 1u
#define HL_IR_MAX_OPERANDS 4u

typedef enum hl_ir_type {
    HL_IR_TYPE_NONE = 0,
    HL_IR_TYPE_I32 = 1,
    HL_IR_TYPE_I64 = 2,
    HL_IR_TYPE_F32 = 3,
    HL_IR_TYPE_F64 = 4,
    HL_IR_TYPE_V128 = 5,
    HL_IR_TYPE_GUEST_ADDRESS = 6,
    HL_IR_TYPE_CONDITION = 7
} hl_ir_type;

typedef enum hl_ir_opcode {
    HL_IR_OP_INVALID = 0,
    HL_IR_OP_CONSTANT = 1,
    HL_IR_OP_COPY = 2,
    HL_IR_OP_ADD = 3,
    HL_IR_OP_SUB = 4,
    HL_IR_OP_AND = 5,
    HL_IR_OP_OR = 6,
    HL_IR_OP_XOR = 7,
    HL_IR_OP_LOAD = 8,
    HL_IR_OP_STORE = 9,
    HL_IR_OP_COMPARE = 10,
    HL_IR_OP_BRANCH = 11,
    HL_IR_OP_BRANCH_CONDITIONAL = 12,
    HL_IR_OP_GUEST_CALL = 13,
    HL_IR_OP_GUEST_RETURN = 14,
    HL_IR_OP_SYSCALL_EXIT = 15,
    HL_IR_OP_FAULT_EXIT = 16,
    HL_IR_OP_SAFEPOINT = 17,
    HL_IR_OP_COUNT = 18
} hl_ir_opcode;

typedef struct hl_ir_value {
    uint32_t id;
    uint16_t type;
    uint16_t reserved;
} hl_ir_value;

typedef struct hl_ir_instruction {
    uint16_t opcode;
    uint16_t flags;
    hl_ir_value result;
    uint8_t operand_count;
    uint8_t reserved[3];
    hl_ir_value operands[HL_IR_MAX_OPERANDS];
    uint64_t immediate;
} hl_ir_instruction;

typedef struct hl_ir_block {
    HL_ABI_HEADER;
    uint64_t guest_pc;
    hl_ir_instruction *instructions;
    uint32_t instruction_count;
    uint32_t instruction_capacity;
    uint32_t next_value_id;
    uint32_t terminated;
} hl_ir_block;

HL_API hl_status hl_ir_block_init(hl_ir_block *block, uint64_t guest_pc, hl_ir_instruction *storage, uint32_t capacity);
HL_API hl_status hl_ir_append(hl_ir_block *block, const hl_ir_instruction *instruction, hl_ir_value *result);
HL_API hl_status hl_ir_validate(const hl_ir_block *block, uint32_t *bad_instruction);
HL_API int hl_ir_opcode_is_terminator(uint16_t opcode);

HL_EXTERN_C_END

#endif
