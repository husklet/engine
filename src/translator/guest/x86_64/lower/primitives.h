#ifndef HL_TRANSLATOR_GUEST_X86_64_LOWER_PRIMITIVES_H
#define HL_TRANSLATOR_GUEST_X86_64_LOWER_PRIMITIVES_H

#include <stdint.h>

#include "../decoder.h"

enum hl_x86_translate_result {
    TX_FALL = 0,
    TX_NEXT = 1,
    TX_BREAK = 2,
};

int alu_kind_primary(uint8_t op);
int byte_val(struct insn *insn, int register_number, int scratch);
void byte_wb(struct insn *insn, int register_number, int value);
int rm_load(struct insn *insn, uint64_t next, int width, int *memory);
void rm_store(struct insn *insn, int width, int value);
int xaludirect_on(void);
void do_alu(int kind, int destination, int left, int right, int width);
void narrow_adcsbb(int adc, int destination, int left, int right, int width);
int lock_rmw(int kind, int width, int source);

void emit_ea(struct insn *insn, uint64_t next);
void emit_ea_core(struct insn *insn, uint64_t next, int bias);
void emit_load_mem(struct insn *insn, uint64_t next, int width, int destination);
int ea_imm_fold(struct insn *insn, int width, int *base, int *offset);
void emit_bus_guard(int address_register, uint64_t size, uint64_t rip);

void e_movz(int destination, uint32_t immediate, int shift);
void e_movconst(int destination, uint64_t value);
void e_bfi(int destination, int source, int least_significant_bit, int width, int sixty_four_bit);
void e_load(int width, int destination, int address);
void e_store(int width, int source, int address);
void e_ldrs(int width, int destination, int address);
void e_store_uoff(int width, int source, int base, unsigned offset);
void e_stur(int width, int source, int base, int offset);
void e_mov_rr(int destination, int source, int sixty_four_bit);
void e_sxt(int destination, int source, int width);
void e_addi(int destination, int source, unsigned immediate, int sixty_four_bit);
void e_subi(int destination, int source, unsigned immediate, int sixty_four_bit);

#endif
