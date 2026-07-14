#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test.h"

#include "hl/codegen.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static hl_ir_instruction instruction(uint16_t opcode, uint16_t result_type) {
    hl_ir_instruction value;
    memset(&value, 0, sizeof(value));
    value.opcode = opcode;
    value.result.type = result_type;
    return value;
}

static uint32_t word(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static hl_status execute_native(const uint8_t *code, size_t code_size, hl_ir_exit *out_exit) {
    long page_size = sysconf(_SC_PAGESIZE);
    size_t mapping_size;
    void *mapping;
    hl_code_entry entry;
    hl_ir_execution execution;
    hl_status status;
    HL_CHECK(page_size > 0);
    mapping_size = (code_size + (size_t)page_size - 1u) & ~((size_t)page_size - 1u);
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    HL_CHECK(mapping != MAP_FAILED);
    memcpy(mapping, code, code_size);
    HL_CHECK(mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) == 0);
    memcpy(&entry, &mapping, sizeof(entry));
    memset(&execution, 0, sizeof(execution));
    execution.abi = HL_IR_EXECUTION_ABI;
    execution.size = sizeof(execution);
    status = entry(&execution);
    *out_exit = execution.exit;
    HL_CHECK(munmap(mapping, mapping_size) == 0);
    return status;
}

static hl_status execute_native_memory(const uint8_t *code, size_t code_size, uint8_t *memory, size_t memory_size,
                                       hl_ir_exit *out_exit) {
    long page_size = sysconf(_SC_PAGESIZE);
    size_t mapping_size = (code_size + (size_t)page_size - 1u) & ~((size_t)page_size - 1u);
    void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    hl_code_entry entry;
    hl_ir_execution execution;
    hl_status status;
    HL_CHECK(mapping != MAP_FAILED);
    memcpy(mapping, code, code_size);
    HL_CHECK(mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) == 0);
    memcpy(&entry, &mapping, sizeof(entry));
    memset(&execution, 0, sizeof(execution));
    execution.abi = HL_IR_EXECUTION_ABI;
    execution.size = sizeof(execution);
    execution.memory = memory;
    execution.memory_size = memory_size;
    status = entry(&execution);
    *out_exit = execution.exit;
    HL_CHECK(munmap(mapping, mapping_size) == 0);
    return status;
}

static int check_memory(void) {
    hl_ir_instruction ins[28], current;
    hl_ir_block block;
    hl_ir_value values[20], loaded;
    uint8_t code_storage[4096], memory[64];
    hl_code_buffer code;
    hl_ir_exit out;
    hl_ir_execution interpreted;
    uint32_t i, isa;
    memset(memory, 0, sizeof(memory));
    HL_CHECK(hl_ir_block_init(&block, 0x700000, ins, HL_ARRAY_COUNT(ins)) == HL_STATUS_OK);
    for (i = 0; i < HL_ARRAY_COUNT(values); ++i) {
        current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
        current.immediate = i == 18 ? 5 : (i == 19 ? UINT64_C(0x1122334455667788) : i);
        HL_CHECK(hl_ir_append(&block, &current, &values[i]) == HL_STATUS_OK);
    }
    current = instruction(HL_IR_OP_STORE, HL_IR_TYPE_NONE);
    current.operand_count = 2;
    current.operands[0] = values[18];
    current.operands[1] = values[19];
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_LOAD, HL_IR_TYPE_I32);
    current.operand_count = 1;
    current.operands[0] = values[18];
    HL_CHECK(hl_ir_append(&block, &current, &loaded) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = loaded;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);
    memset(&interpreted, 0, sizeof(interpreted));
    interpreted.abi = HL_IR_EXECUTION_ABI;
    interpreted.size = sizeof(interpreted);
    interpreted.memory = memory;
    interpreted.memory_size = sizeof(memory);
    HL_CHECK(hl_ir_interpret(&block, &interpreted) == HL_STATUS_OK);
    HL_CHECK(interpreted.exit.kind == HL_IR_EXIT_RETURN && interpreted.exit.value == UINT64_C(0x55667788));
    for (isa = HL_HOST_ISA_AARCH64; isa <= HL_HOST_ISA_X86_64; ++isa) {
        HL_CHECK(hl_code_buffer_init(&code, code_storage, sizeof(code_storage)) == HL_STATUS_OK);
        HL_CHECK(hl_codegen_block(isa, &block, &code) == HL_STATUS_OK);
#if defined(__aarch64__)
        if (isa == HL_HOST_ISA_AARCH64) {
#elif defined(__x86_64__)
        if (isa == HL_HOST_ISA_X86_64) {
#else
        if (0) {
#endif
            memset(memory, 0, sizeof(memory));
            HL_CHECK(execute_native_memory(code_storage, code.code_size, memory, sizeof(memory), &out) == HL_STATUS_OK);
            HL_CHECK(out.kind == HL_IR_EXIT_RETURN && out.value == UINT64_C(0x55667788));
            HL_CHECK(memory[5] == 0x88 && memory[12] == 0x11);
        }
    }

    /* An eight-byte load beginning at byte 60 deterministically faults. */
    HL_CHECK(hl_ir_block_init(&block, 0x700100, ins, HL_ARRAY_COUNT(ins)) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_GUEST_ADDRESS);
    current.immediate = 60;
    HL_CHECK(hl_ir_append(&block, &current, &values[0]) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_LOAD, HL_IR_TYPE_I64);
    current.operand_count = 1;
    current.operands[0] = values[0];
    HL_CHECK(hl_ir_append(&block, &current, &loaded) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = loaded;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);
    HL_CHECK(hl_ir_interpret(&block, &interpreted) == HL_STATUS_OK);
    HL_CHECK(interpreted.exit.kind == HL_IR_EXIT_FAULT && interpreted.exit.value == HL_IR_FAULT_MEMORY &&
             interpreted.exit.detail == 60);
    for (isa = HL_HOST_ISA_AARCH64; isa <= HL_HOST_ISA_X86_64; ++isa) {
        HL_CHECK(hl_code_buffer_init(&code, code_storage, sizeof(code_storage)) == HL_STATUS_OK);
        HL_CHECK(hl_codegen_block(isa, &block, &code) == HL_STATUS_OK);
#if defined(__aarch64__)
        if (isa == HL_HOST_ISA_AARCH64) {
#elif defined(__x86_64__)
        if (isa == HL_HOST_ISA_X86_64) {
#else
        if (0) {
#endif
            HL_CHECK(execute_native_memory(code_storage, code.code_size, memory, sizeof(memory), &out) == HL_STATUS_OK);
            HL_CHECK(out.kind == HL_IR_EXIT_FAULT && out.value == HL_IR_FAULT_MEMORY && out.detail == 60);
        }
    }
    ins[0].immediate = UINT64_MAX - 3u;
    ins[1].immediate = 8;
    HL_CHECK(hl_ir_interpret(&block, &interpreted) == HL_STATUS_OK);
    HL_CHECK(interpreted.exit.kind == HL_IR_EXIT_FAULT && interpreted.exit.detail == 4);
    for (isa = HL_HOST_ISA_AARCH64; isa <= HL_HOST_ISA_X86_64; ++isa) {
        HL_CHECK(hl_code_buffer_init(&code, code_storage, sizeof(code_storage)) == HL_STATUS_OK);
        HL_CHECK(hl_codegen_block(isa, &block, &code) == HL_STATUS_OK);
#if defined(__aarch64__)
        if (isa == HL_HOST_ISA_AARCH64) {
#elif defined(__x86_64__)
        if (isa == HL_HOST_ISA_X86_64) {
#else
        if (0) {
#endif
            HL_CHECK(execute_native_memory(code_storage, code.code_size, memory, sizeof(memory), &out) == HL_STATUS_OK);
            HL_CHECK(out.kind == HL_IR_EXIT_FAULT && out.value == HL_IR_FAULT_MEMORY && out.detail == 4);
        }
    }
    return EXIT_SUCCESS;
}

static int check_native_exit(uint16_t opcode, uint32_t expected_kind) {
    hl_ir_instruction instructions[3];
    hl_ir_instruction current;
    hl_ir_block block;
    hl_ir_value value;
    hl_ir_value detail;
    uint8_t storage[128];
    hl_code_buffer code;
    hl_ir_exit native_exit;
    uint32_t host_isa;

    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x500000), instructions, HL_ARRAY_COUNT(instructions)) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
    current.immediate = UINT64_C(0x1122334455667788);
    HL_CHECK(hl_ir_append(&block, &current, &value) == HL_STATUS_OK);
    current.immediate = UINT64_C(0x8877665544332211);
    HL_CHECK(hl_ir_append(&block, &current, &detail) == HL_STATUS_OK);
    current = instruction(opcode, HL_IR_TYPE_NONE);
    current.operand_count = 2;
    current.operands[0] = value;
    current.operands[1] = detail;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);

#if defined(__aarch64__)
    host_isa = HL_HOST_ISA_AARCH64;
#elif defined(__x86_64__)
    host_isa = HL_HOST_ISA_X86_64;
#else
    host_isa = 0;
#endif
    if (host_isa != 0) {
        HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
        HL_CHECK(hl_codegen_block(host_isa, &block, &code) == HL_STATUS_OK);
        memset(&native_exit, 0xa5, sizeof(native_exit));
        HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
        HL_CHECK(native_exit.kind == expected_kind && native_exit.reserved == 0 &&
                 native_exit.value == UINT64_C(0x1122334455667788) &&
                 native_exit.detail == UINT64_C(0x8877665544332211));
    }
    return EXIT_SUCCESS;
}

static int check_register_spills(void) {
    hl_ir_instruction instructions[24];
    hl_ir_instruction current;
    hl_ir_block block;
    hl_ir_value values[20];
    hl_ir_value sum;
    uint8_t storage[1024];
    hl_code_buffer code;
    hl_ir_exit native_exit;
    uint32_t index;
    uint32_t host_isa;

    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x600000), instructions, HL_ARRAY_COUNT(instructions)) == HL_STATUS_OK);
    for (index = 0; index < HL_ARRAY_COUNT(values); ++index) {
        current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
        current.immediate = UINT64_C(1000) + index;
        HL_CHECK(hl_ir_append(&block, &current, &values[index]) == HL_STATUS_OK);
    }
    current = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I64);
    current.operand_count = 2;
    current.operands[0] = values[18];
    current.operands[1] = values[19];
    HL_CHECK(hl_ir_append(&block, &current, &sum) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);

    for (host_isa = HL_HOST_ISA_AARCH64; host_isa <= HL_HOST_ISA_X86_64; ++host_isa) {
        HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
        HL_CHECK(hl_codegen_block(host_isa, &block, &code) == HL_STATUS_OK);
#if defined(__aarch64__)
        if (host_isa == HL_HOST_ISA_AARCH64) {
#elif defined(__x86_64__)
        if (host_isa == HL_HOST_ISA_X86_64) {
#else
        if (0) {
#endif
            memset(&native_exit, 0xa5, sizeof(native_exit));
            HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
            HL_CHECK(native_exit.kind == HL_IR_EXIT_RETURN && native_exit.reserved == 0 && native_exit.value == 2037 &&
                     native_exit.detail == 0);
        }
    }
    return EXIT_SUCCESS;
}

int main(void) {
    hl_ir_instruction instructions[8];
    hl_ir_block block;
    hl_ir_instruction current;
    hl_ir_value forty;
    hl_ir_value two;
    hl_ir_value sum;
    uint8_t storage[128];
    hl_code_buffer code;
    hl_ir_exit native_exit;

    HL_CHECK(hl_ir_block_init(&block, 0x400000, instructions, HL_ARRAY_COUNT(instructions)) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
    current.immediate = 40;
    HL_CHECK(hl_ir_append(&block, &current, &forty) == HL_STATUS_OK);
    current.immediate = 2;
    HL_CHECK(hl_ir_append(&block, &current, &two) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I64);
    current.operand_count = 2;
    current.operands[0] = forty;
    current.operands[1] = two;
    HL_CHECK(hl_ir_append(&block, &current, &sum) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);

    HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
    HL_CHECK(hl_codegen_block(HL_HOST_ISA_AARCH64, &block, &code) == HL_STATUS_OK);
    HL_CHECK(code.code_size == 40);
    HL_CHECK(word(storage + 0) == UINT32_C(0xd2800501));
    HL_CHECK(word(storage + 4) == UINT32_C(0xd2800042));
    HL_CHECK(word(storage + 8) == UINT32_C(0x8b020023));
#if defined(__aarch64__)
    memset(&native_exit, 0xa5, sizeof(native_exit));
    HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
    HL_CHECK(native_exit.kind == HL_IR_EXIT_RETURN && native_exit.reserved == 0 && native_exit.value == 42 &&
             native_exit.detail == 0);
#endif

    memset(storage, 0, sizeof(storage));
    HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
    HL_CHECK(hl_codegen_block(HL_HOST_ISA_X86_64, &block, &code) == HL_STATUS_OK);
    HL_CHECK(code.code_size == 61);
    HL_CHECK(storage[0] == UINT8_C(0x48) && storage[1] == UINT8_C(0xb8));
#if defined(__x86_64__)
    memset(&native_exit, 0xa5, sizeof(native_exit));
    HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
    HL_CHECK(native_exit.kind == HL_IR_EXIT_RETURN && native_exit.reserved == 0 && native_exit.value == 42 &&
             native_exit.detail == 0);
#endif
    HL_CHECK(check_native_exit(HL_IR_OP_SYSCALL_EXIT, HL_IR_EXIT_SYSCALL) == EXIT_SUCCESS);
    HL_CHECK(check_native_exit(HL_IR_OP_FAULT_EXIT, HL_IR_EXIT_FAULT) == EXIT_SUCCESS);
    HL_CHECK(check_register_spills() == EXIT_SUCCESS);
    HL_CHECK(check_memory() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
