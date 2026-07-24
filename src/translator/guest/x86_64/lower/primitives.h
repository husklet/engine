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
int do_alu_imm12(int kind, int destination, int left, uint64_t immediate, int width);
void narrow_adcsbb(int adc, int destination, int left, int right, int width);
int lock_rmw(int kind, int width, int source);
void emit_rcl_rcr(struct insn *insn, uint64_t next, int width, int rotate_right, int count);
void emit_exit_const(uint64_t rip, uint64_t reason);
void hl_x86_emit_spill(void);
void hl_x86_emit_reload(void);
void hl_x86_emit_host_pointer(int destination, uint64_t pointer);
void hl_x86_emit_vector_reset(void);
void hl_x86_emit_flags_load(void);
uint32_t *hl_x86_emit_cursor(void);
uint64_t hl_x86_guest_pointer(uint64_t address);
void hl_x86_count_rep_movs(void);
void hl_x86_count_rep_stos(void);

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
void emit32(uint32_t instruction);
void e_str(int source, int base, int offset);
void e_ldr(int destination, int base, int offset);
void e_uxt(int destination, int source, int width);
void e_rrr(uint32_t instruction, int destination, int left, int right, int sixty_four_bit, int shift);
void e_shv(uint32_t instruction, int destination, int source, int count, int sixty_four_bit);
void e_ror_i(int destination, int source, int shift, int sixty_four_bit);
void e_rot_flags_cl(int result, int kind, int width);
void e_rot_flags_const(int result, int kind, int width, int count);
void e_asr_i(int destination, int source, int shift, int sixty_four_bit);
void e_lsl_i(int destination, int source, int shift, int sixty_four_bit);
void e_lsr_i(int destination, int source, int shift, int sixty_four_bit);
void e_tst(int source, int sixty_four_bit);
void e_nzcv_save(void);
void e_nzcv_save_setcf(int carry);
void e_nzcv_set_of(int overflow);
void e_pf_save(int source);
void e_csel(int destination, int when_true, int when_false, int condition, int sixty_four_bit);
void e_subi_s(int destination, int source, unsigned immediate, int sixty_four_bit);
void e_addi_s_sh(int destination, int source, unsigned immediate, int sixty_four_bit, int shift12);
void e_subi_s_sh(int destination, int source, unsigned immediate, int sixty_four_bit, int shift12);
void e_cset(int destination, int condition, int sixty_four_bit);
void g_ldr_d(int destination, int address);
void g_str_d(int source, int address);
void e_fmov_to_d(int destination, int source);
void e_fmov_from_d(int destination, int source);
void e_fp_settop(int delta);
void e_fp_ld(int destination, int index);
void e_fp_st(int source, int index);
void e_fp_push(int source);
void e_fcom_setfpsw(int left, int right);
void hl_x86_emit_load_scalar32(int destination, int address);
void hl_x86_emit_insert_scalar32(int destination, int destination_lane, int source, int source_lane);
void hl_x86_emit_vector3(uint32_t base, int destination, int left, int right);
void hl_x86_emit_vector_copy(int destination, int source);
void hl_x86_emit_vector_broadcast32(int destination, int source, int lane);
void hl_x86_emit_vector_extract(int destination, int low, int high, int byte);
void hl_x86_emit_vector_insert32(int destination, int destination_lane, int source, int source_lane);
void hl_x86_emit_vector_insert64(int destination, int destination_lane, int source, int source_lane);
void hl_x86_emit_vector_shift_right(int destination, int source, int width, int shift, int arithmetic);
void hl_x86_emit_vector_load128(int destination, int address, int offset);
void hl_x86_emit_vector_load64(int destination, int address);
void hl_x86_emit_vector_load32(int destination, int address);
void hl_x86_emit_constant_part(int destination, uint32_t immediate, int shift);

#endif
