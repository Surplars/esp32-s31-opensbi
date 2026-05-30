/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 IPI (Inter-Processor Interrupt) driver for OpenSBI.
 *
 * ESP32-S31 has no standard CLINT, so we emulate software interrupts (msip)
 * using CLIC interrupt pending bits.
 *
 * Strategy:
 * - Use a dedicated CLIC interrupt line (IPI_CLIC_INTNUM) for software IPI
 * - Route "CPU_INTR_FROM_CPU" sources to this CLIC line via interrupt matrix
 * - set_msip: set CLIC pending bit for the IPI interrupt on target hart
 * - clear_msip: clear CLIC pending bit
 * - read_msip: read CLIC pending bit
 *
 * For multi-core, we use separate CPU-to-CPU interrupt sources per hart:
 *   Source 0 (CPU_INTR_FROM_CPU_0) → hart 0's IPI CLIC line
 *   Source 1 (CPU_INTR_FROM_CPU_1) → hart 1's IPI CLIC line
 */

#include <sbi/sbi_types.h>
#include <sbi/sbi_ipi.h>
#include "platform_def.h"

/* Extern CLIC functions */
extern void clic_esp_set_pending(int irq);
extern void clic_esp_clear_pending(int irq);
extern int clic_esp_is_pending(int irq);
extern void clic_esp_route_interrupt(int intr_src, int cpu_int_num);

int ipi_esp_init(void)
{
	int i;

	/*
	 * Route CPU-to-CPU interrupt sources to the IPI CLIC line on each hart.
	 * For hart i, route source ESP32S31_CPU_INTR_FROM_CPU_i to
	 * ESP32S31_IPI_CLIC_INTNUM on core 0.
	 *
	 * On a dual-core system, we need to set up routing for both cores.
	 * The interrupt matrix has separate registers per core:
	 *   Core 0: INTR_CORE0_BASE + 4 * source
	 *   Core 1: INTR_CORE1_BASE + 4 * source
	 */
	for (i = 0; i < ESP32S31_HART_COUNT; i++) {
		/* Route CPU_INTR_FROM_CPU_i to IPI_CLIC_INTNUM on core 0 */
		clic_esp_route_interrupt(ESP32S31_CPU_INTR_FROM_CPU_0 + i,
					ESP32S31_IPI_CLIC_INTNUM);
	}

	return 0;
}

void ipi_esp_send(int hart_id)
{
	(void)hart_id;

	/*
	 * Trigger IPI by setting the CLIC pending bit for the IPI interrupt.
	 * In a real multi-core system, this would use the interrupt matrix's
	 * CPU-to-CPU source to trigger on the target core.
	 * For now, we set the pending bit directly.
	 */
	clic_esp_set_pending(ESP32S31_IPI_CLIC_INTNUM);
}

void ipi_esp_clear(void)
{
	clic_esp_clear_pending(ESP32S31_IPI_CLIC_INTNUM);
}

int ipi_esp_read(int hart_id)
{
	return clic_esp_is_pending(ESP32S31_IPI_CLIC_INTNUM);
}
