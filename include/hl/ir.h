#ifndef HL_IR_H
#define HL_IR_H

#include "hl/base.h"

HL_EXTERN_C_BEGIN

#define HL_IR_ABI 3u
#define HL_IR_EXECUTION_ABI 1u
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

typedef enum hl_ir_condition {
    HL_IR_CONDITION_EQ = 1,
    HL_IR_CONDITION_NE = 2,
    HL_IR_CONDITION_SIGNED_LT = 3,
    HL_IR_CONDITION_SIGNED_LE = 4,
    HL_IR_CONDITION_SIGNED_GT = 5,
    HL_IR_CONDITION_SIGNED_GE = 6,
    HL_IR_CONDITION_UNSIGNED_LT = 7,
    HL_IR_CONDITION_UNSIGNED_LE = 8,
    HL_IR_CONDITION_UNSIGNED_GT = 9,
    HL_IR_CONDITION_UNSIGNED_GE = 10
} hl_ir_condition;

/*
 * Interpreter semantics currently cover CONSTANT, COPY, integer
 * ADD/SUB/AND/OR/XOR, LOAD/STORE, COMPARE, branches, SAFEPOINT, GUEST_RETURN,
 * SYSCALL_EXIT, and FAULT_EXIT; native functions lower the same operations.
 * A terminator's immediate is its
 * primary exit value unless operand zero supplies that value.  SYSCALL_EXIT
 * and FAULT_EXIT may additionally supply operand one as exit detail.
 * LOAD/STORE use execution.memory as a zero-based, bounded byte array. Their
 * width is derived from the loaded result or stored value type (I32=4,
 * I64/GUEST_ADDRESS=8); accesses are little-endian and may be unaligned. The
 * unsigned immediate is added to operand zero. Addition overflow or a range
 * beyond memory_size exits with HL_IR_FAULT_MEMORY and the wrapped effective
 * address in detail. A function owns labeled blocks and entry_label selects its
 * entry independently of block storage order. COMPARE produces a CONDITION
 * using hl_ir_condition, and branches name labels through true_label/false_label. Definitions must dominate every use;
 * instructions are re-executed whenever control revisits their block.
 * GUEST_CALL remains rejected until its call-frame contract exists.
 */

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
    uint32_t true_label;
    uint32_t false_label;
} hl_ir_instruction;

typedef struct hl_ir_block {
    HL_ABI_HEADER;
    uint64_t guest_pc;
    uint32_t label;
    uint32_t reserved;
    hl_ir_instruction *instructions;
    uint32_t instruction_count;
    uint32_t instruction_capacity;
    uint32_t next_value_id;
    uint32_t terminated;
} hl_ir_block;

typedef struct hl_ir_function {
    HL_ABI_HEADER;
    hl_ir_block *blocks;
    uint32_t block_count;
    uint32_t block_capacity;
    uint32_t next_value_id;
    uint32_t entry_label;
    uint32_t reserved;
} hl_ir_function;

typedef enum hl_ir_exit_kind {
    HL_IR_EXIT_NONE = 0,
    HL_IR_EXIT_RETURN = 1,
    HL_IR_EXIT_SYSCALL = 2,
    HL_IR_EXIT_FAULT = 3
} hl_ir_exit_kind;

typedef struct hl_ir_exit {
    uint32_t kind;
    uint32_t reserved;
    uint64_t value;
    uint64_t detail;
} hl_ir_exit;

typedef enum hl_ir_fault { HL_IR_FAULT_MEMORY = 1 } hl_ir_fault;

typedef struct hl_ir_execution {
    HL_ABI_HEADER;
    uint8_t *memory;
    uint64_t memory_size;
    hl_ir_exit exit;
} hl_ir_execution;

HL_API hl_status hl_ir_block_init(hl_ir_block *block, uint64_t guest_pc, hl_ir_instruction *storage, uint32_t capacity);
HL_API hl_status hl_ir_append(hl_ir_block *block, const hl_ir_instruction *instruction, hl_ir_value *result);
HL_API hl_status hl_ir_validate(const hl_ir_block *block, uint32_t *bad_instruction);
HL_API hl_status hl_ir_function_init(hl_ir_function *function, uint32_t entry_label, hl_ir_block *storage,
                                     uint32_t capacity);
HL_API hl_status hl_ir_function_add_block(hl_ir_function *function, uint32_t label, uint64_t guest_pc,
                                          hl_ir_instruction *storage, uint32_t capacity, hl_ir_block **block);
HL_API hl_status hl_ir_function_append(hl_ir_function *function, hl_ir_block *block,
                                       const hl_ir_instruction *instruction, hl_ir_value *result);
HL_API hl_status hl_ir_function_validate(const hl_ir_function *function, uint32_t *bad_block,
                                         uint32_t *bad_instruction);
HL_API int hl_ir_opcode_is_terminator(uint16_t opcode);
HL_API hl_status hl_ir_interpret(const hl_ir_block *block, hl_ir_execution *execution);
HL_API hl_status hl_ir_function_interpret(const hl_ir_function *function, hl_ir_execution *execution);

HL_EXTERN_C_END

#endif
