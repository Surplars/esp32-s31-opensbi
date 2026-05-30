# SPDX-License-Identifier: BSD-2-Clause

platform-cppflags-y =
platform-genflags-y = -DOPENSBI_PLATFORM_ESP32S31_CLIC
platform-genflags-y += -DOPENSBI_PLATFORM_ESP32S31_CLIC_TIMER_IRQ=16

platform-cflags-y = -march=rv32imac_zicsr_zifencei -fno-PIE -fno-pic
platform-asflags-y = -march=rv32imac_zicsr_zifencei

platform-ldflags-y = -no-pie -static

PLATFORM_RISCV_XLEN = 32
PLATFORM_RISCV_ABI = ilp32
PLATFORM_RISCV_ISA = rv32imac_zicsr_zifencei

# ESP32-S31 SRAM base address
FW_TEXT_START=0x2F020000

platform-objs-y += platform.o
platform-objs-y += uart_esp.o
platform-objs-y += clic_esp.o
platform-objs-y += timer_esp.o
platform-objs-y += ipi_esp.o

# FW_JUMP=y
FW_PAYLOAD=y
FW_PAYLOAD_OFFSET=0x30000
ifeq ($(PLATFORM_RISCV_XLEN), 32)
# FW_JUMP_ADDR=0x2F050000
else
# FW_JUMP_ADDR=0x2F050000
endif
