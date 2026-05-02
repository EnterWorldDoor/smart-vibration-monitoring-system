# EdgeVib ESP32-S3 Gateway - 企业级AI代理协作规范

> **版本**: 2.0.0  
> **最后更新**: 2026-04-21  
> **ESP-IDF版本**: v5.5.3  
> **目标硬件**: ESP32-S3-DevKitC-1  
> **项目类型**: 工业振动监测系统网关

---

## 📋 目录

1. [项目概述](#1-项目概述)
2. [系统架构](#2-系统架构)
3. [技术栈](#3-技术栈)
4. [模块详细说明](#4-模块详细说明)
5. [数据流管道](#5-数据流管道)
6. [硬件配置](#6-硬件配置)
7. [编译与部署](#7-编译与部署)
8. [编码规范](#8-编码规范)
9. [错误处理体系](#9-错误处理体系)
10. [调试与排错](#10-调试与排错)
11. [测试策略](#11-测试策略)
12. [已知问题与限制](#12-已知问题与限制)
13. [性能优化指南](#13-性能优化指南)

---

## 1. 项目概述

### 1.1 项目定位
EdgeVib ESP32-S3 Gateway 是一个**工业级边缘计算网关设备**，负责：
- **数据采集**: 从ADXL345加速度传感器和STM32温湿度传感器采集原始数据
- **实时处理**: DSP信号处理（FFT/RMS/峰值检测）
- **智能分析**: 故障诊断和异常检测（TinyML推理）
- **云端通信**: 通过MQTT协议将分析结果上传至PC/云平台
- **系统监控**: 实时监控系统健康状态（CPU/内存/任务栈）

### 1.2 核心特性
- ✅ **高可靠性**: 降级运行模式，单点故障不影响整体系统
- ✅ **实时性**: FreeRTOS多任务调度，400Hz采样率
- ✅ **可扩展**: 模块化组件设计，易于添加新功能
- ✅ **可观测**: 统一日志系统和性能监控
- ✅ **安全性**: OTA远程升级，错误恢复机制

### 1.3 应用场景
- 工业电机振动监测
- 预测性维护（Predictive Maintenance）
- 设备健康度评估
- 异常检测与告警

---

## 2. 系统架构

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    Business Layer (业务层)                    │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ esp32-      │  │ mqtt_upload  │  │ task_system_        │  │
│  │ gateway.c   │  │ (MQTT上传)   │  │ monitor (监控)       │  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  │
│         │                │                     │             │
├─────────┼────────────────┼─────────────────────┼─────────────┤
│         │     Service Layer (服务层)              │             │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌───────────▼──────────┐ │
│  │ sensor_     │  │ protocol    │  │ ai_service           │ │
│  │ service     │  │ (UART协议)   │  │ (故障诊断/TinyML)    │ │
│  │ (传感器服务) │  │             │  │                      │ │
│  └──────┬──────┘  └──────┬──────┘  └───────────┬──────────┘ │
│         │                │                     │             │
├─────────┼────────────────┼─────────────────────┼─────────────┤
│         │     Driver Layer (驱动层)               │             │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌───────────▼──────────┐ │
│  │ adxl345     │  │ uart_driver │  │ dsp                  │ │
│  │ (加速度计)  │  │ (UART1)     │  │ (FFT/DSP算法)        │ │
│  └─────────────┘  └─────────────┘  └─────────────────────┘ │
│                                                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ log_system  │  │ config_     │  │ time_sync            │  │
│  │ (日志系统)   │  │ manager     │  │ (SNTP时间同步)       │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 任务架构（FreeRTOS）

| 任务名 | 优先级 | 栈大小 | 核心号 | 职责 |
|--------|--------|--------|--------|------|
| `main_task` | 1 | 4096 | CPU0 | 系统初始化与协调 |
| `uart_rx_task` | 7 | 4096 | CPU0 | UART数据接收与解析 |
| `sensor_task` | 6 | 8192 | CPU0 | 传感器数据处理与分析 |
| `mqtt_upload` | 5 | 8192 | CPU0 | MQTT消息发布 |
| `mqtt_core` | 5 | 4096 | CPU0 | MQTT后台连接管理 |
| `monitor_task` | 2 | 4096 | CPU0 | 系统健康监控 |
| `proto_rx` (内部) | 5 | 6144 | - | 协议栈接收任务 |
| `proto_hb` (内部) | 6 | 6144 | - | 协议栈心跳任务 |

### 2.3 组件依赖关系图

```
esp32-gateway.c (主程序)
    ├── log_system (日志系统) ← 所有模块依赖
    ├── config_manager (配置管理)
    ├── time_sync (时间同步)
    ├── protocol (UART协议栈)
    │   └── common (ringbuf, hardware_config)
    ├── sensor_service (传感器服务)
    │   ├── adxl345 (ADXL345驱动)
    │   ├── dsp (DSP算法库)
    │   ├── temperature_compensation (温度补偿)
    │   └── common (ringbuf, hardware_config)
    ├── mqtt_app (MQTT通信)
    │   └── data_manager (数据管理)
    ├── ai_service (AI故障诊断)
    │   └── fault_diagnosis (故障诊断引擎)
    └── system_monitor (系统监控)
```

---

## 3. 技术栈

### 3.1 开发框架
- **ESP-IDF**: v5.5.3 (Espressif IoT Development Framework)
- **目标芯片**: ESP32-S3 (Xtensa® LX7 双核, 240MHz)
- **RTOS**: FreeRTOS v11.0.0 (LTS)
- **文件系统**: SPIFFS / LittleFS (可选)
- **网络协议栈**: LWiP (IPv4)

### 3.2 通信协议
- **MQTT**: Mosquitto Broker (v2.0), QoS 0/1
- **UART**: 自定义二进制协议 (115200bps, 8N1)
- **SPI**: ADXL345 (SPI2_HOST, 5MHz, Mode 3)

### 3.3 算法库
- **DSP**: FFT (Radix-2 Cooley-Tukey), RMS, Peak Detection
- **窗口函数**: Hann/Hamming/Blackman
- **温度补偿**: 多项式拟合
- **故障诊断**: 阈值检测 + 统计分析

### 3.4 开发工具链
- **编译器**: xtensa-esp32s3-elf-gcc (GCC 14.2.0)
- **构建系统**: CMake + Ninja
- **调试器**: OpenOCD / JTAG
- **串口监视器**: idf.py monitor (115200bps)

---

## 4. 模块详细说明

### 4.1 核心模块清单

#### 📁 **main/** - 主程序入口
**文件**: [esp32-gateway.c](main/esp32-gateway.c)  
**职责**: 
- 系统初始化序列管理
- FreeRTOS任务创建与调度
- 全局错误回调注册
- 硬件GPIO初始化

**关键接口**:
```c
void app_main(void);                          // 程序入口
static int component_init_log(void);          // 初始化日志系统
static int component_init_wifi(void);         // 初始化WiFi
static int component_init_protocol(void);     // 初始化协议栈
static int component_init_sensor(void);       // 初始化传感器服务
static int component_init_mqtt(void);         // 初始化MQTT
static void task_uart_rx(void *arg);          // UART接收任务
static void task_sensor_process(void *arg);   // 传感器处理任务
static void task_mqtt_upload(void *arg);      // MQTT上传任务
static void task_system_monitor(void *arg);   // 监控任务
```

#### 📁 **components/log_system/** - 日志系统
**文件**: [log_system.c](components/log_system/log_system.c), [log_system.h](components/log_system/log_system.h)  
**职责**:
- 统一日志接口 (LOG_INFO/LOG_ERROR/LOG_WARN/LOG_DEBUG)
- 多输出目标支持 (UART/SPIFFS/MQTT)
- 日志级别过滤与格式化
- 异步日志写入 (避免阻塞)

**关键接口**:
```c
int log_system_init(struct log_config *config);
int log_system_deinit(void);
void LOG_INFO(const char *tag, const char *fmt, ...);
void LOG_ERROR(const char *tag, const char *fmt, ...);
void LOG_WARN(const char *tag, const char *fmt, ...);
void LOG_DEBUG(const char *tag, const char *fmt, ...);
```

**配置参数** (在 `esp32-gateway.c` 中设置):
```c
struct log_config {
    enum log_level level;      // 日志等级 (LOG_LEVEL_INFO)
    uint8_t output_targets;    // 输出目标 (UART=0x01)
    bool async_mode;           // 异步模式 (true)
    uint16_t ringbuf_size;     // 环形缓冲区大小 (4096字节)
};
```

#### 📁 **components/common/** - 公共组件
**文件**: 
- [hardware_config.h](components/common/hardware_config.h) - 硬件引脚定义
- [global_error.h](components/common/global_error.h) - 统一错误码
- [global_log.h](components/common/global_log.h) - 全局日志定义
- [ringbuf.c/h](components/common/ringbuf.c) - 环形缓冲区实现

**职责**:
- 集中管理所有GPIO引脚映射
- 定义项目统一错误码体系
- 提供线程安全的环形缓冲区

**硬件引脚定义** ([hardware_config.h](components/common/hardware_config.h)):
```c
// ADXL345 SPI接口
#define ADXL345_SPI_SCLK_PIN      5    // SPI时钟
#define ADXL345_SPI_MOSI_PIN      4    // MOSI
#define ADXL345_SPI_MISO_PIN      11   // MISO
#define ADXL345_SPI_CS_PIN        10   // 片选
#define ADXL345_INT1_PIN          9    // 中断1

// UART1 (STM32通信)
#define STM32_UART_TX_PIN         17   // UART1_TX → STM32_RX
#define STM32_UART_RX_PIN         18   // UART1_RX ← STM32_TX
```

**统一错误码** ([global_error.h](components/common/global_error.h)):
```c
// 成功
#define APP_ERR_OK                 0

// 通用错误 (-1 to -99)
#define APP_ERR_INVALID_PARAM     (-1)   // 参数无效
#define APP_ERR_NO_MEM            (-2)   // 内存不足
#define APP_ERR_TIMEOUT           (-3)   // 操作超时
#define APP_ERR_NOT_FOUND         (-4)   // 未找到
#define APP_ERR_NOT_INIT          (-5)   // 未初始化

// 传感器错误 (-1000 to -1099)
#define APP_ERR_SENSOR_INIT_FAIL  (-1000) // 传感器初始化失败
#define APP_ERR_SENSOR_READ_FAIL  (-1001) // 读取失败
#define APP_ERR_SENSOR_NOT_INIT   (-1002) // 传感器未初始化

// 通信错误 (-1100 to -1199)
#define APP_ERR_PROTO_CRC_FAIL    (-1100) // CRC校验失败
#define APP_ERR_PROTO_ACK_TIMEOUT (-1101) // ACK超时
#define APP_ERR_UART_TX_FAIL      (-1102) // UART发送失败

// MQTT错误 (-1200 to -1299)
#define APP_ERR_MQTT_CONN_FAIL   (-1200) // 连接失败
#define APP_ERR_MQTT_PUB_FAIL    (-1201) // 发布失败
```

#### 📁 **components/adxl345/** - ADXL345驱动
**文件**: [adxl345.c](components/adxl345/adxl345.c), [adxl345.h](components/adxl345/adxl345.h)  
**职责**:
- ADXL345加速度传感器底层驱动
- SPI通信协议实现
- FIFO数据缓冲管理
- 自检与健康状态监控

**关键接口**:
```c
struct adxl345_dev *adxl345_init_spi(
    struct adxl345_spi_config *cfg,
    enum adxl345_range range,
    enum adxl345_rate rate,
    enum adxl345_fifo_mode fifo_mode,
    int gpio_int1,
    struct ringbuf *ext_ringbuf
);

int adxl345_start(struct adxl345_dev *dev, int priority, size_t stack_size);
int adxl345_stop(struct adxl345_dev *dev);
int adxl345_fetch_batch(struct adxl345_dev *dev, struct adxl345_batch_data *batch, int max_samples, int timeout_ms);
int adxl345_self_test(struct adxl345_dev *dev);
void adxl345_get_health(struct adxl345_dev *dev, struct adxl345_health_info *health);
void adxl345_deinit(struct adxl345_dev *dev);
```

**支持的配置**:
- **量程**: ±2g / ±4g / ±8g / ±16g
- **采样率**: 100Hz / 200Hz / 400Hz / 800Hz / 1600Hz / 3200Hz
- **FIFO模式**: Bypass / Stream / Trigger

#### 📁 **components/sensor_service/** - 传感器服务
**文件**: [sensor_service.c](components/sensor_service/sensor_service.c), [sensor_service.h](components/sensor_service/sensor_service.h)  
**职责**:
- 传感器数据采集调度
- DSP信号处理流水线
- 温度补偿
- 分析结果分发
- 降级模式管理

**关键接口**:
```c
int sensor_service_init(struct sensor_config *config);
int sensor_service_start(void);
int sensor_service_stop(void);
int sensor_service_deinit(void);
bool sensor_service_is_running(void);
int sensor_service_get_latest_result(struct analysis_result *result);
void sensor_service_register_result_callback(sensor_result_cb_t callback, void *user_data);
void sensor_service_register_error_callback(sensor_error_cb_t callback, void *user_data);
```

**配置结构体**:
```c
struct sensor_config {
    uint32_t sample_rate_hz;              // 采样频率 (默认400Hz)
    uint16_t fft_size;                    // FFT点数 (默认512)
    enum window_type window_type;         // 窗函数 (Hann)
    bool enable_temp_compensation;        // 温度补偿开关
    bool enable_protocol_temp;            // 是否从STM32获取温度
    uint32_t analysis_interval_ms;        // 分析间隔 (默认1000ms)
};
```

**降级模式**:
当ADXL345不可用时，系统自动进入降级模式：
- ❌ 振动数据不可用
- ✅ 温湿度数据正常（来自STM32）
- ✅ MQTT正常发布环境数据
- ✅ 系统稳定运行不崩溃

#### 📁 **components/protocol/** - UART协议栈
**文件**: [protocol.c](components/protocol/protocol.c), [protocol.h](components/protocol/protocol.h)  
**职责**:
- 自定义二进制协议实现
- 帧封装/解包 (SOF + LEN + CMD + DATA + CRC + EOF)
- CRC16校验
- 心跳机制 (1秒间隔)
- 重传机制 (最多3次)

**帧格式**:
```
+------+------+------+----------+------+------+
| SOF  | LEN  | CMD  | DATA     | CRC  | EOF  |
| 0xAA | 1B   | 1B   | 0-255B   | 2B   | 0x55 |
+------+------+------+----------+------+------+
```

**命令集**:
| CMD值 | 名称 | 方向 | 说明 |
|-------|------|------|------|
| 0x01 | PING | 双向 | 心跳检测 |
| 0x13 | REQ_TEMP | ESP32→STM32 | 请求温湿度数据 |
| 0x14 | RESP_TEMP | STM32→ESP32 | 温湿度响应 |
| 0x20 | REQ_VIB_DATA | ESP32→STM32 | 请求振动数据(预留) |

**关键接口**:
```c
int protocol_init(uart_port_t uart_num, int baud_rate, uint8_t dev_id);
int protocol_start(void);
int protocol_stop(void);
int protocol_request_temp_data(void);
bool protocol_is_peer_alive(void);
int protocol_get_stats(struct proto_stats *stats);
void protocol_register_temp_callback(protocol_temp_cb_t callback, void *user_data);
void protocol_register_error_callback(proto_error_cb_t callback, void *user_data);
```

#### 📁 **components/mqtt_app/** - MQTT通信
**文件**: [mqtt.c](components/mqtt_app/mqtt.c), [mqtt.h](components/mqtt_app/mqtt.h)  
**职责**:
- MQTT客户端连接管理
- 消息发布/订阅
- 自动重连机制
- QoS保证

**主题规划**:
```
EdgeVib/{device_id}/data/sensor      ← 传感器数据 (JSON)
EdgeVib/{device_id}/data/environment ← 环境数据 (JSON)
EdgeVib/{device_id}/status/health    ← 系统健康状态
EdgeVib/{device_id}/cmd/+            ← 命令下发 (预留)
```

**数据格式示例** (传感器数据):
```json
{
    "timestamp_ms": 1713715200000,
    "dev_id": 1,
    "data": {
        "vibration": {
            "rms_x": 0.123,
            "rms_y": 0.456,
            "rms_z": 0.789,
            "overall_rms": 0.923,
            "peak_freq": 120.5,
            "peak_amp": 2.34
        },
        "environment": {
            "temperature_c": 25.6,
            "humidity_rh": 65.2
        }
    }
}
```

#### 📁 **components/dsp/** - DSP算法库
**文件**: [dsp.c](components/dsp/dsp.c), [dsp.h](components/dsp/dsp.h)  
**职责**:
- FFT快速傅里叶变换
- RMS有效值计算
- 峰值频率/幅值检测
- 窗函数应用

**关键接口**:
```c
int dsp_init(struct dsp_config *config);
int dsp_deinit(void);
int dsp_compute_fft(float *input, float *output_magnitude, float *output_phase, int n);
float dsp_compute_rms(float *data, int n);
int dsp_find_peak_frequency(float *fft_magnitude, int n, float sample_rate, float *freq_out, float *amp_out);
void dsp_apply_window(float *data, int n, enum window_type type);
```

#### 📁 **components/system_monitor/** - 系统监控
**文件**: [system_monitor.c](components/system_monitor/system_monitor.c), [system_monitor.h](components/system_monitor/system_monitor.h)  
**职责**:
- CPU/内存使用率监控
- FreeRTOS任务状态跟踪
- 栈溢出预警
- 性能统计报告

**关键接口**:
```c
int system_monitor_init(struct monitor_config *config);
int system_monitor_start(void);
int system_monitor_stop(void);
int system_monitor_get_report(struct monitor_report *report);
```

**监控报告结构体**:
```c
struct monitor_report {
    uint32_t uptime_seconds;              // 运行时间
    struct heap_info heap;                // 堆内存信息
    struct task_info tasks[MONITOR_MAX_TASKS]; // 任务列表
    uint8_t task_info_count;
    uint32_t total_errors;                // 总错误数
    uint32_t uart_frames_received;        // UART帧计数
    uint32_t analysis_published;          // 发布的分析结果数
    struct mqtt_stats mqtt;               // MQTT统计
};
```

#### 📁 **components/time_sync/** - 时间同步
**文件**: [time_sync.c](components/time_sync/time_sync.c), [time_sync.h](components/time_sync/time_sync.h)  
**职责**:
- SNTP时间同步
- 时区管理
- 本地时间获取

**关键接口**:
```c
int time_sync_init(struct time_sync_config *config);
time_t time_sync_get_local_time(void);
struct tm *time_sync_get_local_tm(void);
int64_t time_sync_get_ms_since_boot(void);
```

#### 📁 **components/config_manager/** - 配置管理
**文件**: [config_manager.c](components/config_manager/config_manager.c), [config_manager.h](components/config_manager/config_manager.h)  
**职责**:
- NVS Flash持久化存储
- 配置参数读写
- 默认值管理

#### 📁 **components/temperature_compensation/** - 温度补偿
**文件**: [temperature_compensation.c](components/temperature_compensation/temperature_compensation.c), [temperature_compensation.h](components/temperature_compensation/temperature_compensation.h)  
**职责**:
- 温度对传感器读数的影响修正
- 多项式拟合系数管理

#### 📁 **components/ai_service/** - AI服务
**文件**: [ai_service.c](components/ai_service/ai_service.c), [ai_service.h](components/ai_service/ai_service.h)  
**职责**:
- TinyML模型推理
- 异常检测
- 故障分类

#### 📁 **components/fault_diagnosis/** - 故障诊断
**文件**: [fault_diagnosis.c](components/fault_diagnosis/fault_diagnosis.c), [fault_diagnosis.h](components/fault_diagnosis/fault_diagnosis.h)  
**职责**:
- 振动特征提取
- 故障模式识别
- 阈值告警

---

## 5. 数据流管道

### 5.1 完整数据流

```
[物理层]                    [驱动层]                   [服务层]                   [业务层]
                                                                                
ADXL345 ──SPI──→ adxl345_fetch_batch() ──→ sensor_service ──→ task_sensor_process()
    ↓                         ↓                        ↓                         ↓
 原始加速度              RingBuf缓冲              DSP处理                    封装JSON
 (x,y,z)               (FIFO 32样本)          (FFT/RMS/Peak)              (analysis_result)
                                                                                
STM32 ──UART──→ protocol_parse_frame() ───→ on_protocol_temperature() ──→ 合并到JSON
    ↓                         ↓                        ↓                        
 温湿度数据            帧解包/CRC校验           回调通知                         
                                                                                
                                              ↓                         ↓
                                        task_mqtt_upload() ──→ MQTT Publish
                                              ↓                         ↓
                                        q_analysis队列           EdgeVib/1/data/sensor
```

### 5.2 数据处理时序

```
时间轴:  |---- 1秒 ----|---- 1秒 ----|---- 1秒 ----|

ADXL345: [████████████████████████████████] 400Hz采样
           ↓ 每100ms取一批
Sensor:   [分析][分析][分析][分析][分析][分析][分析][分析][分析][分析]
                                    ↓ 结果入队
MQTT:                              [发布]    [发布]    [发布]
```

---

## 6. 硬件配置

### 6.1 引脚分配总表

| 外设 | GPIO | 功能 | 模式 | 备注 |
|------|------|------|------|------|
| **ADXL345 SPI** | | | | |
| SCLK | GPIO5 | SPI时钟 | 输出 | 5MHz, Mode 3 |
| MOSI | GPIO4 | 数据输出 | 输出 | 主→从 |
| MISO | GPIO11 | 数据输入 | 输入 | 从→主 |
| CS | GPIO10 | 片选 | 输出 | 低电平有效 |
| INT1 | GPIO9 | 中断1 | 输入 | 可选(FIFO满) |
| **UART1 (STM32)** | | | | |
| TX | GPIO18 | 发送 | 输出 | → STM32 RX |
| RX | GPIO17 | 接收 | 输入 | ← STM32 TX |
| **LED指示灯** | | | | |
| STATUS | GPIO2 | 状态灯 | 输出 | 内置LED |
| ERROR | GPIO4 | 错误灯 | 输出 | 可选 |
| **电源** | | | | |
| 3.3V | VCC | 电源 | - | ADXL345必须3.3V! |
| GND | GND | 地 | - | 必须共地! |

### 6.2 接线图示

```
┌──────────────────┐          ┌──────────────────────┐
│   ADXL345 模块    │          │    ESP32-S3 DevKit   │
├──────────────────┤          ├──────────────────────┤
│                  │          │                      │
│  VCC ──────┬─────┼──────────┼──► 3.3V              │
│            │     │          │                      │
│  GND ──────┴─────┼──────────┼──► GND               │
│                  │          │                      │
│  SCL ────────────┼──────────┼──► GPIO5 (SCLK)      │
│                  │          │                      │
│  SDA ────────────┼──────────┼──► GPIO4 (MOSI)      │
│                  │          │                      │
│  SDO ────────────┼──────────┼──► GPIO11 (MISO)     │
│                  │          │                      │
│  CS ─────────────┼──────────┼──► GPIO10 (CS)       │
│                  │          │                      │
│  INT1 ───────────┼──────────┼──► GPIO9 (INT1)      │
│                  │          │                      │
└──────────────────┘          └──────────────────────┘

┌──────────────────┐          ┌──────────────────────┐
│   STM32 开发板    │          │    ESP32-S3 DevKit   │
├──────────────────┤          ├──────────────────────┤
│                  │          │                      │
│  TX ─────────────┼──────────┼──► GPIO17 (RX)       │
│                  │          │                      │
│  RX ─────────────┼──────────┼──► GPIO18 (TX)       │
│                  │          │                      │
│  GND ────────────┼──────────┼──► GND (必须共地!)    │
│                  │          │                      │
└──────────────────┘          └──────────────────────┘
```

### 6.3 重要注意事项

⚠️ **ADXL345供电**
- **必须使用3.3V!** 不支持5V，否则会损坏芯片
- 建议在VCC和GND之间并联100nF去耦电容

⚠️ **SPI信号质量**
- 如果连线超过10cm，建议串联100Ω电阻抑制反射
- SCLK/MOSI/MISO线上不要有分支

⚠️ **UART共地**
- STM32的GND必须与ESP32的GND连接！
- 否则会导致通信失败或数据乱码

⚠️ **GPIO冲突检查**
- 不要使用Flash/PSRAM占用的引脚（通常为GPIO26-32）
- 不要使用Strapping引脚（GPIO0, 3, 45, 46）

---

## 7. 编译与部署

### 7.1 开发环境要求

**必需软件**:
- ESP-IDF v5.5.3 (已安装在 `C:/esp/.espressif/v5.5.3/esp-idf`)
- Python 3.8+
- Git
- CMake 3.15+
- Ninja Build System

**推荐工具**:
- VS Code + ESP-IDF Extension
- PlatformIO IDE
- Serial Port Monitor (如 PuTTY, Tera Term)

### 7.2 编译步骤

```bash
# 1. 进入项目目录
cd d:\smartSystem\firmware\esp32-gateway

# 2. 设置ESP-IDF环境变量 (ESP-IDF Terminal自动完成)
# export IDF_PATH="C:/esp/.espressif/v5.5.3/esp-idf"

# 3. 配置项目 (首次或修改sdkconfig后)
idf.py menuconfig

# 4. 编译项目
idf.py build

# 5. 烧录到ESP32-S3 (替换COM3为你的串口号)
idf.py -p COM3 flash

# 6. 监看串口输出
idf.py -p COM3 monitor

# 7. 一键烧录+监视 (推荐)
idf.py -p COM3 flash monitor
```

### 7.3 关键sdkconfig配置

编辑 [sdkconfig.defaults](sdkconfig.defaults) 或通过 `menuconfig`:

```ini
# ============ Flash配置 ============
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# ============ WiFi配置 ============
CONFIG_ESP32_WIFI_STA_AUTO_CONNECT=y
CONFIG_LWIP_IPV4_NAPT=y

# ============ FreeRTOS配置 ============
CONFIG_FREERTOS_HZ=100

# ============ 日志配置 ============
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y

# ============ MQTT配置 ============
CONFIG_MQTT_CUSTOM_BUFFER_SIZE=4096

# ============ 系统监控配置 ============
CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y  # 启用vTaskGetInfo()

# ============== 关键修复项 ==============
# 修复栈溢出问题
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=8192
CONFIG_ESP_TIMER_TASK_STACK_SIZE=6144
```

### 7.4 分区表配置

[partitions.csv](partitions.csv):
```csv
# Name,   Type, SubType, Offset,   Size, Flags
nvs,      data, nvs,     0x9000,   24K,
phy_init, data, phy,     0xf000,   4K,
factory,  app,  factory,  0x10000,  1M,
```

### 7.5 清理与重新编译

```bash
# 完全清理 (删除所有编译产物)
idf.py fullclean

# 重新编译
idf.py build
```

**何时需要fullclean**:
- 修改了 sdkconfig.defaults
- 更换了ESP-IDF版本
- 出现奇怪的链接错误
- 修改了 CMakeLists.txt 的依赖关系

---

## 8. 编码规范

### 8.1 命名约定

**文件命名**:
- 源文件: `module_name.c` (小写+下划线)
- 头文件: `module_name.h`
- 测试文件: `module_name_test.c`

**函数命名**:
- 模块公开接口: `module_action()` (如 `sensor_service_init()`)
- 模块内部函数: `static void module_internal_func()`
- 回调函数: `on_event_handler()`, `cb_callback_name()`

**变量命名**:
- 全局变量: `g_module_variable` (前缀g_)
- 静态变量: `s_local_var` (前缀s_) 或 `static`
- 局部变量: `camelCase` 或 `snake_case` (保持一致)
- 常量/宏: `UPPER_SNAKE_CASE`

**结构体/枚举**:
- 类型定义: `struct module_name_struct`, `enum module_enum`
- typedef: `typedef struct {...} module_name_t;`
- 成员变量: `snake_case`

### 8.2 错误处理规范

**返回值约定**:
```c
// ✅ 正确: 所有函数返回int错误码
int my_function(int param) {
    if (param < 0) {
        LOG_ERROR("MODULE", "Invalid parameter: %d", param);
        return APP_ERR_INVALID_PARAM;
    }
    
    int ret = dependent_function(param);
    if (ret != APP_ERR_OK) {
        LOG_WARN("MODULE", "Dependent function failed: %d", ret);
        return ret;  // 传播错误!
    }
    
    return APP_ERR_OK;
}

// ❌ 错误: 忽略返回值
void bad_function(void) {
    risky_operation();  // 返回值被忽略!
}

// ❌ 错误: 直接return不带错误码
int another_bad(void) {
    if (error_condition) {
        return;  // 应该 return APP_ERR_XXX;
    }
}
```

**NULL指针检查**:
```c
// ✅ 正确: 使用前检查NULL
if (ptr != NULL) {
    ptr->method();
} else {
    LOG_ERROR("MODULE", "Pointer is NULL!");
    return APP_ERR_NULL_POINTER;
}

// ✅ 正确: 断言 (仅Debug模式)
assert(ptr != NULL);
ptr->method();
```

### 8.3 内存管理规范

**静态 vs 动态分配**:
```c
// ✅ 推荐: 静态分配 (确定大小的数据)
static uint8_t buffer[BUFFER_SIZE];

// ⚠️ 允许: 动态分配 (需要良好理由并注释释放位置)
void *ptr = malloc(size);
if (!ptr) {
    LOG_ERROR("MODULE", "Memory allocation failed");
    return APP_ERR_NO_MEM;
}
// ... 使用ptr ...
free(ptr);  // 必须释放! 注释: 在XXX行释放
ptr = NULL;  // 防止悬空指针
```

**FreeRTOS内存**:
```c
// ✅ 正确: 使用heap_caps_malloc分配特定内存
void *psram_ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
if (!psram_ptr) {
    LOG_WARN("MODULE", "PSRAM alloc failed, trying internal RAM");
    ptr = malloc(size);
}
```

### 8.4 日志使用规范

**日志级别选择**:
```c
LOG_ERROR("TAG", "Fatal error, system cannot continue");  // 严重错误
LOG_WARN("TAG", "Recoverable error, degraded mode");      // 可恢复警告
LOG_INFO("TAG", "State change or important event");        // 状态变化
LOG_DEBUG("TAG", "Detailed debugging info");               // 调试信息
```

**日志格式**:
```c
// ✅ 正确: 包含上下文信息
LOG_INFO("SENSOR", "ADXL345 initialized (rate=%dHz, range=%dg)", rate, range);
LOG_ERROR("PROTO", "CRC mismatch (expected=0x%04X, got=0x%04X)", expected_crc, actual_crc);

// ❌ 错误: 无意义的日志
LOG_DEBUG("SENSOR", "Here");  // 不要这样!
```

**性能敏感区域**:
```c
// 在高频循环中避免频繁日志
for (int i = 0; i < 10000; i++) {
    process_data(data[i]);
    // ❌ 错误: 每次循环都打印
    // LOG_DEBUG("LOOP", "Processed %d", i);
    
    // ✅ 正确: 每1000次打印一次
    if (i % 1000 == 0) {
        LOG_DEBUG("LOOP", "Progress: %d/10000", i);
    }
}
```

### 8.5 多线程编程规范

**互斥量使用**:
```c
static SemaphoreHandle_t s_mutex;

void init(void) {
    s_mutex = xSemaphoreCreateMutex();
}

void thread_safe_operation(void) {
    // ✅ 正确: 加锁保护共享资源
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        modify_shared_resource();
        xSemaphoreGive(s_mutex);
    } else {
        LOG_WARN("MODULE", "Failed to acquire mutex (timeout)");
    }
}
```

**任务创建规范**:
```c
// ✅ 正确: 检查返回值
BaseType_t ret = xTaskCreate(
    task_function,
    "task_name",
    TASK_STACK_SIZE,    // 推荐最小2048, 复杂任务4096+
    NULL,
    TASK_PRIORITY,
    &task_handle
);

if (ret != pdPASS) {
    LOG_ERROR("MAIN", "Failed to create task (ret=%ld)", ret);
    return APP_ERR_TASK_CREATE_FAIL;
}
```

**任务栈大小指南**:
| 任务类型 | 最小栈 | 推荐栈 | 说明 |
|---------|--------|--------|------|
| 简单任务 | 2048 | 4096 | 仅简单逻辑 |
| 中等任务 | 4096 | 6144 | 含字符串处理/JSON |
| 复杂任务 | 6144 | 8192 | 含FFT/DSP计算 |
| 监控任务 | 4096 | 4096 | 打印大量日志 |

### 8.6 注释规范

**文件头注释**:
```c
/**
 * @file filename.c
 * @brief 一句话描述模块功能
 * @author 作者名
 * @date 2026-04-21
 *
 * 详细描述 (可选):
 *   - 功能1
 *   - 功能2
 *
 * 依赖项:
 *   - FreeRTOS
 *   - ESP-IDF driver/gpio
 */
```

**函数注释**:
```c
/**
 * @brief 函数功能的简短描述
 *
 * 详细描述 (如果需要):
 *   解释算法或特殊处理逻辑
 *
 * @param param1 参数1说明
 * @param param2 参数2说明
 * @return
 *   - APP_ERR_OK 成功
 *   - APP_ERR_XXX 失败原因
 *
 * @note 注意事项 (如线程安全、限制等)
 * @code
 * // 使用示例
 * int ret = function_example(123);
 * @endcode
 */
int function_example(int param1, const char *param2);
```

**行内注释**:
```c
// ✅ 好: 解释"为什么"而不是"什么"
if (temperature > 80) {
    shutdown_system();  // 防止硬件损坏 (解释原因)
}

// ❌ 差: 重复代码本身
if (temperature > 80) {  // 如果温度大于80
    shutdown_system();    // 关闭系统
}
```

---

## 9. 错误处理体系

### 9.1 错误码层次

```
APP_ERR_OK (0)
  │
  ├── 通用错误 (-1 ~ -99)
  │   ├── APP_ERR_INVALID_PARAM (-1)
  │   ├── APP_ERR_NO_MEM (-2)
  │   ├── APP_ERR_TIMEOUT (-3)
  │   ├── APP_ERR_NOT_FOUND (-4)
  │   └── APP_ERR_NOT_INIT (-5)
  │
  ├── 传感器错误 (-1000 ~ -1099)
  │   ├── APP_ERR_SENSOR_INIT_FAIL (-1000)
  │   ├── APP_ERR_SENSOR_READ_FAIL (-1001)
  │   └── APP_ERR_SENSOR_NOT_INIT (-1002)
  │
  ├── 通信错误 (-1100 ~ -1199)
  │   ├── APP_ERR_PROTO_CRC_FAIL (-1100)
  │   ├── APP_ERR_PROTO_ACK_TIMEOUT (-1101)
  │   └── APP_ERR_UART_TX_FAIL (-1102)
  │
  ├── MQTT错误 (-1200 ~ -1299)
  │   ├── APP_ERR_MQTT_CONN_FAIL (-1200)
  │   └── APP_ERR_MQTT_PUB_FAIL (-1201)
  │
  └── 系统错误 (-1300 ~ -1399)
      ├── APP_ERR_SYS_STACK_OVERFLOW (-1300)
      └── APP_ERR_SYS_HEAP_EXHAUSTED (-1301)
```

### 9.2 错误传播机制

```c
// 层次化错误处理示例
int top_level_function(void) {
    int ret = mid_level_function();
    if (ret != APP_ERR_OK) {
        // 记录并传播错误
        LOG_ERROR("TOP", "Mid-level failed: %d", ret);
        notify_error_observer(ret);  // 通知上层
        return ret;  // 不吞掉错误!
    }
    return APP_ERR_OK;
}

int mid_level_function(void) {
    int ret = low_level_driver_operation();
    if (ret != APP_ERR_OK) {
        // 可以添加上下文信息
        LOG_ERROR("MID", "Driver operation failed: %s", 
                  error_code_to_string(ret));
        return ret;
    }
    return APP_ERR_OK;
}
```

### 9.3 降级策略

当非关键组件失败时，采用**优雅降级**而非崩溃：

```c
ret = sensor_service_init(&config);
if (ret != APP_ERR_OK) {
    LOG_WARN("MAIN", "Sensor service init failed, entering DEGRADED mode");
    g_degraded_mode = true;
    // 不return! 继续初始化其他组件
    // 后续会跳过振动相关操作
}
```

**降级能力矩阵**:

| 组件失效 | 系统能力 | 影响 |
|---------|---------|------|
| ADXL345失败 | ✅ 温湿度采集, MQTT发布 | 无振动数据 |
| WiFi断开 | ✅ 数据本地缓存 | 无法上传云端 |
| STM32离线 | ✅ 振动数据采集 | 无温湿度数据 |
| MQTT断开 | ✅ 数据采集处理 | 数据暂存本地 |
| Flash损坏 | ❌ 无法启动 | 需要手动恢复 |

---

## 10. 调试与排错

### 10.1 常见问题速查表

#### 问题1: Guru Meditation Error (LoadProhibited)
**症状**: 系统崩溃, 日志显示 `panic'ed (LoadProhibited)`  
**原因**: 空指针访问  
**排查步骤**:
1. 查看 PC 寄存器指向的代码位置
2. 检查是否有未做NULL判断就解引用指针
3. 特别注意: `g_ss.adxl`, `g_proto.mutex` 等全局变量

**修复方法**:
```c
// 在所有可能为NULL的指针使用前添加检查
if (ptr == NULL) {
    LOG_ERROR("TAG", "NULL pointer detected");
    return APP_ERR_NULL_PTR;
}
```

#### 问题2: Stack Overflow in task XXX
**症状**: 日志显示 `stack overflow in task xxx`  
**原因**: 任务栈空间不足  
**排查步骤**:
1. 使用 `uxTaskGetStackHighWaterMark()` 检查剩余栈空间
2. 查看任务中是否有大数组/递归调用
3. 检查printf/LOG是否使用了过多栈空间

**修复方法**:
```c
// 增大任务栈 (在xTaskCreate时)
xTaskCreate(task_func, "task_name", 
           8192,  // 从4096增大到8192
           NULL, priority, &handle);
```

或在 sdkconfig 中调整:
```ini
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=8192
CONFIG_ESP_TIMER_TASK_STACK_SIZE=6144
```

#### 问题3: SPI device add failed: 0x103/0x102
**症状**: ADXL345初始化失败, 显示 `invalid host` 或 `invalid arg`  
**原因**: SPI总线未正确初始化或引脚配置错误  
**排查步骤**:
1. 检查 `spi_bus_initialize()` 是否在 `spi_bus_add_device()` 之前调用
2. 验证GPIO引脚是否与其他外设冲突
3. 确认SPI主机ID (SPI2_HOST=1, SPI3_HOST=2) 是否被占用

**修复方法**:
```c
// 确保先初始化总线
spi_ret = spi_bus_initialize(host_id, &buscfg, DMA_CHAN);
if (spi_ret != ESP_OK) {
    LOG_ERROR("SENSOR", "SPI bus init failed: 0x%X", spi_ret);
    return APP_ERR_SPI_INIT_FAIL;
}

// 再添加设备
dev_ret = spi_bus_add_device(host_id, &devcfg, &spi_handle);
```

#### 问题4: UART driver error
**症状**: 大量 `uart_write_bytes(): uart driver error`  
**原因**: UART驱动未安装或未正确初始化  
**排查步骤**:
1. 检查 `uart_driver_install()` 是否被调用
2. 确认UART端口号和引脚配置正确
3. 验证波特率设置一致

**修复方法**:
```c
// 在使用UART前必须安装驱动
uart_config_t uart_cfg = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
};
uart_param_config(UART_NUM_1, &uart_cfg);
uart_driver_install(UART_NUM_1, BUF_SIZE * 2, BUF_SIZE * 2, 0, 0, 0);
```

#### 问题5: Peer appears offline (no heartbeat)
**症状**: STM32无法连接, 持续显示离线  
**原因**: 
- STM32未上电或未刷固件
- UART接线错误 (TX/RX交叉)
- 波特率不匹配
- 未共地

**排查步骤**:
1. 用万用表检查物理连接
2. 用USB-TTL模块单独测试STM32串口输出
3. 确认两边波特率都是115200
4. **最重要**: 检查GND是否连通!

### 10.2 调试技巧

**启用详细日志**:
```c
// 在sdkconfig中设置
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y

// 或在代码中临时提升某个模块的日志级别
esp_log_level_set("PROTO", ESP_LOG_DEBUG);
```

**使用断言 (Debug模式)**:
```c
#include <assert.h>

void critical_function(int value) {
    assert(value > 0 && "Value must be positive");
    // ...
}
```

**性能剖析**:
```c
int64_t start = esp_timer_get_time();

// ... 待测代码 ...

int64_t end = esp_timer_get_time();
LOG_INFO("PERF", "Operation took %lld us", (end - start));
```

**查看FreeRTOS任务状态**:
```c
// 在monitor_task中打印
vTaskList(char_buffer);  // 需要CONFIG_FREERTOS_USE_TRACE_FACILITY=1
LOG_INFO("TASKS", "\n%s", char_buffer);
```

### 10.3 日志关键词搜索

在串口日志中快速定位问题：

| 关键词 | 含义 | 可能原因 |
|--------|------|---------|
| `Guru Meditation` | 系统崩溃 | 空指针/栈溢出/WDT触发 |
| `LoadProhibited` | 访问非法地址 | NULL指针解引用 |
| `Stack overflow` | 栈空间不足 | 任务栈太小/递归过深 |
| `uart driver error` | UART错误 | 驱动未安装/配置错误 |
| `invalid host` | SPI主机无效 | SPI未初始化/ID错误 |
| `DEGRADED mode` | 降级模式 | 非关键组件失败 |
| `Peer appears offline` | 对端离线 | STM32未响应/接线问题 |
| `ACK timeout` | 应答超时 | 通信中断/协议错误 |

---

## 11. 测试策略

### 11.1 单元测试框架

**框架**: Unity (ESP-IDF内置)  
**测试文件命名**: `{module}_test.c`  
**测试文件位置**: 与源文件同目录

**示例**: [sensor_service_test.c](components/sensor_service/sensor_service_test.c)

```c
#include "unity.h"
#include "sensor_service.h"

void test_sensor_service_init(void) {
    struct sensor_config config = {
        .sample_rate_hz = 400,
        .fft_size = 512,
        .window_type = WINDOW_HANN,
        .enable_temp_compensation = false,
        .enable_protocol_temp = false,
        .analysis_interval_ms = 1000
    };
    
    int ret = sensor_service_init(&config);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(sensor_service_is_running());
    
    sensor_service_deinit();
}

void test_sensor_service_null_config(void) {
    int ret = sensor_service_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

int app_main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sensor_service_init);
    RUN_TEST(test_sensor_service_null_config);
    return UNITY_END();
}
```

### 11.2 运行单元测试

```bash
# 通过menuconfig启用Unity
idf.py menuconfig
# → Component config → Unity → Enable Unity

# 编译并运行测试
idf.py build
idf.py -p COM3 flash monitor
# 测试结果会在终端输出
```

### 11.3 集成测试场景

**场景1: 完整数据流测试**
1. 连接ADXL345和STM32
2. 启动系统,等待WiFi连接
3. 验证MQTT消息发布
4. 检查PC端Mosquitto是否收到数据

**场景2: 降级模式测试**
1. 断开ADXL345连接
2. 启动系统
3. 验证系统仍正常运行
4. 确认只有温湿度数据,无振动数据

**场景3: 网络中断恢复测试**
1. 系统正常运行
2. 断开WiFi
3. 等待30秒
4. 恢复WiFi
5. 验证自动重连和数据补发

### 11.4 性能测试指标

| 指标 | 目标值 | 测量方法 |
|------|--------|---------|
| 采样率稳定性 | 400±1Hz | 示波器测量SPI时钟 |
| FFT延迟 | <50ms | esp_timer_get_time() |
| MQTT发布延迟 | <100ms | 时间戳对比 |
| CPU使用率 | <60% | system_monitor报告 |
| 内存碎片率 | <20% | heap_caps_get_free_size() |
| 栈使用率 | <70% | uxTaskGetStackHighWaterMark() |

---

## 12. 已知问题与限制

### 12.1 当前已知问题

**问题1: ADXL345 SPI初始化不稳定**
- **现象**: 首次上电有时失败,需重启
- **原因**: SPI时序或上电顺序问题
- **临时解决方案**: 系统会自动进入降级模式,重启后通常恢复
- **长期方案**: 增加上电延时和重试机制

**问题2: WiFi热点连接不稳定**
- **现象**: 连接手机热点后偶尔断开重连
- **原因**: 手机热点省电模式导致AP休眠
- **解决方案**: 
  - 手机设置: 关闭热点省电
  - 代码层面: 已实现自动重连 (最多10次)

**问题3: 单元测试残留**
- **现象**: 编译时包含Unity测试代码
- **影响**: 增加固件体积,可能影响生产模式
- **解决方案**: 通过sdkconfig关闭 `CONFIG_UNITY_ENABLE`

### 12.2 系统限制

| 限制项 | 当前值 | 说明 |
|--------|--------|------|
| 最大采样率 | 3200Hz | 受ADXL345硬件限制 |
| FFT最大点数 | 2048 | 受内存限制 |
| 同时连接传感器 | 1个ADXL345 + 1个STM32 | UART/SPI资源有限 |
| MQTT消息大小 | 4KB | 受LwIP缓冲区限制 |
| 日志环形缓冲区 | 4096字节 | 可通过配置调整 |
| NVS存储空间 | 24KB | 存储配置参数 |

### 12.3 未来改进方向

- [ ] 支持多ADXL345传感器 (SPI多从机)
- [ ] 添加SD卡本地数据存储
- [ ] 实现TLS加密MQTT通信
- [ ] 集成TensorFlow Lite for Microcontrollers
- [ ] 支持蓝牙BLE配置
- [ ] 添加Web配网页面
- [ ] 实现OTA差分升级

---

## 13. 性能优化指南

### 13.1 内存优化

**减少静态内存占用**:
```c
// ❌ 差: 大数组在栈上
void processing_function(void) {
    float large_array[4096];  // 16KB栈空间!
    // ...
}

// ✅ 好: 使用静态或堆内存
void processing_function(void) {
    static float large_array[4096];  // 在BSS段
    // 或
    float *large_array = malloc(4096 * sizeof(float));
    // 使用后记得free!
}
```

**PSRAM使用** (如果板载PSRAM):
```c
// 将大缓冲区放入PSRAM
float *fft_input = heap_caps_malloc(4096 * sizeof(float), MALLOC_CAP_SPIRAM);
if (!fft_input) {
    LOG_WARN("DSP", "PSRAM unavailable, using internal RAM");
    fft_input = malloc(4096 * sizeof(float));
}
```

### 13.2 CPU优化

**使用DMA**:
```c
// SPI传输使用DMA (减少CPU占用)
spi_bus_config_t buscfg = {
    // ...
    .max_transfer_sz = 64,  // 启用DMA
};

spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);  // 自动选择DMA通道
```

**减少浮点运算**:
```c
// ❌ 差: 在ISR中进行复杂计算
void IRAM_ATTR isr_handler(void) {
    float result = complex_calculation();  // 太慢!
}

// ✅ 好: ISR只收集数据,处理放到任务中
volatile uint32_t isr_raw_data;
void IRAM_ATTR isr_handler(void) {
    isr_raw_data = read_register_fast();  // 快速读取
}
```

### 13.3 电源优化

**低功耗模式**:
```c
// 在空闲时进入Light Sleep
esp_sleep_enable_timer_wakeup(1000000);  // 1秒后唤醒
esp_light_sleep_start();

// 或使用Automatic Light Sleep (需要menuconfig开启)
CONFIG_PM_ENABLE=y
```

**动态调整CPU频率**:
```c
#include "esp_pm.h"

esp_pm_config_esp32_t pm_config = {
    .max_cpu_freq = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_cpu_freq = 80,  // 低频时降低功耗
};
esp_pm_configure(&pm_config);
```

---

## 附录A: 快速参考卡

### A.1 常用命令速查

```bash
# 编译
idf.py build

# 烧录
idf.py -p COM3 flash

# 监视
idf.py -p COM3 monitor

# 烧录+监视
idf.py -p COM3 flash monitor

# 清理
idf.py fullclean

# 配置菜单
idf.py menuconfig

# 查看分区表
idf.py partition-table

# 烧录bootloader
idf.py -p COM3 bootloader-flash

# 格式化NVS (清除配置)
idf.py -p COM3 erase-region 0x9000 0x6000
```

### A.2 重要文件路径索引

```
esp32-gateway/
├── main/
│   ├── esp32-gateway.c          # 主程序入口 ★★★
│   └── CMakeLists.txt
├── components/
│   ├── common/
│   │   ├── hardware_config.h    # 硬件引脚定义 ★★★
│   │   ├── global_error.h       # 错误码定义 ★★
│   │   └── ringbuf.c/h          # 环形缓冲区
│   ├── adxl345/                 # ADXL345驱动
│   ├── sensor_service/          # 传感器服务 ★★★
│   ├── protocol/                # UART协议栈 ★★
│   ├── mqtt_app/                # MQTT通信 ★★
│   ├── dsp/                     # DSP算法库
│   ├── log_system/              # 日志系统 ★★
│   ├── system_monitor/          # 系统监控
│   ├── time_sync/               # 时间同步
│   ├── config_manager/          # 配置管理
│   ├── temperature_compensation/# 温度补偿
│   ├── ai_service/              # AI服务
│   └── fault_diagnosis/         # 故障诊断
├── partitions.csv               # 分区表
├── sdkconfig.defaults           # 默认配置 ★★
├── sdkconfig                    # 当前配置 (自动生成)
└── AGENTS.md                    # 本文档 ★★★
```

### A.3 紧急联系与支持

**遇到无法解决的问题?**

1. **首先**: 查看本文档第10节"调试与排错"
2. **然后**: 搜索ESP-IDF官方文档 https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/
3. **最后**: 在GitHub Issues提交问题 (附上完整日志)

**常用调试命令**:
```bash
# 查看系统状态 (需要在monitor中输入)
info          # 显示FreeRTOS任务状态
heap info     # 显示堆内存使用
reset         # 软重启
```

---

## 附录B: 版本历史

| 版本 | 日期 | 作者 | 变更内容 |
|------|------|------|---------|
| 1.0.0 | 2026-04-20 | EnterWorldDoor | 初始版本,基础编码规范 |
| 2.0.0 | 2026-04-21 | AI-Assistant | 全面重构,添加架构设计、调试指南、性能优化 |

---

## 附录C: 许可证

本项目采用 MIT 许可证。详见 LICENSE 文件。

---

> **文档维护者**: AI Assistant (基于Trae IDE)  
> **最后审核**: 2026-04-21  
> **适用范围**: EdgeVib ESP32-S3 Gateway 项目所有开发者与AI代理

**🎯 使用本文档的目标**:
- ✅ 让新开发者快速理解项目架构
- ✅ 让AI代理能够准确编写符合规范的代码
- ✅ 减少沟通成本和维护难度
- ✅ 提高代码质量和系统可靠性

**📚 相关文档**:
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ADXL345 Datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ADXL345.pdf)
- [FreeRTOS Documentation](https://www.freertos.org/Documentation/)
- [MQTT Protocol Specification](https://mqtt.org/mqtt-specification/)

---

*🎉 感谢使用 EdgeVib ESP32-S3 Gateway! 如有问题请查阅本文档或提交Issue。*
