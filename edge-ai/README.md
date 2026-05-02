# EdgeVib Edge-AI 企业级TinyML部署系统

## 📋 项目概述

本项目是基于ESP32-S3网关的工业振动监测系统的AI训练与部署平台,采用企业级TinyML(Tiny Machine Learning)架构,实现从数据采集、模型训练到边缘推理的全流程自动化。

## 🏗️ 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    EdgeVib TinyML Pipeline                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────┐   UART    ┌──────────────┐   WiFi/MQTT   ┌──────┐│
│  │  STM32   │ ────────▶ │   ESP32-S3   │ ────────────▶ │  PC  ││
│  │ (采集端) │  温湿度   │  (边缘网关)   │   Training    │(训练)││
│  └──────────┘           └──────────────┘   Mode         └──────┘│
│       │                        │                           │   │
│       ▼                        ▼                           ▼   │
│  DHT11/22              ADXL345振动              Mosquitto Broker│
│  温湿度传感器           加速度传感器              (本地1883端口)  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## 📁 目录结构

```
edge-ai/
├── README.md                          # 项目说明文档
├── AGENTS.md                          # AI Agent协作规范 (企业级)
├── requirements.txt                   # Python依赖
│
├── data_collection/                   # 【第1阶段】数据采集模块
│   ├── __init__.py
│   ├── mqtt_subscriber.py             # MQTT订阅器 (接收ESP32数据)
│   ├── data_validator.py              # 数据验证与清洗
│   ├── csv_converter.py               # JSON→CSV转换器
│   └── config.yaml                    # 采集配置 (Topic/Broker等)
│
├── preprocessing/                     # 【第2阶段】数据预处理
│   ├── __init__.py
│   ├── feature_extractor.py           # 特征提取 (RMS/FFT/峰值等)
│   ├── data_augmentation.py           # 数据增强 (噪声/缩放/时间平移)
│   ├── normalization.py               # 数据标准化 (Z-score/Min-Max)
│   └── train_test_splitter.py         # 数据集划分 (训练/验证/测试)
│
├── models/                            # 【第3阶段】模型定义与训练
│   ├── __init__.py
│   ├── tinyml_models/                 # TinyML模型库
│   │   ├── __init__.py
│   │   ├── vibration_classifier.py    # 振动分类模型 (CNN/LSTM)
│   │   ├── anomaly_detector.py        # 异常检测模型 (Autoencoder)
│   │   └── fault_diagnoser.py         # 故障诊断模型 (多分类)
│   ├── training/                      # 训练脚本
│   │   ├── train_classifier.py        # 分类器训练入口
│   │   ├── train_anomaly.py           # 异常检测训练入口
│   │   └── hyperparameter_tuner.py    # 超参数自动调优
│   └── evaluation/                    # 模型评估
│       ├── metrics.py                 # 评估指标 (准确率/F1/AUC)
│       ├── confusion_matrix.py        # 混淆矩阵可视化
│       └── model_exporter.py          # 模型导出 (TensorFlow Lite)
│
├── deployment/                        # 【第4阶段】边缘部署
│   ├── __init__.py
│   ├── tflite_converter.py            # TensorFlow Lite转换
│   ├── quantization/                  # 模型量化 (INT8/FP16)
│   │   ├── __init__.py
│   │   ├── dynamic_range.py           # 动态范围量化
│   │   └── full_integer.py            # 全整数量化
│   ├── esp32_deployer.py              # ESP32部署工具
│   │   ├── generate_model_header.py   # 生成C头文件 (.h)
│   │   └── create_firmware_patch.py   # 固件补丁生成
│   └── validation/                    # 部署验证
│       ├── accuracy_validator.py      # 精度验证 (PC vs ESP32对比)
│       ├── latency_benchmark.py       # 延迟测试
│       └── memory_profiler.py         # 内存占用分析
│
├── monitoring/                        # 【第5阶段】运行监控
│   ├── __init__.py
│   ├── real_time_inference.py         # 实时推理演示
│   ├── performance_tracker.py         # 性能追踪
│   └── alert_system.py                # 告警系统 (阈值告警)
│
├── utils/                             # 通用工具
│   ├── __init__.py
│   ├── logger.py                      # 日志系统
│   ├── config_loader.py               # 配置加载器
│   └── visualization.py               # 可视化工具 (matplotlib)
│
├── tests/                             # 单元测试 & 集成测试
│   ├── test_data_pipeline.py          # 数据流水线测试
│   ├── test_models.py                 # 模型训练测试
│   └── test_deployment.py             # 部署流程测试
│
└── docs/                              # 文档
    ├── architecture.md                # 架构设计文档
    ├── api_reference.md               # API参考手册
    └── troubleshooting.md             # 故障排查指南
```

## 🚀 快速开始

### 第1步: 安装依赖

```bash
cd edge-ai
pip install -r requirements.txt
```

### 第2步: 启动Mosquitto Broker (PC端)

```bash
# Windows
mosquitto -v

# 或使用Docker
docker run -it -p 1883:1883 eclipse-mosquitto
```

### 第3步: 配置ESP32网关 (Training模式)

编辑 `firmware/esp32-gateway/main/esp32-gateway.c`:
```c
#define WIFI_SSID     "你的手机热点名称"
#define WIFI_PASSWORD "你的热点密码"
```

编译并烧录ESP32固件。

### 第4步: 运行数据采集

```bash
cd edge-ai/data_collection
python mqtt_subscriber.py --config ../config.yaml
```

数据将自动保存为CSV格式供后续训练使用。

## 📊 数据流详解

### 完整数据链路:

```
[STM32 DHT11] 
    ↓ UART (115200bps)
    ↓ 协议帧: [AA55][Len][DevID][04][Seq][TempData][CRC][0D]
[ESP32 Protocol Parser]
    ↓ 解析为 struct temp_humidity_data
    ↓ 温度补偿算法
[ESP32 Sensor Service]
    ↓ 融合 ADXL345 振动数据
    ↓ DSP处理 (RMS/FFT)
    ↓ 生成 struct analysis_result
[ESP32 MQTT Module]
    ↓ 序列化为 JSON
    ↓ Topic: edgevib/train/1/vibration
[Mosquitto Broker (PC)]
    ↓ QoS=1 保证送达
[Python mqtt_subscriber.py]
    ↓ JSON解析
    ↓ 特征提取
    ↓ CSV格式化
[training_data.csv]
    ↓
[TensorFlow/Keras Model Training]
    ↓
[tflite_model.tflite]
    ↓ 量化优化
[esp32_deployer.py]
    ↓ 生成 model_data.h
[ESP32 Firmware (Production Mode)]
    ↓ TensorFlow Lite Micro
[Edge Inference Result]
```

## 🔧 核心技术栈

| 组件 | 技术选型 | 版本要求 |
|------|---------|---------|
| MQTT Broker | Eclipse Mosquitto | >= 2.0 |
| Python | CPython | >= 3.9 |
| Deep Learning | TensorFlow | >= 2.12 |
| TinyML | TensorFlow Lite Micro | latest |
| Data Processing | NumPy / Pandas | >= 1.24 |
| Visualization | Matplotlib | >= 3.7 |
| Serialization | PyYAML / json | built-in |

## 📈 模型性能目标

| 模型类型 | 目标精度 | 推理延迟 | 内存占用 | Flash占用 |
|---------|---------|---------|---------|----------|
| 振动分类 (正常/异常) | >95% | <50ms | <20KB RAM | <100KB Flash |
| 故障诊断 (4类) | >90% | <100ms | <40KB RAM | <200KB Flash |
| 异常检测 (Autoencoder)| AUC>0.98 | <80ms | <30KB RAM | <150KB Flash |

## 🔐 安全考虑

- ✅ TLS 1.2/1.3 加密传输 (生产环境必须启用)
- ✅ 客户端证书双向认证 (可选)
- ✅ 数据脱敏处理 (去除敏感信息)
- ✅ 模型文件加密存储 (可选)

## 📝 许可证

MIT License - 详见 LICENSE 文件

## 👥 贡献指南

请参阅 [AGENTS.md](./AGENTS.md) 了解AI Agent协作规范和开发流程。
