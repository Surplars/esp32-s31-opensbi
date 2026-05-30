/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_string.h>

struct sbiret {
	unsigned long error;
	unsigned long value;
};

struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
			unsigned long arg1, unsigned long arg2,
			unsigned long arg3, unsigned long arg4,
			unsigned long arg5)
{
	struct sbiret ret;

	register unsigned long a0 asm ("a0") = (unsigned long)(arg0);
	register unsigned long a1 asm ("a1") = (unsigned long)(arg1);
	register unsigned long a2 asm ("a2") = (unsigned long)(arg2);
	register unsigned long a3 asm ("a3") = (unsigned long)(arg3);
	register unsigned long a4 asm ("a4") = (unsigned long)(arg4);
	register unsigned long a5 asm ("a5") = (unsigned long)(arg5);
	register unsigned long a6 asm ("a6") = (unsigned long)(fid);
	register unsigned long a7 asm ("a7") = (unsigned long)(ext);
	asm volatile ("ecall"
		      : "+r" (a0), "+r" (a1)
		      : "r" (a2), "r" (a3), "r" (a4), "r" (a5), "r" (a6), "r" (a7)
		      : "memory");
	ret.error = a0;
	ret.value = a1;

	return ret;
}

static inline void sbi_ecall_console_puts(const char *str)
{
	sbi_ecall(SBI_EXT_DBCN, SBI_EXT_DBCN_CONSOLE_WRITE,
		  sbi_strlen(str), (unsigned long)str, 0, 0, 0, 0);
}

static inline void sbi_ecall_shutdown(void)
{
	sbi_ecall(SBI_EXT_SRST, SBI_EXT_SRST_RESET,
		  SBI_SRST_RESET_TYPE_SHUTDOWN, SBI_SRST_RESET_REASON_NONE,
		  0, 0, 0, 0);
}

static inline void sbi_ecall_set_timer(unsigned long next_event)
{
	sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER,
		  next_event, 0, 0, 0, 0, 0);
}

#define CSR_STVEC	0x105
#define CSR_TIME	0xc01

#define csr_stringify_1(x)	#x
#define csr_stringify(x)	csr_stringify_1(x)

#define csr_read(csr)						\
	({							\
		unsigned long __v;				\
		__asm__ __volatile__("csrr %0, " csr_stringify(csr) \
				     : "=r"(__v) : : "memory");	\
		__v;						\
	})

#define csr_write(csr, val)					\
	do {							\
		unsigned long __v = (unsigned long)(val);	\
		__asm__ __volatile__("csrw " csr_stringify(csr) ", %0" \
				     : : "r"(__v) : "memory");	\
	} while (0)

volatile unsigned long timer_seen;

void s_trap_entry(void);

void test_main(unsigned long a0, unsigned long a1)
{
	sbi_ecall_console_puts("\nTest payload running\n");

	timer_seen = 0;
#ifdef OPENSBI_PLATFORM_ESP32S31_CLIC
	csr_write(CSR_STVEC, ((unsigned long)s_trap_entry) | 3);
#else
	csr_write(CSR_STVEC, (unsigned long)s_trap_entry);
#endif
	sbi_ecall_set_timer(csr_read(CSR_TIME) + 40000000);
	sbi_ecall_console_puts("TIME ecall returned\n");

	for (unsigned long i = 0; i < 200000000 && !timer_seen; i++)
		__asm__ __volatile__("" ::: "memory");

	if (!timer_seen)
		sbi_ecall_console_puts("S-mode timer timeout\n");
	else
		sbi_ecall_console_puts("S-mode timer interrupt\n");

	sbi_ecall_shutdown();
	sbi_ecall_console_puts("sbi_ecall_shutdown failed to execute.\n");
}
