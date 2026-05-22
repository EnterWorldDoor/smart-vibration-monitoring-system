# STM32 NDE Node — 非驱动端传感器节点

电机非驱动端(NDE)振动采集与DSP处理节点。通过CAN总线将24维特征向量上行至F407。

## 硬件平台

**主控芯片**: STM32F103C8T6 (Cortex-M3, 72MHz, 20KB SRAM, 64KB Flash)
**开发板**: 蓝板 (Blue Pill) 最小系统板
**传感器**: ADXL345 三轴加速度计 (SPI)
**CAN收发器**: SN65HVD230模块 → 连接至F407的SN65HVD230

## 引脚分配

| 外设 | GPIO | 功能 |
|------|------|------|
| ADXL345 SCLK | PA5 | SPI1_SCK |
| ADXL345 MOSI | PA7 | SPI1_MOSI |
| ADXL345 MISO | PA6 | SPI1_MISO |
| ADXL345 CS | PA4 | 软件GPIO (SPI_NSS) |
| ADXL345 INT1 | PA3 | FIFO中断 (可选) |
| CAN RX | PB8 | CAN_RX (重映射AF, 避免与USB PA11冲突) |
| CAN TX | PB9 | CAN_TX (重映射AF, 避免与USB PA12冲突) |

- USB: PA11(DM)/PA12(DP)，Micro-USB保留用于供电和调试
- CAN使用重映射——默认PA11/PA12冲突

## 供电

- F103蓝板: Micro-USB独立供电
- SN65HVD230: F103侧3.3V供电
- CAN总线: 仅CANH/CANL差分线对连，两端电气隔离

## 固件架构

- 裸机循环 (无RTOS), ADXL345 INT1 FIFO水印中断驱动采样 (16样本/突发)
- 自写dsp_fft_q15定点FFT (无CMSIS-DSP, F103无FPU)
- 每64样本窗口(160ms, 4次FIFO突发)计算24维特征向量, 耗时~3.8ms
- 2s周期CAN发送特征(17帧CRC8), 1s周期心跳(含CRC8)
- 三级健康状态机 + IWDG看门狗(3s超时) + bsp_log精简日志

## CAN协议

| 参数 | 值 |
|------|-----|
| 波特率 | 500 kbps |
| ID | 0x201 (NDE特征数据), 0x202 (NDE心跳) |
| 发送周期 | 2s (特征), 1s (心跳) |
| 帧格式 | 17帧/特征包, 每帧CRC8校验 |
| CRC多项式 | CRC-8-Dallas/Maxim (0x31, 反向) |

### 特征帧格式 (0x201, 17帧/批次, 每2s)

```
帧0:  [seq=0] [window_idx] [feat[0..4] 5字节] [CRC8]  = 8字节
帧1-16: [seq=N] [feat字节 6个]            [CRC8]  = 8字节

CRC8覆盖范围: data[0..6] (seq + payload 7字节)
多项式: 0x31 (x^8 + x^5 + x^4 + 1), 初始值0x00, 不反转
总容量: 17帧 × (6 + 5) = 5 + 96 = 101字节有效载荷 ≥ 96字节特征向量
冗余: 5字节保留，帧16最后1字节payload填充0x00
```

### 心跳帧格式 (0x202, 1帧/次, 每1s)

| 字节 | 字段 | 说明 |
|------|------|------|
| [0] | online | 0x01=在线, 0x00=离线 |
| [1] | error_count | 累积DSP错误计数 |
| [2] | temp_c | 温度(无传感器填22) |
| [3] | CRC8 | data[0..2]的CRC8 |
| [4..7] | reserved | 填充0x00 |

## 特征向量格式 (24维, 与DE完全一致)

```
[0-3]:   rms_x, rms_y, rms_z, overall_rms
[4-5]:   peak_freq_x, peak_amp_x
[6-8]:   skewness_x, kurtosis_x, crest_factor_x
[9-16]:  band_energy_x[0..7]
[17-19]: peak_freq_y, peak_amp_y, crest_factor_y
[20-22]: peak_freq_z, peak_amp_z, crest_factor_z
[23]:    0.0f (无温度传感器)
```

## 架构决策记录

### ADR-001: CAN帧CRC8保护 (2026-05-22)
**决策**: 每帧CRC-8-Dallas/Maxim (0x31多项式), 覆盖seq+payload共7字节.
17帧/批次(原16帧), 总容量101字节≥96字节特征向量.
**理由**: 无校验时CAN总线单比特翻转导致特征向量静默损坏, ESP32下游基于垃圾数据诊断.
**替代方案**: 批次级CRC16 — 无法定位损坏帧, 需重传整个批次.

### ADR-002: ADXL345 FIFO突发读取 (2026-05-22)
**决策**: 弃用TIM2逐点采样, 改用ADXL345 INT1 FIFO水印中断驱动.
INT1在FIFO达16样本时触发(每40ms) → ISR置标志 → 主循环SPI突发读取.
64样本窗口 = 4次FIFO突发(16×4).
**理由**: 现有TIM2+布尔标志方案在DSP计算期间(3.8ms)丢失采样点.
FIFO模式已在ADXL345寄存器配置中启用(水印=16), 仅需修改读取逻辑.
**影响**: TIM2可降级为心跳计时, INT1需要GPIO中断(PA3已预留).

### ADR-004: 精简日志系统 (2026-05-22)
**决策**: 不移植F407完整system_log模块, 在bsp/下放精简版 `bsp_log.h` (~40行).
提供 `pr_info/pr_warn/pr_error` 三个宏, 底层阻塞调用 `HAL_UART_Transmit` 经USART1(PA9/PA10)输出.
**理由**: F407 system_log有环形缓冲+分级过滤+文件存储, F103 64KB Flash放不下也不需要.
调试串口在开发阶段足够, 生产可通过宏开关禁用全部日志输出.
**替代方案**: 零日志(当前) — 线上问题无法定位.

### ADR-005: 精简AGENTS.md (2026-05-22)
**决策**: F103放精简版AGENTS.md (~80行), 编码规范继承F407, 仅补充F103特有约束
(裸机、无CMSIS-DSP、20KB/64KB硬限制、CubeMX保护规则、文件地图).
完整规范引用F407 AGENTS.md, 避免两份文档分叉.
**理由**: AI工具打开F103目录时需本地化上下文, 不能依赖跨目录引用.

### ADR-006: 裸机健康状态机 (2026-05-22)
**决策**: 引入三级健康状态枚举, 主循环内if判断驱动:
```
HEALTH_OK       → 全功能正常, 心跳online=1
HEALTH_DEGRADED → 组件异常(error_count递增, 心跳标记, 尝试自恢复)
HEALTH_CRITICAL → 传感器失联/CAN连续失败>3次(心跳online=0, 等待IWDG复位)
```
外加IWDG独立看门狗(LSI 40kHz, 3s超时), 主循环喂狗.
**理由**: 裸机无RTOS任务隔离, 任一组件失效可能导致静默退化.
**替代方案**: 纯error_count计数(当前) — 无恢复逻辑, 故障不可观测.

### ADR-003: BSP分层架构 (2026-05-22)
**决策**: 严格遵循AGENTS.md分层 `app → bsp → HAL`, 禁止app直接调HAL.
目录结构:
```
App/                        # 纯应用逻辑
├── app_main.c              # 主循环 + 采样调度
├── dsp_nde.c/h             # 特征提取(纯算法)
├── dsp_fft_q15.c/h         # RFFT(纯算法)
└── can_send.c/h            # CAN协议(调bsp接口)
bsp/
├── adxl345/
│   ├── bsp_adxl345.c       # SPI驱动封装
│   └── bsp_adxl345.h
└── can/
    ├── bsp_can.c           # CAN HAL封装
    └── bsp_can.h
```
**理由**: 与F407节点AGENTS.md一致, app层可移植, 硬件变更仅改bsp.
**影响**: 现有adxl345_drv.c→bsp_adxl345.c, can_send中HAL调用抽出到bsp_can.
EXTI中断处理放bsp_adxl345.c, app_main通过回调获取FIFO数据.

## 代码风格规范 (与stm32_node_vibration AGENTS.md对齐)

以下规范从F407 AGENTS.md完整继承, F103特有调整已标注:

- **缩进**: 8空格, 禁止Tab
- **注释**: `/* */` 风格, 禁止 `//`
- **命名**: 小写下划线, 函数=模块名_功能名, 宏=全大写
- **结构体**: 禁止typedef, 函数指针可用typedef后缀`_t`
- **错误处理**: 返回负errno, goto统一清理
- **内存**: 禁止malloc/free, 全静态分配 (F103: 20KB SRAM)
- **日志**: 使用 `pr_info/pr_warn/pr_error` 宏, 精简版 `bsp/bsp_log.h` (~40行, 阻塞USART1输出, 可宏开关全禁)
- **变量**: 函数内变量在开头声明 (Linux内核风格)

## 参考文档

- STM32F103C8T6 Datasheet
- ADXL345 规格书
- SN65HVD230 用户手册

## 内存预算 (2026-05-21)

### SRAM (20KB total)

| 模块 | 大小 | 说明 |
|------|------|------|
| DSP 静态缓冲 | ~480B | s_window_coeff[64] + s_fft_input[64] + s_fft_output[66] + s_magnitude[33] |
| 采样缓冲 (app_main) | ~390B | 3 × buf[64] int16 |
| FIFO突发缓冲 (栈) | ~96B | burst_buf[48] int16 (16×3, 栈上临时) |
| app_main状态 | ~20B | struct app_state |
| HAL 外设句柄 | ~400B | SPI + CAN + TIM + UART + IWDG |
| CRC8表 | ~256B | bsp_can 静态查找表 |
| 栈 (主循环 + ISR + EXTI) | ~1.5KB | 裸机, 无 RTOS |
| **合计** | **~3.1KB** | **~15.7% 使用率, 安全** |

### Flash (64KB total)

| 模块 | 估算 | 说明 |
|------|------|------|
| HAL 库代码 | ~22KB | SPI + CAN + TIM + UART + GPIO + IWDG |
| CMSIS 核心 | ~2KB | startup + system |
| dsp_fft_q15 | ~1.5KB | 自写 radix-2 RFFT |
| dsp_nde | ~2.5KB | 特征提取 (函数拆分后略增) |
| bsp_adxl345 | ~1KB | SPI驱动 + FIFO突发 + EXTI回调 |
| bsp_can | ~1KB | CAN HAL封装 + CRC8表(256B) |
| can_send | ~1KB | CAN协议层 (17帧CRC8组帧) |
| app_main | ~1KB | 主循环 + 健康状态机 + IWDG |
| bsp_log | ~0.3KB | 精简日志宏 |
| newlib-nano | ~5KB | printf/scanf 精简版 |
| **合计** | **~37.3KB** | **约 58% 使用率, 安全** |

### CMSIS-DSP 放弃原因 (2026-05-21)

CMSIS-DSP 常量表为全尺寸 (支持到 4096 点 FFT)，编译进 F103 的 Flash 用量:
- arm_rfft_init_q15.c: realCoefA[8192] + realCoefB[8192] = 32KB
- arm_common_tables.c: CFFT 旋转因子 + bit-reversal 表 = ~25KB
- arm_const_structs.c: 预构建 CFFT 实例结构 = ~5KB
- **CMSIS-DSP 表合计 ~62KB，已超出 64KB Flash 总容量**

**决策**: 自写 `App/dsp_fft_q15.c` 替代 CMSIS-DSP。64 点 radix-2 DIT RFFT，代码+表 ~2KB。
