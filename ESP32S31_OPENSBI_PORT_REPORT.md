# ESP32-S31 OpenSBI 移植阶段报告

日期：2026-05-30

## 目标和环境

目标是在 ESP32-S31 上使用自制 Rust second-stage bootloader 取代官方 second-stage bootloader，加载 OpenSBI，并向 OpenSBI 传入启动参数和 DTB。

当前环境：

- bootloader 仓库：`D:\TOOLS\projects\rustProjects\esp-bootloader`
- OpenSBI 仓库：`D:\TOOLS\projects\rustProjects\opensbi`
- ESP-IDF：`D:\TOOLS\ENV\esp\master\esp-idf`
- 调试方式：无 JTAG，主要依赖 UART 字符探针和串口日志
- OpenSBI 编译和烧录：由用户执行

## 当前状态

目前已经验证：

- 自制 bootloader 能正常运行。
- bootloader 能从 flash 读取 OpenSBI 镜像到 SRAM。
- bootloader 能跳转到 OpenSBI M-mode 入口。
- OpenSBI 能完成 platform init、console init、domain init。
- OpenSBI 能进入 payload。
- S-mode payload 能执行，并能通过 SBI DBCN 输出。
- SBI SRST reset device 已注册，payload 调用 reset 能进入平台 reset 路径。
- SBI TIME set_timer 路径已跑通。
- S-mode timer interrupt 已通过 ESP32-S31 专用 CLIC workaround 跑通。

最新已知成功日志片段：

```text
Test payload running
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

这说明以下链路已经成立：

```text
bootloader -> OpenSBI M-mode -> S-mode payload
S-mode payload -> SBI ecall -> OpenSBI
SYSTIMER -> CLIC M-level IRQ -> OpenSBI timer process
OpenSBI -> software redirect S-mode timer trap -> S-mode payload trap entry
S-mode payload -> SBI SRST -> platform reset handler
```

## 当前内存布局

当前稳定布局：

```text
OpenSBI base      : 0x2f020000
FW_PAYLOAD_OFFSET : 0x30000
Payload address   : 0x2f050000
DTB address       : bootloader 内嵌 DTB 地址，作为 a1 传给 OpenSBI
```

OpenSBI 平台配置使用：

```make
FW_TEXT_START=0x2F020000
FW_PAYLOAD=y
FW_PAYLOAD_OFFSET=0x30000
```

bootloader 从 flash `0x00020000` 读取 OpenSBI 镜像到 `0x2f020000`。

## Bootloader 侧发现

### 固定首指令校验不可靠

早期用固定首 word 判断 OpenSBI 是否加载正确，例如期望：

```text
OpenSBI[0] : 0x00050433
```

后来入口加入 UART 探针或链接布局变化后，首 word 变为：

```text
OpenSBI[0] : 0x2038af37
OpenSBI[1] : 0x04100f93
OpenSBI[2] : 0x01ff2023
OpenSBI[3] : 0x00050433
```

结论：不能用单个固定首指令判断加载成功。应打印前几个 word，确认不是全 0、全 `0xffffffff` 或明显错位数据。

### SRAM 空间压力

曾经把 OpenSBI 目标地址移到 `0x2f040000`，或镜像增大到约 260 KiB 后，出现 flash 读取阶段卡住或异常：

```text
Guru Meditation Error: Core 0 panic'ed (Instruction access fault)
PC : 0x00000000
SP : 0x2f05ffb0
```

推测原因：

- OpenSBI、payload、heap、scratch、bootloader 栈、DTB 之间发生重叠。
- ESP32-S31 片内 SRAM 有限，不能随意提高 OpenSBI base。

当前处理：

- OpenSBI base 固定为 `0x2f020000`。
- payload 放在 `0x2f050000`。
- 控制 `fw_payload.bin` 体积。

### DTB size 打印端序

bootloader 打印过：

```text
DTB magic : 0xedfe0dd0
DTB size  : 0x22050000
```

这是 FDT header big-endian 字段未转换导致的显示问题。`0x22050000` 实际对应 `0x00000522`。这不是 OpenSBI 卡住主因。

### APM/PMS/TEE 放权

bootloader 已加入对 ESP32-S31 APM/PMS/TEE 的放权初始化，涉及区域包括：

```text
HP_APM_BASE     = 0x20504400
HP_MEM_APM_BASE = 0x20504800
CPU_APM_BASE    = 0x20504C00
LP_TEE_BASE     = 0x20706800
LP_APM_BASE     = 0x20706C00
```

OpenSBI 能跑到 banner，说明 bootloader 侧基本加载和权限放行已经可用。

### 官方 bootloader init 顺序对齐

当前自制 bootloader 已按 ESP-IDF `components/bootloader_support/src/esp32s31/bootloader_esp32s31.c` 的主顺序组织早期初始化，保留我们自己的 OpenSBI 加载路径：

```text
hardware_init
bootloader_ana_reset_config
bootloader_super_wdt_auto_feed
bootloader_init_mem / PMS / APM / TEE 放行
bootloader_clock_configure
bootloader_console_init
bootloader_enable_cpu_reset_info
bootloader_config_wdt
bootloader_enable_random
load OpenSBI and jump
```

没有照搬官方 flash image/MMU/app 校验路径，因为当前设计仍然是 ROM flash read 直接把 OpenSBI binary 搬到 SRAM 后跳转。

## OpenSBI 侧关键问题

### OpenSBI 官方 init 路径恢复

早期 bring-up 为了绕过卡死点，曾在 `lib/sbi/sbi_init.c` 中直接判断 ESP32-S31 并调用平台函数，例如硬编码 hart0 cold boot、直接调用 `esp_nascent_init()`。这已经整理回 OpenSBI 官方路径：

```text
sbi_init()
  -> sbi_platform_cold_boot_allowed()
  -> sbi_platform_nascent_init()
  -> init_coldboot()
  -> sbi_hart_init()
  -> sbi_platform_extensions_init()
```

ESP32-S31 的特殊逻辑现在放回 `platform/esp32s31/platform.c`：

- `cold_boot_allowed` 只允许 hart0 cold boot。
- `nascent_init` 做早期 UART 初始化。
- `extensions_init` 提供当前 CLIC-safe 的最小 hart feature 集合。

当前仍保留一个 ESP32-S31 gated 的限制：通用 trap-based CSR/PMP probe 尚未 CLIC-safe，所以 `sbi_hart.c` 在调用 `sbi_platform_extensions_init()` 后跳过 unsafe probe body。这个点后续要继续正规化。

### CLIC mtvec/stvec 模式

ESP32-S31 使用 CLIC。参考 ESP-IDF `vectors_clic.S`，CLIC trap entry 的基地址按类似如下规则解码：

```text
TVEC[31:6] << 6
```

因此 CLIC trap entry 必须 64 字节对齐。

M-mode OpenSBI trap 入口需要：

```asm
.balign 64
_trap_handler:
    ...

mtvec = _trap_handler | 3
```

S-mode payload 的 `s_trap_entry` 也必须 64 字节对齐。之前只做 `.align 3` 时，OpenSBI 准备的状态为：

```text
st=2f050083
me=2f050080
```

但 S-mode timer trap 不能正常完成。改成：

```asm
.balign 64
s_trap_entry:
```

后，S-mode timer interrupt 成功完成。这是 S-mode timer 最后一个关键 blocker。

### CLIC mcause 格式

CLIC 下 `mcause` 不只是普通 cause 编号，还包含额外 CLIC 状态字段。ESP-IDF 中说明：

```text
bit31     interrupt
bit30     MINHV
bit29:28  MPP
bit27     MPIE
bit23:16  MPIL
bit11:0   reason
```

OpenSBI 通用 C 层 trap handler 期望标准 cause。因此 ESP32-S31 路径需要把传给 C 层的 cause 标准化：

```text
cause = (mcause & interrupt_bit) | (mcause & 0xfff)
```

这样 U-mode ecall 能识别为 `c=8`，S-mode ecall 能识别为 `c=9`，CLIC timer IRQ 能识别为 `c=0x80000010`。

IDF 还说明，CLIC trap return 前必须恢复原始 `mcause`，因为 `mret` 会从 `mcause` 恢复之前的 interrupt threshold/status。当前 ESP32-S31 路径保存原始 CLIC `mcause`，返回前写回，同时 C 层仍使用标准化 cause。

注意：`mcause` 和 `scause` 的 CLIC 扩展字段布局不同。`mcause` 有 `MPP[29:28]`，而 `scause` 只有 `SPP[28]`。不能把 `mcause[30:16]` 直接拷贝到 `scause`。

### ecall delegation

为了让低特权 payload 的 ecall 回到 OpenSBI M-mode，需要在切换模式前清理对应 delegation：

- 切到 U-mode：清 `MEDELEG[CAUSE_USER_ECALL]`
- 切到 S-mode：清 `MEDELEG[CAUSE_SUPERVISOR_ECALL]`

当前日志中：

```text
Boot HART MEDELEG : 0x0000b100
```

S-mode ecall 仍能进入 M-mode，说明当前清理路径已经生效。

## SBI TIME 和 timer 频率

ESP-IDF 明确说明 ESP32-S31 SYSTIMER 时钟为：

```text
XTAL 40MHz / 2.5 = 16MHz
```

串口日志反推 `rdtime` 增量约为 SYSTIMER 增量的 2.5 倍。因此：

```text
SYSTIMER frequency : 16 MHz
time CSR frequency : 40 MHz
```

早期把 `ESP32S31_TIME_CSR_FREQ` 猜成 48 MHz 是错误的。症状是 SYSTIMER compare 已经触发，但 `sbi_timer_value()` 还没达到 S-mode event 的目标时间，导致同一个 S-mode timer event 被反复 reprogram，并连续进入 M-level CLIC timer IRQ：

```text
{c=80000010,e=2f050080,...}
{c=80000010,e=2f050084,...}
```

当前修正：

```c
#define ESP32S31_SYSTIMER_FREQ 16000000
#define ESP32S31_TIME_CSR_FREQ 40000000
```

## 为什么标准 OpenSBI STIP 路径不可用

标准 OpenSBI timer delivery 依赖：

```text
MIP.STIP
MIDELEG.STIP
S-mode interrupt CSRs
```

ESP32-S31 当前表现：

```text
MIDELEG = 0
MEDELEG = 0x0000b100
```

`mideleg` 对相关 interrupt delegation 路径表现为只读 0。S-mode 也不能直接沿用普通 RISC-V payload 的 `sie/sip` 初始化方式，直接写会触发 illegal instruction。

因此当前不能依赖 OpenSBI 通用 `MIP_STIP + mideleg.STIP` 模型。现在使用 ESP32-S31 平台专用 workaround：

1. SYSTIMER target0 通过 interrupt matrix 路由到 CLIC M-level line。
2. CLIC timer IRQ 进入 OpenSBI M-mode。
3. OpenSBI 调用 `sbi_timer_process()`。
4. 如果被打断的上下文是 S/U-mode，则构造 S-mode timer trap。
5. 通过 `sbi_trap_redirect()` 跳到 S-mode `stvec`。

该逻辑必须保留 ESP32-S31 平台条件，不能影响 OpenSBI 普通平台。

## CLIC timer IRQ 清理

SYSTIMER interrupt 和 CLIC line 都需要处理。当前 timer driver 会：

- 重新编程 compare 前 disable CLIC timer line。
- 清 CLIC pending。
- 清 SYSTIMER target0 interrupt。
- 装载 compare 后重新 enable CLIC timer line。

这避免同一个 M-level CLIC timer IRQ 在进入 S-mode trap handler 时反复抢占。

## S-mode payload 验证

当前 S-mode payload 已验证：

- S-mode payload 可以执行。
- S-mode payload 可以通过 SBI DBCN 打印。
- S-mode ecall 能进入 OpenSBI M-mode。
- SBI TIME ecall 能返回。
- S-mode timer interrupt 能通过 CLIC workaround 投递。
- SBI SRST reset 能进入平台 reset handler。

当前 S-mode timer trap entry 的关键约束：

- CLIC `stvec` mode 3 需要 64 字节对齐。
- trap entry 必须保存被使用的寄存器，因为 interrupt 是异步发生的。
- 直接 S-mode UART MMIO 不作为可靠路径，payload 输出优先使用 DBCN ecall。
- `sie/sip` 不能按普通 RISC-V payload 直接写。

当前 payload handler 形态：

```asm
.balign 64
s_trap_entry:
    save caller-clobbered GPRs
    check scause == S-mode timer interrupt
    set timer_seen
    restore caller-clobbered GPRs
    sret
```

当前测试 payload 写入 `stvec = s_trap_entry | 3`，以匹配 ESP32-S31 CLIC mode 3。这个入口仍然是测试 payload 用的最小实现，不是完整 OS trap frame。

## SRST / reset

早期 payload 打印：

```text
sbi_ecall_shutdown failed to execute.
```

原因是平台没有注册 `sbi_system_reset_device`。当前已加入临时 `esp32s31-reset` device。成功日志为：

```text
[esp32s31-reset]
```

shutdown 在开发板上通常没有真实断电语义，当前实现重点是“不返回 payload”，用于验证 SBI SRST 路径。

## PMP 和 CSR probe 问题

OpenSBI 当前可能报告：

```text
Boot HART PMP Count: 0
```

这不等于硬件没有 PMP。当前 ESP32-S31 路径为了避免早期卡死，对 PMP/CSR 探测做了规避或 stub。

原始 PMP/CSR probe 卡死的可能原因：

- OpenSBI CSR probing 会临时切换 `mtvec`，并假设普通 direct trap 行为。
- ESP32-S31 处于 CLIC mode 3。
- CLIC `mcause` 有扩展字段。
- CLIC trap entry 需要 64 字节对齐。
- 早期 expected-trap 机制不是 CLIC-safe。

后续应实现 ESP32-S31 专用、分阶段、CLIC-safe 的 CSR/PMP probe。

## CLIC 是否不符合 spec

当前现象不能直接说明 ESP32-S31 CLIC 不符合 spec。

更准确的判断：

1. 如果只看基础 RISC-V privileged spec，`mtvec mode=3` 不属于普通 direct/vectored 模型。
2. 如果按 CLIC 模型看，ESP32-S31 的行为和 ESP-IDF 代码一致：`mtvec/stvec` 要 mode 3，entry 要 64 字节对齐，`mcause` 要按 CLIC 格式处理。
3. OpenSBI 通用 trap 入口默认按普通 `mtvec` 和普通 `mcause` 处理，所以需要平台级 CLIC 适配。
4. CLIC 长期处于扩展/草案生态，vendor 细节可能不同。当前更像“OpenSBI 默认假设不适配 ESP32-S31 CLIC”，不是“硬件一定违规”。

## 多核 / SMP 状态

ESP32-S31 是多核 SoC，但当前 OpenSBI 平台配置仍是：

```c
#define ESP32S31_HART_COUNT 1
```

原因：

- 当前 bring-up 只验证 core0。
- IDF 的 S31 restart path 有多核分支，会 reset/stall 另一个 core。
- APP core 启动地址、clock/reset、stall/un-stall、IPI、中断矩阵尚未完整移植。

因此当前不能称为 SMP 已支持。后续需要参考：

```text
components/hal/esp32s31/include/hal/cpu_utility_ll.h
components/esp_system/port/soc/esp32s31/system_internal.c
LP_AONCLKRST_HPCORE1_RESET_CTRL_REG
LP_SYSTEM_REG_BOOT_ADDR_HP_CORE1_REG
```

## 重要 ESP-IDF 参考文件

```text
components/riscv/vectors_clic.S
components/riscv/vectors.S
components/riscv/include/esp_private/vectors_const.h
components/riscv/include/riscv/csr_clic.h
components/riscv/interrupt_clic.c
components/esp_hw_support/port/esp32s31/systimer.c
components/soc/esp32s31/include/soc/clic_reg.h
components/soc/esp32s31/register/soc/systimer_reg.h
components/soc/esp32s31/register/soc/interrupt_core0_reg.h
```

关键结论：

- CLIC external interrupt offset 是 16。
- S31 CLIC 使用 `mtvec/stvec` mode 3。
- CLIC trap entry 必须 64 字节对齐。
- CLIC `mcause` 带 interrupt level 和 previous mode 状态。
- SYSTIMER 是 16 MHz。
- IDF 没有提供标准 OpenSBI 风格的 S-mode STIP delegation 方案。

## 当前修改范围概览

主要涉及：

```text
firmware/fw_base.S
firmware/fw_base.ldS
firmware/objects.mk
firmware/payloads/test_head.S
firmware/payloads/test_main.c
lib/sbi/sbi_domain.c
lib/sbi/sbi_hart.c
lib/sbi/sbi_init.c
lib/sbi/sbi_trap.c
platform/esp32s31/
```

早期 UART 探针已经从默认路径移除，包括 OpenSBI 汇编入口、`sbi_init`、trap handler、timer redirect 等阶段的字符探针。bootloader 跳转前用于验证执行流的单字符 `X` 也已删除。

最新干净基线日志：

```text
Test payload running
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

这说明关闭探针后核心路径仍然成立，当前状态可作为后续整理 CLIC 支持和 CSR/PMP probe 的基线。

## 后续建议

短期：

1. 保持当前 clean baseline，不再默认打开 UART 探针。
2. 把当前 ESP32-S31 CLIC workaround 整理成清晰的、平台 gated 的正式实现。
3. 继续把测试 payload 扩展成回归用例，覆盖连续 timer interrupt 和异常重定向。

中期：

1. 重做 hart feature CSR probe，避免长期硬编码。
2. 系统验证 S-mode external interrupt、exception redirect、`satp`、页表、缓存/内存属性。
3. 验证连续 timer interrupt 和长时间运行，确认 CLIC pending/enable 清理没有边界问题。

长期：

1. 决定最终模型是 OpenSBI M-mode + S-mode OS，还是 ESP-TEE 风格 M-mode runtime + U-mode REE。
2. 如果继续 S-mode OS 路线，需要补齐 CLIC/interrupt delegation 和多核启动。
3. 如果走 U-mode REE 路线，需要定义或复用清晰的 SBI/TEE service ABI。
