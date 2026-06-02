# ESP32-S31 OpenSBI 移植说明

本文记录 ESP32-S31 OpenSBI 平台移植的关键设计、硬件差异和已验证能力，供后续维护者理解当前代码取舍。本文不依赖本机目录路径。

## 启动模型

当前启动链路：

```text
ROM -> OpenIon ESP Bootloader -> OpenSBI M-mode -> S-mode OS
```

推荐 OpenSBI 使用 `fw_jump`：

```make
FW_TEXT_START=0x2f020000
FW_JUMP=y
FW_JUMP_ADDR=0x50000000
```

bootloader 负责：

- 初始化基础 PMS/APM/TEE 权限；
- 初始化 PSRAM 并映射到 `0x50000000`；
- 从 flash 加载 OpenSBI 到 `0x2f020000`；
- 从 flash 加载 S-mode OS raw image 到 `0x50000000`；
- 通过 `a1` 向 OpenSBI 传入 DTB 地址。

OpenSBI banner 中应能看到：

```text
Platform Name               : Espressif ESP32-S31
Platform HART Count         : 2
Platform Timer Device       : esp32s31-timer @ 40000000Hz
Platform IPI Device         : esp32s31-ipi
Platform HSM Device         : esp32s31-hsm
Platform HART Protection    : pmp
Domain0 Next Address        : 0x50000000
```

## 已验证能力

当前平台已验证：

- OpenSBI M-mode cold boot；
- S-mode next-stage handoff；
- SBI DBCN console 输出；
- SBI TIME set_timer；
- ESP32-S31 CLIC timer 中断投递到 S-mode；
- SBI SRST reset；
- 双 hart 平台枚举；
- HSM 启动 hart1；
- IPI 唤醒 stopped hart；
- RFENCE 跨 hart TLB shootdown；
- generic PMP/CSR/debug trigger probe；
- Sv32 4 KiB page、megapage、权限 fault、MXR、SUM、A/D fault-on-clear、ASID readback 和 RFENCE。

代表性回归输出：

```text
Test payload running
HSM hart1 start observed
Sv32 strict test OK
Sv32 RFENCE test OK
TIME ecall returned
S-mode timer interrupt
[esp32s31-reset]
```

## CLIC 适配

ESP32-S31 使用 CLIC，不是标准 `mtvec` direct/vectored 模型。关键约束：

- `mtvec/stvec` 需要使用 CLIC mode 3；
- trap entry 必须 64 字节对齐；
- CLIC `mcause` 携带 interrupt level / previous mode 等扩展字段；
- trap return 前需要恢复原始 CLIC `mcause`，否则 `mret` 的中断级别恢复语义会被破坏。

OpenSBI generic trap handler 仍然期望标准化 cause。因此 ESP32-S31 路径需要：

```text
standard_cause = (raw_mcause & interrupt_bit) | (raw_mcause & 0xfff)
```

注意 `mcause` 和 `scause` 的 CLIC 扩展字段布局不同，不能把 M-mode raw `mcause` 的扩展字段直接复制到 `scause`。

## expected-trap 与 CSR/PMP probe

早期卡死的主因不是 PMP 缺失，而是 OpenSBI generic CSR/PMP probe 会临时把 `mtvec` 指向 `__sbi_expected_trap`。ESP32-S31 CLIC 要求该入口 64 字节对齐并使用 mode 3；否则硬件会按 CLIC 规则截断低位，跳到错误入口。

当前修复原则：

- `__sbi_expected_trap` / `__sbi_expected_trap_hext` 在 ESP32-S31 CLIC 下 64 字节对齐；
- expected-trap 写入 `mtvec = handler | 3`；
- 恢复 OpenSBI generic `hart_detect_features()` 路径；
- 不再硬编码跳过 PMP/CSR/debug trigger probe。

当前 probe 结果：

```text
Boot HART Priv Version      : v1.11
Boot HART ISA Extensions    : zicntr,sdtrig
Boot HART PMP Count         : 16
Boot HART PMP Granularity   : 7 bits
Boot HART PMP Address Bits  : 30
Boot HART Debug Triggers    : 4 triggers
```

## Timer

ESP32-S31 的 timer 路径使用 SYSTIMER + CLIC：

```text
SYSTIMER target0 -> interrupt matrix -> CLIC M-level IRQ -> OpenSBI timer process
```

当前频率：

```text
SYSTIMER frequency : 16 MHz
time CSR frequency : 40 MHz
```

标准 `MIP.STIP + MIDELEG.STIP` 路径当前不可依赖：

- `mideleg` 表现为 0；
- S-mode 直接读写部分 interrupt CSR 会触发 illegal instruction；
- 因此 OpenSBI 平台代码通过 M-mode CLIC timer IRQ 调用 `sbi_timer_process()`，并在需要时重定向到 S-mode trap。

## IPI 与 HSM

ESP32-S31 平台当前声明 2 个 hart。hart1 可由平台 HSM device 拉起：

- 使用 SoC core1 reset/bootaddr/clock/unstall 寄存器序列；
- hart1 进入 OpenSBI warmboot；
- OpenSBI generic HSM 状态机负责 `STOPPED` / `START_PENDING` / `STARTED` 状态；
- IPI 使用 HP_SYSTEM `CPU_INT_FROM_CPU_1` 经 interrupt matrix 路由到 CLIC line 17。

重要细节：

- stopped hart 在 `wfi` 前需要短暂打开 `mstatus.MIE`，否则 CLIC IPI 不能唤醒；
- hart1 start 优先走平台 HSM device，避免 hart1 仍在 reset/stall 时 raw IPI 无效；
- RFENCE 继续走平台 IPI device。

## Sv32

ESP32-S31 的 Sv32 不是 ESP32-S3 风格 simple MMU。已验证：

- 标准两级 Sv32 page table walker；
- 4 KiB page；
- megapage 和 megapage alignment fault；
- R/W/X 权限检查；
- illegal PTE `W=1,R=0` fault；
- MXR；
- U/S 和 SUM；
- S-mode 执行 U page fault；
- A/D clear 采用 fault-on-clear；
- `satp.ASID` 至少支持 bit0；
- RFENCE remote sfence path 可用。

限制：

- 当前测试证明功能链路可用，但不是长期压力测试；
- 尚未覆盖 ASID 版本的 `remote_sfence_vma_asid`；
- 尚未覆盖大范围 VA flush 和高并发 TLB shootdown。

## PMU

OpenSBI 的 PMU 指 RISC-V performance monitoring unit/SBI PMU，不是 ESP32-S31 SoC power PMU。

当前 probe：

```text
Platform PMU Device         : ---
Boot HART MHPM Info         : 0 (0x00000000)
```

结论：

- 暂不注册 `esp32s31-pmu`；
- `cycle/time/instret` 固定计数器由 generic 路径处理；
- 只有确认存在可编程 HPM event selector 或平台 firmware counter 后，再补 SBI PMU device。

## 移植边界

平台代码应保持在 OpenSBI 官方初始化路径内：

```text
sbi_init()
  -> sbi_platform_cold_boot_allowed()
  -> sbi_platform_nascent_init()
  -> init_coldboot()
  -> sbi_hart_init()
  -> sbi_platform_extensions_init()
```

ESP32-S31 特殊处理应限制为平台 gated 的 CLIC/HSM/IPI/timer 差异，不应长期保留绕过 generic init 的硬编码路径。

## 后续建议

- 保持 OpenSBI generic PMP/debug trigger/CSR probe 开启；
- 将 CLIC expected-trap、cause 标准化和 HSM WFI 行为整理为更清晰的平台 hook；
- 继续验证真实 OS 下的外部中断、Sv32 长时间运行、跨 hart RFENCE 和 cache/TLB 一致性；
- bootloader 侧建议逐步使用 ESP-IDF bring-up shim 替代手写 PSRAM/cache/APM 初始化，OpenSBI 侧继续专注 M-mode firmware 和 SBI 能力。
