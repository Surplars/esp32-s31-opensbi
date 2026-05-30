/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 UART driver for OpenSBI console.
 */

#include <sbi/sbi_types.h>
#include "platform_def.h"

/* Register access helpers */
static inline void reg_write(unsigned long addr, u32 val)
{
	*(volatile u32 *)addr = val;
}

static inline u32 reg_read(unsigned long addr)
{
	return *(volatile u32 *)addr;
}

static unsigned long uart_base;

int uart_esp_init(unsigned long base, u32 input_freq, u32 baudrate)
{
	u32 clkdiv;

	uart_base = base;

	/* Set baud rate: CLKDIV = input_freq / baudrate - 1 */
	clkdiv = (input_freq + baudrate / 2) / baudrate - 1;
	reg_write(uart_base + ESP32S31_UART_CLKDIV, clkdiv & 0xFFF);

	/* Reset FIFOs */
	reg_write(uart_base + ESP32S31_UART_CONF0,
		  reg_read(uart_base + ESP32S31_UART_CONF0) | (1 << 23) | (1 << 22));
	reg_write(uart_base + ESP32S31_UART_CONF0,
		  reg_read(uart_base + ESP32S31_UART_CONF0) & ~((1 << 23) | (1 << 22)));

	return 0;
}

void uart_esp_putc(char ch)
{
	if (!uart_base)
		return;

	/* Wait for TX FIFO space */
	while (((reg_read(uart_base + ESP32S31_UART_STATUS) &
		 ESP32S31_UART_TXFIFO_CNT_M) >> ESP32S31_UART_TXFIFO_CNT_S) >= 128)
		;

	reg_write(uart_base + ESP32S31_UART_FIFO, (u32)ch);
}

int uart_esp_getc(void)
{
	u32 status;

	if (!uart_base)
		return -1;

	status = reg_read(uart_base + ESP32S31_UART_STATUS);
	if (((status & ESP32S31_UART_RXFIFO_CNT_M) >> ESP32S31_UART_RXFIFO_CNT_S) == 0)
		return -1;

	return (int)(reg_read(uart_base + ESP32S31_UART_FIFO) & 0xFF);
}
