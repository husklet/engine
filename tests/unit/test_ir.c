#include "test.h"

#include "hl/ir.h"

#include <string.h>

static hl_ir_instruction instruction(uint16_t opcode, uint16_t result_type) {
    hl_ir_instruction value;
    memset(&value, 0, sizeof(value));
    value.opcode = opcode;
    value.result.type = result_type;
    return value;
}

int main(void) {
    hl_ir_instruction storage[8];
    hl_ir_block block;
    hl_ir_instruction constant;
    hl_ir_instruction add;
    hl_ir_instruction exit_instruction;
    hl_ir_value first;
    hl_ir_value second;
    hl_ir_value sum;
    hl_ir_exit ir_exit;
    uint32_t bad;
    static const uint16_t reserved_opcodes[] = {
        HL_IR_OP_LOAD,      HL_IR_OP_STORE, HL_IR_OP_COMPARE, HL_IR_OP_BRANCH, HL_IR_OP_BRANCH_CONDITIONAL,
        HL_IR_OP_GUEST_CALL};

    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400000), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
    constant = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
    constant.immediate = 40;
    HL_CHECK(hl_ir_append(&block, &constant, &first) == HL_STATUS_OK);
    constant.immediate = 2;
    HL_CHECK(hl_ir_append(&block, &constant, &second) == HL_STATUS_OK);

    add = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I64);
    add.operand_count = 2;
    add.operands[0] = first;
    add.operands[1] = second;
    HL_CHECK(hl_ir_append(&block, &add, &sum) == HL_STATUS_OK);

    exit_instruction = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    exit_instruction.operand_count = 1;
    exit_instruction.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &exit_instruction, NULL) == HL_STATUS_OK);
    HL_CHECK(hl_ir_validate(&block, &bad) == HL_STATUS_OK);
    HL_CHECK(hl_ir_interpret(&block, &ir_exit) == HL_STATUS_OK);
    HL_CHECK(ir_exit.kind == HL_IR_EXIT_RETURN && ir_exit.value == 42);
    HL_CHECK(hl_ir_append(&block, &constant, NULL) == HL_STATUS_INVALID_ARGUMENT);

    storage[2].operands[1].id = 99;
    HL_CHECK(hl_ir_validate(&block, &bad) == HL_STATUS_CORRUPT && bad == 2);

    /* I32 arithmetic wraps at 32 bits independently of the host integer width. */
    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400100), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
    constant = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I32);
    constant.immediate = UINT64_C(0xffffffff);
    HL_CHECK(hl_ir_append(&block, &constant, &first) == HL_STATUS_OK);
    constant.immediate = 2;
    HL_CHECK(hl_ir_append(&block, &constant, &second) == HL_STATUS_OK);
    add = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I32);
    add.operand_count = 2;
    add.operands[0] = first;
    add.operands[1] = second;
    HL_CHECK(hl_ir_append(&block, &add, &sum) == HL_STATUS_OK);
    exit_instruction = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    exit_instruction.operand_count = 1;
    exit_instruction.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &exit_instruction, NULL) == HL_STATUS_OK);
    HL_CHECK(hl_ir_interpret(&block, &ir_exit) == HL_STATUS_OK);
    HL_CHECK(ir_exit.kind == HL_IR_EXIT_RETURN && ir_exit.value == 1);

    /* Exit payload semantics are identical for dispatch and fault boundaries. */
    for (uint16_t opcode = HL_IR_OP_SYSCALL_EXIT; opcode <= HL_IR_OP_FAULT_EXIT; ++opcode) {
        HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400180), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
        constant = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
        constant.immediate = UINT64_C(0x1122334455667788);
        HL_CHECK(hl_ir_append(&block, &constant, &first) == HL_STATUS_OK);
        constant.immediate = UINT64_C(0x8877665544332211);
        HL_CHECK(hl_ir_append(&block, &constant, &second) == HL_STATUS_OK);
        exit_instruction = instruction(opcode, HL_IR_TYPE_NONE);
        exit_instruction.operand_count = 2;
        exit_instruction.operands[0] = first;
        exit_instruction.operands[1] = second;
        HL_CHECK(hl_ir_append(&block, &exit_instruction, NULL) == HL_STATUS_OK);
        memset(&ir_exit, 0xa5, sizeof(ir_exit));
        HL_CHECK(hl_ir_interpret(&block, &ir_exit) == HL_STATUS_OK);
        HL_CHECK(ir_exit.kind == (opcode == HL_IR_OP_SYSCALL_EXIT ? HL_IR_EXIT_SYSCALL : HL_IR_EXIT_FAULT));
        HL_CHECK(ir_exit.reserved == 0 && ir_exit.value == UINT64_C(0x1122334455667788) &&
                 ir_exit.detail == UINT64_C(0x8877665544332211));
    }

    /* Reserved operations cannot become apparently valid IR before their execution contracts exist. */
    for (size_t i = 0; i < HL_ARRAY_COUNT(reserved_opcodes); ++i) {
        hl_ir_instruction reserved = instruction(reserved_opcodes[i], HL_IR_TYPE_NONE);
        HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400200), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
        HL_CHECK(hl_ir_append(&block, &reserved, NULL) == HL_STATUS_NOT_SUPPORTED);

        storage[0] = reserved;
        block.instruction_count = 1;
        block.terminated = (uint32_t)hl_ir_opcode_is_terminator(reserved.opcode);
        HL_CHECK(hl_ir_validate(&block, &bad) == HL_STATUS_CORRUPT && bad == 0);
    }
    return EXIT_SUCCESS;
}
