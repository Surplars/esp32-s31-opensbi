/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 startup wrapper for OpenSBI.
 *
 * ESP32-S31 ROM bootloader does NOT pass hartid or DTB in a0/a1.
 * This wrapper sets up the environment and jumps to OpenSBI's _start
 * with the expected arguments.
 *
 * Boot flow:
 *   ROM bootloader → second-stage bootloader → esp32s31_start → OpenSBI _start
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>

	.section .text.entry, "ax", %progbits
	.align 3
	.globl	esp32s31_start
	.type	esp32s31_start, @function
esp32s31_start:
	/*
	 * We arrive here from ESP bootloader with:
	 *   - M-mode
	 *   - Interrupts disabled
	 *   - No valid a0/a1 (hartid/DTB)
	 *   - Stack pointer set by bootloader (may be invalid for us)
	 */

	/* Set up our own stack for hart 0 */
	lla	sp, _stack_end

	/* Clear BSS */
	lla	t0, _bss_start
	lla	t1, _bss_end
1:
	bgeu	t0, t1, 2f
	REG_S	zero, 0(t0)
	addi	t0, t0, __SIZEOF_LONG__
	j	1b
2:

	/*
	 * Set up arguments for OpenSBI _start:
	 *   a0 = hartid (0 for core 0)
	 *   a1 = DTB address (use embedded DTB or 0)
	 *   a2 = 0
	 */
	li	a0, 0
	lla	a1, _dtb_start
	li	a2, 0

	/* Jump to OpenSBI entry point */
	lla	t0, _start
	jr	t0

	.size	esp32s31_start, . - esp32s31_start

	/* Stack for hart 0 (8KB) */
	.section .bss.stack, "aw", @nobits
	.align 4
	.globl _stack_start
_stack_start:
	.space 8192
	.globl _stack_end
_stack_end:

	/* Embedded device tree (placeholder - replace with real DTB) */
	.section .dtb, "aw", %progbits
	.align 4
	.globl _dtb_start
_dtb_start:
	/* Minimal empty DTB header (will be replaced with real DTB) */
	.word	0xedfe0dd0	/* magic: 0xd00dfeed */
	.word	0x00000040	/* totalsize: 64 */
	.word	0x00000038	/* off_dt_struct */
	.word	0x00000038	/* off_dt_strings */
	.word	0x00000038	/* off_mem_rsvmap */
	.word	0x00000011	/* version */
	.word	0x00000010	/* last_comp_version */
	.word	0x00000000	/* boot_cpuid_phys */
	.word	0x00000000	/* size_dt_strings */
	.word	0x00000008	/* size_dt_struct */
	/* FDT_END */
	.word	0x00000009
	.word	0x00000000
