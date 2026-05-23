# STM32 Node Vibration — 硬件域上下文

## 硬件平台

**主控芯片**: STM32F407IGT6 (Cortex-M4F, 168MHz, FPU+DSP, LQFP176)
**开发板**: 正点原子 ATK-DMF407 V1.0 (电机开发板)
**电机驱动**: 正点原子 ATK-PD6010D (直流有刷驱动板)

---

## STM32F407IGT6 芯片参数

### 核心规格
- **内核**: ARM Cortex-M4F, 210 DMIPS, 单周期 DSP + FPU
- **频率**: 最高 168MHz, ART加速器 (0等待Flash执行)
- **Flash**: 1024KB (1MB)
- **SRAM**: 192KB (含 64KB CCM data RAM)
- **OTP**: 512 bytes
- **封装**: LQFP176 (140个可用GPIO)

### 外设资源
- **定时器**: 2个高级TIM1/TIM8, 8个通用TIM2-5/TIM9-14, 2个基本TIM6-7
- **ADC**: 3×12位, 2.4MSPS, 最多24通道, 三交错模式下7.2MSPS
- **DAC**: 2×12位
- **DMA**: 16-stream DMA控制器, 支持FIFO和Burst
- **SPI**: 3路 (SPI1 42Mbit/s, SPI2/3 21Mbit/s), 2路复用全双工I2S
- **I2C**: 3路 (SMBus/PMBus)
- **USART**: 4路 (USART1/2/3/6, 最高10.5Mbit/s), UART4/5 2路
- **CAN**: 2路 (2.0B Active)
- **USB**: OTG_FS + OTG_HS (带片上PHY + ULPI)
- **以太网**: 10/100M MAC, 支持MII/RMII, IEEE 1588v2
- **SDIO**: SD/SDIO/MMC v4.2
- **DCMI**: 8-14位摄像头接口, 最大54MB/s
- **FSMC**: 灵活静态存储控制器 (支持CF/SRAM/PSRAM/NOR/NAND)
- **RTC**: 亚秒精度, 硬件日历
- **硬件随机数生成器**
- **CRC计算单元**

### 电源与低功耗
- 电压: VDD 1.8V-3.6V, I/O支持5V耐压 (138个)
- 低功耗: Sleep/Stop/Standby模式
- VBAT供电区域: 20×32位备份寄存器 + 可选4KB备份SRAM

### 封装引脚分布
- LQFP176: 176引脚, GPIO分布在PA0-15, PB0-15, PC0-15, PD0-15, PE0-15, PF0-15, PG0-15, PH0-15, PI0-11
- 专用引脚: NRST(31), PH0-OSC_IN(29), PH1-OSC_OUT(30), PC14-OSC32_IN(9), PC15-OSC32_OUT(10)
- Boot: BOOT0(58), BOOT1/PB2

---

## DMF407 电机开发板硬件资源

### 板载规格
- **型号**: ATK-DMF407 V1.0
- **尺寸**: 110mm × 196.5mm
- **供电**: 5V(USB) / DC6-24V(DC005)
- **工作电流**: 100-140mA @5V
- **温度范围**: 0℃ ~ +70℃

### 板载存储
- **SPI Flash**: 16MB (25Q128, SPI2)
- **EEPROM**: 256B (24C02, I2C2)

### 板载通信接口
- **以太网**: RJ45百兆 (LAN8720A PHY, RMII接口)
- **RS232**: DB9母口 (TPT3232E, UART5)
- **RS485**: HT396R端子 (TP8485, USART3, 自动收发)
- **CAN**: HT396R端子 (TJA1050, CAN1, 120R终端电阻)
- **USB串口**: CH340C (USART1)
- **USB SLAVE**: Type-C (内带USB FS)
- **USB HOST**: USB-A座
- **温湿度传感器**: DHT11/DS18B20接口 (PG15, 单总线)

### 电机控制接口
- **直流有刷/无刷驱动接口**: 2路 (PM1/PM2, 2×12P牛角座)
- **步进电机驱动接口**: 4路 (STEP1-4, HT396R, 光耦隔离)
- **舵机接口**: 3路 (SERVO1-3, 2.54排针, 电平转换)
- **编码器接口**: 2路 (HT396R, A/B/Z相)
- **模拟量采集**: 6路 (HT396R)
- **NTC扩展**: 1路 (XH2.54)
- **刹车输入**: 2路 (TIM1_BKIN, TIM8_BKIN)

### 通用IO
- **隔离输入**: 12路 (光耦隔离)
- **隔离输出**: 4路 (光耦隔离)
- **LED**: DS0(红, PE0), DS1(绿, PE1), PWR(蓝, 电源)
- **按键**: KEY0(PE2), KEY1(PE3), KEY2(PE4), RESET
- **蜂鸣器**: 有源蜂鸣器 (PF0)

### 电源系统
- DCDC 5V/3A, LDO 3.3V/2A (均带TVS)
- 4路功率保护开关
- 扩展电源输出: 5V/1.5A + 3.3V/1.5A

---

## DMF407 引脚分配 — 本系统关键IO

### ESP32通信 (UART4)
| 引脚 | GPIO | 功能 |
|------|------|------|
| 139 | PC10 | UART4_TX → ESP32 RX |
| 140 | PC11 | UART4_RXD ← ESP32 TX |

### CAN总线 (SN65HVD230, 500kbps)
| 引脚 | GPIO | 功能 |
|------|------|------|
| 198 | PB9 | CAN1_TX → SN65HVD230 TXD (AF9) |
| 7 | PI9 | CAN1_RX ← SN65HVD230 RXD (AF9) |

- 收发器: SN65HVD230 CAN Board 模块
- 总线: CAN2.0B, 500kbps, 标准11-bit ID
- NDE通信: F407作为CAN主站，接收F103的NDE特征帧(CAN ID 0x201)和心跳帧(0x202)
- 16帧重组为96字节特征向量(CAN帧: seq+crc8+6字节payload)
- 校验通过后封装UART CMD 0x17上行给ESP32

### Orange Pi 备份通道 (UART5/RS232)
| 引脚 | GPIO | 功能 |
|------|------|------|
| — | PC12 | UART5_TX → TPT3232E → DB9 RS232 → USB-RS232 → Orange Pi |
| — | PD2 | UART5_RX ← TPT3232E ← DB9 RS232 ← USB-RS232 ← Orange Pi |

- 电平转换: TPT3232E (TTL ↔ RS232)
- 连接方式: DMF407 DB9母口 → RS232公头线 → USB-RS232转换器 → Orange Pi USB
- 角色: 路径B备份通道, ESP32离线时切换
- 切换策略: F407监测ESP32 UART4心跳, 丢失>3s切UART5, 恢复后切回
- 承载: OTA固件包、关键告警、电机电流/电压(CMD 0x06)保底数据

### Orange Pi RS485/Modbus (USART3, 可选增强)
| 引脚 | GPIO | 功能 |
|------|------|------|
| — | PB10 | USART3_TX → TP8485 → RS485端子 → USB-RS485 → Orange Pi |
| — | PB11 | USART3_RX ← TP8485 ← RS485端子 ← USB-RS485 ← Orange Pi |

- 收发器: TP8485 (自动方向控制)
- 协议: Modbus RTU, F407做从站
- Orange Pi 侧: USB-RS485模块 + OPC UA Server 做 Modbus ↔ OPC UA 协议网关

### 调试串口 (USART1)
| 引脚 | GPIO | 功能 |
|------|------|------|
| 164 | PB6 | USART1_TX → CH340 RX |
| 165 | PB7 | USART1_RXD ← CH340 TX |

### LCD显示 (FSMC Bank4 + 触摸)
| 引脚 | GPIO | 功能 |
|------|------|------|
| 66 | PG0 | FSMC_A10 (LCD RS) |
| 155 | PG12 | FSMC_NE4 (LCD CS) |
| - | NRST | LCD RESET (共用一个复位按钮) |
| 86 | PH9 | TIM12_CH2 (LCD背光 PWM) |
| 104-105,142-143,68-78 | PD14-15, PD0-1, PE7-15 | FSMC D0-D15 (16位数据) |
| 99 | PD11 | T_MISO (触摸SPI) |
| 85 | PH8 | T_MOSI (触摸SPI) |
| 83 | PH6 | T_SCK (触摸SPI) |
| 67 | PG1 | T_CS (触摸CS) |
| 84 | PH7 | T_PEN (触摸中断) |

### 温湿度传感器 (DHT11/DS18B20)
| 引脚 | GPIO | 功能 |
|------|------|------|
| 160 | PG15 | 1WIRE_DQ (单总线数据) |

### 外扩SPI Flash + EEPROM
| 引脚 | GPIO | 功能 |
|------|------|------|
| 131 | PI0 | SPI2_CS (Flash) |
| 132 | PI1 | SPI2_SCK |
| 134 | PI3 | SPI2_MOSI |
| 133 | PI2 | SPI2_MISO |
| 45 | PH4 | I2C2_SCL (24C02) |
| 46 | PH5 | I2C2_SDA (24C02) |

### DMF407 直流有刷驱动接口1 (PM1) — 全功能接口
| 引脚 | GPIO | 外设 | 信号 |
|------|------|------|------|
| 119 | PA8 | TIM1_CH1 | PM1_PWM_UH (上桥PWM) |
| 120 | PA9 | TIM1_CH2 | PM1_PWM_VH |
| 121 | PA10 | TIM1_CH3 | PM1_PWM_WH |
| 93 | PB13 | TIM1_CH1N | PM1_PWM_UL (下桥PWM) |
| 94 | PB14 | TIM1_CH2N | PM1_PWM_VL |
| 95 | PB15 | TIM1_CH3N | PM1_PWM_WL |
| 92 | PB12 | TIM1_BKIN | PM1刹车信号 |
| 56 | PB0 | ADC1_IN8 | PM1_AMPU (U相电流) |
| 52 | PA6 | ADC12_IN6 | PM1_AMPV (V相电流) |
| 47 | PA3 | ADC2_IN3 | PM1_AMPW (W相电流) |
| 57 | PB1 | ADC1_IN9 | PM1_VBUS (母线电压) |
| 40 | PA0 | ADC1_IN0 | PM1_VTEMP (温度) |
| 115 | PC6 | TIM3_CH1 | PM1_ENCA (编码器A) |
| 116 | PC7 | TIM3_CH2 | PM1_ENCB (编码器B) |
| 5 | PE6 | TIM9_CH2 | PM1_ENCZ (编码器Z) |
| 87 | PH10 | TIM5_CH1 | PM1_HALLU (霍尔U) |
| 88 | PH11 | TIM5_CH2 | PM1_HALLV (霍尔V) |
| 89 | PH12 | TIM5_CH3 | PM1_HALLW (霍尔W) |
| 27 | PF9 | ADC3_IN7 | PM1_BEMFU (反电动势U) |
| 26 | PF8 | ADC3_IN6 | PM1_BEMFV (反电动势V) |
| 25 | PF7 | ADC3_IN5 | PM1_BEMFW (反电动势W) |
| 28 | PF10 | - | PM1_CTRL_SD (停机控制) |

### 关键复用冲突
- **PI5/PI6/PI7**: 3路复用 — 直流驱动板2上桥 / 步进驱动1-3脉冲 / 舵机1-3
- **PA4/PA5**: 2路复用 — DAC输出 或 驱动板2电流/电压采集
- **PD5**: 2路复用 — FSMC NWE 或 HMI串口屏TX (USART2)

---

## ATK-PD6010D 直流有刷驱动板

### 电气参数
- **供电**: DC 12~60V (宽范围)
- **最大输出**: 10A, 600W
- **H桥方案**: 4×IRFS3607 MOSFET + 2×IR2104S 半桥驱动
- **PWM控制**: 2路 (UH/UL), 双路高速光耦EL0631隔离
- **编码器**: 3相 A/B/Z, 5V/11V供电可选 (默认5V)
- **过流保护**: 硬件10A迟滞比较器 (LMV331), 支持周期/锁存两种模式
- **信号隔离**: 控制信号全光耦隔离, 采集信号ESD保护

### 模拟信号采集
- **电流**: 20mR采样电阻 → TP2412差分放大 (增益6倍) → Iout = 6×0.02×I + 1.27V
- **电压**: 母线POWER经12K+12K+1K分压 → VBUS = POWER/25
- **温度**: NTC 10KΩ@25℃ + 4.7K分压 → VTEMP = 3.3V/(Rt+4.7K)×4.7K

### 接口定义 (与DMF407 PM1/PM2直连)
- **CN4** (2×12P牛角座): 专用控制采集接口 → DMF407 PMx
  - PWM_UH, PWM_UL (2路PWM)
  - ENCA/B/Z (3相编码器)
  - CURT (电流), VBUS (电压), VTEMP (温度)
  - CTRL_SD (停机控制)
- **CN3** (10P HT396R): 备用控制接口 (其他开发板)
- **CN2** (6P HT396R): 编码器接口
- **CN1** (4P KF7620): 电机线(M+/M-) + 电源(VIN/GND)

### 使用注意事项
1. 请勿过压过载使用 (DC12~60V, 10A max)
2. 大功率电机启动时使用缓启动, 避免触发过流保护
3. 关闭H桥时使用缓慢降速, 避免反电动势烧毁驱动板
4. 按丝印正确接线, 先接控制线和电机线, 再接电源线, 最后上电

---

## 系统供电架构

```
外部电源 DC6~24V
  └→ DMF407 DCDC → 5V/3A → LDO → 3.3V/2A
       ├→ STM32F407IGT6 (3.3V)
       ├→ LCD (3.3V)
       ├→ 传感器 (3.3V/5V)
       └→ ATK-PD6010D (独立DC 12~60V供电)
            └→ 内部 DCDC → 11V(半桥IC) → 5V(光耦) → LDO → 3.3V(运放)
```

## 增强模块 (待实现)

### 本地声光报警 (alarm-service)

#### 输出引脚映射 (DMF407 隔离输出端子 CN25)

光耦 LTV-247 共阴极接法, MCU推挽HIGH→光耦导通→输出端LOW(灌电流), 4.7K上拉默认输出HIGH(灯灭):

| 输出 | GPIO | 功能 | 驱动逻辑 | 用途 |
|------|------|------|----------|------|
| OUT1 | **PB4** | 绿灯 AD16-22DS | HIGH=光耦导通=灯亮 | 正常运行 |
| OUT2 | **PB5** | 黄灯 AD16-22DS | HIGH=灯亮 | 预警 WARNING |
| OUT3 | **PF1** | 红灯 AD16-22DS | HIGH=灯亮 | 故障 CRITICAL |
| OUT4 | **PC13** | 保留 | HIGH=吸合 | 工业继电器 |
| — | **PF0** | 有源蜂鸣器 | HIGH=响 | 声音报警 |
| DS0 | PE0 | 板载红灯 | LOW=亮 | 调试辅助 |
| DS1 | PE1 | 板载绿灯 | LOW=亮 | 调试辅助 |

#### alarm_service 模块架构

```
Modules/alarm_service/
├── alarm_service.h       # 公共接口
└── alarm_service.c       # LED驱动 + 蜂鸣器PWM + 输出状态机
```

核心接口:
```c
int  alarm_service_init(void);                          // 初始化 GPIO + PWM (PF0, TIMx_CHx)
void alarm_service_update_health(health_level_t level); // 更新健康等级 (来自AI/规则引擎)
void alarm_service_refresh(void);                       // 读取系统状态 → 查矩阵 → 刷新输出
void alarm_emergency_stop(void);                        // ISR安全: 直接关PWM+红灯+蜂鸣连续
```

`alarm_service_refresh()` 在 app_enterprise 主循环中周期调用, 读取 `iso_get_system_state()` 查状态矩阵驱动输出。

### 隔离输入 (digital-io, 12路)

#### 物理按钮选型 (LA38-11BN 22mm)

LA38-11BN 底部 4 螺丝 = 1NO + 1NC 两组触点，本项目每按钮仅用一组:

| 按钮 | 功能 | 触点 | 类型 | 接线 | 逻辑 |
|------|------|------|------|------|------|
| IN1 | 急停 (E-Stop) | **NC (常闭)** | LA38-11ZS 蘑菇头旋转复位 (ISO 13850) | 红螺丝对 | 正常=闭合(1), 拍下=断开(0)→停机, 旋转复位后才闭合 |
| IN2 | 手动/自动切换 | **NO (常开)** | 自锁式 (机械保持) | 绿螺丝对 | 抬起=手动(0), 按下锁住=自动(1) |
| IN3 | 报警复位/静音 | **NO (常开)** | 自复式 (瞬动, 松开弹开) | 绿螺丝对 | 按下=上升沿→复位/静音 |

**选型理由 (ADL — Architecture Decision Log)**:
- **急停用 NC**: 断线/松动=信号断开=Fail-Safe, 符合 ISO 13850。接 NO 则线断了永不发现。
- **模式切换用 NO + 自锁**: 机械保持状态, 上电一次读到; NO 天然表达"激活"语义。
- **报警复任用 NO + 自复**: 上升沿触发, 防重复触发; 松开自动弹开, 无需软件去抖复位。

#### 软件架构: 语义统一层 (方案 B)

`Modules/digital_io/` 模块提供 `iso_input_read(ch)` 接口，内部完成 NC/NO 物理反转：

```
物理层 (GPIO读)          语义层 (iso_input_read)       业务层
NC→闭合=GPIO_SET  ──→  1 = 正常运行 / 0 = 停机       alarm_service
NO→闭合=GPIO_SET  ──→  1 = 自动模式 / 0 = 手动模式    app_enterprise
NO→闭合=GPIO_SET  ──→  1 = 触发复位 / 0 = 空闲        alarm_service
```

**理由**: 上层代码不感知某路输入是 NC 还是 NO。换传感器类型只需改配置表，不动业务逻辑。

#### 输入配置表设计 (每通道)

```c
struct iso_input_cfg {
    uint8_t channel;       /* IN1..IN12 */
    GPIO_TypeDef *port;    /* GPIO端口 */
    uint16_t pin;          /* GPIO引脚 */
    bool is_nc;            /* true=NC触点(需反转), false=NO触点 */
    bool is_latching;      /* true=自锁(电平型), false=自复(边沿型) */
    const char *name;      /* 名称 (日志用) */
};
```

- IN4-IN12: 保留, 可接外部工业传感器 (光电/接近/限位开关), 光耦隔离抗干扰
- 12路全部可配置为 NC/NO 和 电平/边沿 检测模式

#### GPIO 引脚映射 (DMF407 隔离输入端子)

从 CubeMX IOC 文件确认, 所有 `GPIO_Input + PULLUP` 引脚共12个, 对应12路隔离输入:

| 通道 | GPIO | EXTI线 | 分配 | 检测方式 | 激活边沿 |
|------|------|--------|------|----------|----------|
| IN1 | **PG3** | EXTI3 | 急停 (NC) | **EXTI 下降沿** | 断开=停机 |
| IN2 | PG4 | EXTI4 | 模式切换 (NO自锁) | **轮询 500ms** | — |
| IN3 | **PG5** | EXTI5 | 报警复位 (NO自复) | **EXTI 上升沿** | 按下=复位 |
| IN4 | PG6 | EXTI6 | 保留 | — | — |
| IN5 | PG7 | **EXTI7** ⚠️ | 保留 | — | — |
| IN6 | PG8 | **EXTI8** ⚠️ | 保留 | — | — |
| IN7 | PG9 | EXTI9 | 保留 | — | — |
| IN8 | PG10 | EXTI10 | 保留 | — | — |
| IN9 | PC8 | **EXTI8** ⚠️ | 保留 | — | — |
| IN10 | PD3 | **EXTI3** ⚠️ | 保留 | — | — |
| IN11 | PD7 | **EXTI7** ⚠️ | 保留 | — | — |
| IN12 | PG2 | EXTI2 | 保留 | — | — |

**EXTI 共享冲突** (STM32F4 每 EXTI 线仅一个 Port):
- `EXTI3`: PG3(IN1,已用) ↔ PD3(IN10) — IN10 不能再配 EXTI
- `EXTI7`: PG7(IN5) ↔ PD7(IN11) — 二选一
- `EXTI8`: PG8(IN6) ↔ PC8(IN9) — 二选一

#### FreeRTOS 任务架构 (方案 C — ISR 直连 + 轮询寄生)

```
EXTI3_IRQ (IN1急停, 下降沿)          EXTI5_IRQ (IN3复位, 上升沿)
    │                                       │
    ├─ alarm_emergency_stop()               ├─ 设事件标志 ALARM_RESET
    ├─ TIM1 PWM 强制关闭                     │
    └─ 系统状态 → EMERGENCY                  │
                                            │
app_enterprise 主循环 (1s)                  │
    ├── iso_input_poll_mode_switch()  ←─────┘ 检测事件标志
    ├── iso_input_process_events()        ├─ alarm_clear()
    ├── send_temp_to_esp32()              └─ UART4 上行 状态变化事件
    └── osDelay(1000)
```

**选型理由**:
- 急停 ISR 直连, 0任务延迟 — 安全硬需求
- 不复用独立 FreeRTOS 任务 (省 1KB 栈, SRAM 仅 192KB)
- EXTI 回调极短 (<10μs), 仅设标志+关PWM, 不影响调度
- 轮询 IN2 寄生在现有 1s 循环中, 500ms 足够捕获人工按钮动作
- 与现有 wdg_heartbeat 注册制天然集成 (`iso_input` 心跳插槽已预留)

#### 急停安全联锁状态机 (双动作恢复)

```
                        EXTI3 下降沿 (IN1 NC断开)
NORMAL ──────────────────────────────────────→ EMERGENCY
  │                                                │
  │  绿灯(OUT1)                                     │  TIM1 BDTR.MOE=0 (硬件关断PWM)
  │  电机正常运行                                    │  红灯常亮(OUT3) + 蜂鸣连续(PF0)
  │                                                │  UART4上行 CMD 0x10 (紧急停机事件)
  │                                                │  系统状态锁存, 所有电机CMD被忽略
  │                                                │
  │                                         EXTI3 上升沿 (IN1 旋转复位, NC重新闭合)
  │                                                │
  │                                        WAIT_RESET ←──┘
  │                                                │
  │                                                │  黄灯闪烁(OUT2) + 蜂鸣停止
  │                                                │  等待手动复位确认
  │                                                │  电机**不自动恢复**!
  │                                                │
  └──── IN3 上升沿 (报警复位确认) ────────────────────┘
                                                    │
                                                    │  绿灯恢复(OUT1)
                                                    │  TIM1 BDTR.MOE=1
                                                    │  UART4上行 CMD 0x10 (恢复运行)
                                                    │  电机CMD恢复接收
                                                    │
                                                   NORMAL
```

**安全规则**:
1. 急停复位后绝不自动重启电机, 必须等待 IN3 手动复位确认 (双动作恢复)
2. EMERGENCY 状态下所有电机控制指令 (CMD 0x15) 被忽略并回复 REJECT
3. 上电检测: 如果 IN1 已断开, 直接进入 EMERGENCY 状态 (Fail-Safe 启动)
4. IN1 断线 (NC 开路) = 立即 EMERGENCY (Fail-Safe)

#### 手动/自动模式切换 (IN2 自锁按钮)

IN2 改变的是**电机控制权**，数据上行不受影响:

| 模式 | IN2 输入 | 电机控制策略 | 数据上行 | 典型场景 |
|------|---------|-------------|----------|----------|
| **自动** | NO闭合→语义1 | ESP32 AI 推理结果 → F407 自动执行 | 正常 | 预测性维护运行 |
| **手动** | NO断开→语义0 | 忽略 AI 控制指令, 电机保持当前状态 | 正常 | 调试/维护/故障排查 |

**具体行为**:
- 自动模式: `PROTO_CMD_MOTOR_CONTROL (0x15)` 回调正常执行
- 手动模式: `on_proto_downlink()` 收到 0x15 → 丢弃 + 回复 REJECT 应答 (status=MANUAL_MODE)
- 模式切换事件即时通过 UART4 上行 ESP32 (含当前模式状态)
- 上电默认: 读取 IN2 物理状态决定初始模式

#### 系统状态模型 (三维优先级合并)

三个独立状态维度合并为统一系统行为, 优先级: **安全状态 > 健康等级 > 运行模式**

| 安全状态  | 健康等级  | 运行模式 | 电机行为   | LED 输出      | 蜂鸣器 |
|----------|----------|---------|-----------|---------------|--------|
| EMERGENCY | 任意     | 任意    | 强制停机   | 红灯常亮(OUT3)  | 连续   |
| WAIT_RESET | 任意    | 任意    | 强制停机   | 黄灯闪烁(OUT2)  | 停止   |
| NORMAL   | CRITICAL | AUTO    | AI决定停机 | 红灯常亮(OUT3)  | 连续   |
| NORMAL   | CRITICAL | MANUAL  | 保持当前   | 红灯常亮(OUT3)  | 静音   |
| NORMAL   | WARNING  | AUTO    | AI决定    | 黄灯常亮(OUT2)  | 1s间歇 |
| NORMAL   | WARNING  | MANUAL  | 保持当前   | 黄灯常亮(OUT2)  | 静音   |
| NORMAL   | NORMAL   | AUTO    | AI控制    | 绿灯常亮(OUT1)  | 停止   |
| NORMAL   | NORMAL   | MANUAL  | 保持当前   | 绿灯常亮(OUT1)  | 停止   |

**设计要点**:
- EMERGENCY/WAIT_RESET 最高优先级 — 无视模式和健康等级, 电机必须停
- 手动模式不执行 AI 控制, CRITICAL 时只亮红灯告警 (调试场景不破坏现场)
- 自动模式 + CRITICAL → 电机停机 — 预测性维护核心闭环
- 电机恢复条件: NORMAL安全 + (NORMAL/WARNING健康) + (AUTO/MANUAL任意) 同时满足
- 状态变化时即时 UART4 上行 ESP32

#### digital_io 模块 API (Modules/digital_io/)

```
Modules/digital_io/
├── digital_io.h          # 公共接口 + 配置结构体
├── digital_io.c          # 核心: iso_input_read(), 状态机, 轮询
├── digital_io_exti.c     # EXTI ISR (IN1下降沿, IN3上升沿)
└── digital_io_config.c   # 12路输入配置表 (静态数组)
```

核心接口:
```c
int digital_io_init(void);                    // 注册心跳, 配EXTI, 读上电状态
bool iso_input_read(uint8_t ch);              // 读通道 ch 语义值 (已做NC/NO反转)
bool iso_is_estopped(void);                   // 快捷: 急停是否激活
bool iso_is_auto_mode(void);                  // 快捷: 是否自动模式
int  iso_input_poll(void);                    // 轮询 IN2 模式开关 (app_enterprise 500ms)
void iso_input_process_events(void);          // 处理 EXTI 事件队列, 推进安全状态机
system_state_t iso_get_system_state(void);    // 返回合并后的系统状态
int  iso_register_callback(uint8_t ch, iso_event_cb_t cb, void *data); // 注册事件回调
```

使用方式: `digital_io_init()` 在 app_enterprise 启动时调用; `iso_input_poll()` + `iso_input_process_events()` 在 1s 主循环中调用

### Ethernet 第三备份通道 (LAN8720)
- F407 百兆 Ethernet (RJ45, LAN8720A PHY) → 千兆交换机 → Orange Pi
- 角色: 第三数据通道, 大文件OTA固件传输 (>1MB)
- 协议: lwIP TCP Server, 端口 5000
- 优先级: UART4(WiFi) > UART5(RS232) > Ethernet(TCP)

#### UART4 系统状态上报 (CMD 0x07)

IO事件和系统状态变化即时上行 ESP32, 事件驱动无固定周期:

```
CMD 0x07 — RESP_SYSTEM_STATUS, payload 8字节:
[0]    system_state    uint8_t   0=NORMAL, 1=WARNING, 2=CRITICAL, 3=EMERGENCY, 4=WAIT_RESET
[1]    operation_mode  uint8_t   0=MANUAL, 1=AUTO
[2]    e_stop_state    uint8_t   0=NORMAL, 1=EMERGENCY, 2=WAIT_RESET
[3]    health_level    uint8_t   0=NORMAL, 1=WARNING, 2=CRITICAL
[4]    event_source    uint8_t   1=IN1急停, 2=IN2模式切换, 3=IN3复位, 4=AI结果变更
[5-7]  reserved        uint8_t[3]
```

发送时机: IN1(急停按下/复位), IN2(模式切换), IN3(复位确认), AI健康等级变化 — 均事件驱动立即发送

---

## UART协议扩展 (NDE数据通道)

在原有协议帧格式基础上，新增三个CMD用于NDE传感器节点数据上行：

### 新增CMD

| CMD | 名称 | 周期 | 数据载荷 | 说明 |
|-----|------|------|----------|------|
| 0x17 | RESP_NDE_FEATURE | 2s | 100字节(4头+96特征) | NDE 24维特征向量(24×float32) |
| 0x18 | RESP_NDE_HEARTBEAT | 1s | 4字节(状态+DSP统计) | NDE节点心跳，含在线状态和错误计数 |
| 0x19 | RESP_DUAL_DIAG | 预留 | TBD | 双通道对比诊断结果(F407侧备用) |
| 0x06 | RESP_MOTOR_DATA | 2s | 12字节 | 电机电流/电压/功率 (专用于Orange Pi电流电压分析) |

### CMD 0x06 电机数据帧 payload (12字节)
```
[0-3]   float32  母线电压(V), 从PD6010D PM1_VBUS (PB1/ADC1_IN9)
[4-7]   float32  电机电流(A), 从PD6010D PM1_AMPU (PB0/ADC1_IN8)
[8-11]  float32  电机功率(W), F407计算: 电压×电流
```

### 数据流

```
ADXL345[NDE] → F103: DSP → CAN(0x201/0x202) → F407: 多帧重组+CRC校验
                                                      ↓ UART4 0x17/0x18
                                                 ESP32-S3: 双通道诊断
```

F407只做纯字节搬运(带CRC校验)，不解码特征内容。

## DMA 设计决策

**数据流向与DMA外设优先级** (2026-05-21):

| 外设 | 方向 | DMA需求 | 优先级 | 状态 |
|------|------|---------|--------|------|
| UART4 RX | ESP32→F407 | DMA1_Stream2 + IDLE中断 + 协议解析 | P0 最高 | 待实现 |
| UART4 TX | F407→ESP32 | 流DMA, 替代阻塞HAL | P1 | 待实现 |
| ADC1 (3通道) | 电机采样→内存 | 扫描+DMA, CIRCULAR | P1 | CubeMX DMA已配, 需扩展通道 |
| SPI2 | Flash R/W | DMA可选 | P3 低 | 暂缓 |
| UART5 TX/RX | RS232备份 | 与UART4同架构 | P3 低 | 暂缓 |

**ADC1 DMA 现状与整改** (2026-05-21):
- CubeMX已配置: DMA2_Stream0, CIRCULAR, 半字对齐, 高优先级
- 硬件连接 (均在ADC1): PB0/IN8(U相电流), PB1/IN9(母线电压), PA0/IN0(NTC温度)
- 当前缺陷: ScanConvMode=DISABLE, NbrOfConversion=1 (仅CH8), 软件触发
- 需改为: ScanConvMode=ENABLE, NbrOfConversion=3, TIM3触发, ContinuousConv+DMA
- 采样率建议: 1kHz (TIM3周期), 三个通道交替 → 每通道333Hz有效采样率
- DMA流: DMA2_Stream0 (与UART4的DMA1_Stream2不同DMA控制器, 可并行)

**UART4 TX DMA 冲突与解决** (2026-05-21):
- 冲突: DMA1_Stream4 同时被 SPI2_TX(Channel0) 和 UART4_TX(Channel4) 争用
- 当前CubeMX: SPI2_TX 占用 DMA1_Stream4, UART4_TX 无法配置
- 决策: SPI2_TX 放弃DMA (Flash OTA仅几周一次, 阻塞发送几字节无影响)
- SPI2_RX DMA (DMA1_Stream3) 保留不动 — OTA读Flash大数据量需要
- 实现方式: **不动CubeMX**, 纯代码修改
  - `usart.c` USER CODE 0: 声明 `DMA_HandleTypeDef hdma_uart4_tx`
  - `usart.c` USER CODE 1: 实现 `uart4_dma_tx_init()` (配置DMA1_Stream4,Channel4,Memory-to-Peripheral)
  - `main.c` USER CODE Init 末尾: 所有 MX_*_Init 之后调用 `uart4_dma_tx_init()`
  - 顺序依赖: SPI2_MspInit在UART4_Init之后执行, 所以UART4 TX DMA必须在所有init之后重新夺取DMA1_Stream4

**UART4 RX DMA 方案** (2026-05-21):
- 方案: `HAL_UARTEx_ReceiveToIdle_DMA` + 10状态帧解析器, 扩展现有 `Modules/protocol/`
- 模块结构: `protocol.c` (帧构建+解析+CMD分发) + `protocol_dma.c` (DMA接收+IDLE回调)
- DMA缓冲区: 512字节 (NORMAL模式, IDLE后重新挂载)
- 解析器: 独立 FreeRTOS proto_rx 任务, IDLE回调发信号量 → 任务中解析
- CMD分发: `proto_register_callback(cmd, cb, user_data)` 注册制, 与ESP32一致
- 参考: ESP32侧 protocol.c 10状态解析器 (STATE_WAIT_HEADER_H → ... → STATE_WAIT_TAIL)

**决策理由**: ESP32侧同一模块同时负责收发+解析+分发, F407侧保持一致架构
- ADC1后推: 电机电流/电压采样暂用模拟数据, 不阻塞端到端数据流

**协议解析器缺口** (2026-05-21):
- F407 `Modules/protocol/protocol.c` 仅有 `proto_parse_header()` (2字节帧头检测)
- 缺少完整帧解析(10状态: LEN→DEV→CMD→SEQ→DATA→CRC→TAIL)和CMD回调分发
- 需补齐的ESP32→F407下行CMD处理: 0x10(AI结果), 0x11(控制), 0x12(配置), 0x14(时间同步), 0x15(电机控制), 0x16(电机查询)

**决策理由**:
- UART4 RX+DMA+解析器整合: IDLE回调是解析器天然入口, 避免先做中断逐字节再扔掉
- UART4 TX第二: 大帧(NDE特征100字节有效载荷)不再阻塞任务1秒周期
- ADC1后推: 电机电流/电压采样暂用模拟数据, 不阻塞端到端数据流

## 看门狗设计决策

**架构: 注册制心跳守护任务 (IWDG only)** (2026-05-21):

```
IWDG (3s, LSI 40kHz, 独立时钟)  ← wdg_daemon_task (osPriorityHigh, 1024B, 500ms周期)
                                       │
                                       ├─ 遍历心跳注册表 (最多16槽位)
                                       ├─ 调用每个 check() 回调
                                       ├─ 超时 → reset_on_fail? 停止喂狗 : 仅告警
                                       └─ 所有健康 → wdg_feed_iwdg()
```

**注册接口**:
```c
int wdg_heartbeat_register(const char *name, uint32_t timeout_ms,
                           bool (*check)(void), bool reset_on_fail);
```
- `name`: 模块名称 (日志用)
- `timeout_ms`: 心跳超时阈值
- `check()`: 健康检查回调 (由各模块实现, ISR安全)
- `reset_on_fail`: true=超时触发系统复位, false=仅日志告警

**当前注册表** (2026-05-21):

| 模块 | 超时 | 复位? | 健康检查方式 |
|------|------|-------|------------|
| `app_enterprise` | 3s | 是 | 循环计数器递增检测 |
| `lvgl_gui` | 1s | 是 | 帧渲染时间戳 |
| `can_nde` | 10s | 否 | 最后CAN收帧时间 |
| `uart4_tx` | 5s | 是 | 最后成功发送时间 |
| `uart4_rx` | 10s | 否 | 最后收帧时间 (未来) |

**未来扩展** (无需改守护任务代码):
- `rs485_modbus` — Modbus RTU 从站心跳
- `rs232_backup` — Orange Pi 备份通道心跳
- `iso_input` — 隔离输入状态变化检测 (急停/模式切换)
- `iso_output` — 隔离输出健康状态 (继电器/LED)

**WWDG 放弃原因** (2026-05-21):
- STM32F407 WWDG 预分频器最大 8 档, 超时上限 ~49ms (PCLK1=42MHz)
- FreeRTOS 任务周期 1s/5ms, 49ms 窗口无法配合秒级任务
- IWDG (LSI 独立时钟, 3s 可调) + 守护任务软件心跳检测替代 WWDG 功能

**设计要点**:
- 守护任务优先级 osPriorityHigh (高于 lvgl_gui 的 AboveNormal 和 app_enterprise 的 Normal)
- 栈大小: 1024B (简单轮询+检查, 无复杂计算)
- 喂狗周期: 500ms (为 IWDG 3s 留 6 倍余量)
- 注册表用静态数组, 无动态内存分配
- 超时后: CAN/通信丢失仅告警; 任务级超时主动调用 NVIC_SystemReset()

**已实现 (2026-05-21)**:
- `Modules/wdg/wdg_heartbeat.h` — 注册接口 `wdg_heartbeat_register()` + `wdg_heartbeat_update()`
- `Modules/wdg/wdg_heartbeat.c` — wdg_daemon_task (osPriorityHigh, 4KB栈, 500ms周期)
- main.c: MX_IWDG_Init() 已启用, WWDG 已废弃
- freertos.c: wdg_daemon 任务由 MX_FREERTOS_Init() 自动创建
- 已注册心跳: app_enterprise(3s/reset), uart4_tx(5s/reset), can_nde(10s/no-reset)

#### 启动初始化顺序 (安全关键, 2026-05-22)

```
main(): MX_GPIO_Init() → MX_TIM1_Init() → ... → osKernelStart()
    └── app_enterprise_task:
          digital_io_init()           # 1. 读IN1物理状态 → 决定上电安全状态
          alarm_service_init()        # 2. LED/蜂鸣器初始化, 显示初始状态
          proto_parse_init()          # 3. (已有) UART4协议解析器
          can_nde_init()              # 4. (已有) CAN NDE接收
          while (1):
              iso_input_poll()            # IN2 模式开关轮询 (极快返回)
              iso_input_process_events()  # EXTI事件处理 → 推进安全状态机
              alarm_service_refresh()     # 查状态矩阵 → 刷新LED/蜂鸣器
              main_loop_enterprise()      # 原有逻辑 (simulator + TX + motor)
              osDelay(1000)
```

上电安全: `digital_io_init()` 先读 IN1(NC), 急停已按下→EMERGENCY, TIM1 MOE 不打开。
EXTI 配置在 `digital_io_init()` 内部 (USER CODE), CubeMX 重新生成不会覆盖。

## 实现路线图 (2026-05-21)

### Phase 1: 基础设施 (优先)

| 序号 | 任务 | 状态 | 依赖 | 说明 |
|------|------|------|------|------|
| 1.1 | IWDG 启用 | ✅ 完成 | 无 | main.c: MX_IWDG_Init + HAL_IWDG_Start |
| 1.2 | wdg_daemon 守护任务 | ✅ 完成 | 1.1 | `wdg_heartbeat_register()` + 注册表 + 喂狗循环 |
| 1.3 | Modules/protocol/ 10状态解析器 | ✅ 完成 | 无 | protocol_parser.c: 10状态机 + CMD分发, 6个下行回调已注册 |
| 1.4 | protocol_dma.c | ✅ 完成 | 1.3 | DMA1_S2 RX, IDLE→信号量, 512B缓冲 |
| 1.5 | proto_rx FreeRTOS任务 | ✅ 完成 | 1.3, 1.4 | osPriorityAboveNormal, 2KB栈, 已注册心跳 |
| 1.6 | UART4 TX DMA | ✅ 完成 | 无 | DMA1_S4 reclaimed from SPI2_TX, 非阻塞发送 |

### Phase 2: 数据链路完善

| 序号 | 任务 | 依赖 | 说明 |
|------|------|------|------|
| 2.1 | ADC1 3通道扫描+DMA | ✅ 完成 | 无 | TIM2触发(1kHz), ScanConvMode=ENABLE, 3通道DMA, ADC回调+getter |
| 2.2 | 电机状态真实数据 (CMD 0x06) | ⬜ | 2.1 | 电流/电压/功率 → UART4 → ESP32 |
| 2.3 | CAN NDE 接入心跳注册 | ✅ 完成 | 1.2 | can_nde.c: slot 注册, 收帧时更新, 10s超时无复位 |

### Phase 3: 扩展通信

| 序号 | 任务 | 说明 |
|------|------|------|
| 3.1 | RS485 Modbus RTU 从站 | RS485端子 → Orange Pi |
| 3.2 | RS232 备份通道 | UART5, ESP32离线时切换 |

### Phase 4: 扩展 IO

| 序号 | 任务 | 状态 | 说明 |
|------|------|------|------|
| 4.1 | 隔离输入 (12路) — digital_io 模块 | ✅ 完成 | IN1急停(EXTI), IN2模式(轮询), IN3复位(EXTI), IN4-12保留 |
| 4.2 | 隔离输出 (4路) — alarm_service 模块 | ✅ 完成 | OUT1绿(正常), OUT2黄(预警), OUT3红(故障), OUT4保留, PF0蜂鸣器, PE0/PE1板载LED |
| 4.3 | 系统状态机整合 | ✅ 完成 | 安全状态×健康等级×运行模式 三维合并, CMD 0x07上行 |
| 4.4 | 启动安全序列 | ✅ 完成 | digital_io_init() 先读急停→上电安全状态→PWM开关 |

### Phase 5: 文档与架构对齐

| 序号 | 任务 | 状态 | 说明 |
|------|------|------|------|
| 5.1 | SystemArchitectureDesign.md 重构 | ✅ 完成 | 对齐实际代码: 数据流/硬件/协议/模块名 |
| 5.2 | IndustrialPredictiveMaintenanceSystem.md 重构 | ✅ 完成 | 对齐实际架构: F103 NDE + F407 DMF407 + ESP32-S3 + Orange Pi |

## 参考文档

- STM32F407xx Datasheet (DS8626 Rev 10)
- DMF407硬件参考手册 V1.0 (正点原子)
- ATK-PD6010D直流有刷驱动板用户手册 V1.00 (正点原子)
