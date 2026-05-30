/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 CLIC (Core Local Interrupt Controller) driver for OpenSBI.
 *
 * CLIC is a RISC-V interrupt controller that supports:
 * - Up to 48 interrupt sources on ESP32-S31
 * - Per-interrupt priority and enable
 * - Vectored and non-vectored interrupt modes
 * - Level and edge triggering
 *
 * Register layout per interrupt (at CLIC_CTRL_BASE + i*4):
 *   Byte 0: IP (Interrupt Pending)
 *   Byte 1: IE (Interrupt Enable)
 *   Byte 2: ATTR (SHV, TRIG, MODE)
 *   Byte 3: CTL (Priority)
 */

#include <sbi/sbi_types.h>
#include <sbi/riscv_encoding.h>
#include "platform_def.h"

/* Register access helpers */
static inline void clic_write8(unsigned long addr, u8 val)
{
	*(volatile u8 *)addr = val;
}

static inline u8 clic_read8(unsigned long addr)
{
	return *(volatile u8 *)addr;
}

static inline void clic_write32(unsigned long addr, u32 val)
{
	*(volatile u32 *)addr = val;
}

static inline u32 clic_read32(unsigned long addr)
{
	return *(volatile u32 *)addr;
}

/*
 * Per-interrupt control register byte addresses:
 *   IP  = CTRL_BASE + i*4 + 0
 *   IE  = CTRL_BASE + i*4 + 1
 *   ATTR = CTRL_BASE + i*4 + 2
 *   CTL = CTRL_BASE + i*4 + 3
 */
#define CLIC_INT_IP(i)		(ESP32S31_CLIC_CTRL_BASE + (i) * 4 + 0)
#define CLIC_INT_IE(i)		(ESP32S31_CLIC_CTRL_BASE + (i) * 4 + 1)
#define CLIC_INT_ATTR(i)	(ESP32S31_CLIC_CTRL_BASE + (i) * 4 + 2)
#define CLIC_INT_CTL(i)		(ESP32S31_CLIC_CTRL_BASE + (i) * 4 + 3)

/* CLIC configuration register */
#define CLIC_INT_CONFIG_REG	(ESP32S31_CLIC_BASE + 0x0)
#define CLIC_INT_THRESH_REG	(ESP32S31_CLIC_BASE + 0x8)

/* ATTR register bits */
#define CLIC_ATTR_SHV		(1 << 0)	/* Selective Hardware Vectored */
#define CLIC_ATTR_TRIG_LEVEL	0x00		/* Level trigger */
#define CLIC_ATTR_TRIG_RISE	0x02		/* Rising edge trigger */
#define CLIC_ATTR_TRIG_FALL	0x06		/* Falling edge trigger */

int clic_esp_init(void)
{
	int i;

	/*
	 * Initialize CLIC: set all interrupts to non-vectored, disabled,
	 * level-triggered, with lowest priority.
	 */
	for (i = 0; i < ESP32S31_CLIC_INTNUM; i++) {
		/* Disable all interrupts */
		clic_write8(CLIC_INT_IE(i), 0);
		/* Clear all pending bits */
		clic_write8(CLIC_INT_IP(i), 0);
		/* Non-vectored, level-triggered */
		clic_write8(CLIC_INT_ATTR(i), CLIC_ATTR_TRIG_LEVEL);
		/* Lowest priority */
		clic_write8(CLIC_INT_CTL(i), 0);
	}

	/* Set machine-mode interrupt threshold to 0 (allow all priorities) */
	clic_write32(CLIC_INT_THRESH_REG, 0);

	return 0;
}

int clic_esp_enable_irq(int irq)
{
	if (irq < 0 || irq >= ESP32S31_CLIC_INTNUM)
		return -1;

	clic_write8(CLIC_INT_IE(irq), 1);
	return 0;
}

int clic_esp_disable_irq(int irq)
{
	if (irq < 0 || irq >= ESP32S31_CLIC_INTNUM)
		return -1;

	clic_write8(CLIC_INT_IE(irq), 0);
	return 0;
}

int clic_esp_set_priority(int irq, int priority)
{
	u8 ctl;

	if (irq < 0 || irq >= ESP32S31_CLIC_INTNUM)
		return -1;

	/* Priority is stored in upper NLBITS of the CTL byte */
	ctl = (u8)(priority << (8 - ESP32S31_CLIC_NLBITS));
	clic_write8(CLIC_INT_CTL(irq), ctl);
	return 0;
}

int clic_esp_set_vectored(int irq, bool vectored)
{
	u8 attr;

	if (irq < 0 || irq >= ESP32S31_CLIC_INTNUM)
		return -1;

	attr = clic_read8(CLIC_INT_ATTR(irq));
	if (vectored)
		attr |= CLIC_ATTR_SHV;
	else
		attr &= ~CLIC_ATTR_SHV;
	clic_write8(CLIC_INT_ATTR(irq), attr);
	return 0;
}

void clic_esp_set_pending(int irq)
{
	if (irq >= 0 && irq < ESP32S31_CLIC_INTNUM)
		clic_write8(CLIC_INT_IP(irq), 1);
}

void clic_esp_clear_pending(int irq)
{
	if (irq >= 0 && irq < ESP32S31_CLIC_INTNUM)
		clic_write8(CLIC_INT_IP(irq), 0);
}

int clic_esp_is_pending(int irq)
{
	if (irq < 0 || irq >= ESP32S31_CLIC_INTNUM)
		return 0;
	return (clic_read8(CLIC_INT_IP(irq)) & 1) != 0;
}

/*
 * Route an interrupt source to a CPU interrupt line via the interrupt matrix.
 *
 * ESP32-S31 has an interrupt matrix that maps peripheral interrupt sources
 * to CPU CLIC interrupt lines. Each source register is at:
 *   CORE0_BASE + 4 * source_number
 * The lower 6 bits of the register value select the CPU interrupt line.
 */
void clic_esp_route_interrupt(int intr_src, int cpu_int_num)
{
	unsigned long reg_addr = ESP32S31_INTR_CORE0_BASE + 4 * intr_src;
	u32 val;

	val = *(volatile u32 *)reg_addr;
	val &= ~(ESP32S31_INTR_MAP_CPU_INT_MASK |
		 ESP32S31_INTR_MAP_PASS_LEVEL_MASK);
	val |= (cpu_int_num & ESP32S31_INTR_MAP_CPU_INT_MASK) |
	       ESP32S31_INTR_MAP_PASS_LEVEL_M;
	*(volatile u32 *)reg_addr = val;
}
