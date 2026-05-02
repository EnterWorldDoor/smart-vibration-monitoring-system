# EdgeVib Edge-AI 企业级 AI Agent 协作规范

> **版本**: 2.1.0
> **最后更新**: 2026-05-01
> **适用范围**: edge-ai/ 目录下所有Python模块及AI训练/部署流程
> **训练环境**: Anaconda 隔离虚拟环境 (TensorFlow 2.15+)
> **部署目标**: ESP32-S3-DevKitC-1 (双模型级联推理)
> **未来扩展**: Raspberry Pi 4B/5 (高性能模型，待定)

---

## 📌 文档目的

本规范定义了EdgeVib边缘AI系统的Agent角色、职责边界、双模型协作流程和质量标准。
核心创新: **主模型(TFLite INT8 CNN/LSTM) + 兜底模型(C规则引擎) 双模级联架构**，
全部部署在ESP32-S3上，确保工业场景下的高可用性。

---

## 🔧 训练环境：Anaconda 隔离环境

### 创建并激活环境

```bash
conda create -n edgevib-tf python=3.11 -y
conda activate edgevib-tf

pip install tensorflow==2.15.1
pip install -r requirements.txt
```

### TensorFlow 版本锁定

| 组件 | 版本 | 说明 |
|------|------|------|
| Python | 3.11.x | Anaconda 环境 |
| TensorFlow | 2.15.1 | 最后的 Keras 2 稳定版，兼容 TFLite Micro |
| numpy | 1.26.x | 匹配 TF 2.15 |
| pandas | 2.0+ | 数据处理 |

---

## 🤖 Agent 角色定义

### Agent 1: DataEngineer (数据工程师)

**职责范围**:
- MQTT数据采集与验证 (从ESP32 Gateway Training模式)
- 数据清洗、去重、异常值处理
- 时域/频域特征工程提取
- 数据集构建与版本管理
- **运行位置**: PC (Anaconda 环境)

**输入**:
- Mosquitto Broker上的实时传感器数据 (Topic: `edgevib/train/#`)
- 配置文件 `data_collection/config.yaml`

**输出**:
- 清洗后的CSV训练集 (`datasets/processed_train.csv`)
- 特征增强数据集 (`datasets/features_train.csv`)
- 数据质量报告 (`reports/data_quality_report.json`)

**质量标准**:
- 数据完整性 > 99.5% (零缺失值)
- 特征维度: 时域8维 + 频域10维 + 统计6维 = 24维特征向量
- 异常值检测率 > 95%

---

### Agent 2: MLResearcher (机器学习研究员)

**职责范围**:
- **主模型设计**: 1D-CNN振动分类 + LSTM时序建模 → TFLite INT8 部署到ESP32
- **无监督预训练**: Autoencoder异常检测 (利用现有3000+条unknown数据)
- **兜底模型**: ISO 10816规则引擎标定 → C代码部署到ESP32 (fault_diagnosis.c)
- 超参数调优与交叉验证
- **运行位置**: PC (Anaconda 环境，GPU可选)

**输入**:
- 特征增强数据集 (来自DataEngineer)
- 模型配置 `models/config.yaml`

**输出**:
- 训练好的主模型 (`models/saved_models/primary_model.h5`)
- 兜底模型标定配置 (`models/saved_models/fallback_config.json`)
- Autoencoder模型 (`models/saved_models/autoencoder.h5`)
- 训练日志 (`logs/training_*.log`)
- 评估报告 (`reports/evaluation_report.md`)

**质量标准**:
- **主模型**: 分类准确率 > 90%, F1-Score > 0.88
- **兜底模型**: 异常检出率 > 85%, 误报率 < 5%
- **Autoencoder**: 重构误差AUC > 0.95
- 过拟合控制: val_loss < train_loss * 1.15

---

### Agent 3: DeploymentEngineer (部署工程师)

**职责范围**:
- TensorFlow Lite模型转换 (Keras .h5 → .tflite)
- INT8全整数量化 + 代表性数据集校准
- ESP32固件集成 (C头文件 `model_data.h` 生成)
- 量化精度对比验证
- **运行位置**: PC (Anaconda 环境)

**输入**:
- Keras模型 (.h5) (来自MLResearcher)
- ESP32硬件约束 (Flash: 4MB, PSRAM: 8MB, SRAM: 512KB)

**输出**:
- TFLite模型 (`deployment/models/edgevib_classifier_int8.tflite`)
- C头文件 (`deployment/esp32_model_data.h` → 拷贝到 `firmware/esp32-gateway/components/ai_service/`)
- 量化对比报告 (`reports/quantization_report.json`)

**ESP32 部署目标**:
| 指标 | 目标 | 说明 |
|------|------|------|
| 推理延迟 | < 80ms | ESP32-S3 @240MHz |
| RAM占用 | < 35KB | TFLite arena + 模型权重 |
| Flash占用 | < 200KB | INT8量化后模型 |
| 精度损失 | < 1.5% | vs Keras FP32 baseline |

---

## 🔄 协作工作流

### 整体架构：边缘训练 + 边缘推理

```
┌──────────────────────────────────────────────────────────────────────┐
│                        EdgeVib 完整架构                              │
│                                                                      │
│  ┌──────────┐   UART    ┌──────────────────┐   WiFi/MQTT   ┌───────┐│
│  │  STM32   │ ────────▶ │   ESP32-S3        │ ────────────▶ │  PC   ││
│  │ (采集端) │  温湿度   │  (边缘网关+推理)    │   Training    │(训练) ││
│  └──────────┘           │                    │   Mode        └───┬───┘│
│                         │  ┌──────────────┐  │                   │    │
│  ADXL345 ──────────────▶│  │ 主模型 TFLite │  │    Anaconda       │    │
│  (SPI振动)              │  │ (INT8 CNN)    │  │    TensorFlow     │    │
│                         │  ├──────────────┤  │    ↓ 训练         │    │
│                         │  │ 兜底 规则引擎  │  │    .h5 → .tflite │    │
│                         │  │ (C 阈值检测)   │  │    ──────────────┘    │
│                         │  └──────────────┘  │    model_data.h        │
│                         │        │           │    ↓ 烧录到ESP32       │
│                         │  诊断结果→MQTT发布  │                        │
│                         └────────────────────┘                        │
│                                                                      │
│  ┌──────────────────┐  (未来)                                        │
│  │  Raspberry Pi     │  更高性能模型                                   │
│  │  (待定)           │  (ResNet/EfficientNet等)                       │
│  └──────────────────┘                                                │
└──────────────────────────────────────────────────────────────────────┘
```

### 双模型级联推理流水线 (ESP32端)

```
┌──────────────────────────────────────────────────────────────┐
│              ESP32-S3 推理任务 (ai_service)                    │
│                                                              │
│  sensor_service                                              │
│  ── analysis_result ──▶                                     │
│                         ┌─────────────────────┐              │
│                         │  模型级联控制器      │              │
│                         │  (ai_service.c)      │              │
│                         └─────────┬───────────┘              │
│                                   │                          │
│                    ┌──────────────▼──────────┐               │
│                    │  主模型 (Primary)        │               │
│                    │  TFLite Micro INT8       │               │
│                    │  置信度 > 0.85?          │               │
│                    └──────┬─────┬────────────┘               │
│                           │     │                            │
│                       是  │     │  否                         │
│                           │     ▼                            │
│                           │  ┌────────────────────┐          │
│                           │  │ 兜底模型 (Fallback)  │          │
│                           │  │ fault_diagnosis.c   │          │
│                           │  │ ISO 10816 + SPC      │          │
│                           │  └──────────┬─────────┘          │
│                           │             │                     │
│                           ▼             ▼                     │
│                    ┌─────────────────────────┐                │
│                    │   诊断结果 → MQTT 发布   │                │
│                    └─────────────────────────┘                │
└──────────────────────────────────────────────────────────────┘
```

### 阶段门禁 (Stage Gates)

| 门禁 | 进入条件 | 退出标准 | 运行位置 |
|------|---------|---------|---------|
| G1: 数据就绪 | MQTT Broker在线, CSV存在 | 数据量>1000条, 完整率>99% | PC |
| G2: 特征就绪 | 数据清洗完成 | 特征维度=24, 无NaN | PC |
| G3: 兜底标定 | 特征数据可用 | ISO阈值计算完成, 规则参数导出 | PC |
| G4: 主模型收敛 | 数据集已划分 | val_accuracy>90%, 无过拟合 | PC (GPU可选) |
| G5: 量化验证 | TFLite INT8模型生成 | 精度损失<1.5% | PC |
| G6: ESP32部署 | C头文件就绪, TFLite已集成 | 延迟<80ms, RAM<35KB | ESP32 |

---

## 📊 ESP32-S3 硬件资源评估

### 目标硬件: ESP32-S3-DevKitC-1

| 资源 | 规格 | 当前占用 | 双模型可用 |
|------|------|---------|-----------|
| Flash | 4MB (factory分区) | ~1MB (固件) | **~3MB 可用** |
| SRAM | 512KB internal | ~150KB (RTOS+WiFi+DSP) | **~350KB 可用** |
| PSRAM | 8MB external | 未使用 | **8MB 可用** |
| CPU | Xtensa LX7 双核 240MHz | ~40% 负载 | **60% 可用** |

### 双模型资源需求 (INT8 量化后)

| 组件 | Flash | RAM | 推理时间 |
|------|-------|-----|---------|
| 主模型 TFLite (INT8 CNN/LSTM) | ~150KB | ~25KB | ~50ms |
| 兜底模型 (C规则引擎) | ~2KB | ~0.5KB | <1ms |
| TFLite Micro 运行时 | ~50KB | ~5KB | - |
| **合计** | **~202KB** | **~30.5KB** | **~51ms** |

> ✅ **结论**: ESP32-S3 完全可以部署双模型。Flash仅占 5%，RAM仅占 6%。

### Flash 分区规划 (建议)

```
# partitions_ai.csv (建议新增)
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,    0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x340000,   # 固件 3.25MB
models,   data, fat,            0x350000, 0xB0000,   # 模型存储区 0.7MB
```

---

## 🆕 创新点

### 1. 双模型级联架构 (Model Cascade) — ESP32端
- **主模型高精度**: INT8 TFLite CNN/LSTM，50ms内完成推理
- **兜底模型高可用**: C规则引擎零依赖，模型加载失败时无缝切换
- **置信度门控**: 主模型输出置信度 < 0.85 时自动降级到规则引擎
- **现有基础**: fault_diagnosis.c 已经实现阈值检测，直接复用

### 2. 无监督预训练管线
- 利用3000+条 `unknown` 标签数据训练Autoencoder
- 重构误差作为异常度分数，无需标注即可检测异常
- Autoencoder编码器可作为CNN分类器初始化权重（迁移学习）

### 3. 自适应阈值管理
- 基于运行工况(温度/转速)动态调整ISO 10816振动阈值
- SPC统计过程控制：EWMA指数加权移动平均实现漂移检测

### 4. 边缘-云端协同
- **ESP32**: TFLite INT8 双模型推理 + 规则引擎
- **PC (Anaconda)**: 模型训练 + 超参数优化 + 数据湖
- **Raspberry Pi (未来)**: 更高性能模型支撑 (ResNet/EfficientNet)

### 5. 可解释AI
- 规则引擎输出人类可读的诊断结论
- SHAP值分析特征重要性 (PC端)
- 兜底模型作为漂移期间的稳定保障

### 6. 模型漂移检测 (PC端)
- PSI (Population Stability Index) 监控特征分布偏移
- 自动触发模型再训练建议
- 兜底模型在漂移期间提供不间断保障

---

## 📁 代码规范

### 命名约定

```python
# 文件名: snake_case
feature_extractor.py, vibration_cnn.py, rule_engine.py

# 类名: PascalCase
class FeatureExtractor: ...
class VibrationCNNLSTM: ...
class RuleEngine: ...

# 函数/变量: snake_case
def extract_time_domain_features(data): ...
primary_model = ...
fallback_config = ...

# 常量: UPPER_SNAKE_CASE
MAX_SEQUENCE_LENGTH = 512
ISO_10816_ALARM_THRESHOLD = 7.1
```

### 文档字符串标准

每个公开函数必须包含Google风格的docstring

### 日志规范

```python
from utils.logger import get_logger

logger = get_logger(__name__)

logger.info("Training started: model=%s, epochs=%d", model_name, epochs)
logger.warning("Low confidence inference: score=%.3f, fallback to rule engine", score)
```

---

## 🔗 接口契约

### DataEngineer → MLResearcher

**特征CSV Schema** (features_train.csv):

```csv
dev_id,window_start_ms,window_end_ms,
rms_x,rms_y,rms_z,overall_rms,peak_freq,peak_amp,
skewness_x,kurtosis_x,crest_factor_x,
freq_band_0_50,freq_band_50_100,freq_band_100_200,freq_band_200_400,
temp_c,humidity_rh,temp_comp_active,
label
```

### MLResearcher → DeploymentEngineer

**模型元数据 (JSON)**:

```json
{
  "model_name": "edgevib_vibration_classifier_v2",
  "framework": "tensorflow",
  "version": "2.15.1",
  "architecture": "cnn_lstm_hybrid_int8",
  "input_shape": [256, 24],
  "output_classes": ["normal", "imbalance", "misalignment", "bearing_fault"],
  "accuracy": 0.953,
  "f1_score": 0.948,
  "inference_time_ms": 48.5,
  "model_size_kb": 145,
  "ram_usage_kb": 28,
  "fallback_config": {
    "iso_10816_class": "class_II",
    "alarm_threshold_mm_s": 7.1,
    "trip_threshold_mm_s": 11.0
  },
  "training_timestamp": "2026-05-01T10:30:00Z",
  "training_environment": "Anaconda edgevib-tf (Python 3.11, TF 2.15.1)"
}
```

---

## 📊 模型性能目标

| 模型类型 | 目标准确率 | 推理延迟 | 内存占用 | 运行位置 |
|---------|-----------|---------|---------|---------|
| 主模型 (TFLite INT8 CNN/LSTM) | >90% | <80ms | <35KB RAM, <200KB Flash | **ESP32-S3** |
| 兜底模型 (C规则引擎) | >85% | <1ms | <1KB RAM, <5KB Flash | **ESP32-S3** |
| Autoencoder (FP32) | AUC>0.95 | <15ms | <50MB RAM | PC训练用 |
| 未来RPi模型 | 待定 | 待定 | 待定 | Raspberry Pi (未来) |

---

## 🔐 安全规范

- MQTT连接必须支持TLS 1.2+加密
- 模型文件完整性校验 (SHA256)
- 敏感配置 (Broker密码等) 不允许硬编码
- 训练数据本地处理，不上传云端

---

**文档维护者**: EnterWorldDoor (AI System Architect)
**最后审核**: 2026-05-01
**下次计划更新**: 2026-05-15
