#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

#define MEMORY_SIZE 0x10000 /* Full 16-bit i8080 address space (64KB) */

static uint8_t memory[MEMORY_SIZE];

/* -- ROM loading -- */
static long load_file(const char *path, uint16_t addr) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "Error: could not open '%s': %s\n", path, strerror(errno));
		return -1;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);

	if (size < 0) {
		fprintf(stderr, "Error: could not determine size of '%s'\n", path);
		fclose(f);
		return -1;
	}

	if ((long)addr + size > MEMORY_SIZE) {
		fprintf(stderr, "Error: '%s' (%ld bytes) does not fit in memory at 0x%04X\n",
		        path, size, addr);
		fclose(f);
		return -1;
	}

	size_t read_bytes = fread(&memory[addr], 1, (size_t)size, f);
	fclose(f);

	if ((long)read_bytes != size) {
		fprintf(stderr, "Error: short read on '%s'\n", path);
		return -1;
	}

	return size;
}

/* -- CP/M (Control Program for Microcomputers) BDOS emulation (minimal) -- */
/* Classic i8080 diagnostic ROMs were built to run under CP/M,
 * which lives at low memory and exposes its API through a single fixed entry point: "CALL 5" (the BDOS).
 * The function number goes in reg_c, and only two functions are ever used by these test ROMs to print their PASS/FAIL results:
 *   C=2 (Console Output)  - print the single character in reg_e
 *   C=9 (Print String)    - print the '$'-terminated string at (reg_d:reg_e)
 *
 * We don't need a real CP/M: we just need to satisfy these two calls well enough for the test ROM to report its results to stdout.
 * Everything else about CP/M (the actual OS, disk I/O, etc.) is irrelevant to a CPU test.
 */

static void cpm_bdos_call(Cpu *cpu) {
	switch (cpu->reg_c) {
		case 2:
			putchar(cpu->reg_e);
			break;

		case 9: {
			uint16_t addr = (uint16_t)((cpu->reg_d << 8) | cpu->reg_e);
			while (memory[addr] != '$') {
				putchar(memory[addr]);
				addr++;
			}

			break;
		}

		default:
			/* Unhandled BDOS function; harmless to ignore for CPU tests. */
			break;
	}
}

static void print_usage(const char *prog_name) {
	fprintf(stderr,
		"Usage: %s <rom_file> [options]\n"
		"Options:\n"
		"  --cpm            Enable CP/M BDOS emulation (CALL 5 intercept).\n"
		"                   Required for classic test ROMs (TST8080.COM, etc).\n"
		"  --addr 0xNNNN    Load address and entry point (default: 0x0100 with\n"
		"                   --cpm, 0x0000 otherwise).\n"
		"  --dump           Print final register state and cycle count on exit.\n",
		prog_name);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	const char *rom_path = argv[1];
	bool cpm_mode = false;
	bool dump_state = false;

	uint16_t load_addr = 0x0000;
	bool addr_explicit = false;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--cpm") == 0) {
			cpm_mode = true;
		}

		else if (strcmp(argv[i], "--dump") == 0) {
			dump_state = true;
		}

		else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
			load_addr = (uint16_t)strtoul(argv[++i], NULL, 0);
			addr_explicit = true;
		}

		else {
			fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* CP/M programs are always assembled to run from 0x0100 (the "TPA", Transient Program Area),
	 * since 0x0000-0x00FF is reserved by CP/M itself for the reset vector and BDOS/BIOS jump table.
	 * Raw i8080 programs with no OS underneath conventionally start at 0x0000.
	 */
	if (!addr_explicit) {
		load_addr = cpm_mode ? 0x0100 : 0x0000;
	}

	memset(memory, 0, sizeof(memory));

	long rom_size = load_file(rom_path, load_addr);
	if (rom_size < 0) {
		return EXIT_FAILURE;
	}

	printf("Loaded '%s' (%ld bytes) at 0x%04X\n", rom_path, rom_size, load_addr);

	Cpu cpu;
	cpu_init(&cpu);
	cpu.reg_pc = load_addr;

	if (cpm_mode) {
		/* Test ROMs signal completion by jumping to address 0x0000 (CP/M's "warm boot" vector).
		 * We patch a HLT there so the emulator stops cleanly instead of executing
		 * whatever garbage byte happens to be sitting in zeroed-out memory.
		 *
		 * Address 0x0005 (the BDOS entry point) is never actually executed
		 * as code: we intercept CALL 5 in the run loop below before
		 * cpu_step() ever gets a chance to fetch/decode whatever is there.
		 * Leaving it zeroed out is fine.
		 */
		memory[0x0000] = 0x76; /* HLT */
	}

	printf("--- Execution start ---\n");

	while (!cpu.halted) {
		if (cpm_mode && cpu.reg_pc == 0x0005) {
			cpm_bdos_call(&cpu);

			/* Simulate the RET a real BDOS would perform: manually pop the
			 * return address that the test ROM's "CALL 5" pushed onto the
			 * stack, then resume execution from there. We do this by hand
			 * here (rather than calling into cpu.c's internal pop16)
			 * since that helper is intentionally kept static/private to cpu.c.
			 */
			uint16_t ret_addr = (uint16_t)(memory[cpu.reg_sp] | (memory[cpu.reg_sp + 1] << 8));
			cpu.reg_sp += 2;
			cpu.reg_pc = ret_addr;
			continue;
		}

		cpu_step(&cpu, memory);
	}

	printf("\n--- Execution halted ---\n");

	if (dump_state) {
		printf("PC=%04X SP=%04X A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X\n",
		       cpu.reg_pc, cpu.reg_sp, cpu.reg_a, cpu.reg_f,
		       cpu.reg_b, cpu.reg_c, cpu.reg_d, cpu.reg_e,
		       cpu.reg_h, cpu.reg_l);

		printf("Flags: S=%d Z=%d AC=%d P=%d CY=%d\n",
		       cpu.flag_s, cpu.flag_z, cpu.flag_ac, cpu.flag_p, cpu.flag_cy);

		printf("Total cycles: %llu\n", (unsigned long long)cpu.cycles);
	}
	return EXIT_SUCCESS;
}
