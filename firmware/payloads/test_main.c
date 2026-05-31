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

static inline struct sbiret sbi_ecall_hart_start(unsigned long hartid,
						 unsigned long start_addr,
						 unsigned long opaque)
{
	return sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
			 hartid, start_addr, opaque, 0, 0, 0);
}

static inline struct sbiret sbi_ecall_hart_get_status(unsigned long hartid)
{
	return sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_GET_STATUS,
			 hartid, 0, 0, 0, 0, 0);
}

static inline struct sbiret sbi_ecall_hart_stop(void)
{
	return sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STOP,
			 0, 0, 0, 0, 0, 0);
}

static inline struct sbiret sbi_ecall_remote_sfence_vma(unsigned long hmask,
							unsigned long hbase,
							unsigned long start,
							unsigned long size)
{
	return sbi_ecall(SBI_EXT_RFENCE, SBI_EXT_RFENCE_REMOTE_SFENCE_VMA,
			 hmask, hbase, start, size, 0, 0);
}

#define CSR_STVEC	0x105
#define CSR_SSTATUS	0x100
#define CSR_SATP	0x180
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
volatile unsigned long secondary_seen;
volatile unsigned long sv32_fault_expected;
volatile unsigned long sv32_fault_seen;
volatile unsigned long sv32_fault_cause;
volatile unsigned long sv32_fault_tval;
volatile unsigned long sv32_fault_epc;
volatile unsigned long sv32_fault_resume_pc;
volatile unsigned long rfence_cmd;
volatile unsigned long rfence_ready;
volatile unsigned long rfence_before;
volatile unsigned long rfence_after;
volatile unsigned long rfence_err;
volatile unsigned long sv32_strict_ad_load;
volatile unsigned long sv32_strict_ad_store;
volatile unsigned long sv32_strict_asid;

void s_trap_entry(void);
void test_secondary_entry(void);
void test_rfence_secondary_entry(void);
void sv32_identity_exec_probe(void);
unsigned long sv32_fault_load_probe(unsigned long addr);
void sv32_fault_store_probe(unsigned long addr, unsigned long val);
void sv32_fault_exec_probe(unsigned long addr);

#define SV32_MODE		0x80000000UL
#define SV32_ROOT_VA		0x2f000000UL
#define SV32_ALIAS_BASE		0x30000000UL
#define SV32_ALIAS_DATA		(SV32_ALIAS_BASE + 0x0000)
#define SV32_ALIAS_CODE		(SV32_ALIAS_BASE + 0x1000)
#define SV32_BAD_MEGA		0x30400000UL
#define SV32_ASID_SHIFT		22
#define SV32_ASID_MASK		0x1ffUL
#define SV32_PTE_V		(1UL << 0)
#define SV32_PTE_R		(1UL << 1)
#define SV32_PTE_W		(1UL << 2)
#define SV32_PTE_X		(1UL << 3)
#define SV32_PTE_U		(1UL << 4)
#define SV32_PTE_G		(1UL << 5)
#define SV32_PTE_A		(1UL << 6)
#define SV32_PTE_D		(1UL << 7)
#define SV32_PTE_FLAGS		(SV32_PTE_V | SV32_PTE_R | SV32_PTE_W | \
				 SV32_PTE_X | SV32_PTE_G | SV32_PTE_A | \
				 SV32_PTE_D)
#define SSTATUS_SUM		(1UL << 18)
#define SSTATUS_MXR		(1UL << 19)

static unsigned long sv32_root_pt[1024] __attribute__((aligned(4096)));
static unsigned long sv32_l1_pt[1024] __attribute__((aligned(4096)));
static unsigned long sv32_data_page[1024] __attribute__((aligned(4096)));
static unsigned long sv32_data_page2[1024] __attribute__((aligned(4096)));
static volatile unsigned long sv32_test_word = 0x12345678;

static void test_put_hex(unsigned long val)
{
	char buf[2 + sizeof(unsigned long) * 2 + 2];
	unsigned long i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < sizeof(unsigned long) * 2; i++) {
		unsigned long shift = (sizeof(unsigned long) * 8) - 4 - i * 4;
		unsigned long nib = (val >> shift) & 0xf;

		buf[2 + i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
	}
	buf[2 + sizeof(unsigned long) * 2] = '\n';
	buf[3 + sizeof(unsigned long) * 2] = '\0';
	sbi_ecall_console_puts(buf);
}

static inline void local_sfence_vma(void)
{
	__asm__ __volatile__("sfence.vma" ::: "memory");
}

static void sv32_clear_pt(unsigned long *pt)
{
	for (unsigned long i = 0; i < 1024; i++)
		pt[i] = 0;
}

static void sv32_map_identity_megapage(void)
{
	unsigned long root_idx = SV32_ROOT_VA >> 22;

	sv32_root_pt[root_idx] = ((SV32_ROOT_VA >> 12) << 10) |
				 SV32_PTE_FLAGS;
}

static void sv32_map_l1_root(unsigned long va, unsigned long *l1)
{
	unsigned long root_idx = va >> 22;

	sv32_root_pt[root_idx] = (((unsigned long)l1 >> 12) << 10) |
				 SV32_PTE_V;
}

static void sv32_map_4k(unsigned long va, unsigned long pa,
			unsigned long flags)
{
	unsigned long idx = (va >> 12) & 0x3ff;

	sv32_l1_pt[idx] = ((pa >> 12) << 10) | flags | SV32_PTE_V;
	local_sfence_vma();
}

static int sv32_enable_current_root(void)
{
	unsigned long root_ppn = ((unsigned long)sv32_root_pt) >> 12;
	unsigned long satp;

	csr_write(CSR_SATP, SV32_MODE | root_ppn);
	local_sfence_vma();
	__asm__ __volatile__("fence.i" ::: "memory");

	satp = csr_read(CSR_SATP);
	return ((satp & SV32_MODE) == SV32_MODE) ? 0 : -1;
}

static void sv32_disable(void)
{
	csr_write(CSR_SATP, 0);
	local_sfence_vma();
}

static void sv32_prepare_fault(void)
{
	sv32_fault_seen = 0;
	sv32_fault_cause = 0;
	sv32_fault_tval = 0;
	sv32_fault_epc = 0;
	sv32_fault_resume_pc = 0;
	sv32_fault_expected = 1;
}

static int sv32_finish_fault(void)
{
	sv32_fault_expected = 0;
	sv32_fault_resume_pc = 0;

	return sv32_fault_seen ? 0 : -1;
}

static int sv32_finish_no_fault(void)
{
	int fault_seen = sv32_fault_seen;

	sv32_fault_expected = 0;
	sv32_fault_resume_pc = 0;

	return fault_seen ? -1 : 0;
}

static int sv32_load_no_fault(unsigned long addr, unsigned long *val)
{
	sv32_prepare_fault();
	*val = sv32_fault_load_probe(addr);
	return sv32_finish_no_fault();
}

static int sv32_store_no_fault(unsigned long addr, unsigned long val)
{
	sv32_prepare_fault();
	sv32_fault_store_probe(addr, val);
	return sv32_finish_no_fault();
}

static int sv32_exec_no_fault(unsigned long addr)
{
	sv32_prepare_fault();
	sv32_fault_exec_probe(addr);
	return sv32_finish_no_fault();
}

static int test_sv32(int do_fault)
{
	unsigned long old_word;

	sv32_clear_pt(sv32_root_pt);
	sv32_map_identity_megapage();

	if (sv32_enable_current_root())
		return -1;

	old_word = sv32_test_word;
	sv32_test_word = old_word ^ 0x00ff00ffUL;
	if (sv32_test_word != (old_word ^ 0x00ff00ffUL)) {
		sv32_disable();
		return -2;
	}
	sv32_test_word = old_word;

	sv32_identity_exec_probe();

	if (do_fault) {
		sv32_prepare_fault();
		sv32_fault_load_probe(0x1000);
	}

	sv32_disable();

	if (do_fault && sv32_finish_fault())
		return -3;

	return 0;
}

static int test_sv32_extended(void)
{
	unsigned long code_pa = (unsigned long)sv32_identity_exec_probe;
	unsigned long code_page_pa = code_pa & ~0xfffUL;
	unsigned long code_alias = SV32_ALIAS_CODE + (code_pa & 0xfffUL);
	unsigned long saved_sstatus;
	unsigned long val;

	sv32_clear_pt(sv32_root_pt);
	sv32_clear_pt(sv32_l1_pt);
	sv32_map_identity_megapage();
	sv32_map_l1_root(SV32_ALIAS_BASE, sv32_l1_pt);

	sv32_data_page[0] = 0x11223344;
	sv32_data_page2[0] = 0x55667788;
	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_W | SV32_PTE_A | SV32_PTE_D);
	sv32_map_4k(SV32_ALIAS_CODE, code_page_pa,
		    SV32_PTE_X | SV32_PTE_A | SV32_PTE_D);

	sbi_ecall_console_puts("Sv32 ext setup OK\n");
	if (sv32_enable_current_root())
		return -10;
	sbi_ecall_console_puts("Sv32 ext satp OK\n");

	if (sv32_load_no_fault(SV32_ALIAS_DATA, &val)) {
		sv32_disable();
		return -11;
	}
	if (val != 0x11223344) {
		sv32_disable();
		return -12;
	}
	if (sv32_store_no_fault(SV32_ALIAS_DATA, 0x22334455)) {
		sv32_disable();
		return -13;
	}
	if (sv32_data_page[0] != 0x22334455) {
		sv32_disable();
		return -14;
	}
	sbi_ecall_console_puts("Sv32 ext 4K RW OK\n");

	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_A | SV32_PTE_D);
	if (sv32_load_no_fault(SV32_ALIAS_DATA, &val)) {
		sv32_disable();
		return -15;
	}
	if (val != 0x22334455) {
		sv32_disable();
		return -16;
	}
	sv32_prepare_fault();
	sv32_fault_store_probe(SV32_ALIAS_DATA, 0);
	if (sv32_finish_fault()) {
		sv32_disable();
		return -17;
	}
	sbi_ecall_console_puts("Sv32 ext R perm OK\n");

	sv32_prepare_fault();
	sv32_fault_exec_probe(SV32_ALIAS_DATA);
	if (sv32_finish_fault()) {
		sv32_disable();
		return -20;
	}

	if (sv32_exec_no_fault(code_alias)) {
		sv32_disable();
		return -21;
	}
	sbi_ecall_console_puts("Sv32 ext X perm OK\n");

	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_U | SV32_PTE_A | SV32_PTE_D);
	saved_sstatus = csr_read(CSR_SSTATUS);
	csr_write(CSR_SSTATUS, saved_sstatus & ~SSTATUS_SUM);
	sv32_prepare_fault();
	sv32_fault_load_probe(SV32_ALIAS_DATA);
	if (sv32_finish_fault()) {
		csr_write(CSR_SSTATUS, saved_sstatus);
		sv32_disable();
		return -30;
	}
	csr_write(CSR_SSTATUS, saved_sstatus | SSTATUS_SUM);
	if (sv32_load_no_fault(SV32_ALIAS_DATA, &val)) {
		csr_write(CSR_SSTATUS, saved_sstatus);
		sv32_disable();
		return -31;
	}
	if (val != sv32_data_page[0]) {
		csr_write(CSR_SSTATUS, saved_sstatus);
		sv32_disable();
		return -32;
	}
	csr_write(CSR_SSTATUS, saved_sstatus);
	sbi_ecall_console_puts("Sv32 ext U/S SUM OK\n");

	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_W | SV32_PTE_A | SV32_PTE_D);
	if (sv32_store_no_fault(SV32_ALIAS_DATA, 0x01020304)) {
		sv32_disable();
		return -40;
	}
	if (sv32_data_page[0] != 0x01020304) {
		sv32_disable();
		return -41;
	}
	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page2,
		    SV32_PTE_R | SV32_PTE_W | SV32_PTE_A | SV32_PTE_D);
	if (sv32_load_no_fault(SV32_ALIAS_DATA, &val)) {
		sv32_disable();
		return -42;
	}
	if (val != 0x55667788) {
		sv32_disable();
		return -43;
	}
	if (sv32_store_no_fault(SV32_ALIAS_DATA, 0x99aabbcc)) {
		sv32_disable();
		return -44;
	}
	if (sv32_data_page2[0] != 0x99aabbcc ||
	    sv32_data_page[0] != 0x01020304) {
		sv32_disable();
		return -45;
	}
	sbi_ecall_console_puts("Sv32 ext sfence remap OK\n");

	sv32_disable();
	return 0;
}

static int test_sv32_strict(void)
{
	unsigned long code_pa = (unsigned long)sv32_identity_exec_probe;
	unsigned long code_page_pa = code_pa & ~0xfffUL;
	unsigned long code_alias = SV32_ALIAS_CODE + (code_pa & 0xfffUL);
	unsigned long saved_sstatus;
	unsigned long val;
	unsigned long pte;
	unsigned long root_ppn;
	unsigned long satp;

	sv32_strict_ad_load = 0;
	sv32_strict_ad_store = 0;
	sv32_strict_asid = 0;

	sv32_clear_pt(sv32_root_pt);
	sv32_clear_pt(sv32_l1_pt);
	sv32_map_identity_megapage();
	sv32_map_l1_root(SV32_ALIAS_BASE, sv32_l1_pt);

	sv32_data_page[0] = 0x13572468;
	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_W | SV32_PTE_A | SV32_PTE_D);
	sv32_map_4k(SV32_ALIAS_CODE, code_page_pa,
		    SV32_PTE_X | SV32_PTE_A | SV32_PTE_D);
	sv32_root_pt[SV32_BAD_MEGA >> 22] =
		((((unsigned long)sv32_data_page + 0x1000) >> 12) << 10) |
		SV32_PTE_FLAGS;

	if (sv32_enable_current_root())
		return -70;

	sv32_prepare_fault();
	sv32_fault_load_probe(SV32_ALIAS_DATA);
	if (sv32_finish_fault()) {
		sv32_disable();
		return -71;
	}

	sv32_prepare_fault();
	sv32_fault_load_probe(SV32_BAD_MEGA);
	if (sv32_finish_fault()) {
		sv32_disable();
		return -72;
	}

	saved_sstatus = csr_read(CSR_SSTATUS);
	csr_write(CSR_SSTATUS, saved_sstatus & ~SSTATUS_MXR);
	sv32_prepare_fault();
	sv32_fault_load_probe(code_alias);
	if (sv32_finish_fault()) {
		csr_write(CSR_SSTATUS, saved_sstatus);
		sv32_disable();
		return -73;
	}
	csr_write(CSR_SSTATUS, saved_sstatus | SSTATUS_MXR);
	if (sv32_load_no_fault(code_alias, &val)) {
		csr_write(CSR_SSTATUS, saved_sstatus);
		sv32_disable();
		return -74;
	}
	csr_write(CSR_SSTATUS, saved_sstatus);

	sv32_map_4k(SV32_ALIAS_CODE, code_page_pa,
		    SV32_PTE_X | SV32_PTE_U | SV32_PTE_A | SV32_PTE_D);
	sv32_prepare_fault();
	sv32_fault_exec_probe(code_alias);
	if (sv32_finish_fault()) {
		sv32_disable();
		return -75;
	}

	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_W);
	sv32_prepare_fault();
	sv32_fault_load_probe(SV32_ALIAS_DATA);
	pte = sv32_l1_pt[(SV32_ALIAS_DATA >> 12) & 0x3ff];
	if (sv32_fault_seen) {
		sv32_strict_ad_load = 1;
		sv32_finish_fault();
	} else {
		sv32_finish_no_fault();
		if (!(pte & SV32_PTE_A)) {
			sv32_disable();
			return -76;
		}
		sv32_strict_ad_load = 2;
	}

	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_W | SV32_PTE_A);
	sv32_prepare_fault();
	sv32_fault_store_probe(SV32_ALIAS_DATA, 0x24681357);
	pte = sv32_l1_pt[(SV32_ALIAS_DATA >> 12) & 0x3ff];
	if (sv32_fault_seen) {
		sv32_strict_ad_store = 1;
		sv32_finish_fault();
	} else {
		sv32_finish_no_fault();
		if (!(pte & SV32_PTE_D)) {
			sv32_disable();
			return -77;
		}
		sv32_strict_ad_store = 2;
	}

	root_ppn = ((unsigned long)sv32_root_pt) >> 12;
	csr_write(CSR_SATP, SV32_MODE | (1UL << SV32_ASID_SHIFT) | root_ppn);
	local_sfence_vma();
	satp = csr_read(CSR_SATP);
	sv32_strict_asid = (satp >> SV32_ASID_SHIFT) & SV32_ASID_MASK;
	csr_write(CSR_SATP, SV32_MODE | root_ppn);
	local_sfence_vma();

	sv32_disable();
	return 0;
}

void test_rfence_secondary_main(void)
{
	unsigned long val;
	struct sbiret ret;

	csr_write(CSR_STVEC, (unsigned long)s_trap_entry);

	rfence_err = 0;
	if (sv32_enable_current_root()) {
		rfence_err = 1;
		goto done;
	}
	if (sv32_load_no_fault(SV32_ALIAS_DATA, &val)) {
		rfence_err = 2;
		goto done_disable;
	}
	rfence_before = val;
	rfence_ready = 1;

	while (!rfence_cmd)
		__asm__ __volatile__("" ::: "memory");

	if (sv32_load_no_fault(SV32_ALIAS_DATA, &val)) {
		rfence_err = 3;
		goto done_disable;
	}
	rfence_after = val;

done_disable:
	sv32_disable();
done:
	rfence_ready = 2;
	ret = sbi_ecall_hart_stop();
	rfence_err = ret.error ? 4 : rfence_err;

	while (1)
		__asm__ __volatile__("wfi");
}

static int test_sv32_rfence(void)
{
	struct sbiret ret;

	rfence_cmd = 0;
	rfence_ready = 0;
	rfence_before = 0;
	rfence_after = 0;
	rfence_err = 0;

	sv32_clear_pt(sv32_root_pt);
	sv32_clear_pt(sv32_l1_pt);
	sv32_map_identity_megapage();
	sv32_map_l1_root(SV32_ALIAS_BASE, sv32_l1_pt);

	sv32_data_page[0] = 0x0badc0de;
	sv32_data_page2[0] = 0x00c0ffee;
	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page,
		    SV32_PTE_R | SV32_PTE_W | SV32_PTE_A | SV32_PTE_D);

	sbi_ecall_console_puts("RFENCE setup OK\n");
	ret = sbi_ecall_hart_start(1,
				   (unsigned long)test_rfence_secondary_entry,
				   0);
	if (ret.error)
		return -50;
	sbi_ecall_console_puts("RFENCE hart1 start OK\n");

	for (unsigned long i = 0; i < 200000000 && rfence_ready != 1; i++)
		__asm__ __volatile__("" ::: "memory");
	if (rfence_ready != 1)
		return -51;
	if (rfence_err)
		return -52;
	if (rfence_before != 0x0badc0de)
		return -53;
	sbi_ecall_console_puts("RFENCE hart1 ready\n");

	sv32_map_4k(SV32_ALIAS_DATA, (unsigned long)sv32_data_page2,
		    SV32_PTE_R | SV32_PTE_W | SV32_PTE_A | SV32_PTE_D);
	sbi_ecall_console_puts("RFENCE remap done\n");

	ret = sbi_ecall_remote_sfence_vma(1, 1, SV32_ALIAS_DATA, 4096);
	if (ret.error)
		return -54;
	sbi_ecall_console_puts("RFENCE ecall returned\n");

	rfence_cmd = 1;
	for (unsigned long i = 0; i < 200000000 && rfence_ready != 2; i++)
		__asm__ __volatile__("" ::: "memory");
	if (rfence_ready != 2)
		return -55;
	if (rfence_err)
		return -56;
	if (rfence_after != 0x00c0ffee)
		return -57;
	sbi_ecall_console_puts("RFENCE hart1 observed remap\n");

	return 0;

}

void test_secondary_main(void)
{
	struct sbiret ret;

	sbi_ecall_console_puts("Secondary hart running\n");
	secondary_seen = 1;

	ret = sbi_ecall_hart_stop();
	sbi_ecall_console_puts("Secondary hart_stop returned err=");
	test_put_hex(ret.error);

	while (1)
		__asm__ __volatile__("wfi");
}

void test_main(unsigned long a0, unsigned long a1)
{
	struct sbiret ret;

	sbi_ecall_console_puts("\nTest payload running\n");

	secondary_seen = 0;
	ret = sbi_ecall_hart_get_status(1);
	sbi_ecall_console_puts("HSM hart1 status before start=");
	test_put_hex(ret.value);

	ret = sbi_ecall_hart_start(1, (unsigned long)test_secondary_entry, 0);

	if (!ret.error) {
		for (unsigned long i = 0;
		     i < 200000000 && secondary_seen != 1; i++)
			__asm__ __volatile__("" ::: "memory");
	}

	sbi_ecall_console_puts("HSM hart1 start err=");
	test_put_hex(ret.error);

	if (ret.error) {
		sbi_ecall_console_puts("HSM hart1 start failed\n");
	} else if (secondary_seen == 1) {
		sbi_ecall_console_puts("HSM hart1 start observed\n");
	} else {
		sbi_ecall_console_puts("HSM hart1 start timeout\n");
	}

	for (unsigned long i = 0; i < 200000000; i++) {
		ret = sbi_ecall_hart_get_status(1);
		if (ret.value == 1)
			break;
		__asm__ __volatile__("" ::: "memory");
	}
	sbi_ecall_console_puts("HSM hart1 status after stop=");
	test_put_hex(ret.value);

	sbi_ecall_console_puts("Sv32 identity test start\n");
	ret.error = test_sv32(0);
	if (!ret.error) {
		sbi_ecall_console_puts("Sv32 identity test OK\n");
	} else {
		sbi_ecall_console_puts("Sv32 identity test failed err=");
		test_put_hex(ret.error);
	}

	sbi_ecall_console_puts("Sv32 fault test start\n");
	csr_write(CSR_STVEC, (unsigned long)s_trap_entry);
	ret.error = test_sv32(1);
	if (!ret.error) {
		sbi_ecall_console_puts("Sv32 fault test OK\n");
	} else {
		sbi_ecall_console_puts("Sv32 fault test failed err=");
		test_put_hex(ret.error);
		sbi_ecall_console_puts("Sv32 fault cause=");
		test_put_hex(sv32_fault_cause);
		sbi_ecall_console_puts("Sv32 fault tval=");
		test_put_hex(sv32_fault_tval);
	}

	sbi_ecall_console_puts("Sv32 extended test start\n");
	ret.error = test_sv32_extended();
	if (!ret.error) {
		sbi_ecall_console_puts("Sv32 extended test OK\n");
	} else {
		sbi_ecall_console_puts("Sv32 extended test failed err=");
		test_put_hex(ret.error);
		sbi_ecall_console_puts("Sv32 fault cause=");
		test_put_hex(sv32_fault_cause);
		sbi_ecall_console_puts("Sv32 fault tval=");
		test_put_hex(sv32_fault_tval);
	}

	sbi_ecall_console_puts("Sv32 strict test start\n");
	ret.error = test_sv32_strict();
	if (!ret.error) {
		sbi_ecall_console_puts("Sv32 strict test OK\n");
		sbi_ecall_console_puts("Sv32 AD load mode=");
		test_put_hex(sv32_strict_ad_load);
		sbi_ecall_console_puts("Sv32 AD store mode=");
		test_put_hex(sv32_strict_ad_store);
		sbi_ecall_console_puts("Sv32 ASID readback=");
		test_put_hex(sv32_strict_asid);
	} else {
		sbi_ecall_console_puts("Sv32 strict test failed err=");
		test_put_hex(ret.error);
		sbi_ecall_console_puts("Sv32 fault cause=");
		test_put_hex(sv32_fault_cause);
		sbi_ecall_console_puts("Sv32 fault tval=");
		test_put_hex(sv32_fault_tval);
	}

	sbi_ecall_console_puts("Sv32 RFENCE test start\n");
	ret.error = test_sv32_rfence();
	if (!ret.error) {
		sbi_ecall_console_puts("Sv32 RFENCE test OK\n");
	} else {
		sbi_ecall_console_puts("Sv32 RFENCE test failed err=");
		test_put_hex(ret.error);
		sbi_ecall_console_puts("RFENCE before=");
		test_put_hex(rfence_before);
		sbi_ecall_console_puts("RFENCE after=");
		test_put_hex(rfence_after);
		sbi_ecall_console_puts("RFENCE worker err=");
		test_put_hex(rfence_err);
	}

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
