<div align="center">

# EdgeVib · 工业预测性维护边缘智能振动监测系统

**从传感器到边缘 AI 到工业协议的端-边-云全栈闭环**

Smart Vibration Monitoring System for Industrial Predictive Maintenance

[![Firmware](https://img.shields.io/badge/Firmware-STM32%20%7C%20ESP32--S3-blue)]()
[![Edge](https://img.shields.io/badge/Edge-Orange%20Pi%204%20Pro%20%7C%20A733-orange)]()
[![Kernel](https://img.shields.io/badge/Linux%20Kernel-6.6%20Drivers%20D1--D7-black)]()
[![AI](https://img.shields.io/badge/AI-TFLite%20Micro%20%7C%20ONNX%20%7C%20LLM-green)]()
[![Lang](https://img.shields.io/badge/Lang-C%20%7C%20Go%20%7C%20Python-yellow)]()
[![Infra](https://img.shields.io/badge/Infra-Docker%20%7C%20MQTT%20%7C%20TimescaleDB-9cf)]()

</div>

---

## 目录

- [一句话简介](#一句话简介)
- [为什么做这个项目](#为什么做这个项目)
- [系统亮点](#系统亮点)
- [系统架构总览](#系统架构总览)
- [硬件清单 (BOM)](#硬件清单-bom)
- [技术栈](#技术栈)
- [仓库结构](#仓库结构)
- [快速开始](#快速开始)
- [核心数据链路](#核心数据链路)
- [关键技术指标](#关键技术指标)
- [项目进度与路线图](#项目进度与路线图)
- [文档索引](#文档索引)

---

## 一句话简介

**EdgeVib** 是一套面向旋转机械（电机、泵、风机、压缩机）的**双通道振动预测性维护系统**。它以 STM32 + ESP32-S3 + Orange Pi 4 Pro 异构硬件为载体，构建了从 **设备采集 → 边缘 AI 实时诊断 → 工业安全联锁 → 边缘网关聚合与二次 AI → PC 模型训练回灌** 的完整工业 IoT 技术闭环。

系统同时在**驱动端 (DE)** 与**非驱动端 (NDE)** 双测点采集振动，通过对比诊断区分"电机侧故障"与"负载侧故障"——这是单测点方案无法做到的。

---

## 为什么做这个项目

在旋转机械运行中，振动是设备故障最关键的早期信号，ISO 10816 标准据此定义了振动烈度健康分级。传统维护方式存在明显痛点：

- 人工巡检成本高、间隔长、主观性强；
- 计划性维护导致过度维修或漏检；
- 云端 AI 分析延迟高（秒级），**无法实现毫秒级安全联锁**；
- 单一测点（仅 DE）**无法定位故障源**（电机侧 vs 负载侧）。

EdgeVib 用"**边缘实时 + 网关趋势 + 云端训练**"三级 AI 协同，把故障检测下沉到设备端（<80 ms），把趋势预测与剩余寿命放在网关，把模型训练留在 PC，形成可持续迭代的工业诊断体系。

---

## 系统亮点

| 亮点 | 说明 |
|------|------|
| 🎯 **双通道对比诊断** | DE (ESP32-S3) + NDE (STM32F103) 双测点，24 维特征全链路对齐，区分电机侧/负载侧故障 |
| 🧠 **三级 AI 协同** | ESP32 端 TFLite Micro 实时 4 分类 · 网关 ONNX 自编码器趋势/RUL · 网关本地 LLM 生成中文故障报告 |
| 🔒 **工业级安全联锁** | 急停 + 双动作恢复（ISO 13850）+ 12 路隔离 IO + 双看门狗（IWDG+WWDG），急停→PWM 硬件关断 < 100 μs |
| 🛰️ **多协议工业通信** | CAN 2.0B (CRC8) · UART CRC16-MODBUS · MQTT · RS232 备份链路 · OPC UA · Modbus (预留) |
| 🐧 **Linux 内核驱动栈** | D1–D7：虚拟 CAN / IIO / 块设备 / GPIO+IRQ / 软件 RTC / HWMON / evdev / V4L2，把工业数据以标准 Linux 子系统暴露 |
| 🏭 **跨车间告警广播** | edge-router 实现车间级 AI 告警联动，从"时序对比"扩展到"空间对比"诊断 |
| 📦 **一键容器化部署** | Docker Compose 编排 15+ 服务 + 完整 Prometheus/Grafana 可观测性栈 |
| 🔁 **模型闭环** | 网关数据 → PC 训练 → TFLite/ONNX 量化 → OTA 热部署回设备，无需人工烧录 |

---

## 系统架构总览

系统分为四层：**设备层 → 边缘 AI 层 → 边缘网关层 → 平台层**。

```
┌──────────────────────── 设备层 (Device Layer) ────────────────────────┐
│                                                                        │
│  ADXL345[DE] ──SPI──► ESP32-S3 (边缘AI网关 / DE端)                       │
│   驱动端               ├─ sensor_service  400Hz FIFO 采集                │
│                        ├─ dsp             FFT/RMS/峰度/8频带能量          │
│                        ├─ ai_service      1D-CNN (TFLite Micro) 4分类     │
│                        └─ fault_diagnosis ISO 10816-3 规则引擎兜底        │
│                                                                        │
│  ADXL345[NDE] ─SPI─► STM32F103 (裸机)                                    │
│   非驱动端            ├─ dsp_fft_q15  自写定点 64点 RFFT                  │
│                       ├─ dsp_nde      24维特征                            │
│                       └─ can_send     CAN 0x201(17帧CRC8) + 0x202心跳     │
│                                │ CAN 2.0B 500kbps                        │
│                                ▼                                         │
│  STM32F407 (DMF407 主控)                                                 │
│   ├─ can_nde     CAN主站 · 17帧重组 96字节特征                            │
│   ├─ motor       PD6010D 电机控制 (PWM/PID/编码器/ADC/故障)               │
│   ├─ digital_io  12路隔离输入 + 安全状态机 (急停/模式/复位)                │
│   ├─ alarm_svc   4路隔离输出 (绿/黄/红/继电器) + 蜂鸣器                    │
│   ├─ protocol    UART CRC16-MODBUS (含 OTA 0x20-0x23)                    │
│   ├─ wdg         双看门狗 IWDG(3s)+WWDG(~300ms)                          │
│   └─ LVGL GUI    2.8" TFT 平板风界面                                      │
└────────────────────────────────┬───────────────────────────────────────┘
                                  │ UART4 921600 (0x17/0x18双通道特征)
                                  │ WiFi/MQTT      RS232 备份链路
                                  ▼
┌────────────── 边缘网关层 (Orange Pi 4 Pro · A733 · Ubuntu 22.04 · kernel 6.6) ──────────────┐
│                                                                                             │
│  基础设施:  Mosquitto(MQTT) · TimescaleDB(PG16, 9张时序表) · Grafana(7仪表盘)                 │
│  可观测性:  Prometheus · Node-Exporter · cAdvisor · Alertmanager · alert-webhook             │
│                                                                                             │
│  微服务集群 (Docker + systemd, 11个):                                                        │
│   data-aggregator(Go)  MQTT去重入库      inference-engine(Py) ONNX自编码器趋势/RUL/健康评分   │
│   llm-analyzer(Py)     Qwen2.5-1.5B 报告  edge-router(Go)      跨车间告警广播                  │
│   api-server(Go)       REST+WebSocket     rs232-gateway(C)     RS232备份链路网关              │
│   opcua-server(C)      OPC UA :4840       vision-service(Py)   USB摄像头巡检抓拍              │
│   audio-monitor(Py)    声学异常监测        ota-server(Go)        固件OTA分发                    │
│   model-deploy(Go)     AI模型分发/回滚                                                        │
│                                                                                             │
│  Linux 内核驱动栈 (kernel module + Go daemon, D1–D7):                                         │
│   D1 vcan(SocketCAN) · D2 IIO振动 · D3 块设备+虚拟GPIO · D4 软件RTC                           │
│   D5 HWMON电机健康 · D6 急停evdev · D7 V4L2视频+MIPI CSI模板                                   │
└────────────────────────────────┬────────────────────────────────────────────────────────────┘
                                  │ WiFi / LAN / HTTP CSV导出
                                  ▼
┌──────────────────── 平台层 (Edge-AI PC · GPU) ────────────────────┐
│  edge-ai/  TensorFlow 2.15 训练管线                                │
│   data_collection → data_pipeline → models → deployment          │
│   数据采集(MQTT/HTTP) → 清洗/特征 → 1D-CNN分类 + 自编码器          │
│   → TFLite(INT8) / ONNX 量化 → OTA 回灌 ESP32 / Orange Pi         │
└───────────────────────────────────────────────────────────────────┘
```

> 更完整的分层架构、协议表、安全状态机与 ADR 见 [`docs/SystemArchitectureDesign.md`](docs/SystemArchitectureDesign.md)。

---

## 硬件清单 (BOM)

| 设备 | 型号 | 角色 | 关键外设 |
|------|------|------|----------|
| **ESP32-S3** | DevKitC-1 (≥16MB Flash + PSRAM) | 边缘 AI 网关 + DE 传感器 | ADXL345 (SPI), DHT11, WiFi |
| **STM32F407** | 正点原子 ATK-DMF407 V1.0 (Cortex-M4F 168MHz) | 主控 + 电机 + 隔离IO + CAN主站 | PD6010D 驱动板, 12路隔离IO, 2.8" TFT |
| **STM32F103** | Blue Pill C8T6 (Cortex-M3 72MHz) | NDE 传感器节点 (裸机) | ADXL345 (SPI), SN65HVD230 |
| **Orange Pi 4 Pro** | 全志 A733 (2×A76 + 6×A55, NPU 3TOPS), 4GB | 边缘网关 (MQTT/DB/AI/视觉/声学) | Ubuntu 22.04, kernel 6.6, MIPI CSI, USB Cam, I2S Mic |
| **电机驱动** | ATK-PD6010D | 直流有刷电机驱动 | DC12-60V, 10A/600W, 编码器, I/V/T 传感 |
| **急停按钮** | LA38-11ZS | 安全联锁 (ISO 13850) | 蘑菇头旋转复位, NC 常闭 |
| **CAN 收发器** | SN65HVD230 ×2 | NDE 总线 | 500kbps 差分, 两端电气隔离 |
| **Edge-AI PC** | x86 + GPU | 模型训练与部署 | TensorFlow 2.15 |

---

## 技术栈

| 层级 | 语言 / 框架 | 关键技术 |
|------|------------|----------|
| **固件 (STM32F407)** | C, FreeRTOS 10.3.1 | HAL, LVGL v8.3, CMSIS-RTOS2, Linux 内核编码风格, 全静态内存 |
| **固件 (STM32F103)** | C, 裸机 | 自写定点 FFT (无 FPU/无 CMSIS-DSP), FIFO 中断驱动 |
| **固件 (ESP32-S3)** | C/C++, ESP-IDF + FreeRTOS 11 | esp-tflite-micro, esp-nn, CMSIS-DSP, NVS, SPIFFS OTA |
| **边缘服务 (Go)** | Go 1.22 | chi, gorilla/websocket, pgx/v5, Paho MQTT, slog |
| **边缘服务 (Python)** | Python 3.10/3.11 | ONNX Runtime, llama.cpp (Qwen2.5-1.5B), OpenCV, sounddevice, asyncio |
| **边缘服务 (C)** | C | open62541 (OPC UA), libyaml, CRC16-MODBUS, Paho C |
| **内核驱动** | C (kernel 6.6) + Go daemon | SocketCAN, IIO, blk-mq, gpio/irqchip, rtc_class, hwmon, input/evdev, V4L2 |
| **基础设施** | Docker Compose | Mosquitto, TimescaleDB (PG16), Grafana, Prometheus 栈 |
| **AI 训练** | Python, TensorFlow 2.15 / Keras 2 | 1D-CNN, Autoencoder, tf2onnx, TFLite INT8 量化, scikit-learn |

---

## 仓库结构

```
smartSystem/
├── firmware/                     # 设备层固件
│   ├── esp32-gateway/            # ESP32-S3 边缘AI网关 (ESP-IDF, 19个组件)
│   ├── stm32_node_vibration/     # STM32F407 DMF407 主控节点 (FreeRTOS)
│   └── stm32_node_nde/           # STM32F103 NDE 传感器节点 (裸机)
│
├── edge-gateway/                 # Orange Pi 4 Pro 边缘网关
│   ├── services/                 # 11个微服务 (Go/Python/C)
│   ├── drivers/                  # Linux 内核驱动 D1–D7 (kernel module + Go daemon)
│   ├── docker/                   # docker-compose + TimescaleDB/Grafana/Prometheus 配置
│   ├── config/                   # 各服务/驱动 YAML 配置
│   ├── CONTEXT.md                # 领域上下文与全部架构决策记录 (ADR)
│   └── KERNEL-LESSONS.md         # 内核 6.6 编译经验教训
│
├── edge-ai/                      # 平台层 PC 训练管线 (TensorFlow)
│   ├── data_collection/          # MQTT / HTTP 数据采集
│   ├── data_pipeline/            # 清洗 + 特征工程
│   ├── models/                   # CNN + 自编码器 + 集成级联 + 规则/统计兜底
│   ├── deployment/               # TFLite / ONNX 量化导出
│   └── prepare_and_train.py      # 端到端训练主脚本
│
├── docs/                         # 设计文档 (架构 / 需求 / drawio 图)
├── tests/                        # 硬件通路验证工程 (ADXL345/DHT11/NTC 等 sketch)
└── tools/                        # 辅助工具 (esp-mcp 等)
```

---

## 快速开始

### 1. 边缘网关服务栈 (Orange Pi 4 Pro)

```bash
cd edge-gateway
# 一键启动基础设施 + 全部容器化微服务
docker compose -f docker/docker-compose.yml up -d

# 单独启动基础设施
docker compose -f docker/docker-compose.yml up -d mosquitto timescaledb grafana

# 查看日志
docker compose -f docker/docker-compose.yml logs -f data-aggregator
```

访问入口：Grafana `:3000` (admin/admin123) · API `:8080` · OPC UA `:4840` · Prometheus `:9090`

> 硬件直通类服务 (rs232-gateway / opcua-server / vision-service / audio-monitor) 通过各自 `systemd/` unit 部署，直接访问串口/USB/I2S 设备。

### 2. Linux 内核驱动 (以 D2 IIO 振动设备为例)

```bash
cd edge-gateway/drivers/iio_vibration
make                 # 编译内核模块 (需内核头文件)
sudo make load       # insmod 加载
bash test/test_module.sh   # 在 Orange Pi 上跑集成测试
```

### 3. ESP32-S3 固件

```bash
cd firmware/esp32-gateway
idf.py set-target esp32s3
idf.py build flash monitor
```

### 4. STM32 固件

使用 STM32CubeIDE 打开 `firmware/stm32_node_vibration`（F407）或 `firmware/stm32_node_nde`（F103），编译烧录。F407 支持 bootloader + OTA。

### 5. AI 模型训练 (PC)

```bash
cd edge-ai
pip install -r requirements.txt
python prepare_and_train.py   # 采集→特征→训练→TFLite/ONNX 量化导出
```

---

## 核心数据链路

```
F103 NDE ──CAN 0x201/0x202──► F407 can_nde ──UART4 0x17/0x18──► ESP32-S3 ──MQTT──► Orange Pi
  24维特征        17帧CRC8         96字节重组      CRC16双通道      1D-CNN实时诊断    data-aggregator
                                                                        │                │
                                                                        ▼                ▼
                                                              F407 安全联锁       TimescaleDB.sensor_data
                                                              (急停/健康等级)             │
                                                                                         ▼
                                                              inference-engine ──► ai_reports ──► .../ai/report
                                                                (ONNX 趋势/RUL)                       │
                                        ┌──────────────────────────────────────────────────┬─────────┴────────┐
                                        ▼                     ▼                ▼            ▼                  ▼
                                  llm-analyzer          vision-service    audio-monitor  edge-router       api-server
                                  中文故障报告            事件抓拍          声学异常        跨车间广播          REST+WS
```

`.../ai/report` 是被 **5 个下游服务同时订阅**的核心扇出主题。所有服务通过 **MQTT（Mosquitto）事件总线** 解耦，通过 **TimescaleDB** 共享状态。MQTT 主题命名规范：`EdgeVib/{site_id}/{device_type}/{device_id}/{data_type}`。

---

## 关键技术指标

| 指标 | 数值 |
|------|------|
| 振动采样率 | 400 Hz（三轴同步） |
| 特征窗口 | 160 ms（64 样本）· 推理窗口 32×24 |
| 特征向量维度 | 24（DE / NDE 全链路一致） |
| ESP32 AI 推理延迟 | < 80 ms（1D-CNN, TFLite Micro）|
| ESP32 模型体积 | < 200 KB Flash / < 35 KB RAM |
| 安全回路响应（急停→PWM 关断） | < 100 μs（EXTI ISR 直连）|
| CAN 总线速率 | 500 kbps（每帧 CRC-8-Dallas/Maxim）|
| UART 帧校验 | CRC16-MODBUS |
| 看门狗 | IWDG 3s + WWDG ~300ms 级联 |
| F103 资源占用 | Flash 55% (35KB/64KB) · SRAM 15% (3KB/20KB) |
| 网关 LLM | Qwen2.5-1.5B Q4_K_M, ~1.5GB 内存, 5–8 tok/s |
| 时序库保留策略 | 传感器 90 天 / 视觉·音频 60 天 |

---

## 项目进度与路线图

### ✅ 已完成

- **设备层**：F103 NDE 采集 + 定点 FFT · F407 主控（电机/安全IO/CAN主站/双看门狗/LVGL）· ESP32 双后端级联 AI + ISO10816 规则引擎 + OTA 热更新
- **边缘网关层**：11 个微服务全部落地 · TimescaleDB 9 表数据模型 · Grafana 7 仪表盘 · Prometheus 全监控栈 · 内核驱动 D1–D7 · 跨车间广播 edge-router
- **平台层**：TensorFlow 训练管线 · TFLite/ONNX 量化 · 模型 OTA 回灌闭环
- **通信**：CAN / UART CRC16 / MQTT / RS232 备份 / OPC UA / NTP 时间同步

### 🚧 进行中 / 规划

- audio-monitor 设备端 I2S 采集固件（当前在 `feature/audio-monitor` 分支）
- D7 MIPI CSI 真实 OV5640 摄像头硬件验证
- RS485 Modbus RTU 从站 · Ethernet 第三备份通道 (lwIP)
- edge-router Phase 2：真实多台 Orange Pi 跨机 MQTT bridge
- inference-engine NPU (3 TOPS) 加速接入

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [`docs/SystemArchitectureDesign.md`](docs/SystemArchitectureDesign.md) | 系统架构设计：分层、硬件、通信协议、安全架构、数据模型、ADR、性能指标 |
| [`docs/IndustrialPredictiveMaintenanceSystem.md`](docs/IndustrialPredictiveMaintenanceSystem.md) | 项目总述：背景动机、目标、AI 诊断体系、数据管线、进度 |
| [`docs/EdgeGatewayPlatform.md`](docs/EdgeGatewayPlatform.md) | 边缘网关平台：11 个微服务 + 内核驱动 D1–D7 详解、部署编排 |
| [`docs/diagrams/`](docs/diagrams/) | drawio 架构图（系统/软件/硬件）|
| [`edge-gateway/CONTEXT.md`](edge-gateway/CONTEXT.md) | 边缘网关领域上下文与全部架构决策记录（权威）|
| 各子工程 `AGENTS.md` | 固件/服务的编码规范与模块说明 |

---

<div align="center">

**EdgeVib** — 让每一次异常振动，都在故障发生前被听见。

</div>
