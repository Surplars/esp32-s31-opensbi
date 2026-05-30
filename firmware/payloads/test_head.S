/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_encoding.h>
#define __ASM_STR(x)	x

#if __riscv_xlen == 64
#define __REG_SEL(a, b)		__ASM_STR(a)
#define RISCV_PTR		.dword
#elif __riscv_xlen == 32
#define __REG_SEL(a, b)		__ASM_STR(b)
#define RISCV_PTR		.word
#else
#error "Unexpected __riscv_xlen"
#endif

#define REG_L		__REG_SEL(ld, lw)
#define REG_S		__REG_SEL(sd, sw)
#define REG_ADDW	__REG_SEL(addw, add)
#define S_TRAP_REGS	16

	.section .entry, "ax", %progbits
	.align 3
	.globl _start
_start:
	/* Pick one hart to run the main boot sequence */
	lla	a3, _hart_lottery
	li	a2, 1
#if defined(__riscv_atomic) || defined(__riscv_zaamo)
	amoadd.w a3, a2, (a3)
#elif defined(__riscv_zalrsc)
_sc_fail:
	lr.w	t0, (a3)
	REG_ADDW t1, t0, a2
	sc.w	t1, t1, (a3)
	bnez	t1, _sc_fail
	move	a3, t0
#else
#error "need a or zalrsc"
#endif
	bnez	a3, _start_hang

	/* Save a0 and a1 */
	lla	a3, _boot_a0
	REG_S	a0, 0(a3)
	lla	a3, _boot_a1
	REG_S	a1, 0(a3)

	/* Zero-out BSS */
	lla	a4, _bss_start
	lla	a5, _bss_end
_bss_zero:
	REG_S	zero, (a4)
	add	a4, a4, __SIZEOF_POINTER__
	blt	a4, a5, _bss_zero

_start_warm:
	/* Disable and clear all interrupts */
#ifdef OPENSBI_PLATFORM_ESP32S31_CLIC
	/* ESP32-S31 CLIC path does not tolerate direct SIE/SIP writes. */
#else
	csrw	CSR_SIE, zero
	csrw	CSR_SIP, zero
#endif

	/* Setup exception vectors */
	lla	a3, _start_hang
	csrw	CSR_STVEC, a3

	/* Setup stack */
	lla	a3, _payload_end
	li	a4, 0x2000
	add	sp, a3, a4

	/* Jump to C main */
	lla	a3, _boot_a0
	REG_L	a0, 0(a3)
	lla	a3, _boot_a1
	REG_L	a1, 0(a3)
	call	test_main

	/* We don't expect to reach here hence just hang */
	j	_start_hang

	.section .entry, "ax", %progbits
	.align 3
	.globl _start_hang
_start_hang:
	wfi
	j	_start_hang

	.section .text
#ifdef OPENSBI_PLATFORM_ESP32S31_CLIC
	.balign 64
#else
	.align 3
#endif
	.globl s_trap_entry
s_trap_entry:
	addi	sp, sp, -(S_TRAP_REGS * __SIZEOF_POINTER__)
	REG_S	ra, (0 * __SIZEOF_POINTER__)(sp)
	REG_S	t0, (1 * __SIZEOF_POINTER__)(sp)
	REG_S	t1, (2 * __SIZEOF_POINTER__)(sp)
	REG_S	t2, (3 * __SIZEOF_POINTER__)(sp)
	REG_S	a0, (4 * __SIZEOF_POINTER__)(sp)
	REG_S	a1, (5 * __SIZEOF_POINTER__)(sp)
	REG_S	a2, (6 * __SIZEOF_POINTER__)(sp)
	REG_S	a3, (7 * __SIZEOF_POINTER__)(sp)
	REG_S	a4, (8 * __SIZEOF_POINTER__)(sp)
	REG_S	a5, (9 * __SIZEOF_POINTER__)(sp)
	REG_S	a6, (10 * __SIZEOF_POINTER__)(sp)
	REG_S	a7, (11 * __SIZEOF_POINTER__)(sp)
	REG_S	t3, (12 * __SIZEOF_POINTER__)(sp)
	REG_S	t4, (13 * __SIZEOF_POINTER__)(sp)
	REG_S	t5, (14 * __SIZEOF_POINTER__)(sp)
	REG_S	t6, (15 * __SIZEOF_POINTER__)(sp)
	csrr	t0, CSR_SCAUSE
	li	t1, 0x80000000
	and	t1, t0, t1
	beqz	t1, _s_trap_done
	li	t1, 0xfff
	and	t0, t0, t1
	li	t1, IRQ_S_TIMER
	bne	t0, t1, _s_trap_done
	lla	t0, timer_seen
	li	t1, 1
	REG_S	t1, 0(t0)
_s_trap_done:
	REG_L	ra, (0 * __SIZEOF_POINTER__)(sp)
	REG_L	t0, (1 * __SIZEOF_POINTER__)(sp)
	REG_L	t1, (2 * __SIZEOF_POINTER__)(sp)
	REG_L	t2, (3 * __SIZEOF_POINTER__)(sp)
	REG_L	a0, (4 * __SIZEOF_POINTER__)(sp)
	REG_L	a1, (5 * __SIZEOF_POINTER__)(sp)
	REG_L	a2, (6 * __SIZEOF_POINTER__)(sp)
	REG_L	a3, (7 * __SIZEOF_POINTER__)(sp)
	REG_L	a4, (8 * __SIZEOF_POINTER__)(sp)
	REG_L	a5, (9 * __SIZEOF_POINTER__)(sp)
	REG_L	a6, (10 * __SIZEOF_POINTER__)(sp)
	REG_L	a7, (11 * __SIZEOF_POINTER__)(sp)
	REG_L	t3, (12 * __SIZEOF_POINTER__)(sp)
	REG_L	t4, (13 * __SIZEOF_POINTER__)(sp)
	REG_L	t5, (14 * __SIZEOF_POINTER__)(sp)
	REG_L	t6, (15 * __SIZEOF_POINTER__)(sp)
	addi	sp, sp, (S_TRAP_REGS * __SIZEOF_POINTER__)
	sret

	.section .data
	.align	3
_hart_lottery:
	RISCV_PTR	0
_boot_a0:
	RISCV_PTR	0
_boot_a1:
	RISCV_PTR	0

	/* This will be called when the stack corruption is detected */
	.section .text
	.align 3
	.globl __stack_chk_fail
	.type __stack_chk_fail, %function
	.equ  __stack_chk_fail, _start_hang

	/* Initial value of the stack guard variable */
	.section .data
	.align 3
	.globl __stack_chk_guard
	.type __stack_chk_guard, %object
__stack_chk_guard:
	RISCV_PTR	0x95B5FF5A
