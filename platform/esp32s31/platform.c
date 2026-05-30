/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 platform support for OpenSBI.
 *
 * ESP32-S31 is a dual-core RV32IMAFC SoC with:
 * - CLIC interrupt controller (not PLIC/CLINT)
 * - ESP custom UART (not 16550)
 * - TimerG hardware timers
 * - 512KB SRAM at 0x2F000000
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_const.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_system.h>

#include "platform_def.h"

/* External driver functions */
extern int uart_esp_init(unsigned long base, u32 input_freq, u32 baudrate);
extern void uart_esp_putc(char ch);
extern int uart_esp_getc(void);

extern int clic_esp_init(void);
extern int clic_esp_enable_irq(int irq);
extern int clic_esp_set_priority(int irq, int priority);
extern void clic_esp_route_interrupt(int intr_src, int cpu_int_num);

extern int timer_esp_init(unsigned long base, u32 freq);
extern u64 timer_esp_get_mtime(void);
extern void timer_esp_set_mtimecmp(u64 value);
extern void timer_esp_clear_irq(void);
extern u32 timer_esp_get_freq(void);

extern int ipi_esp_init(void);
extern void ipi_esp_send(int hart_id);
extern void ipi_esp_clear(void);

static void esp_raw_uart_putc(char ch)
{
	while (((*(volatile u32 *)(ESP32S31_UART0_BASE + ESP32S31_UART_STATUS) &
		 ESP32S31_UART_TXFIFO_CNT_M) >> ESP32S31_UART_TXFIFO_CNT_S) >= 128)
		;

	*(volatile u32 *)(ESP32S31_UART0_BASE + ESP32S31_UART_FIFO) = (u32)ch;
}

static void esp_raw_uart_puts(const char *str)
{
	while (*str) {
		if (*str == '\n')
			esp_raw_uart_putc('\r');
		esp_raw_uart_putc(*str++);
	}
}

/* Console device */
static void esp_console_putc(char ch)
{
	uart_esp_putc(ch);
}

static int esp_console_getc(void)
{
	return uart_esp_getc();
}

static struct sbi_console_device esp_console = {
	.name		= "esp32s31-uart",
	.console_putc	= esp_console_putc,
	.console_getc	= esp_console_getc,
};

/* Timer device */
static u64 esp_timer_value(void)
{
	return timer_esp_get_mtime();
}

static void esp_timer_event_start(u64 next_event)
{
	timer_esp_clear_irq();
	timer_esp_set_mtimecmp(next_event);
}

static void esp_timer_event_stop(void)
{
	/* Set mtimecmp to max value to stop events */
	timer_esp_set_mtimecmp(~0ULL);
	timer_esp_clear_irq();
}

static int esp_timer_warm_init(void)
{
	return 0;
}

static struct sbi_timer_device esp_timer = {
	.name		= "esp32s31-timer",
	.timer_value	= esp_timer_value,
	.timer_event_start = esp_timer_event_start,
	.timer_event_stop = esp_timer_event_stop,
	.warm_init	= esp_timer_warm_init,
};

/* IPI device */
static void esp_ipi_send(u32 hart_index)
{
	ipi_esp_send(hart_index);
}

static void esp_ipi_clear(void)
{
	ipi_esp_clear();
}

static struct sbi_ipi_device esp_ipi = {
	.name		= "esp32s31-ipi",
	.rating		= 1,
	.ipi_send	= esp_ipi_send,
	.ipi_clear	= esp_ipi_clear,
};

/* System reset device */
static int esp_system_reset_check(u32 reset_type, u32 reset_reason)
{
	if (reset_reason != SBI_SRST_RESET_REASON_NONE &&
	    reset_reason != SBI_SRST_RESET_REASON_SYSFAIL)
		return 0;

	switch (reset_type) {
	case SBI_SRST_RESET_TYPE_SHUTDOWN:
	case SBI_SRST_RESET_TYPE_COLD_REBOOT:
	case SBI_SRST_RESET_TYPE_WARM_REBOOT:
		return 255;
	default:
		return 0;
	}
}

static void esp_system_reset(u32 reset_type, u32 reset_reason)
{
	(void)reset_reason;

	esp_raw_uart_puts("[esp32s31-reset]\n");

	if (reset_type == SBI_SRST_RESET_TYPE_COLD_REBOOT ||
	    reset_type == SBI_SRST_RESET_TYPE_WARM_REBOOT) {
		volatile u32 *sys_ctrl =
			(volatile u32 *)(ESP32S31_LP_SYS_BASE +
					 ESP32S31_LP_SYSTEM_SYS_CTRL);
		volatile u32 *core0_reset =
			(volatile u32 *)(ESP32S31_LP_AONCLKRST_BASE +
					 ESP32S31_LP_AONCLKRST_HPCORE0_RESET_CTRL);

		*sys_ctrl |= ESP32S31_LP_SYSTEM_SYS_SW_RST;
		*core0_reset |= ESP32S31_LP_AONCLKRST_HPCORE_SW_RESET;
	}

	while (1)
		wfi();
}

static struct sbi_system_reset_device esp_reset = {
	.name			= "esp32s31-reset",
	.system_reset_check	= esp_system_reset_check,
	.system_reset		= esp_system_reset,
};

/* Platform operations */
static bool esp_cold_boot_allowed(u32 hartid)
{
	/* Only hart 0 does cold boot */
	return hartid == 0;
}

static int esp_extensions_init(struct sbi_hart_features *hfeatures)
{
	hfeatures->priv_version = SBI_HART_PRIV_VER_1_10;
	__set_bit(SBI_HART_EXT_ZICNTR, hfeatures->extensions);
	__set_bit(SBI_HART_CSR_CYCLE, hfeatures->csrs);
	__set_bit(SBI_HART_CSR_TIME, hfeatures->csrs);
	__set_bit(SBI_HART_CSR_INSTRET, hfeatures->csrs);

	return 0;
}

static int esp_nascent_init(void)
{
	uart_esp_init(ESP32S31_UART0_BASE,
		      ESP32S31_UART_INPUT_FREQ,
		      ESP32S31_UART_BAUDRATE);
	return 0;
}

static int esp_early_init(bool cold_boot)
{
	int rc;

	if (!cold_boot)
		return 0;

	/* Initialize UART for console output */
	rc = uart_esp_init(ESP32S31_UART0_BASE,
			   ESP32S31_UART_INPUT_FREQ,
			   ESP32S31_UART_BAUDRATE);
	if (rc)
		return rc;

	/* Register console device */
	sbi_console_set_device(&esp_console);

	sbi_system_reset_add_device(&esp_reset);

	/* Initialize IPI via CLIC software interrupts */
	rc = ipi_esp_init();
	if (rc)
		return rc;

	/* Register IPI device so OpenSBI core can use it */
	sbi_ipi_add_device(&esp_ipi);

	return 0;
}

static int esp_final_init(bool cold_boot)
{
	if (!cold_boot)
		return 0;

	return 0;
}

static int esp_irqchip_init(void)
{
	/*
	 * Initialize CLIC as the interrupt controller.
	 * OpenSBI handles external interrupts via the irqchip interface.
	 * For CLIC-based platforms, we initialize the CLIC and enable
	 * the external interrupt line.
	 */
	clic_esp_init();

	/*
	 * Enable external interrupt (IRQ 11 on CLIC) with medium priority.
	 * This is the interrupt line where peripheral interrupts are routed.
	 */
	clic_esp_set_priority(11, 4);
	clic_esp_enable_irq(11);

	/* Route SYSTIMER target0 alarm to the CLIC line handled as M-timer. */
	clic_esp_route_interrupt(ESP32S31_SYSTIMER_TARGET0_INTR_SOURCE,
				 ESP32S31_TIMER_CLIC_INTNUM);
	clic_esp_set_priority(ESP32S31_TIMER_CLIC_INTNUM, 4);
	clic_esp_enable_irq(ESP32S31_TIMER_CLIC_INTNUM);

	return 0;
}

static int esp_timer_init(void)
{
	int rc;

	/* Initialize TimerG0 */
	rc = timer_esp_init(ESP32S31_TIMERG0_BASE, ESP32S31_TIMER_FREQ);
	if (rc)
		return rc;

	/* Register timer device */
	esp_timer.timer_freq = timer_esp_get_freq();
	sbi_timer_set_device(&esp_timer);

	return 0;
}

/* Platform descriptor */
const struct sbi_platform_operations platform_ops = {
	.cold_boot_allowed	= esp_cold_boot_allowed,
	.nascent_init		= esp_nascent_init,
	.extensions_init	= esp_extensions_init,
	.early_init		= esp_early_init,
	.final_init		= esp_final_init,
	.irqchip_init		= esp_irqchip_init,
	.timer_init		= esp_timer_init,
};

const struct sbi_platform platform = {
	.opensbi_version	= OPENSBI_VERSION,
	.platform_version	= SBI_PLATFORM_VERSION(0x01, 0x00),
	.name			= "Espressif ESP32-S31",
	.features		= SBI_PLATFORM_DEFAULT_FEATURES,
	.hart_count		= ESP32S31_HART_COUNT,
	.hart_stack_size	= SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
	.heap_size		= SBI_PLATFORM_DEFAULT_HEAP_SIZE(ESP32S31_HART_COUNT),
	.platform_ops_addr	= (unsigned long)&platform_ops,
};
