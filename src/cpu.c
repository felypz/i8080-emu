#include "cpu.h"
#include <stdint.h>

/* -- fetch helpers -- */
static uint8_t fetch8(Cpu *cpu, const uint8_t *memory) {
	return memory[cpu->reg_pc++];
}

static uint16_t fetch16(Cpu *cpu, const uint8_t *memory) {
	uint8_t lo = fetch8(cpu, memory);
	uint8_t hi = fetch8(cpu, memory);
	return (uint16_t)(hi << 8) | lo;
}

/* -- Regs pair helper -- */
/* The i8080 has no native 16-bit registers  reg_bc, reg_de, reg_hl and reg_af are pairs of 8-bit registers
 * that some instructions (e.g. LXI, DAD, PUSH/POP, INX/DCX) treat as a single 16-bit value.
 * By hardware convention, they are named after the first letter of the pair - hence the (high << 8) | low pattern below. -
 * although they are treated as two letters here for better code readability.
 *
 * PLease check "cpu.h" source code for more information, or check docs about the topic for hardware convention.
 */
static uint16_t get_reg_bc(Cpu *cpu) { return (uint16_t)(cpu->reg_b << 8) | cpu->reg_c; }
static uint16_t get_reg_de(Cpu *cpu) { return (uint16_t)(cpu->reg_d << 8) | cpu->reg_e; }
static uint16_t get_reg_hl(Cpu *cpu) { return (uint16_t)(cpu->reg_h << 8) | cpu->reg_l; }
static void set_reg_bc(Cpu *cpu, uint16_t pair) { cpu->reg_b = pair >> 8; cpu->reg_c = pair & 0xFF; }
static void set_reg_de(Cpu *cpu, uint16_t pair) { cpu->reg_d = pair >> 8; cpu->reg_e = pair & 0xFF; }
static void set_reg_hl(Cpu *cpu, uint16_t pair) { cpu->reg_h = pair >> 8; cpu->reg_l = pair & 0xFF; }

/* -- 3-bit regs decode -- */
/* Many instructions (e.g. MOV, MVI, the whole ALU group) encode "which register" as 3 bits inside the opcode itself,
 * always using this table:
 *   000=B 001=C 010=D 011=E 
 *   100=H 101=L 110=M 111=A
 * --
 * "M" (index 6) is NOT a real register, it's assembly notation for "the memory byte at the address in reg_hl".
 * It's the i8080's only general-purpose indirect memory addressing mode,
 * which is why this one case reaches into `memory[get_reg_hl(cpu)]` instead of returning a register field.
 * This also explains why M operations cost more cycles than plain register ones (7 vs 4/5) — they touch external
 * memory, not just an internal register.
 */
static uint8_t get_reg8(Cpu *cpu, uint8_t *memory, uint8_t reg_idx) {
	switch (reg_idx) {
		case 0: return cpu->reg_b;
		case 1: return cpu->reg_c;
		case 2: return cpu->reg_d;
		case 3: return cpu->reg_e;
		case 4: return cpu->reg_h;
		case 5: return cpu->reg_l;
		case 6: return memory[get_reg_hl(cpu)];
		case 7: return cpu->reg_a;
	}
	return 0;
}

static void set_reg8(Cpu *cpu, uint8_t *memory, uint8_t reg_idx, uint8_t new_value) {
	switch (reg_idx) {
		case 0: cpu->reg_b = new_value; break;
		case 1: cpu->reg_c = new_value; break;
		case 2: cpu->reg_d = new_value; break;
		case 3: cpu->reg_e = new_value; break;
		case 4: cpu->reg_h = new_value; break;
		case 5: cpu->reg_l = new_value; break;
		case 6: memory[get_reg_hl(cpu)] = new_value; break;
		case 7: cpu->reg_a = new_value; break;
	}
}

/* -- Stack helpers -- */
/* The i8080 stack grows DOWNWARD: reg_sp is decremented BEFORE writing on push, and incremented AFTER reading on pop.
 * Values are stored little-endian, same as fetch16, but note push16 writes high byte first (at the higher address)
 * then low byte (at the lower address), so a subsequent pop16 still reads low-then-high correctly.
 */
static void push16(Cpu *cpu, uint8_t *memory, uint16_t value) {
	memory[--cpu->reg_sp] = (value >> 8) & 0xFF;
	memory[--cpu->reg_sp] = value & 0xFF;
}

static uint16_t pop16(Cpu *cpu, uint8_t *memory) {
	uint16_t lo = memory[cpu->reg_sp++];
	uint16_t hi = memory[cpu->reg_sp++];
	return (uint16_t)(hi << 8) | lo;
}

/* -- Flag helpers -- */
static uint8_t parity(uint8_t result) {
	uint8_t set_bits = 0;
	for (uint8_t each_bit = 0; each_bit < 8; each_bit++) {
		set_bits += (result >> each_bit) & 1;
	}
	return (set_bits % 2) == 0;
}

static void update_zsp(Cpu *cpu, uint8_t result) {
	cpu->flag_z = (result == 0);
	cpu->flag_s = (result & 0x80) != 0;
	cpu->flag_p = parity(result);
}

/* -- INR/DCR core -- */
static uint8_t inr_core(Cpu *cpu, uint8_t value) {
	uint8_t result = value + 1;
	cpu->flag_ac = (value & 0x0F) == 0x0F;
	update_zsp(cpu, result);
	return result;
}

static uint8_t dcr_core(Cpu *cpu, uint8_t value) {
	uint8_t result = value - 1;
	cpu->flag_ac = (value & 0x0F) != 0x00;
	update_zsp(cpu, result);
	return result;
}

/* -- ALU (Arithmetic logical instructions) core -- */
/* Shared logic between opcode pairs that only differ by whether they fold in the current CY flag (ADD/ADC, SUB/SBB).
 * Two flags need extra care here:
 * CY (carry/borrow):
 *   computed from the full 8-bit result overflowing into a wider type (uint16_t), since plain uint8_t math would wrap
 *   around silently and lose that information.
 * AC (auxiliary carry):
 *   same idea, but computed independently on just the low nibble (& 0x0F). Only matters for the DAA instruction,
 *   but the real hardware always computes it, so we replicate that.
 * ANA/XRA/ORA always clear flag_cy (no such thing as carry in a bitwise op);
 * ANA sets flag_ac via a hardware-specific rule (OR of bit 3 of both operands),
 * XRA/ORA always clear it.
 * CMP is SUB without storing the result back into reg_a.
 */
static void alu_add(Cpu *cpu, uint8_t operand, bool with_carry) {
	uint8_t carry_in = with_carry ? cpu->flag_cy : 0;
	uint16_t result = (uint16_t)cpu->reg_a + operand + carry_in;

	cpu->flag_cy = result > 0xFF;
	cpu->flag_ac = ((cpu->reg_a & 0x0F) + (operand & 0x0F) + carry_in) > 0x0F;

	cpu->reg_a = (uint8_t)result;
	update_zsp(cpu, cpu->reg_a);
}

static void alu_sub(Cpu *cpu, uint8_t operand, bool with_borrow) {
	uint8_t borrow_in = with_borrow ? cpu->flag_cy : 0;
	uint16_t result = (uint16_t)cpu->reg_a - operand - borrow_in;

	cpu->flag_cy = result > 0xFF;
	cpu->flag_ac = ((int)(cpu->reg_a & 0x0F) - (operand & 0x0F) - borrow_in) >= 0;

	cpu->reg_a = (uint8_t)result;
	update_zsp(cpu, cpu->reg_a);
}

static void alu_ana(Cpu *cpu, uint8_t operand) {
	cpu->flag_ac = ((cpu->reg_a | operand) & 0x08) != 0;
	cpu->reg_a &= operand;
	cpu->flag_cy = 0;
	update_zsp(cpu, cpu->reg_a);
}

static void alu_xra(Cpu *cpu, uint8_t operand) {
	cpu->reg_a ^= operand;
	cpu->flag_cy = 0;
	cpu->flag_ac = 0;
	update_zsp(cpu, cpu->reg_a);
}

static void alu_ora(Cpu *cpu, uint8_t operand) {
	cpu->reg_a |= operand;
	cpu->flag_cy = 0;
	cpu->flag_ac = 0;
	update_zsp(cpu, cpu->reg_a);
}

static void alu_cmp(Cpu *cpu, uint8_t operand) {
	uint16_t result = (uint16_t)cpu->reg_a - operand;
	cpu->flag_cy = result > 0xFF;
	cpu->flag_ac = ((int)(cpu->reg_a & 0x0F) - (operand & 0x0F)) >= 0;
	update_zsp(cpu, (uint8_t)result);
}

/* DAA (Decimal Adjust Accumulator) */
/* After an ADD/ADC on two BCD (Binary Coded Decimal) numbers, where each nibble represents one decimal digit (0-9),
 * the raw binary result can land on an invalid nibble value (0x0A-0x0F) or produce a bad carry.
 * DAA corrects reg_a back into valid two-digit BCD form.
 * It's the only instruction that reads AC as an input rather than just writing it.
 */
static void daa(Cpu *cpu) {
	uint8_t correction = 0;
	uint8_t lsb = cpu->reg_a & 0x0F;
	uint8_t msb = cpu->reg_a >> 4;

	if (cpu->flag_ac || lsb > 9) {
		correction += 0x06;
	}

	if (cpu->flag_cy || msb > 9 || (msb == 9 && lsb > 9)) {
		correction += 0x60;
		cpu->flag_cy = 1;
	}

	cpu->flag_ac = ((lsb + (correction & 0x0F)) > 0x0F);
	cpu->reg_a += correction;
	update_zsp(cpu, cpu->reg_a);
}

/* -- cpu logic and cycle -- */
void cpu_step(Cpu *cpu, uint8_t *memory) {
	if (cpu->halted) {
		return;
	}

	uint8_t opcode = fetch8(cpu, memory);
	uint8_t cycles = 4;

	/* -- OPCODES -- */
	/* Warning: the i8080 opcode is a single byte, giving exactly 256 possible combinations,
	 * but the official instruction set doesn't use all 256 uniquely.
	 * Some instructions (mainly ones with no register operand, like NOP, JMP, CALL, RET)
	 * are identified by the hardware's decode logic using only a few specific bits of the opcode,
	 * the remaining bits are never checked ("don't care"). Because of this, several different opcode byte values
	 * end up triggering the exact same instruction, since the CPU hardware never distinguishes between them.
	 * These are known as "undocumented opcodes": never part of Intel's official docs, but functionally identical to the instruction they duplicate.
	 */
	switch (opcode) {
		/* -- Misc/Control instructions -- */
		/* NOP; & duplicates */
		case 0x00: case 0x08: case 0x10: case 0x18:
		case 0x20: case 0x28: case 0x30: case 0x38:
			cycles = 4;
			break;
		
		/* HALT */
		case 0x76:
			cpu->halted = true;
			cycles = 7;
			break;
			
		case 0xD3: fetch8(cpu, memory); cycles = 10; break; /* OUT d8 */		
		case 0xDB: fetch8(cpu, memory); cycles = 10; break; /* IN d8 */	
		
		/* DI (disables interrupts) */
		case 0xF3:
			cpu->interrupt_enable = false;
			cycles = 4;
			break;

		/* EI (enables interrupts) */
		case 0xFB:
			cpu->interrupt_enable = true;
			cycles = 4;
			break;

		/* -- 8-bit arithmetic/logical instructions -- */
		/* INR r
		 * 0x04 - INR reg_b
		 * 0x0C - INR reg_c
		 * 0x14 - INR reg_d
		 * 0x1C - INR reg_e
		 * 0x24 - INR reg_h
		 * 0x2C - INR reg_l
		 * 0x34 - INR M
		 * 0x3C - INR reg_a
		 */
		case 0x04: case 0x0C: case 0x14: case 0x1C:
		case 0x24: case 0x2C: case 0x34: case 0x3C: {
			uint8_t reg_idx = (opcode >> 3) & 0x07;
			uint8_t current = get_reg8(cpu, memory, reg_idx);

			set_reg8(cpu, memory, reg_idx, inr_core(cpu, current));
			cycles = (reg_idx == 6) ? 10 : 5;
			break;
		}

		/* DCR r
		 * 0x05 - DCR reg_b
		 * 0x0D - DCR reg_c
		 * 0x15 - DCR reg_d
		 * 0x1D - DCR reg_e
		 * 0x25 - DCR reg_h
		 * 0x2D - DCR reg_l
		 * 0x35 - DCR M
		 * 0x3D - DCR reg_a
		 */
		case 0x05: case 0x0D: case 0x15: case 0x1D:
		case 0x25: case 0x2D: case 0x35: case 0x3D: {
			uint8_t reg_idx = (opcode >> 3) & 0x07;
			uint8_t current = get_reg8(cpu, memory, reg_idx);

			set_reg8(cpu, memory, reg_idx, dcr_core(cpu, current));
			cycles = (reg_idx == 6) ? 10 : 5;
			break;
		}

		/* RLC */
		case 0x07: { 
			cpu->flag_cy = (cpu->reg_a & 0x80) != 0;
			cpu->reg_a = (cpu->reg_a << 1) | cpu->flag_cy;
			cycles = 4;
			break;
		}

		/* RRC */
		case 0x0F: { 
			cpu->flag_cy = cpu->reg_a & 0x01;
			cpu->reg_a = (cpu->reg_a >> 1) | (cpu->flag_cy << 7);
			cycles = 4;
			break;
		}

		/* RAL */
		case 0x17: {
			uint8_t old_cy = cpu->flag_cy;

			cpu->flag_cy = (cpu->reg_a & 0x80) != 0;
			cpu->reg_a = (cpu->reg_a << 1) | old_cy;
			cycles = 4;
			break;
		}

		/* RAR */
		case 0x1F: {
			uint8_t old_cy = cpu->flag_cy;

			cpu->flag_cy = cpu->reg_a & 0x01;
			cpu->reg_a = (cpu->reg_a >> 1) | (old_cy << 7);
			cycles = 4;
			break;
		}

		case 0x27: daa(cpu); cycles = 4; break; /* DAA */
		case 0x2F: cpu->reg_a = ~cpu->reg_a; cycles = 4; break; /* CMA */
		case 0x37: cpu->flag_cy = 1; cycles = 4; break; /* STC */
		case 0x3F: cpu->flag_cy = !cpu->flag_cy; cycles = 4; break; /* CMC */

		/* ADD r
		 * 0x80 - ADD reg_b
		 * 0x81 - ADD reg_c
		 * 0x82 - ADD reg_d
		 * 0x83 - ADD reg_e
		 * 0x84 - ADD reg_h
		 * 0x85 - ADD reg_l
		 * 0x86 - ADD M
		 * 0x87 - ADD reg_a
		 */
		case 0x80: case 0x81: case 0x82: case 0x83:
		case 0x84: case 0x85: case 0x86: case 0x87: {
			uint8_t src = opcode & 0x07;

			alu_add(cpu, get_reg8(cpu, memory, src), false);
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* ADC r
		 * 0x88 - ADC reg_b
		 * 0x89 - ADC reg_c
		 * 0x8A - ADC reg_d
		 * 0x8B - ADC reg_e
		 * 0x8C - ADC reg_h
		 * 0x8D - ADC reg_l
		 * 0x8E - ADC M
		 * 0x8F - ADC reg_a
		 */
		case 0x88: case 0x89: case 0x8A: case 0x8B:
		case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
			uint8_t src = opcode & 0x07;

			alu_add(cpu, get_reg8(cpu, memory, src), true);
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* SUB r
		 * 0x90 - SUB reg_b
		 * 0x91 - SUB reg_c
		 * 0x92 - SUB reg_d
		 * 0x93 - SUB reg_e
		 * 0x94 - SUB reg_h
		 * 0x95 - SUB reg_l
		 * 0x96 - SUB M
		 * 0x97 - SUB reg_a
		 */
		case 0x90: case 0x91: case 0x92: case 0x93:
		case 0x94: case 0x95: case 0x96: case 0x97: {
			uint8_t src = opcode & 0x07;

			alu_sub(cpu, get_reg8(cpu, memory, src), false);
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* SBB r
		 * 0x98 - SBB reg_b
		 * 0x99 - SBB reg_c
		 * 0x9A - SBB reg_d
		 * 0x9B - SBB reg_e
		 * 0x9C - SBB reg_h
		 * 0x9D - SBB reg_l
		 * 0x9E - SBB M
		 * 0x9F - SBB reg_a
		 */
		case 0x98: case 0x99: case 0x9A: case 0x9B:
		case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
			uint8_t src = opcode & 0x07;

			alu_sub(cpu, get_reg8(cpu, memory, src), true);
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* ANA r
		 * 0xA0 - ANA reg_b
		 * 0xA1 - ANA reg_c
		 * 0xA2 - ANA reg_d
		 * 0xA3 - ANA reg_e
		 * 0xA4 - ANA reg_h
		 * 0xA5 - ANA reg_l
		 * 0xA6 - ANA M
		 * 0xA7 - ANA reg_a
		 */
		case 0xA0: case 0xA1: case 0xA2: case 0xA3:
		case 0xA4: case 0xA5: case 0xA6: case 0xA7: {
			uint8_t src = opcode & 0x07;

			alu_ana(cpu, get_reg8(cpu, memory, src));
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* XRA r
		 * 0xA8 - XRA reg_b
		 * 0xA9 - XRA reg_c
		 * 0xAA - XRA reg_d
		 * 0xAB - XRA reg_e
		 * 0xAC - XRA reg_h
		 * 0xAD - XRA reg_l
		 * 0xAE - XRA M
		 * 0xAF - XRA reg_a
		 */
		case 0xA8: case 0xA9: case 0xAA: case 0xAB:
		case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
			uint8_t src = opcode & 0x07;

			alu_xra(cpu, get_reg8(cpu, memory, src));
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* ORA r
		 * 0xB0 - ORA reg_b
		 * 0xB1 - ORA reg_c
		 * 0xB2 - ORA reg_d
		 * 0xB3 - ORA reg_e
		 * 0xB4 - ORA reg_h
		 * 0xB5 - ORA reg_l
		 * 0xB6 - ORA M
		 * 0xB7 - ORA reg_a
		 */
		case 0xB0: case 0xB1: case 0xB2: case 0xB3:
		case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
			uint8_t src = opcode & 0x07;

			alu_ora(cpu, get_reg8(cpu, memory, src));
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		/* CMP r
		 * 0xB8 - CMP reg_b
		 * 0xB9 - CMP reg_c
		 * 0xBA - CMP reg_d
		 * 0xBB - CMP reg_e
		 * 0xBC - CMP reg_h
		 * 0xBD - CMP reg_l
		 * 0xBE - CMP M
		 * 0xBF - CMP reg_a
		 */
		case 0xB8: case 0xB9: case 0xBA: case 0xBB:
		case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
			uint8_t src = opcode & 0x07;

			alu_cmp(cpu, get_reg8(cpu, memory, src));
			cycles = (src == 6) ? 7 : 4;
			break;
		}

		case 0xC6: alu_add(cpu, fetch8(cpu, memory), false); cycles = 7; break; /* ADI d8 */
		case 0xCE: alu_add(cpu, fetch8(cpu, memory), true);  cycles = 7; break; /* ACI d8 */
		case 0xD6: alu_sub(cpu, fetch8(cpu, memory), false); cycles = 7; break; /* SUI d8 */
		case 0xDE: alu_sub(cpu, fetch8(cpu, memory), true);  cycles = 7; break; /* SBI d8 */
		case 0xE6: alu_ana(cpu, fetch8(cpu, memory)); cycles = 7; break; /* ANI d8 */
		case 0xEE: alu_xra(cpu, fetch8(cpu, memory)); cycles = 7; break; /* XRI d8 */
		case 0xF6: alu_ora(cpu, fetch8(cpu, memory)); cycles = 7; break; /* ORI d8 */
		case 0xFE: alu_cmp(cpu, fetch8(cpu, memory)); cycles = 7; break; /* CPI d8 */

		/* -- 8-bit load/store/move instructions -- */
		/* STAX/LDAX move exactly ONE byte (reg_a)
		 * the "16-bit register" here (reg_bc or reg_de) is being used purely as a memory ADDRESS, not as data.
		 * Same idea as "M" above, just using reg_bc/reg_de as the pointer instead of reg_hl.
		 *
		 * STA/LDA do the identical thing but with a direct 16-bit immediate address instead of a register pair.
		 */

		/* STAX r; 16-bit reg */
		case 0x02: memory[get_reg_bc(cpu)] = cpu->reg_a; cycles = 7; break; /* STAX reg_bc (memory[reg_bc] = reg_a) */
		case 0x12: memory[get_reg_de(cpu)] = cpu->reg_a; cycles = 7; break; /* STAX reg_de (memory[reg_de] = reg_a) */

		/* LDAX r; 16-bit reg */
		case 0x0A: cpu->reg_a = memory[get_reg_bc(cpu)]; cycles = 7; break; /* LDAX reg_bc (reg_a = memory[reg_bc]) */
		case 0x1A: cpu->reg_a = memory[get_reg_de(cpu)]; cycles = 7; break; /* LDAX reg_de (reg_a = memory[reg_de]) */

		/* STA a16; memory[a16] = reg_a */
		case 0x32:
			memory[fetch16(cpu, memory)] = cpu->reg_a;
			cycles = 13;
			break;

		/* LDA a16; reg_a = memory[a16] */
		case 0x3A: 
			cpu->reg_a = memory[fetch16(cpu, memory)];
			cycles = 13;
			break;

		/* MVI r, d8
		 * 0x06 - MVI reg_b, d8
		 * 0x0E - MVI reg_c, d8
		 * 0x16 - MVI reg_d, d8
		 * 0x1E - MVI reg_e, d8
		 * 0x26 - MVI reg_h, d8
		 * 0x2E - MVI reg_l, d8
		 * 0x36 - MVI M, d8
		 * 0x3E - MVI reg_a, d8
		 */
		case 0x06: case 0x0E: case 0x16: case 0x1E:
		case 0x26: case 0x2E: case 0x36: case 0x3E: {
			uint8_t dst = (opcode >> 3) & 0x07;
			uint8_t imm = fetch8(cpu, memory);

			set_reg8(cpu, memory, dst, imm);
			cycles = (dst == 6) ? 10 : 7;
			break;
		}

		/* MOV r, r
		 * Opcode format: 01DDDSSS
		 *   DDD (bits 5-3) = destination register
		 *   SSS (bits 2-0) = source register
		 * Both fields use the same 3-bit table:
		 *   000=B 001=C 010=D 011=E 100=H 101=L 110=M(HL) 111=A
		 *
		 * 0x40 - MOV reg_b, reg_b      0x58 - MOV reg_e, reg_b      0x70 - MOV M, reg_b
		 * 0x41 - MOV reg_b, reg_c      0x59 - MOV reg_e, reg_c      0x71 - MOV M, reg_c
		 * 0x42 - MOV reg_b, reg_d      0x5A - MOV reg_e, reg_d      0x72 - MOV M, reg_d
		 * 0x43 - MOV reg_b, reg_e      0x5B - MOV reg_e, reg_e      0x73 - MOV M, reg_e
		 * 0x44 - MOV reg_b, reg_h      0x5C - MOV reg_e, reg_h      0x74 - MOV M, reg_h
		 * 0x45 - MOV reg_b, reg_l      0x5D - MOV reg_e, reg_l      0x75 - MOV M, reg_l
		 * 0x46 - MOV reg_b, M          0x5E - MOV reg_e, M          **** - ****
		 * 0x47 - MOV reg_b, reg_a      0x5F - MOV reg_e, reg_a      0x77 - MOV M, reg_a
		 *
		 * 0x48 - MOV reg_c, reg_b      0x60 - MOV reg_h, reg_b      0x78 - MOV reg_a, reg_b
		 * 0x49 - MOV reg_c, reg_c      0x61 - MOV reg_h, reg_c      0x79 - MOV reg_a, reg_c
		 * 0x4A - MOV reg_c, reg_d      0x62 - MOV reg_h, reg_d      0x7A - MOV reg_a, reg_d
		 * 0x4B - MOV reg_c, reg_e      0x63 - MOV reg_h, reg_e      0x7B - MOV reg_a, reg_e
		 * 0x4C - MOV reg_c, reg_h      0x64 - MOV reg_h, reg_h      0x7C - MOV reg_a, reg_h
		 * 0x4D - MOV reg_c, reg_l      0x65 - MOV reg_h, reg_l      0x7D - MOV reg_a, reg_l
		 * 0x4E - MOV reg_c, M          0x66 - MOV reg_h, M          0x7E - MOV reg_a, M
		 * 0x4F - MOV reg_c, reg_a      0x67 - MOV reg_h, reg_a      0x7F - MOV reg_a, reg_a
		 *
		 * 0x50 - MOV reg_d, reg_b      0x68 - MOV reg_l, reg_b
		 * 0x51 - MOV reg_d, reg_c      0x69 - MOV reg_l, reg_c
		 * 0x52 - MOV reg_d, reg_d      0x6A - MOV reg_l, reg_d
		 * 0x53 - MOV reg_d, reg_e      0x6B - MOV reg_l, reg_e
		 * 0x54 - MOV reg_d, reg_h      0x6C - MOV reg_l, reg_h
		 * 0x55 - MOV reg_d, reg_l      0x6D - MOV reg_l, reg_l
		 * 0x56 - MOV reg_d, M          0x6E - MOV reg_l, M
		 * 0x57 - MOV reg_d, reg_a      0x6F - MOV reg_l, reg_a
		 */
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
		case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
		case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
		case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
		case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:            case 0x77:
		case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
			uint8_t dst = (opcode >> 3) & 0x07;
			uint8_t src = opcode & 0x07;

			set_reg8(cpu, memory, dst, get_reg8(cpu, memory, src));
			cycles = (dst == 6 || src == 6) ? 7 : 5;
			break;
		}

		/* -- 16-bit arithmetic/logical instructions -- */
		/* INX r */
		case 0x03: set_reg_bc(cpu, get_reg_bc(cpu) + 1); cycles = 5; break; /* INX reg_bc++ */
		case 0x13: set_reg_de(cpu, get_reg_de(cpu) + 1); cycles = 5; break; /* INX reg_de++ */
		case 0x23: set_reg_hl(cpu, get_reg_hl(cpu) + 1); cycles = 5; break; /* INX reg_hl++ */
		case 0x33: cpu->reg_sp++; cycles = 5; break; /* INX reg_sp++ */

		/* DAD r
		 * 0x09 - DAD reg_bc (reg_hl += reg_bc)
		 * 0x19 - DAD reg_de (reg_hl += reg_de)
		 * 0x29 - DAD reg_hl (reg_hl += reg_hl, i.e. reg_hl *= 2)
		 * 0x39 - DAD reg_sp (reg_hl += reg_sp)
		 */
		case 0x09: {
			uint32_t result = (uint32_t)get_reg_hl(cpu) + get_reg_bc(cpu);

			cpu->flag_cy = result > 0xFFFF;
			set_reg_hl(cpu, (uint16_t)result);
			cycles = 10;
			break;
		}

		case 0x19: {
			uint32_t result = (uint32_t)get_reg_hl(cpu) + get_reg_de(cpu);

			cpu->flag_cy = result > 0xFFFF;
			set_reg_hl(cpu, (uint16_t)result);
			cycles = 10;
			break;
		}

		case 0x29: {
			uint32_t result = (uint32_t)get_reg_hl(cpu) + get_reg_hl(cpu);

			cpu->flag_cy = result > 0xFFFF;
			set_reg_hl(cpu, (uint16_t)result);
			cycles = 10;
			break;
		}

		case 0x39: {
			uint32_t result = (uint32_t)get_reg_hl(cpu) + cpu->reg_sp;

			cpu->flag_cy = result > 0xFFFF;
			set_reg_hl(cpu, (uint16_t)result);
			cycles = 10;
			break;
		}

		/* DCX r */
		case 0x0B: set_reg_bc(cpu, get_reg_bc(cpu) - 1); cycles = 5; break;
		case 0x1B: set_reg_de(cpu, get_reg_de(cpu) - 1); cycles = 5; break;
		case 0x2B: set_reg_hl(cpu, get_reg_hl(cpu) - 1); cycles = 5; break;
		case 0x3B: cpu->reg_sp--; cycles = 5; break;

		/* -- 16-bit load/store/move instructions -- */
		/* LXI r, d16 */
		case 0x01: set_reg_bc(cpu, fetch16(cpu, memory)); cycles = 10; break;
		case 0x11: set_reg_de(cpu, fetch16(cpu, memory)); cycles = 10; break;
		case 0x21: set_reg_hl(cpu, fetch16(cpu, memory)); cycles = 10; break;
		case 0x31: cpu->reg_sp = fetch16(cpu, memory); cycles = 10; break;

		/* SHLD a16 - memory[a16]=reg_l, memory[a16+1]=reg_h */
		case 0x22: {
			uint16_t addr = fetch16(cpu, memory);

			memory[addr] = cpu->reg_l;
			memory[(uint16_t) (addr + 1)] = cpu->reg_h;
			cycles = 16;
			break;
		}

		/* LHLD a16 - reg_l=memory[a16], reg_h=memory[a16+1] */
		case 0x2A: { 
			uint16_t addr = fetch16(cpu, memory);

			cpu->reg_l = memory[addr];
			cpu->reg_h = memory[(uint16_t) (addr + 1)];
			cycles = 16;
			break;
		}

		/* POP r */
		case 0xC1: set_reg_bc(cpu, pop16(cpu, memory)); cycles = 10; break;
		case 0xD1: set_reg_de(cpu, pop16(cpu, memory)); cycles = 10; break;
		case 0xE1: set_reg_hl(cpu, pop16(cpu, memory)); cycles = 10; break;
		case 0xF1: {
			uint16_t popped = pop16(cpu, memory);

			cpu->reg_a = popped >> 8;
			cpu->reg_f = popped & 0xFF;
			cpu->pad1 = 1; cpu->pad3 = 0; cpu->pad5 = 0;
			cycles = 10;
			break;
		}

		/* PUSH r */
		case 0xC5: push16(cpu, memory, get_reg_bc(cpu)); cycles = 11; break;
		case 0xD5: push16(cpu, memory, get_reg_de(cpu)); cycles = 11; break;
		case 0xE5: push16(cpu, memory, get_reg_hl(cpu)); cycles = 11; break;
		case 0xF5: {
			cpu->pad1 = 1; cpu->pad3 = 0; cpu->pad5 = 0;
			push16(cpu, memory, (uint16_t)(cpu->reg_a << 8) | cpu->reg_f);
			cycles = 11;
			break;
		}

		/* XTHL */
		case 0xE3: {
			uint16_t top = pop16(cpu, memory);

			push16(cpu, memory, get_reg_hl(cpu));
			set_reg_hl(cpu, top);
			cycles = 18;
			break;
		}

		/* XCHG */
		case 0xEB: {
			uint16_t tmp = get_reg_hl(cpu);

			set_reg_hl(cpu, get_reg_de(cpu));
			set_reg_de(cpu, tmp);
			cycles = 4;
			break;
		}

		/* SPHL */
		case 0xF9:
			cpu->reg_sp = get_reg_hl(cpu);
			cycles = 5;
			break;

		/* -- Jump/Calls -- */
		/* Rcond */
		case 0xC0: if (!cpu->flag_z)  { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RNZ */
		case 0xC8: if ( cpu->flag_z)  { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RZ */
		case 0xD0: if (!cpu->flag_cy) { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RNC */
		case 0xD8: if ( cpu->flag_cy) { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RC */
		case 0xE0: if (!cpu->flag_p)  { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RPO */
		case 0xE8: if ( cpu->flag_p)  { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RPE */
		case 0xF0: if (!cpu->flag_s)  { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RP */
		case 0xF8: if ( cpu->flag_s)  { cpu->reg_pc = pop16(cpu, memory); cycles = 11; } else cycles = 5; break; /* RM */

		/* Jcond a16 */
		/* conditional jump. Cycle count is ALWAYS 10 on real i8080 hardware, whether the branch is taken or not.
		 * Unlike Ccond/Rcond below, where taking vs not taking the branch costs different cycles, 
		 * because CALL/RET also touch the stack, which Jcond never does.
		 */
		case 0xC2: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_z)  cpu->reg_pc = addr; cycles = 10; break; } /* JNZ a16 */
		case 0xCA: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_z)  cpu->reg_pc = addr; cycles = 10; break; } /* JZ  a16 */
		case 0xD2: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_cy) cpu->reg_pc = addr; cycles = 10; break; } /* JNC a16 */
		case 0xDA: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_cy) cpu->reg_pc = addr; cycles = 10; break; } /* JC  a16 */
		case 0xE2: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_p)  cpu->reg_pc = addr; cycles = 10; break; } /* JPO a16 */
		case 0xEA: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_p)  cpu->reg_pc = addr; cycles = 10; break; } /* JPE a16 */
		case 0xF2: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_s)  cpu->reg_pc = addr; cycles = 10; break; } /* JP  a16 */
		case 0xFA: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_s)  cpu->reg_pc = addr; cycles = 10; break; } /* JM  a16 */
		
		/* JMP a16; and duplicate */
		case 0xC3: case 0xCB: cpu->reg_pc = fetch16(cpu, memory); cycles = 10; break;
		
		/* Ccond a16 */
		case 0xC4: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_z)  { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CNZ a16 */
		case 0xCC: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_z)  { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CZ  a16 */
		case 0xD4: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_cy) { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CNC a16 */
		case 0xDC: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_cy) { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CC  a16 */
		case 0xE4: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_p)  { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CPO a16 */
		case 0xEC: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_p)  { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CPE a16 */
		case 0xF4: { uint16_t addr = fetch16(cpu, memory); if (!cpu->flag_s)  { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CP  a16 */
		case 0xFC: { uint16_t addr = fetch16(cpu, memory); if ( cpu->flag_s)  { push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = addr; cycles = 17; } else cycles = 11; break; } /* CM  a16 */
		
		/* RST n */
		/* A compact 1-byte CALL to one of 8 fixed addresses (n*8).
		 * Mainly used by interrupt vectors and by CP/M-style short system calls,
		 * since it avoids the extra 2 bytes a normal CALL a16 needs to encode the target address.
		 */
		case 0xC7: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x00; cycles = 11; break; /* RST 0 */
		case 0xCF: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x08; cycles = 11; break; /* RST 1 */
		case 0xD7: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x10; cycles = 11; break; /* RST 2 */
		case 0xDF: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x18; cycles = 11; break; /* RST 3 */
		case 0xE7: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x20; cycles = 11; break; /* RST 4 */
		case 0xEF: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x28; cycles = 11; break; /* RST 5 */
		case 0xF7: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x30; cycles = 11; break; /* RST 6 */
		case 0xFF: push16(cpu, memory, cpu->reg_pc); cpu->reg_pc = 0x38; cycles = 11; break; /* RST 7 */

		/* RET; and duplicate */
		case 0xC9: case 0xD9:
			cpu->reg_pc = pop16(cpu, memory);
			cycles = 10;
			break;

		/* CALL a16; and duplicates */
		case 0xCD: case 0xDD: case 0xED: case 0xFD: {
			uint16_t addr = fetch16(cpu, memory);

			push16(cpu, memory, cpu->reg_pc);
			cpu->reg_pc = addr;
			cycles = 17;
			break;
		}

		/* PCHL */
		case 0xE9: cpu->reg_pc = get_reg_hl(cpu); cycles = 5; break;

		/* -- Unimplemented or invalid opcode -- */
		default:
			break;
	}
	cpu->cycles += cycles;
}

/* -- Interrupt handling -- */
/* On real i8080 hardware, there's no dedicated "interrupt vector" like on later CPUs.
 * Instead, external hardware (an interrupt controller, a video chip signaling VBlank, etc.) asserts the INT line
 * and then places an instruction directly on the data bus for the CPU to execute,
 * in the overwhelming majority of real-world systems (arcade boards, CP/M machines),
 * that injected instruction is one of the 8 RST n opcodes,
 * since it's a single byte and needs no extra fetch cycle to know where to jump.
 *
 * cpu_interrupt() simulates exactly that: it's the equivalent of external hardware injecting a "RST rst_num"
 * from outside, without the CPU ever fetching it from memory[reg_pc] the normal way.
 * This is why it needs its own push16() call and PC assignment, identical in effect to the RST n cases in cpu_step(),
 * just triggered from outside the fetch/decode loop instead of from an opcode byte.
 *
 * Two behaviors from the real hardware are reproduced here:
 * 1. interrupt_enable acts as a hardware gate. If it's off (after a DI, or still off after reset),
 * the CPU physically ignores the INT line, the pulse is lost with no side effects on reg_pc or the stack.
 * We still record interrupt_pending here purely as an external/debug observation hook,
 * not something the real i8080 exposes, cpu_step() never reads it.
 *
 * 2. HALT only stops instruction fetch; it does NOT disable the CPU. A valid, enabled interrupt "wakes up" a halted CPU
 * and executes normally from there, so we clear cpu->halted here rather than leaving cpu_step() stuck returning early forever.
 *
 * Like a real DI, accepting the interrupt automatically clears interrupt_enable,
 * so a second interrupt can't preempt the handler routine before it's ready.
 * It's up to the interrupt handler code (the RST target) to re-enable interrupts with EI once it's safe to do so,
 * exactly like a normal ISR would on this CPU.
 */
void cpu_interrupt(Cpu *cpu, uint8_t *memory, uint8_t rst_num) {
	if (!cpu->interrupt_enable) {
		cpu->interrupt_pending = true;
		return;
	}

	cpu->interrupt_pending = false;
	cpu->halted = false;
	cpu->interrupt_enable = false;

	push16(cpu, memory, cpu->reg_pc);
	cpu->reg_pc = (uint16_t)(rst_num * 8);
	cpu->cycles += 11;
}
