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
    return EXIT_SUCCESS;
}
