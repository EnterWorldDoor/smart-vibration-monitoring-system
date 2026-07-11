# EdgeVib：面向工业预测性维护的边缘智能振动监测系统

---

# 1. 项目背景

## 1.1 工业场景

在旋转机械（电机、泵、风机、压缩机）的运行中，振动是故障最关键的早期信号之一。ISO 10816 标准定义了基于振动烈度的设备健康分级。

传统维护方式存在显著问题：
- 人工巡检成本高、间隔长、主观性强
- 计划维护导致过度维修或漏检
- 云端 AI 分析延迟高 (秒级), 无法实现毫秒级安全联锁
- 单一测点 (驱动端 DE) 无法定位故障源 (电机侧 vs 负载侧)

## 1.2 项目动机

EdgeVib 构建一个从 **双通道设备采集 → 边缘 AI 对比诊断 → 安全联锁 → 数据汇聚** 的完整工业系统，
以 STM32 + ESP32 + Orange Pi 异构硬件平台为载体，展示工业 IoT 从端到边缘到云的技术闭环。

---

# 2. 项目目标

- 实现双通道 (DE 驱动端 + NDE 非驱动端) 振动数据同步采集
- 实现边缘 AI 实时故障检测 (TFLite CNN-LSTM + ISO 10816 规则引擎)
- 实现双通道对比诊断 — 区分电机侧故障与负载侧故障
- 实现工业级安全联锁 (急停 + 双动作恢复 + 隔离 IO + 看门狗)
- 实现多协议工业通信 (CAN 2.0B / UART CRC16 / MQTT / RS485 Modbus)
- 构建完整数据管线: 边缘采集 → 本地聚合 → PC 训练 → 模型部署

---

# 3. 系统架构

## 3.1 设备拓扑

```
                      ADXL345[DE] (驱动端)
                           │ SPI
                      ESP32-S3 (边缘AI网关)
                      ├─ DSP (FFT/RMS/Peak)
                      ├─ CNN-LSTM (TFLite)
                      ├─ ISO 10816 规则引擎
                      │
ADXL345[NDE] (非驱动端)  │  UART4 (CRC16-MODBUS)
     │ SPI               │
STM32F103C8T6 ──CAN──→ STM32F407 (DMF407)
(蓝板, 裸机)   500kbps  ├─ CAN主站 + NDE数据接收
                        ├─ 电机控制 (PD6010D)
                        ├─ 隔离IO (急停/模式/LED/蜂鸣)
                        ├─ 安全状态机
                        │
                        ↓ 双通道特征向量 (UART4 CMD 0x17/0x18)
                        ESP32-S3
                        ├─ 双通道对比诊断
                        │    ├─ DE特征 vs NDE特征
                        │    ├─ 故障源定位 (电机侧/负载侧)
                        │    └─ 异常评分 + 健康等级
                        │
                        ↓ MQTT
                        Orange Pi 4 Pro (Ubuntu)
                        ├─ Mosquitto Broker
                        ├─ 数据聚合 (SQLite/InfluxDB)
                        ├─ Python 趋势分析
                        └─ Docker 服务
                        │
                        ↓ WiFi/LAN
                        Edge-AI PC (GPU)
                        ├─ 数据收集 + 标注
                        ├─ 模型训练 (Keras/TensorFlow)
                        └─ TFLite 模型导出 → 部署到 ESP32
```

## 3.2 硬件清单

| 设备 | 型号 | 角色 | 传感器/外设 |
|------|------|------|------------|
| ESP32-S3 | DevKitC-1 | 边缘AI + DE传感器 | ADXL345 (SPI), DHT11 |
| STM32F407 | DMF407 V1.0 | 主控 + 电机 + IO + CAN主站 | ADXL345 (预留), PD6010D驱动板, 隔离IO, 2.8" TFT |
| STM32F103 | Blue Pill | NDE传感器节点 | ADXL345 (SPI), SN65HVD230 |
| Orange Pi 4 Pro | 全志A733 | 边缘网关 + MQTT Broker | 4GB RAM, Ubuntu, Camera |
| ATK-PD6010D | 正点原子 | 直流有刷电机驱动 | DC12-60V, 10A/600W, 编码器, 电流/电压/温度传感 |

## 3.3 通信矩阵

| 链路 | 协议 | 速率 | 用途 |
|------|------|------|------|
| ADXL345 → ESP32-S3 | SPI | 4.5MHz | DE振动数据 (400Hz) |
| ADXL345 → STM32F103 | SPI | 4.5MHz | NDE振动数据 (400Hz) |
| STM32F103 → STM32F407 | CAN 2.0B | 500kbps | NDE特征向量 + 心跳 |
| STM32F407 → ESP32-S3 | UART4 CRC16 | 921600bps | 双通道特征 + 电机状态 + 系统状态 |
| ESP32-S3 → Orange Pi | MQTT/WiFi | — | 特征数据 + 健康状态 |
| STM32F407 → Orange Pi | RS232 (UART5) | 115200bps | 备份通道 (OTA + 关键告警) |
| STM32F407 → Orange Pi | RS485 (USART3) | 115200bps | Modbus RTU (预留) |
| Orange Pi → 上位机/SCADA | OPC UA | :4840 | 工业协议暴露 (opcua-server) |
| Orange Pi → 外部客户端 | REST + WebSocket | :8080 | 查询 + 实时事件推送 (api-server) |
| Orange Pi ↔ Orange Pi | MQTT (跨车间) | — | 告警广播 (edge-router, Phase1 单机模拟) |

---

# 4. AI 诊断体系

## 4.1 特征工程

从原始三轴振动加速度 (400Hz, 64样本窗口=160ms) 提取 24 维特征向量：

| 维度 | 特征 | 说明 |
|------|------|------|
| [0-3] | rms_x, rms_y, rms_z, overall_rms | 振动烈度 (ISO 10816 基础) |
| [4-5] | peak_freq_x, peak_amp_x | 主频 + 幅值 |
| [6-8] | skewness_x, kurtosis_x, crest_factor_x | 波形统计特征 |
| [9-16] | band_energy_x[0..7] | 8频带能量分解 |
| [17-19] | peak_freq_y, peak_amp_y, crest_factor_y | Y轴频域特征 |
| [20-22] | peak_freq_z, peak_amp_z, crest_factor_z | Z轴频域特征 |
| [23] | temperature | 温度 (DE=DHT11, NDE=22°C占位) |

## 4.2 模型架构

### ESP32-S3 设备端: 1D-CNN (TFLite Micro)

```
输入: 24维特征向量 × 32时间窗口 [32,24]
  ↓
1D-CNN: Conv1D(32/64/128) + BatchNorm + Dropout
  ↓
MaxPool1D → GlobalAvgPool1D
  ↓
Dense(64) + Dropout → Softmax
  ↓
4分类: normal / imbalance / misalignment / bearing_fault + 置信度
```

> **说明**: 早期设计为 CNN-LSTM 级联，但 TFLite Micro / ESP-NN 不支持 hybrid LSTM 算子，实际部署去掉 LSTM，采用纯 1D-CNN（Keras 精度 0.933，<200KB Flash，<80ms）。时序趋势建模上移到网关侧 ONNX 自编码器。

### Orange Pi 网关端: ONNX 自编码器 (趋势/异常)

```
24维特征 → Encoder(128→64→32→16→latent 8) → Decoder(镜像) → 重构
  ↓
重构误差 (MSE) + 分位数阈值(p95) → 无监督异常检测
  + 趋势分析 + 剩余寿命(RUL) + 电机综合健康评分(0-100)
```

### ISO 10816 规则引擎 (确定性诊断)

- 基于 overall_rms (振动烈度) 查表分级
- 与 CNN-LSTM 输出融合: 模型高置信度 OR 规则超阈值 → 告警
- 模型不确定时退回到规则引擎 (Fail-Safe)

## 4.3 双通道对比诊断

| 场景 | DE (驱动端) | NDE (非驱动端) | 诊断结论 |
|------|------------|---------------|----------|
| 正常 | 正常 | 正常 | 设备健康 |
| 电机轴承故障 | 高振动 | 正常 | 电机侧故障 |
| 负载侧不平衡 | 正常 | 高振动 | 负载侧故障 |
| 联轴器不对中 | 高振动 (轴向) | 高振动 (径向) | 联轴器故障 |
| 基础松动 | 高振动 | 高振动 | 安装基础问题 |

---

# 5. 工业安全设计

## 5.1 安全联锁硬件

- **急停按钮**: LA38-11ZS 蘑菇头旋转复位 (ISO 13850), NC 常闭触点
- **隔离输入**: 12 路光耦 LTV-247 (DMF407 CN25 端子), 可接工业传感器
- **隔离输出**: 4 路光耦 (绿灯/黄灯/红灯/继电器), 直接驱动 AD16-22DS 工业指示灯
- **蜂鸣器**: 有源蜂鸣器 PF0 (高分贝)
- **看门狗**: IWDG 独立看门狗 (LSI 40kHz, 3s 超时) + 注册制软件心跳守护

## 5.2 安全状态机

```
拍下急停 → EMERGENCY (PWM硬件关断, 红灯, 蜂鸣连续)
旋转复位 → WAIT_RESET (黄灯闪烁, 电机不恢复)
按下复位 → NORMAL (绿灯, 电机恢复控制)
```

- **Fail-Safe**: IN1 断线 = 立即 EMERGENCY
- **上电安全**: 启动时检测急停状态, 已拍下 → 直接 EMERGENCY
- **双动作恢复**: 绝不自动重启电机

---

# 6. 数据管线

## 6.1 实时流

```
传感器 (400Hz原始) → 特征提取 (每160ms) → 2s批次 → ESP32推理 → MQTT → Orange Pi存储
                                                                       ↓
                                                                  ISO 10816规则 → 健康等级 → F407安全联锁
```

## 6.2 训练管线

```
Orange Pi 数据导出 → Edge-AI PC 训练集构建 → Keras/TensorFlow 训练
                                                      ↓
                                              TFLite 量化模型 (.h5 → .tflite)
                                                      ↓
                                              ESP32-S3 模型部署 (TFLite Micro)
```

---

# 7. 项目进度

## 7.1 已实现

### 设备层 (固件)

| 模块 | 平台 | 状态 |
|------|------|------|
| ADXL345 传感器驱动 + FIFO | ESP32-S3, STM32F103 | 完成 |
| DSP (FFT/RMS/Peak/Kurtosis/8频带) | ESP32-S3, STM32F103 | 完成 |
| 24维特征提取 (自写定点FFT) | STM32F103 (dsp_fft_q15) | 完成 |
| 1D-CNN TFLite Micro 推理 (4分类) | ESP32-S3 (esp-tflite-micro + esp-nn) | 完成 |
| ISO 10816-3 规则引擎兜底 | ESP32-S3 (fault_diagnosis) | 完成 |
| AI 双后端级联 (本地+远程) | ESP32-S3 | 完成 |
| 模型 OTA 热更新 (SPIFFS 分区) | ESP32-S3 (model_loader/updater) | 完成 |
| UART CRC16-MODBUS 协议栈 (含OTA 0x20-0x23) | STM32F407, ESP32-S3 | 完成 |
| CAN NDE 17帧重组 (CRC8校验) | STM32F407 | 完成 |
| 电机控制 (PWM/PID/编码器/ADC/故障) | STM32F407 (PD6010D) | 完成 |
| 12路隔离输入 + 安全状态机 | STM32F407 | 完成 |
| 4路隔离输出 + 蜂鸣器 + LED 矩阵 | STM32F407 | 完成 |
| 双看门狗级联 (IWDG+WWDG) | STM32F407 | 完成 |
| LVGL GUI + bootloader/OTA | STM32F407 | 完成 |
| 企业级组件 (config/monitor/log/data) | ESP32-S3 | 完成 |

### 边缘网关层 (Orange Pi 4 Pro)

| 模块 | 技术 | 状态 |
|------|------|------|
| 基础设施 (Mosquitto/TimescaleDB/Grafana) | Docker | 完成 |
| data-aggregator (MQTT去重入库) | Go | 完成 |
| inference-engine (ONNX 趋势/RUL/健康评分) | Python | 完成 |
| llm-analyzer (Qwen2.5-1.5B 中文报告) | Python/llama.cpp | 完成 |
| edge-router (跨车间告警广播) | Go | 完成 |
| api-server (REST + WebSocket + CSV导出) | Go | 完成 |
| rs232-gateway (串口备份链路) | C/systemd | 完成 |
| opcua-server (OPC UA :4840) | C/open62541 | 完成 |
| vision-service (USB摄像头巡检) | Python/OpenCV | 完成 |
| audio-monitor (声学异常监测) | Python | 完成 |
| ota-server / model-deploy (OTA分发) | Go | 完成 |
| Prometheus 全监控栈 + Alertmanager | Docker | 完成 |
| 内核驱动栈 D1–D7 (CAN/IIO/块/GPIO/RTC/HWMON/input/V4L2) | kernel 6.6 + Go daemon | 完成 |

### 平台层 (Edge-AI PC)

| 模块 | 技术 | 状态 |
|------|------|------|
| 数据采集 (MQTT/HTTP 同步) | Python/paho | 完成 |
| 数据清洗 + 特征工程 | Python/pandas | 完成 |
| 1D-CNN + 自编码器 + 集成级联 + 规则/统计兜底 | TensorFlow 2.15 | 完成 |
| TFLite(INT8) / ONNX 量化导出 | Python | 完成 |
| 模型 OTA 回灌闭环 | — | 完成 |

## 7.2 待实现 / 进行中

| 模块 | 说明 |
|------|------|
| audio-monitor 设备端 I2S 采集固件 | 当前在 `feature/audio-monitor` 分支，未合入 |
| D7 MIPI CSI 真实 OV5640 摄像头 | 驱动模板就绪，待硬件验证 |
| RS485 Modbus RTU 从站 | F407 ↔ Orange Pi 通过 TP8485 |
| Ethernet 第三备份通道 (lwIP) | F407 ↔ Orange Pi, OTA 大文件传输 |
| edge-router Phase 2 (真实多台 Pi 跨机 bridge) | 当前为单机多站模拟 |
| inference-engine NPU (3 TOPS) 加速 | 当前 CPU/OpenBLAS |

---

# 8. 关键技术指标

| 指标 | 数值 |
|------|------|
| 振动采样率 | 400 Hz (三轴同步) |
| 特征窗口 | 160 ms (64样本) · 推理窗口 32×24 |
| 特征向量维度 | 24 (DE/NDE 全链路一致) |
| AI 推理延迟 (ESP32) | < 80ms (1D-CNN) |
| ESP32 模型体积 | < 200KB Flash / < 35KB RAM |
| CAN 总线速率 | 500 kbps |
| CAN 特征帧 CRC | CRC-8-Dallas/Maxim (每帧) |
| UART 帧 CRC | CRC16-MODBUS |
| 安全回路响应 (急停→PWM关) | < 100μs (EXTI ISR) |
| 看门狗 | IWDG 3s + WWDG ~300ms 级联 |
| F407 任务栈 | app_enterprise 8KB, proto_rx 2KB, wdg_daemon 1KB |
| F103 Flash 使用 | ~35KB / 64KB (55%) |
| F103 SRAM 使用 | ~3KB / 20KB (15%) |
| 网关时序库 | TimescaleDB (PG16), 9 张 hypertable |
| 网关 LLM | Qwen2.5-1.5B Q4_K_M, ~1.5GB 内存, 5–8 tok/s |
| 目标内核 | 6.6.98-sun60iw2 (全志 A733) |
