#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct Cpu {
	uint64_t cycles;
	
	/* -- i8080 regs --
	 * 8-bit reg_a - Accumulator reg
	 * 8-bit reg_f - 16-bit reg_af (PSW) reg  - Flag regs
	 * --
	 * 8-bit reg_b, reg_c - 16-bit reg_bc (B) reg - General purpose regs
	 * 8-bit reg_d, reg_e - 16-bit reg_de (D) reg - General purpose regs
	 * 8-bit reg_h, reg_l - 16-bit reg_hl (H) reg - High and Low regs
	 * --
	 * 16-bit reg_sp - Stack pointer
	 * 16-bit reg_pc - Program counter
	 * --
	 *
	 * Warning: The i8080 has no native 16-bit registers, B, D, H, PSW
	 * are pairs of 8-bit registers (B + C, D + E, H + L and A + F respectively)
	 * that by hardware convention are named after the first letter of the reg pair.
	 * However for the sake of simplicity, the 16-bit reg pairs
	 * will be treated as reg_bc, reg_de, reg_hl, and reg_af respectively
	 * for better comprehension and learning.
	 *
	 * cheers ;)
	 */
	uint16_t reg_sp, reg_pc;
	uint8_t reg_a, reg_b, reg_c, reg_d, reg_e, reg_h, reg_l;

	/* -- i8080 flags reg_f --
	 * flag_s  - Sign
	 * flag_z  - Zero
	 * flag_ac - Auxiliary Carry
	 * flag_p  - Parity
	 * flag_cy - Carry
	 *
	 * Bit: 7  6  5  4  3  2  1  0
	 *      S  Z  0  AC 0  P  1  C
	 * The bits 1; 3; and 5 are fixed (1; 0; 0 respectively) in the reg_af (PSW) register
	 *
	 * The union keyword allow to read/write individual flags OR the raw byte without duplicating state.
	 */
	union {
		struct {
		uint8_t flag_cy : 1;
		uint8_t pad1    : 1;
		uint8_t flag_p  : 1;
		uint8_t pad3    : 1;
		uint8_t flag_ac : 1;
		uint8_t pad5    : 1;
		uint8_t flag_z  : 1;
		uint8_t flag_s  : 1;
		};
		uint8_t reg_f;
	};
	bool halted, interrupt_pending, interrupt_enable;
} Cpu;

void cpu_init(Cpu *cpu);
void cpu_step(Cpu *cpu, uint8_t *memory);
void cpu_interrupt(Cpu *cpu, uint8_t *memory, uint8_t rst_num);
