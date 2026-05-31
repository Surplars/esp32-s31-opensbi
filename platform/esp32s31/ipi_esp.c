/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 IPI (Inter-Processor Interrupt) driver for OpenSBI.
 *
 * ESP32-S31 has no standard CLINT, so we emulate software interrupts (msip)
 * using HP_SYSTEM CPU_INT_FROM_CPU trigger registers routed into CLIC.
 *
 * Strategy:
 * - Use a dedicated CLIC interrupt line (IPI_CLIC_INTNUM) for software IPI
 * - Route "CPU_INTR_FROM_CPU" sources to this CLIC line via interrupt matrix
 * - set_msip: assert HP_SYSTEM CPU_INT_FROM_CPU for the target hart
 * - clear_msip: clear HP_SYSTEM CPU_INT_FROM_CPU and local CLIC pending bit
 * - read_msip: read HP_SYSTEM CPU_INT_FROM_CPU state
 *
 * For multi-core, we use separate CPU-to-CPU interrupt sources per hart:
 *   Source 0 (CPU_INTR_FROM_CPU_0) -> hart 0's IPI CLIC line
 *   Source 1 (CPU_INTR_FROM_CPU_1) -> hart 1's IPI CLIC line
 */

#include <sbi/sbi_types.h>
#include <sbi/sbi_ipi.h>
#include <sbi/riscv_barrier.h>
#include "platform_def.h"

/* Extern CLIC functions */
extern void clic_esp_clear_pending(int irq);
extern void clic_esp_route_interrupt_to_hart(int hart_id, int intr_src,
					     int cpu_int_num);

static inline u32 esp_current_hartid(void)
{
	u32 hartid;

	asm volatile("csrr %0, mhartid" : "=r"(hartid));
	return hartid;
}

static inline void esp_cpu_intr_write(int hart_id, u32 val)
{
	*(volatile u32 *)(ESP32S31_HP_SYSTEM_BASE +
			  ESP32S31_HP_SYSTEM_CPU_INT_FROM_CPU(hart_id)) = val;
}

static inline u32 esp_cpu_intr_read(int hart_id)
{
	return *(volatile u32 *)(ESP32S31_HP_SYSTEM_BASE +
				 ESP32S31_HP_SYSTEM_CPU_INT_FROM_CPU(hart_id));
}

static void ipi_esp_route_hart(int hart_id)
{
	clic_esp_route_interrupt_to_hart(hart_id,
					 ESP32S31_CPU_INTR_FROM_CPU_0 + hart_id,
					 ESP32S31_IPI_CLIC_INTNUM);
}

int ipi_esp_init(void)
{
	int i;

	/*
	 * Route CPU-to-CPU interrupt sources to the IPI CLIC line on each hart.
	 * For hart i, route source ESP32S31_CPU_INTR_FROM_CPU_i to
	 * ESP32S31_IPI_CLIC_INTNUM on hart i.
	 *
	 * On a dual-core system, we need to set up routing for both cores.
	 * The interrupt matrix has separate registers per core:
	 *   Core 0: INTR_CORE0_BASE + 4 * source
	 *   Core 1: INTR_CORE1_BASE + 4 * source
	 */
	for (i = 0; i < ESP32S31_HART_COUNT; i++) {
		/* Route CPU_INTR_FROM_CPU_i to IPI_CLIC_INTNUM on hart i. */
		ipi_esp_route_hart(i);
		esp_cpu_intr_write(i, 0);
	}

	return 0;
}

void ipi_esp_send(int hart_id)
{
	if (hart_id < 0 || hart_id >= ESP32S31_HART_COUNT)
		return;

	/*
	 * IDF treats CPU_INT_FROM_CPU_n as a per-target-core crosscore
	 * interrupt latch. Re-arm it before asserting so HSM hart_start gets a
	 * fresh wakeup even if the previous raw IPI was left asserted.
	 */
	ipi_esp_route_hart(hart_id);
	esp_cpu_intr_write(hart_id, 0);
	wmb();
	esp_cpu_intr_write(hart_id, ESP32S31_HP_SYSTEM_CPU_INT_FROM_CPU_BIT);
}

void ipi_esp_clear(void)
{
	u32 hart_id = esp_current_hartid();

	if (hart_id < ESP32S31_HART_COUNT)
		esp_cpu_intr_write(hart_id, 0);
	clic_esp_clear_pending(ESP32S31_IPI_CLIC_INTNUM);
}

int ipi_esp_read(int hart_id)
{
	if (hart_id < 0 || hart_id >= ESP32S31_HART_COUNT)
		return 0;

	return (esp_cpu_intr_read(hart_id) &
		ESP32S31_HP_SYSTEM_CPU_INT_FROM_CPU_BIT) != 0;
}
