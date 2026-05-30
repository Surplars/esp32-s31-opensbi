/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 Timer driver for OpenSBI.
 *
 * Uses SYSTIMER target0 as the compare source. This keeps SBI TIME's
 * absolute timestamp in the same time domain as the CPU time CSR.
 */

#include <sbi/sbi_types.h>
#include <sbi/sbi_timer.h>
#include "platform_def.h"

static unsigned long timg_base;
static u32 timer_freq;

static inline void timer_write(unsigned long offset, u32 val)
{
	*(volatile u32 *)(timg_base + offset) = val;
}

static inline u32 timer_read(unsigned long offset)
{
	return *(volatile u32 *)(timg_base + offset);
}

static inline u32 reg_read(unsigned long addr)
{
	return *(volatile u32 *)addr;
}

static inline void reg_write(unsigned long addr, u32 val)
{
	*(volatile u32 *)addr = val;
}

static inline unsigned long timer_clic_ctrl(void)
{
	return ESP32S31_CLIC_CTRL_BASE + 4 * ESP32S31_TIMER_CLIC_INTNUM;
}

static inline void timer_clic_disable_clear(void)
{
	*(volatile u8 *)(timer_clic_ctrl() + 1) = 0;
	*(volatile u8 *)(timer_clic_ctrl() + 0) = 0;
}

static inline void timer_clic_enable(void)
{
	*(volatile u8 *)(timer_clic_ctrl() + 0) = 0;
	*(volatile u8 *)(timer_clic_ctrl() + 1) = 1;
}

static u64 rdtime64(void)
{
	u32 lo, hi, tmp;

	__asm__ __volatile__("1:\n"
			     "rdtimeh %0\n"
			     "rdtime %1\n"
			     "rdtimeh %2\n"
			     "bne %0, %2, 1b"
			     : "=&r"(hi), "=&r"(lo), "=&r"(tmp));
	return ((u64)hi << 32) | lo;
}

static u64 muldiv64(u64 value, u32 mul, u32 div)
{
	u64 q = value / div;
	u64 r = value % div;

	return q * mul + (r * mul) / div;
}

static u64 systimer_read_unit0(void)
{
	u32 lo, hi;

	timer_write(ESP32S31_SYSTIMER_UNIT0_OP,
		    ESP32S31_SYSTIMER_UNIT0_UPDATE);
	for (int i = 0; i < 1000; i++) {
		if (timer_read(ESP32S31_SYSTIMER_UNIT0_OP) &
		    ESP32S31_SYSTIMER_UNIT0_VALUE_VALID)
			break;
	}

	hi = timer_read(ESP32S31_SYSTIMER_UNIT0_VALUE_HI);
	lo = timer_read(ESP32S31_SYSTIMER_UNIT0_VALUE_LO);
	return ((u64)hi << 32) | lo;
}

int timer_esp_init(unsigned long base, u32 freq)
{
	u32 clkrst;

	(void)base;
	timg_base = ESP32S31_SYSTIMER_BASE;
	timer_freq = freq;

	clkrst = reg_read(ESP32S31_HP_SYS_CLKRST_BASE +
			  ESP32S31_HP_SYS_CLKRST_SYSTIMER_CTRL0);
	clkrst |= ESP32S31_HP_SYS_CLKRST_SYSTIMER_APB_CLK_EN |
		  ESP32S31_HP_SYS_CLKRST_SYSTIMER_CLK_EN |
		  ESP32S31_HP_SYS_CLKRST_SYSTIMER_FORCE_NORST;
	clkrst &= ~ESP32S31_HP_SYS_CLKRST_SYSTIMER_RST_EN;
	reg_write(ESP32S31_HP_SYS_CLKRST_BASE +
		  ESP32S31_HP_SYS_CLKRST_SYSTIMER_CTRL0, clkrst);

	timer_write(ESP32S31_SYSTIMER_INT_CLR,
		    ESP32S31_SYSTIMER_TARGET0_INT);
	timer_write(ESP32S31_SYSTIMER_INT_ENA,
		    ESP32S31_SYSTIMER_TARGET0_INT);
	timer_write(ESP32S31_SYSTIMER_TARGET0_CONF, 0);
	timer_write(ESP32S31_SYSTIMER_CONF,
		    timer_read(ESP32S31_SYSTIMER_CONF) |
		    ESP32S31_SYSTIMER_CLK_EN |
		    ESP32S31_SYSTIMER_UNIT0_WORK_EN);

	return 0;
}

u64 timer_esp_get_mtime(void)
{
	return rdtime64();
}

void timer_esp_set_mtimecmp(u64 value)
{
	u32 conf;
	u64 now;
	u64 systimer_now;
	u64 delta;
	u64 target;

	now = rdtime64();
	systimer_now = systimer_read_unit0();
	delta = value > now ? value - now : 1;
	target = systimer_now + muldiv64(delta, ESP32S31_SYSTIMER_FREQ,
					 ESP32S31_TIME_CSR_FREQ);

	conf = timer_read(ESP32S31_SYSTIMER_CONF);
	timer_clic_disable_clear();
	timer_write(ESP32S31_SYSTIMER_CONF,
		    conf & ~ESP32S31_SYSTIMER_TARGET0_WORK_EN);
	timer_write(ESP32S31_SYSTIMER_INT_CLR,
		    ESP32S31_SYSTIMER_TARGET0_INT);

	timer_write(ESP32S31_SYSTIMER_TARGET0_HI, (u32)(target >> 32));
	timer_write(ESP32S31_SYSTIMER_TARGET0_LO, (u32)target);
	timer_write(ESP32S31_SYSTIMER_TARGET0_CONF, 0);
	timer_write(ESP32S31_SYSTIMER_COMP0_LOAD_REG,
		    ESP32S31_SYSTIMER_COMP0_LOAD_BIT);

	timer_write(ESP32S31_SYSTIMER_CONF,
		    conf | ESP32S31_SYSTIMER_CLK_EN |
		    ESP32S31_SYSTIMER_UNIT0_WORK_EN |
		    ESP32S31_SYSTIMER_TARGET0_WORK_EN);
	timer_clic_enable();

}

void timer_esp_clear_irq(void)
{
	timer_clic_disable_clear();
	timer_write(ESP32S31_SYSTIMER_INT_CLR,
		    ESP32S31_SYSTIMER_TARGET0_INT);
}

u32 timer_esp_get_freq(void)
{
	return timer_freq;
}
