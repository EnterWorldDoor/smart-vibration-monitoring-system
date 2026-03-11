# Industrial Predictive Maintenance System
## 系统架构设计文档

版本：v1.0  
作者：XXX  
日期：2026  

---

# 1. 系统概述

本系统是一个基于 **工业物联网（IIoT）和边缘计算技术** 的预测性维护平台。

系统通过振动传感器实时采集设备运行数据，并利用边缘AI模型进行故障检测。同时，系统将数据发送到数据平台进行存储与分析。

由于学生开发资源有限，系统的大数据平台通过 **Docker容器部署在笔记本虚拟机中进行模拟**。

系统整体架构包含：

1. 传感节点层（Sensor Node）
2. 通信层（IoT Communication）
3. 边缘网关层（Edge Gateway）
4. 数据平台层（Data Platform）
5. 可视化监控层（Dashboard）

---

# 2. 技术栈选择

## 2.1 嵌入式节点

硬件：

- STM32F4（振动采集）
- ADXL345（三轴加速度传感器）
- ESP32（WiFi通信）

软件：

- FreeRTOS
- C/C++
- CMSIS DSP（FFT）

---

## 2.2 边缘网关

硬件：

- Raspberry Pi 4

软件：

- Linux
- Python
- MQTT Broker（Mosquitto）
- Docker

AI框架：

- TensorFlow Lite
- Scikit-learn

---

## 2.3 数据平台（Docker）

部署在笔记本虚拟机中：

组件：

- Kafka（数据流）
- Spark（流式分析）
- InfluxDB（时序数据库）
- PostgreSQL（业务数据库）

部署方式：

Docker Compose

---

## 2.4 Web可视化

前端：

- Vue.js
- ECharts

后端：

- Java Spring Boot

---

# 3. 系统总体架构
+----------------------------------------------------+
| Laptop Data Platform |
| |
| +-----------+ +---------+ +----------------+ |
| | Kafka |-->| Spark |-->| InfluxDB | |
| +-----------+ +---------+ +----------------+ |
| |
| +-------------------+ |
| | Spring Boot API | |
| +-------------------+ |
| |
| +-------------------+ |
| | Web Dashboard | |
| +-------------------+ |
+----------------------------------------------------+
         ↑ MQTT
+----------------------------------------------------+
| Edge Gateway (Raspberry Pi) |
| |
| +--------------+ |
| | MQTT Broker | |
| +--------------+ |
| |
| +--------------+ |
| | Edge AI | |
| | (Fault Detect)| |
| +--------------+ |
| |
| +--------------+ |
| | Data Forward | |
| +--------------+ |
| |
| +--------------+ |
| | Camera AI | (Future) |
| +--------------+ |
+----------------------------------------------------+
                 ↑ WiFi
+----------------------------------------------------+
| Sensor Node (Embedded) |
| |
| STM32F4 + ADXL345 |
| |
| Tasks: |
| - Sensor Sampling |
| - FFT Analysis |
| - Feature Extraction |
| |
| ESP32 |
| |
| - MQTT Publish |
| |
+----------------------------------------------------+

---

# 4. 模块划分

系统分为以下模块：

## 4.1 Sensor Node Module

运行在 STM32

功能：

- 振动采集
- FFT分析
- 特征提取
- 数据发送

FreeRTOS任务：
TaskSensorCollect
TaskSignalProcessing
TaskFeatureExtract
TaskDataTransmit

---

## 4.2 Communication Module

运行在 ESP32

功能：

- MQTT连接
- 数据发送
- OTA升级

协议：

MQTT

---

## 4.3 Edge Gateway Module

运行在 Raspberry Pi

模块：

- MQTT Broker
- Edge AI
- Data Forwarder

---

## 4.4 Edge AI Module

功能：

振动故障识别

输入：
RMS
Kurtosis
Peak
FFT spectrum

输出：
Normal
Imbalance
Bearing Fault
Misalignment


---

## 4.5 Data Platform Module

部署方式：

Docker

组件：
Kafka
Spark
InfluxDB
PostgreSQL

---

## 4.6 Visualization Module

功能：

- 实时数据
- 历史曲线
- 故障报警

---

# 5. 通信协议设计

## 5.1 MQTT Topic
factory/device1/vibration
factory/device1/status
factory/device1/alert
factory/device1/ai_result


---

## 5.2 MQTT消息格式

JSON

示例：
{
"device_id": "motor_01",
"timestamp": 1710000000,
"rms": 0.23,
"peak": 1.02,
"kurtosis": 3.8,
"temperature": 40
}


---

# 6. API接口定义

后端：

Spring Boot

Base URL
http://localhost:8080/api

---

## 获取设备列表
GET /devices
返回：
[
{
"deviceId":"motor_01",
"status":"normal"
}
]


---

## 获取设备数据
GET /device/{id}/data

---

## 获取报警
GET /alerts

---

# 7. 硬件连接设计

## STM32 与 ADXL345

SPI连接：
STM32 ADXL345

3.3V -> VCC
GND -> GND
PA5 -> SCK
PA6 -> MISO
PA7 -> MOSI
PA4 -> CS

---

## STM32 与 ESP32

UART连接：
STM32 ESP32

TX -> RX
RX -> TX
GND -> GND


---

# 8. 摄像头扩展设计（未来）

系统未来支持：

视觉预测维护

新增模块：

Camera Node

硬件：

- Raspberry Pi Camera
- USB Camera

AI：
YOLO
OpenCV


功能：

- 设备表面缺陷检测
- 热成像检测

---

# 9. 系统部署方案

开发环境：
STM32CubeIDE
VSCode
Docker
IDEA

Docker部署：
docker-compose


包含：
kafka
spark
influxdb
postgres

---

# 10. 系统可扩展性

未来支持：

- 多设备节点
- 5G通信
- 云端平台
- 数字孪生