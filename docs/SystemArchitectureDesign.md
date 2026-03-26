#  EdgeVib 系统设计文档（System Design Document）

---

# 1.  系统概述

## 1.1 系统名称
EdgeVib：工业预测性维护边缘智能系统

## 1.2 系统目标

构建一个从**设备采集 → 边缘AI → 网关 → 大数据 → Web可视化**的完整工业系统，实现：

- 振动监测
- 故障检测
- 预测性维护
- 数据分析

---

# 2.  总体架构设计

## 2.1 架构分层

| 层级 | 组件 |
|------|------|
| 设备层 | STM32（DMF407）+ ADXL345 + 电机 + 工业IO |
| 边缘层 | ESP32（AI）+ 树莓派（Linux网关） |
| 平台层 | Kafka + Spark + Flink（Docker） |
| 应用层 | Spring Boot + Vue |

---

## 2.2 数据流

STM32 → ESP32 → 树莓派 → MQTT → Kafka → Spark → Web

---

# 3. ⚙️ 硬件系统设计

## 3.1 STM32（核心节点）

### 功能划分

#### 数据采集任务
- ADXL345（I2C/SPI）
- ADC采样
- 编码器测速

#### 控制任务
- PWM控制直流电机
- 模拟设备负载变化

#### 工业IO任务
- 隔离输入（读取外部信号）
- 隔离输出（控制继电器）

#### 通信任务
- UART（ESP32通信）
- RS485（Modbus）
- CAN总线
- Ethernet TCP

---

## 3.2 ESP32（边缘AI）

- FFT振动分析
- RMS计算
- TinyML异常检测
- WiFi通信

---

## 3.3 树莓派（Linux网关）

### 驱动设计

- I2C驱动（ADXL345）
- GPIO驱动（控制IO）
- SPI驱动（扩展设备）
- 字符设备驱动（/dev/vib）

### 网关功能

- MQTT客户端
- 数据缓存
- 本地AI分析（Python）

---

# 4. 软件架构设计

## 4.1 STM32软件架构（FreeRTOS）

### 任务划分

| 任务 | 功能 |
|------|------|
| SensorTask | 采集振动数据 |
| MotorTask | 控制电机 |
| CommTask | 通信 |
| AlarmTask | 报警 |
| IoTask | IO处理 |

---

## 4.2 ESP32软件架构

- 数据接收（UART）
- FFT分析模块
- AI模型模块
- WiFi通信模块

---

## 4.3 树莓派软件架构

- Linux驱动层
- 用户态采集程序（C/Python）
- MQTT客户端
- 数据转发模块

---

## 4.4 后端架构（Spring Boot）

- 设备管理模块
- 数据接收模块
- WebSocket推送
- 报警系统

---

# 5.  通信设计

## 5.1 STM32 ↔ ESP32

- UART通信
- 数据格式：JSON

示例：

```json
{
  "device_id": "node01",
  "vibration": [0.12, 0.08, 0.15],
  "speed": 1200
}