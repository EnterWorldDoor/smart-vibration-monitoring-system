# EdgeVib 系统架构设计文档（System Architecture Design Document）

---

# 1. 系统概述

## 1.1 系统名称

EdgeVib：面向工业预测性维护的边缘智能振动监测系统

## 1.2 系统目标

构建从 **设备采集 → 边缘AI → 网关 → 数据平台** 的完整工业系统，实现：

- 双通道 (DE+NDE) 振动监测与对比诊断
- 边缘 AI 实时故障检测 (CNN-LSTM + ISO 10816)
- 工业级安全联锁 (急停 + 双动作恢复 + 隔离IO)
- 多协议通信 (CAN / UART CRC16 / MQTT)

---

# 2. 总体架构设计

## 2.1 架构分层

| 层级 | 组件 | 硬件平台 |
|------|------|----------|
| 设备层 (DE端) | ESP32-S3 传感器服务 + DSP + AI推理 | ESP32-S3-DevKitC-1 + ADXL345 |
| 设备层 (NDE端) | STM32F103 NDE振动采集 + DSP + CAN上行 | STM32F103C8T6 蓝板 + ADXL345 |
| 设备层 (主控) | STM32F407 电机控制 + CAN主站 + 协议路由 + 隔离IO | DMF407 + PD6010D 驱动板 |
| 边缘AI层 | ESP32-S3 CNN-LSTM推理 + ISO 10816规则引擎 | ESP32-S3 (同DE端硬件, 不同功能) |
| 边缘网关层 | Orange Pi 4 Pro MQTT Broker + 数据聚合 + Docker | 全志A733, 4GB, Ubuntu |
| 平台层 | PC: 数据收集 + 模型训练 + 部署 | x86 GPU服务器 |

## 2.2 数据流

```
设备层                              边缘AI层                 边缘网关层             平台层
                                                          
ADXL345[DE] ──SPI──→ ESP32-S3                                                       
  (驱动端)           ├─ sensor_service (400Hz FIFO采集)                               
                     ├─ dsp (FFT/RMS/Peak)                                           
                     ├─ ai_service (CNN-LSTM推理)                                    
                     │     ├─ 24维特征向量                                           
                     │     ├─ 异常评分                                               
                     │     └─ ISO 10816 规则引擎                                     
                     │                                                               
ADXL345[NDE] ──SPI──→ STM32F103                ┌─ UART 0x17/0x18 ──→ ESP32-S3       
  (非驱动端)          ├─ dsp_fft_q15 (64点RFFT)  │   (经F407转发)     ├─ 双通道对比诊断
                     ├─ dsp_nde (24维特征)       │                    ├─ 异常源定位
                     ├─ CAN CRC8 (17帧/批) ──→ STM32F407 ──────────┘                 
                     └─ 1s心跳(0x202) ──────→  CAN主站           ↓                  
                                                                 MQTT ──→ Orange Pi 4 Pro
                                            F407 隔离IO             (Mosquitto Broker)
                                            ├─ IN1 急停 (NC)              ├─ 数据聚合
                                            ├─ IN2 模式切换               ├─ 高级AI (Python)
                                            ├─ IN3 报警复位               └─ Docker服务
                                            ├─ OUT1-3 LED
                                            └─ PF0 蜂鸣器                ↓
                                                                   Edge-AI PC
                                            F407 电机控制               ├─ 数据收集
                                            ├─ TIM1 PWM (PD6010D)      ├─ 模型训练
                                            ├─ ADC1 I/V/T 采样         └─ TFLite导出
                                            └─ 隔离保护
```

---

# 3. 硬件系统设计

## 3.1 STM32F407 主控节点 (DMF407)

### 硬件平台
- **MCU**: STM32F407IGT6 (Cortex-M4F, 168MHz, 1MB Flash, 192KB SRAM)
- **开发板**: 正点原子 ATK-DMF407 V1.0
- **电机驱动**: 正点原子 ATK-PD6010D (直流有刷, DC12-60V, 10A/600W)

### 核心功能

#### 数据采集
- ADC1 3通道 DMA 扫描 (TIM2 1kHz触发): 电机电流/电压/温度
- ADC 外设: PB0/IN8(U相电流), PB1/IN9(母线电压), PA0/IN0(NTC温度)
- 温湿度模拟器 (Random Walk算法, 验证数据通路)

#### CAN 主站 (NDE 数据接收)
- 收发器: SN65HVD230, 500kbps, 标准11-bit ID
- 接收 F103 NDE 节点: 特征帧 0x201 (17帧/批, CRC8校验) + 心跳帧 0x202
- 17帧重组为96字节特征向量 → UART4 CMD 0x17 上行 ESP32
- CAN NDE 看门狗心跳注册 (10s超时, 不触发复位)

#### 工业 IO (安全联锁)
- **隔离输入** (12路, 光耦 LTV-247):
  - IN1 (PG3, NC): 急停按钮, EXTI3 下降沿 → 关PWM, 红灯, 蜂鸣连续
  - IN2 (PG4, NO自锁): 手动/自动模式切换, 500ms 轮询
  - IN3 (PG5, NO自复): 报警复位确认, EXTI5 上升沿
  - IN4-12: 保留 (可接光电/接近/限位开关)
- **隔离输出** (4路, 光耦):
  - OUT1 (PB4): 绿灯 — 正常运行
  - OUT2 (PB5): 黄灯 — 预警 WARNING
  - OUT3 (PF1): 红灯 — 故障 CRITICAL
  - OUT4 (PC13): 保留 — 工业继电器
- **板载**: PF0 有源蜂鸣器, PE0/PE1 调试LED
- **安全状态机**: EMERGENCY → WAIT_RESET → NORMAL (双动作恢复)
- **三维状态合并**: 安全状态 > 健康等级 > 运行模式

#### 电机控制
- TIM1 6路PWM (UH/UL/VH/VL/WH/WL) → PD6010D
- TIM3 编码器接口 (A/B/Z相)
- TIM1_BKIN 刹车输入
- 安全联锁: EMERGENCY/WAIT_RESET 时硬件关断 BDTR.MOE

#### 通信接口
- **UART4** (PC10/PC11): ESP32 主通道, DMA TX + DMA RX(IDLE), CMD 0x04/0x06/0x07/0x17/0x18
- **UART5/RS232** (PC12/PD2): Orange Pi 备份通道, TPT3232E 电平转换
- **CAN1** (PB9/PI9): NDE 传感器总线, SN65HVD230
- **USART3/RS485** (PB10/PB11): Modbus RTU 从站 (预留), TP8485 自动收发
- **USART1** (PB6/PB7): 调试串口, CH340C USB-TTL
- **Ethernet** (RMII): RJ45 百兆, LAN8720A PHY, 第三备份通道 (预留)

#### 人机界面
- 2.8" TFT LCD (320×240, ILI9341/ST7789V), FSMC 16-bit Bank4
- LVGL v8.3 GUI (200Hz刷新, 5卡片平板风格)
- 4路按键 (KEY0-2 + RESET), 3路LED (DS0红/DS1绿/PWR蓝)

### 固件架构
- **RTOS**: FreeRTOS 10.3.1 (CMSIS-RTOS2 wrapper)
- **任务**:
  - wdg_daemon (osPriorityHigh, 1024B, 500ms): 注册制心跳守护 + IWDG 3s喂狗
  - proto_rx (osPriorityAboveNormal, 2KB): UART4 DMA IDLE 协议解析
  - app_enterprise (osPriorityNormal, 8KB, 1s): 主业务 + 安全状态机 + IO轮询
  - lvgl_gui (osPriorityNormal, 8KB, 5ms): GUI 渲染
- **模块层次**: App → Modules → bsp → HAL (严格分层)
- **编码风格**: Linux 内核 (8空格缩进, 小写下划线, 禁止 typedef 结构体)
- **内存**: 全静态分配, 禁止 malloc/free

---

## 3.2 STM32F103 NDE 传感器节点

### 硬件平台
- **MCU**: STM32F103C8T6 (Cortex-M3, 72MHz, 64KB Flash, 20KB SRAM)
- **开发板**: Blue Pill 蓝板最小系统
- **传感器**: ADXL345 三轴加速度计 (SPI, 400Hz)
- **CAN**: SN65HVD230 收发器, 500kbps

### 核心功能
- 裸机循环 (无 RTOS), ADXL345 INT1 FIFO 水印中断驱动采样 (16样本/突发)
- 自写 dsp_fft_q15 定点 FFT (无 CMSIS-DSP, F103 无 FPU)
- 每 64 样本窗口 (160ms, 4次FIFO突发) 计算 24 维特征向量
- 2s 周期 CAN 发送特征 (17帧 CRC8), 1s 周期心跳 (含 CRC8)
- 三级健康状态机 + IWDG 看门狗 (3s超时)
- 内存预算: SRAM 3.1KB/20KB (15.7%), Flash 37.3KB/64KB (58%)

### 特征向量 (24维, 与 DE 端完全一致)
```
[0-3]:   rms_x, rms_y, rms_z, overall_rms
[4-5]:   peak_freq_x, peak_amp_x
[6-8]:   skewness_x, kurtosis_x, crest_factor_x
[9-16]:  band_energy_x[0..7]
[17-19]: peak_freq_y, peak_amp_y, crest_factor_y
[20-22]: peak_freq_z, peak_amp_z, crest_factor_z
[23]:    0.0f (无温度传感器)
```

---

## 3.3 ESP32-S3 边缘 AI 网关

### 硬件平台
- **MCU**: ESP32-S3-DevKitC-1 (WROOM-1/1U/2)
- **传感器**: ADXL345 三轴加速度计 (SPI, 驱动端 DE), DHT11 温湿度
- **通信**: WiFi (MQTT), UART (STM32 F407)

### 核心功能

#### 传感器服务 (sensor_service)
- ADXL345 SPI 驱动 (400Hz FIFO 水印中断)
- 数据缓冲 + 时间戳

#### DSP 数字信号处理 (dsp)
- FFT 频谱分析 (CMSIS-DSP)
- RMS/Peak/Crest Factor/Kurtosis/Skewness 计算
- 24 维特征向量提取
- 8 频带能量分解

#### AI 推理服务 (ai_service)
- **模型**: CNN-LSTM 级联架构 (TFLite Micro)
- **输入**: 24 维特征向量 (与 NDE 端格式一致)
- **输出**: 异常评分 (0.0-1.0) + 故障类别
- **双通道对比**: DE 特征 vs NDE 特征 → 故障源定位 (电机侧/负载侧)
- **ISO 10816 规则引擎**: 基于振动烈度等级的确定性诊断

#### 通信
- **MQTT**: `EdgeVib/{device_id}/data/sensor` (特征数据), `EdgeVib/{device_id}/status/health` (健康状态)
- **UART 协议**: CRC16-MODBUS 二进制帧 → STM32F407
- **协议帧格式**: `[AA 55] [LEN_H LEN_L] [DEV] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]`

#### 企业级模块
- **config_manager**: NVS 存储 + CRC32 校验, 版本迁移, 工厂重置
- **system_monitor**: CPU/内存/任务栈/WDT 监控, 阈值告警
- **log_system**: 环形缓冲 + 分级过滤 + 文件存储
- **ota_update**: SHA256 校验, 同步/异步模式, 断点续传
- **time_sync**: NTP 时间同步
- **data_manager**: 数据缓存 + 批量上传

---

## 3.4 Orange Pi 4 Pro 边缘网关

### 硬件平台
- **SoC**: 全志 A733, 4GB RAM
- **OS**: Ubuntu
- **角色**: MQTT Broker + 数据聚合 + 高级 AI + 视觉监测 + Docker 服务

### 核心功能
- **Mosquitto MQTT Broker**: 本地消息总线
- **数据聚合**: ESP32 MQTT → SQLite/InfluxDB
- **Python AI**: 趋势分析 + 剩余寿命预测 (复杂模型)
- **Docker**: 容器化服务部署
- **视觉监测**: MIPI CSI 相机

---

# 4. 通信协议设计

## 4.1 STM32F407 ↔ ESP32-S3 (UART4)

### 物理层
- 接口: UART4, 921600 bps, 8N1
- 芯片: STM32F407 PC10(TX)/PC11(RX) ↔ ESP32-S3

### 链路层
- 帧格式: `[AA 55] [LEN_H LEN_L] [DEV_ID] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]`
- CRC: CRC16-MODBUS (多项式 0xA001, 初始值 0xFFFF), 大端序
- 覆盖范围: LEN(2B) + DEV(1B) + CMD(1B) + SEQ(1B) + DATA(LEN B)

### 协议架构
- STM32 侧: 10 状态帧解析器 (protocol_parser.c) + DMA1_Stream2 RX (IDLE中断) + proto_rx 任务
- ESP32 侧: 10 状态帧解析器 (protocol.c) + 回调注册制 CMD 分发

### 命令字表

| CMD | 方向 | 名称 | 周期 | 载荷 |
|-----|------|------|------|------|
| 0x04 | F407→ESP32 | 温湿度数据 | 1s | 16B (温度+湿度+时间戳) |
| 0x06 | F407→ESP32 | 电机状态 | 2s | 12B (电压+电流+功率) |
| 0x07 | F407→ESP32 | 系统状态 | 事件驱动 | 8B (安全+健康+模式) |
| 0x10 | ESP32→F407 | AI分析结果 | 事件驱动 | TBD |
| 0x15 | ESP32→F407 | 电机控制 | 按需 | 子命令+参数 |
| 0x17 | F407→ESP32 | NDE特征向量 | 2s | 100B (4头+96特征) |
| 0x18 | F407→ESP32 | NDE心跳 | 1s | 4B (在线+错误+温度) |

---

## 4.2 STM32F103 ↔ STM32F407 (CAN)

### 物理层
- 接口: CAN 2.0B, 500 kbps, 标准 11-bit ID
- 收发器: 两端均 SN65HVD230
- 连接: 仅 CANH/CANL 差分线对连, 两端电气隔离

### 特征帧 (0x201, 17帧/批次, 每2s)
```
帧0:  [seq=0] [window_idx] [feat[0..4] 5字节] [CRC8]  = 8字节
帧1-16: [seq=N] [feat字节 6个]            [CRC8]  = 8字节

CRC8: CRC-8-Dallas/Maxim (0x31), 覆盖 data[0..6]
总容量: 101字节有效载荷 ≥ 96字节特征向量
```

### 心跳帧 (0x202, 1帧/次, 每1s)
```
[0] online (0x01=在线, 0x00=离线)
[1] error_count
[2] temp_c (int8)
[3] CRC8 (data[0..2])
[4-7] reserved
```

---

## 4.3 ESP32-S3 → Orange Pi 4 Pro (MQTT)

- Topic: `EdgeVib/{device_id}/data/sensor` (JSON payload)
- Topic: `EdgeVib/{device_id}/status/health` (JSON payload)
- Broker: Mosquitto (运行在 Orange Pi)

---

# 5. 安全架构

## 5.1 安全状态机 (双动作恢复, ISO 13850 兼容)

```
                    IN1 NC断开 (拍下急停)
NORMAL ──────────────────────────────────→ EMERGENCY
  │                                            │  TIM1 BDTR.MOE=0 (硬件关PWM)
  │  绿灯 电机运行                              │  红灯常亮 蜂鸣连续
  │                                            │  所有电机CMD被忽略
  │                                   IN1 NC重新闭合 (旋转复位)
  │                                            │
  │                                  WAIT_RESET ←──┘
  │                                            │  黄灯闪烁 蜂鸣停止
  │                                            │  电机不自动恢复!
  │                                            │
  └──── IN3 按下 (报警复位确认) ───────────────────┘
                                               │
                                              NORMAL
                                               绿灯 电机CMD恢复
```

## 5.2 安全规则

1. 急停复位后不自动重启电机 — 必须 IN3 手动确认 (双动作恢复)
2. EMERGENCY/WAIT_RESET 状态下所有电机控制指令被忽略 + 回复 REJECT
3. 上电检测: IN1 已断开 → 直接 EMERGENCY (Fail-Safe 启动)
4. IN1 断线 (NC 开路) = 立即 EMERGENCY (Fail-Safe)
5. 手动模式: AI 控制指令丢弃, 电机保持当前状态

## 5.3 看门狗策略
- **IWDG**: LSI 40kHz, 3s 超时, 独立时钟
- **wdg_daemon**: osPriorityHigh, 500ms 周期, 注册制 (最多16槽位)
- **注册项**: app_enterprise(3s/复位), uart4_tx(5s/复位), can_nde(10s/不复位)
- **WWDG 放弃原因**: 最大超时 ~49ms, 不匹配 FreeRTOS 秒级任务

---

# 6. 关键架构决策记录 (ADR)

### ADR: 协议选择 CRC16-MODBUS 二进制而非 JSON
- **决策**: UART 通信使用紧凑二进制帧 (典型 30-100 字节) 而非 JSON
- **理由**: JSON 解析需数百字节缓冲区 + 浮点格式化, 二进制帧开销固定
- **ESP32 ↔ STM32**: 两端使用相同的 10 状态帧解析器

### ADR: STM32F103 自写定点 FFT 放弃 CMSIS-DSP
- **决策**: 自写 64 点 radix-2 RFFT (~1.5KB Flash) 替代 CMSIS-DSP
- **理由**: CMSIS-DSP 全尺寸常量表 ~62KB, F103 总 Flash 仅 64KB

### ADR: CAN 帧级 CRC8 而非批次级 CRC16
- **决策**: 每帧 CRC-8-Dallas/Maxim (0x31), 覆盖 seq+payload 共 7 字节
- **理由**: 可精确定位损坏帧, 无需重传整个批次

### ADR: 双动作急停恢复 (ISO 13850 兼容)
- **决策**: 急停拍下→EMERGENCY; 复位→WAIT_RESET; 确认→NORMAL
- **理由**: 防止急停意外复位后设备自动启动 (工业安全标准)

---

# 7. 性能指标

| 指标 | 目标值 | 实现状态 |
|------|--------|----------|
| F103 DSP 特征计算延迟 | < 5ms | ~3.8ms |
| CAN 批次传输延迟 | < 50ms | ~10ms (17帧 @ 500kbps) |
| F407 UART 帧延迟 | < 10ms | DMA, 非阻塞 |
| ESP32 AI 推理延迟 | < 50ms | TFLite Micro |
| 安全回路响应 (急停→PWM关) | < 100μs | EXTI ISR 直连 |
| IWDG 看门狗超时 | 3s | LSI 40kHz |
| F103 Flash 使用率 | < 80% | 58% |
| F103 SRAM 使用率 | < 70% | 15.7% |

---

# 8. 项目文件结构

```
firmware/
├── esp32-gateway/              # ESP32-S3 边缘 AI 网关
│   ├── main/
│   └── components/
│       ├── sensor_service/     # ADXL345 驱动 + 数据采集
│       ├── dsp/                # FFT/RMS/Peak 特征提取
│       ├── ai_service/         # CNN-LSTM + ISO 10816
│       ├── mqtt_app/           # MQTT 客户端
│       ├── protocol/           # UART 协议栈
│       ├── config_manager/     # 配置管理 (NVS+CRC32)
│       ├── system_monitor/     # 系统监控
│       ├── log_system/         # 日志系统
│       ├── ota_update/         # OTA 固件更新
│       └── time_sync/          # 时间同步
├── stm32_node_vibration/       # STM32F407 DMF407 主控节点
│   ├── App/
│   │   ├── app_main.c          # 企业级主任务 (安全状态机 + IO轮询)
│   │   ├── can_nde.c/h         # NDE CAN 接收 + 多帧重组
│   │   └── gui/                # LVGL GUI
│   ├── Modules/
│   │   ├── digital_io/         # 12路隔离输入 + 安全状态机
│   │   ├── alarm_service/      # 4路隔离输出 + 蜂鸣器 + LED
│   │   ├── protocol/           # UART 协议栈 (10状态解析器 + DMA)
│   │   ├── wdg/                # 看门狗心跳守护
│   │   ├── global_error/       # 统一错误码
│   │   └── system_log/         # 日志系统
│   └── Core/                   # CubeMX 生成 (HAL + 外设)
└── stm32_node_nde/             # STM32F103 NDE 传感器节点
    ├── App/
    │   ├── app_main.c          # 裸机主循环 + 健康状态机
    │   ├── dsp_nde.c/h         # 24维特征提取
    │   ├── dsp_fft_q15.c/h     # 自写定点 64点 RFFT
    │   └── can_send.c/h        # CAN 17帧 CRC8 组帧
    ├── bsp/
    │   ├── adxl345/             # ADXL345 SPI 驱动 + FIFO 突发
    │   ├── can/                 # CAN HAL 封装 + CRC8 表
    │   └── bsp_log.h           # 精简日志宏
    └── Core/                   # CubeMX 生成 (HAL + 外设)
```
