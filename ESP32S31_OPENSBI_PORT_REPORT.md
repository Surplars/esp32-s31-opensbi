# ESP32-S31 OpenSBI 移植阶段报告

日期：2026-05-31

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
- S-mode timer interrupt 已通过 ESP32-S31 专用 CLIC 适配路径跑通。
- 双 hart OpenSBI/HSM 路径已跑通，hart1 可由 SBI HSM 启动到 S-mode payload。
- Sv32 identity、fault、权限、A/D、ASID 和 RFENCE 测试均已跑通。
- OpenSBI generic hart feature/PMP/debug trigger probe 已恢复，当前不再通过跳过 probe 规避卡死。

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
- `extensions_init` 只提供平台侧必需的基础信息；hart feature、PMP、debug trigger 等能力由 OpenSBI generic probe 继续探测。

之前 `sbi_hart.c` 曾为 ESP32-S31 跳过 generic trap-based CSR/PMP probe。根因已经明确：OpenSBI 的 `csr_read_allowed()` / `csr_write_allowed()` 会临时把 `mtvec` 指到 `__sbi_expected_trap`，而 ESP32-S31 CLIC 要求 trap entry 64 字节对齐且 `mtvec` mode 为 3。旧的 expected-trap 只有普通对齐，硬件会截断低 6 位后跳到错误入口，表现为 probe 卡死。

当前修复：

- `__sbi_expected_trap` 和 `__sbi_expected_trap_hext` 在 ESP32-S31 CLIC 下使用 `.balign 64`。
- `sbi_hart_expected_trap` 在 ESP32-S31 CLIC 下写入 `handler | 3`。
- `sbi_hart_init()` 恢复执行 generic `hart_detect_features()`，不再跳过 PMP/CSR/debug trigger 探测。
- `MIP`、`SIE`、`UTVEC/USCRATCH/UIE` 等可能不可写 CSR 的初始化改为在 ESP32-S31 CLIC 下走 `csr_write_allowed()`，保留官方初始化语义，并由 expected-trap 吸收非法 CSR trap。

保留的 ESP32-S31 平台分支不再是绕过官方 init，而是 CLIC 适配：`mtvec/stvec` mode 3、trap entry 64 字节对齐、CLIC `mcause` 规范化/恢复、CLIC timer/IPI IRQ 号映射，以及 HSM stopped hart 在 `wfi` 前临时打开 `mstatus.MIE` 以允许 CLIC IPI 唤醒。

另一个保留的早期汇编条件是 `CLEAR_MDT`：ESP32-S31 当前报告 privileged architecture v1.11，未暴露 Smdbltrp/MDT 语义；该路径又发生在 C 层 CSR probing 可用之前，因此 ESP32-S31 下不写 `MSTATUSH.MDT`。这属于按已探测硬件能力收敛后的平台条件，不再作为临时绕过处理。

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

因此当前不能依赖 OpenSBI 通用 `MIP_STIP + mideleg.STIP` 模型。现在使用 ESP32-S31 平台专用 CLIC timer 适配：

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
- S-mode timer interrupt 能通过 CLIC timer 适配路径投递。
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

原因是平台没有注册 `sbi_system_reset_device`。当前已正式注册 `esp32s31-reset` device。成功日志为：

```text
[esp32s31-reset]
```

shutdown 在开发板上通常没有真实断电语义，当前实现重点是“不返回 payload”，用于验证 SBI SRST 路径。

## PMP 和 CSR probe 问题

OpenSBI 早期曾报告：

```text
Boot HART PMP Count: 0
```

这不等于硬件没有 PMP。当时的问题不是 PMP 缺失，而是 expected-trap 不符合 ESP32-S31 CLIC 的入口要求，导致 generic CSR/PMP probe 在临时切换 `mtvec` 后卡死。

当前 expected-trap 修复后，OpenSBI 已能通过 generic probe 得到：

```text
Boot HART Priv Version      : v1.11
Boot HART ISA Extensions    : zicntr,sdtrig
Boot HART PMP Count         : 16
Boot HART PMP Granularity   : 7 bits
Boot HART PMP Address Bits  : 30
Boot HART Debug Triggers    : 4 triggers
```

因此当前不再需要对 PMP count、PMP granularity、PMP address bits 或 debug triggers 写死，也不再保留 `esp_allow_su_all_pmp()` 这类 bring-up fallback。PMP/domain 权限完全交给 OpenSBI generic probe 和 protection 配置路径处理。

## CLIC 是否不符合 spec

当前现象不能直接说明 ESP32-S31 CLIC 不符合 spec。

更准确的判断：

1. 如果只看基础 RISC-V privileged spec，`mtvec mode=3` 不属于普通 direct/vectored 模型。
2. 如果按 CLIC 模型看，ESP32-S31 的行为和 ESP-IDF 代码一致：`mtvec/stvec` 要 mode 3，entry 要 64 字节对齐，`mcause` 要按 CLIC 格式处理。
3. OpenSBI 通用 trap 入口默认按普通 `mtvec` 和普通 `mcause` 处理，所以需要平台级 CLIC 适配。
4. CLIC 长期处于扩展/草案生态，vendor 细节可能不同。当前更像“OpenSBI 默认假设不适配 ESP32-S31 CLIC”，不是“硬件一定违规”。

## 多核 / SMP 状态

ESP32-S31 当前按双 hart 平台运行：

```c
#define ESP32S31_HART_COUNT 2
```

已验证：

- bootloader 侧 core1 probe 可从 SRAM 运行，`mhartid=1`，`misa=0x40943127`。
- OpenSBI banner 显示 `Platform HART Count : 2`，Domain0 包含 `0x0*,0x1*`。
- hart1 通过 OpenSBI warm path 进入 HSM stopped。
- S-mode payload 可通过 SBI HSM `hart_start(1, ...)` 启动 hart1。
- CLIC IPI 使用 HP_SYSTEM `CPU_INT_FROM_CPU_1` 经 interrupt matrix 路由到 CLIC line 17，能够唤醒 stopped hart。
- 跨 hart RFENCE 测试已验证 IPI/TLB request 路径可用。

当前可以称为 OpenSBI 双 hart/HSM/RFENCE 路径可用。是否作为完整 SMP OS 平台还需要后续 OS 侧调度、中断亲和性、长时间并发和压力测试确认。后续仍可参考：

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
2. 把当前 ESP32-S31 CLIC 适配整理成清晰的、平台 gated 的正式实现。
3. 继续把测试 payload 扩展成回归用例，覆盖连续 timer interrupt 和异常重定向。

中期：

1. 保持 generic hart feature/PMP/debug trigger probe 路径开启，后续只在确有硬件差异时增加平台 gated 处理。
2. 系统验证 S-mode external interrupt、exception redirect、`satp`、页表、缓存/内存属性。
3. 验证连续 timer interrupt、双 hart 并发和长时间运行，确认 CLIC pending/enable 清理没有边界问题。

长期：

1. 决定最终模型是 OpenSBI M-mode + S-mode OS，还是 ESP-TEE 风格 M-mode runtime + U-mode REE。
2. 如果继续 S-mode OS 路线，需要补齐 CLIC/interrupt delegation 和多核启动。
3. 如果走 U-mode REE 路线，需要定义或复用清晰的 SBI/TEE service ABI。

## Dual-hart / HSM progress, 2026-05-31

当前已经确认 ESP32-S31 的第二个 hart 不是规格更低的假核：bootloader 侧 probe 能让 core1 从 SRAM 执行代码，`mhartid=1`，`misa=0x40943127`，并且可以读写 SRAM 和基本 CSR。OpenSBI 平台 hart count 改为 2 后，banner 能显示 `Platform HART Count : 2`，Domain0 也能包含 `0x0*,0x1*`。

已验证的 HSM 链路：

```text
Test payload running
HSM hart1 status before start=0x00000001
[ipi send h1 map=00000203 reg 00000000->00000001]
Secondary hart running
HSM hart1 start err=0x00000000
HSM hart1 start observed
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

这说明：
- hart1 已经能经过 OpenSBI warm path 进入 HSM stopped 状态。
- S-mode payload 发起 `hart_start(1, test_secondary_entry, 0)` 后，OpenSBI HSM 状态机能把 hart1 切到 `START_PENDING`。
- hart1 能执行 secondary S-mode payload，并打印 `Secondary hart running`。
- 后续 TIME、S-mode timer interrupt、SRST 仍然正常。

但这条日志里的 `map=00000203` 表示当时 CPU_INTR_FROM_CPU_1 被映射到 CLIC line 3。该结果主要证明 HSM 状态机和 secondary payload 路径可用；如果当时 `sbi_hsm_hart_wait()` 仍使用轮询或旧镜像，则不能单独证明 CLIC line 3 可以从 WFI 真正唤醒 stopped hart。

当前源码状态：
- `ESP32S31_IPI_CLIC_INTNUM` 已改为 17。
- `OPENSBI_PLATFORM_ESP32S31_CLIC_IPI_IRQ` 也应为 17。
- `sbi_hsm_hart_wait()` 的 ESP32-S31 gated 路径会在 WFI 前短暂打开 `mstatus.MIE`，使 CLIC interrupt 可以唤醒 stopped hart。
- `ipi_esp_send()` 的 debug print 已移除。

当前提交前状态：
- 保留 HP_SYSTEM `CPU_INT_FROM_CPU_0/1` 作为 IPI source，保留 interrupt matrix 到 CLIC line 17 的映射。
- `lib/sbi/sbi_hsm.c` 中的 ESP32-S31 CLIC WFI 处理已收束为 `hsm_wait_for_interrupt()`，后续若 OpenSBI 增加通用 CLIC/HSM hook，可再迁移到正式 hook。
- 保留 payload HSM 测试作为 bring-up payload，或在提交前按需要关闭。

line 17 + WFI 后续已经验证成功：

```text
Test payload running
HSM hart1 status before start=0x00000001
[ipi send h1 map=00000211 reg 00000000->00000001]
Secondary hart running
HSM hart1 start err=0x00000000
HSM hart1 start observed
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

这确认了 HP_SYSTEM `CPU_INT_FROM_CPU_1` -> interrupt matrix -> CLIC line 17 -> M-mode trap -> HSM start finish 的完整路径。

后续在恢复 generic PMP/debug trigger probe 后出现过一次回归：

```text
HSM hart1 status before start=0x00000001
HSM hart1 start err=0x00000000
HSM hart1 start timeout
```

该日志说明 HSM 状态机已经把 hart1 从 `STOPPED` 切到 `START_PENDING`，`sbi_hsm_hart_start()` 也成功返回，但 hart1 没有被 IPI 唤醒到 warm path。原因更可能在平台 IPI 唤醒链路，而不是 PMP/domain 权限：当时 banner 已显示 Domain0 覆盖 payload 地址，且 PMP probe 已正常显示 16 entries。

处理方式是把 `platform/esp32s31/ipi_esp.c` 的 IPI 发送路径正式对齐 ESP-IDF crosscore 语义：

- 每次发送前重新确认目标 hart 的 interrupt-matrix route：`CPU_INTR_FROM_CPU_i -> CLIC line 17 on hart i`。
- 发送前先清 `HP_SYSTEM_CPU_INT_FROM_CPU_i`，加 write barrier，再写 1 触发。
- 保持 OpenSBI 官方 HSM 路径不变：`sbi_hsm_hart_start()` 仍通过 `sbi_ipi_raw_send()` 调用平台 IPI device，不改 HSM 状态机。

这类改动属于平台 IPI device 的正式化，不是用轮询绕过 HSM。仍需用户侧重新编译烧录后确认 `HSM hart1 start observed` 恢复稳定。

后续又观察到 banner 中 `Platform HSM Device : ---`，同时 payload 仍打印：

```text
HSM hart1 status before start=0x00000001
HSM hart1 start err=0x00000000
HSM hart1 start timeout
```

这说明上一轮只修了“hart1 已经在 OpenSBI warm wait 时如何用 CLIC IPI 唤醒”的路径；如果 hart1 仍处于 reset/stall，OpenSBI 没有平台 HSM device 时只能改 HSM 状态并发 raw IPI，无法真正把 core1 从硬件 reset/stall 中拉起来。

因此新增 `esp32s31-hsm` secondary-boot HSM device：

- `platform/esp32s31/platform.c` 注册 `sbi_hsm_set_device(&esp_hsm)`。
- `hart_start(1, warmboot_addr)` 使用 IDF/bootloader app CPU 启动寄存器序列：assert core1 reset、写 `LP_SYSTEM_BOOT_ADDR_HP_CORE1`、barrier、打开 CPU/CLIC clock、释放 reset、unstall core1。
- 没有新增 generic OpenSBI HSM 状态机改动；这只是 ESP32-S31 平台把“未进入 OpenSBI 的 secondary hart”带到 OpenSBI warmboot 的硬件启动 hook。

实测新 banner 已显示 `Platform HSM Device : esp32s31-hsm`，但仍 timeout。这进一步说明 hart1 已经至少进入过 OpenSBI warmboot，导致 OpenSBI generic HSM 按 `secondary_boot && !init_count` 规则不再调用 platform `hart_start()`，而是回退到 raw IPI 唤醒。

ESP32-S31 目前增加一个平台 gated HSM start 选择：

- `lib/sbi/sbi_hsm.c` 中仅在 `OPENSBI_PLATFORM_ESP32S31_CLIC` 下，对 `hartid == 1` 的 HSM START 优先调用 `hsm_device_hart_start()`。
- 这样 stopped hart1 无论是仍在 OpenSBI warm wait，还是被 SoC reset/stall 控制保持，都通过同一套 core1 reset/start 寄存器回到 OpenSBI warmboot。
- 该改动不影响 RFENCE/普通 IPI；那些路径仍走 `esp32s31-ipi`。

进一步诊断显示 hart1 warm init 能完整走到 `sbi_hsm_hart_start_finish()`：

```text
[h1 warm finish]
[hsm finish h1 next=2f050078 arg1=0 mode=1]
[h1 trap mcause=1 mepc=2f024386 mtval=2f024386 stvec=2f050043]
```

这里 `next=0x2f050078` 是正确的 secondary payload 入口，但 trap 的 `mepc=0x2f024386` 落在 OpenSBI text 内。根因是 ESP32-S31 CLIC 路径在 `sbi_hart_switch_mode()` 中先写了 `MSTATUS/MEPC`，随后又调用 `csr_write_allowed(CSR_SIE, ...)`。如果这个 allowed CSR 写触发 expected-trap，trap 处理会覆盖 `MEPC/MSTATUS`，导致最终 `mret` 以 S-mode 回到 OpenSBI 内部地址并 instruction access fault。

修复方式：

- 非 ESP32-S31 路径保持 OpenSBI 原顺序。
- ESP32-S31 CLIC 下，把最终 `MSTATUSH/MSTATUS/MEPC` 写入移动到所有可能触发 expected-trap 的 CSR 操作之后，紧贴 `mret`。

## Sv32 payload test, 2026-05-31

在 HSM/IPI 稳定后，payload 增加了最小 Sv32 自测：

```text
Test payload running
HSM hart1 status before start=0x00000001
HSM hart1 start err=Secondary hart running
0x00000000
HSM hart1 start observed
HSM hart1 status after stop=0x00000001
Sv32 identity test start
Sv32 identity test OK
Sv32 fault test start
Sv32 fault test OK
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

测试覆盖：
- `satp.MODE=Sv32` 能写入并生效。
- 以 `0x2f000000` 为起点的 4 MiB identity megapage 能支持 S-mode 取指和数据读写。
- 访问未映射 VA `0x1000` 能进入 S-mode fault handler，并通过显式 resume PC 正常返回。
- Sv32 测试后，SBI TIME、S-mode timer interrupt、SRST 仍然正常。

调试中发现 fault 测试必须在触发 page fault 前把 `stvec` 设置为 direct `s_trap_entry`。此前 `stvec` 仍指向启动汇编里的 `_start_hang`，导致 page fault 后主核直接挂住，看起来像执行流跳乱。timer 测试前仍需恢复 ESP32-S31 CLIC 使用的 `stvec = s_trap_entry | 3`。

当前结论：ESP32-S31 的 Sv32 基本功能可用，但这还不是完整 MMU 合规测试。尚未覆盖 4 KiB 二级页表、R/W/X 权限组合、U/S 权限、A/D 行为、ASID、`sfence.vma` 粒度、跨 hart TLB shootdown。

后续已补充 Sv32 extended testbench，覆盖 1-4：

```text
Sv32 extended test start
Sv32 ext setup OK
Sv32 ext satp OK
Sv32 ext 4K RW OK
Sv32 ext R perm OK
Sv32 ext X perm OK
Sv32 ext U/S SUM OK
Sv32 ext sfence remap OK
Sv32 extended test OK
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

新增覆盖：
- 4 KiB 二级页表 alias 映射。
- R/W 数据页读写。
- R-only 页 store fault。
- non-X 数据页 exec fault。
- X-only code alias 可执行。
- U page 在 `SUM=0` 下 S-mode load fault。
- U page 在 `SUM=1` 下 S-mode load 成功。
- 修改同一个 VA 的 PTE 指向新物理页后，`sfence.vma` 生效，读写命中新物理页。

调试中发现 exec fault probe 不能使用 `jalr ra, 0(a0)`，因为跳转前会覆盖 `ra`，fault handler resume 后 `ret` 会回到自身形成死循环。已改为 `jr a0`，让 fault resume 和执行成功两条路径都能回到 C 调用者。

当前 Sv32 结论：ESP32-S31 Sv32 的 megapage、4 KiB page、基础 R/W/X fault、U/S+SUM 和 `sfence.vma` remap 均可用。剩余更高阶项主要是 A/D 位硬件行为、ASID、跨 hart TLB shootdown/RFENCE。

## Sv32 RFENCE / cross-hart TLB test, 2026-05-31

后续补充跨 hart RFENCE testbench，并已通过：

```text
Sv32 RFENCE test start
RFENCE setup OK
RFENCE hart1 start OK
RFENCE hart1 ready
RFENCE remap done
RFENCE ecall returned
RFENCE hart1 observed remap
Sv32 RFENCE test OK
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

测试过程：
- hart0 准备共享 Sv32 页表，alias VA `0x30000000` 先映射到 `sv32_data_page`。
- hart0 通过 HSM 再次启动 hart1。
- hart1 在 S-mode 开启同一套 Sv32 页表，读取 alias，得到 page1 的 `0x0badc0de`。
- hart0 修改 alias PTE，使同一个 VA 指向 `sv32_data_page2`。
- hart0 调用 SBI RFENCE `remote_sfence_vma(hmask=1, hbase=1, start=0x30000000, size=4096)`。
- RFENCE ecall 返回后，hart1 再读同一 alias，得到 page2 的 `0x00c0ffee`。

这说明：
- OpenSBI RFENCE extension 在 ESP32-S31 平台上可调用。
- RFENCE 使用的跨 hart IPI/TLB request 路径能到达 hart1，并能返回 hart0。
- hart1 在 S-mode + Sv32 环境下能被远程要求执行 `sfence.vma`。
- remap 后 hart1 能观察到新 PTE 指向的新物理页。

限制：
- 该测试证明 RFENCE 功能链路有效，但不严格证明硬件一定存在可长期保持旧翻译的 TLB；如果硬件 TLB 很小、自动失效、或页表 walker 行为较保守，测试仍可能通过。
- 尚未覆盖 ASID 版本的 `remote_sfence_vma_asid`。
- 尚未覆盖多 VA range、全地址空间 flush、以及压力型 TLB shootdown。

## Sv32 strict test, 2026-05-31

补充 strict Sv32 testbench 后，结果如下：

```text
Sv32 strict test start
Sv32 strict test OK
Sv32 AD load mode=0x00000001
Sv32 AD store mode=0x00000001
Sv32 ASID readback=0x00000001
```

覆盖项：
- 非法 PTE `W=1, R=0` 触发 fault。
- misaligned megapage 触发 fault。
- `sstatus.MXR=0` 时，从 X-only page load 触发 fault。
- `sstatus.MXR=1` 时，从 X-only page load 成功。
- S-mode 执行 U page 触发 fault。
- A/D clear 行为为 fault-on-clear：
  - `AD load mode=1` 表示 A=0 load 触发 page fault，而不是硬件自动置 A。
  - `AD store mode=1` 表示 D=0 store 触发 page fault，而不是硬件自动置 D。
- `ASID readback=1` 表示 `satp.ASID` 至少支持 bit0，ASIDLEN >= 1。

当前判断：ESP32-S31 的 Sv32 不是 ESP32-S3 风格 simple MMU。它具备标准 Sv32 两级页表 walker、4 KiB page、megapage 对齐检查、PTE 合法性检查、权限检查、MXR、U/S 语义、software-managed A/D fault、ASID WARL 和 RFENCE 路径。剩余应力测试主要面向性能/一致性边界，而不是基础 Sv32 完整性。

## HSM cleanup / final validation, 2026-05-31

后续验证中，hart1 的 HSM START 已经稳定通过两类场景：
- 普通 secondary payload：hart1 从 `STOPPED` 经 `START_PENDING` 进入 S-mode，打印 `Secondary hart running`，随后 `hart_stop()` 回到 `STOPPED`。
- RFENCE worker payload：hart1 再次被 HSM 启动，在 S-mode+Sv32 下响应 RFENCE/TLB shootdown，观察到 remap 后的新物理页。

代表性日志：
```text
HSM hart1 start observed
Secondary hart running
HSM hart1 status after stop=0x00000001
...
RFENCE hart1 ready
RFENCE remap done
RFENCE ecall returned
RFENCE hart1 observed remap
Sv32 RFENCE test OK
```

这说明当前正式路径成立：
- 平台注册 `esp32s31-hsm`，负责用 ESP-IDF/bootloader 同类 core1 reset/bootaddr/clock/unstall 寄存器序列把 hart1 带回 OpenSBI warmboot。
- OpenSBI generic HSM 状态机仍然负责 `STOPPED`、`START_PENDING`、`STARTED` 的状态转换；ESP32-S31 只在 `hartid == 1` 时优先使用平台 `hart_start()`，避免 stopped hart1 因 CLIC WFI/raw IPI 组合不稳定而无法被唤醒。
- warm hart 在 `sbi_hsm_hart_wait()` 中围绕 `wfi` 短暂打开 `mstatus.MIE`，保证 CLIC IPI 可以打断等待状态。
- `sbi_hart_switch_mode()` 的 ESP32-S31 CLIC 路径在所有可能触发 expected-trap 的 CSR 探测之后才最终写 `MSTATUSH/MSTATUS/MEPC`，避免 `mret` 前目标入口被 trap 恢复逻辑覆盖。

之前用于定位问题的 `[h1 warm ...]`、`[hsm start ...]`、`[hsm finish ...]`、`[h1 trap ...]`、`[h1 nascent]` 和 secondary entry marker 均已删除。保留的是正式支持代码和 payload testbench。

## PMU decision, 2026-05-31

OpenSBI 的 `PMU` 指 RISC-V performance monitoring unit/SBI PMU，不是 ESP32-S31 SoC 内用于电源、reset、stall 的 PMU 外设。当前 generic probe 结果为：

```text
Platform PMU Device         : ---
Boot HART MHPM Info         : 0 (0x00000000)
```

这表示标准 `mhpmcounter3+ / mhpmevent3+` 可编程性能计数器没有被探测到。S31 仍有 `zicntr`，所以 `cycle/time/instret` 固定计数器可用；但这些固定计数器由 OpenSBI generic PMU 路径处理，不需要注册平台 PMU device。

因此当前不添加 `esp32s31-pmu`，避免把 ESP power PMU 误报为 SBI performance PMU。后续只有在确认存在可编程 performance event selector 或非标准 HPM counter 后，再补 `pmu_init` / `pmu_xlate_to_mhpmevent` / platform firmware counter。

## Firmware mode decision, 2026-05-31

ESP32-S31 平台默认固件类型已从 bring-up 用 `FW_PAYLOAD` 切回真实链路使用的 `FW_JUMP`：

```make
FW_JUMP=y
FW_JUMP_ADDR=0x2F050000
```

这与当前内存布局一致：bootloader 将 OpenSBI 放到 `0x2F020000`，OpenSBI 完成 M-mode 初始化后跳到 `0x2F050000` 的下一阶段 S-mode 固件。测试 payload 仍保留，但作为显式回归模式使用：

```sh
make PLATFORM=esp32s31 FW_JUMP= FW_PAYLOAD=y FW_PAYLOAD_OFFSET=0x30000
```

因此后续真实固件验证应基于 `fw_jump.bin`，而不是默认嵌入 test payload 的 `fw_payload.bin`。
