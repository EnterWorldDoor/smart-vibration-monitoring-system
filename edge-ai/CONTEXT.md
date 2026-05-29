# Edge-AI PC — Training Pipeline Domain Context

## 训练环境

- **OS**: Windows 11 + Anaconda (conda env: `edgevib-tf`)
- **Python**: 3.11 + TensorFlow 2.15.1 + numpy/pandas/scikit-learn
- **硬件**: PC (CPU 训练，GPU 可选)
- **MQTT Broker**: 开发模式 PC 本机 Mosquitto (port 1883)

## 模型产出

Edge-AI PC 端训练管线产出两个模型，部署到两个平台：

| 产出 | 格式 | 目标平台 | 部署方式 |
|------|------|---------|---------|
| CNN-LSTM 4 分类器 | `.tflite` (FP32) | ESP32-S3 | SPIFFS 分区文件 → `TfLiteModelCreate()` 运行时加载 |
| Autoencoder 异常检测 | `.onnx` | Orange Pi 4 Pro | model-deploy → inference-engine ONNX Runtime 热重载 |

## 训练数据

### 开发模式（当前）
ESP32 直连 PC Mosquitto → `mqtt_subscriber.py` 订阅 `edgevib/train/#` → CSV

### 生产模式（P1 Training Data Sync）
api-server `GET /api/v1/data/export` → `http_sync.py` HTTP 拉取 → CSV

两种模式的 CSV 格式一致，`prepare_and_train.py` 不感知数据来源。

### CSV 格式 (training_data.csv)
```
timestamp_ms, dev_id, rms_x, rms_y, rms_z, overall_rms, peak_freq, peak_amp,
temperature_c, humidity_rh, label
```

## 训练管线 (prepare_and_train.py)

端到端脚本，从 CSV 到模型产出，7 步流水线：

| Step | 操作 | 输入 | 输出 |
|------|------|------|------|
| 0 | CSV 准备 + 伪标签 (RMS 阈值 + 频域排序) | `training_data.csv` | labeled rows |
| 1 | 24 维特征提取 (ESP32 对齐) | labeled rows | feature vectors |
| 2 | 数据增强 (高斯噪声) | feature vectors | augmented features |
| 3 | Z-score 标准化 + 时间序列窗口 [32, 24] | augmented features | normalized windows |
| 4 | 1D-CNN 训练 (TFLite Micro 安全，无 LSTM) | windows [32, 24] | `edgevib_cnn.h5` |
| 5 | TFLite 转换 (FP32, TFLITE_BUILTINS only) | `.h5` | `edgevib_classifier.tflite` + C 头文件 |
| 6 | C 头文件生成 (`model_data.h`) | `.tflite` | `deployment/model_data.h` |
| 7 | Autoencoder 训练 (24-dim 输入) | normal-class features | `autoencoder_24.h5` |

**24 维特征向量** (与 ESP32 `ai_service.c:push_feature_vector()` 严格对齐):
```
[rms_x, rms_y, rms_z, overall_rms,
 peak_freq_x, peak_amp_x, skewness_x, kurtosis_x, crest_factor_x,
 band_energy_x[0..7],
 peak_freq_y, peak_amp_y, crest_factor_y,
 peak_freq_z, peak_amp_z, crest_factor_z,
 temperature_c]
```

## 数据采集模块 (data_collection/)

| 文件 | 模式 | 用途 |
|------|------|------|
| `mqtt_subscriber.py` | 开发 | MQTT 订阅 `edgevib/train/#` → CSV |
| `http_sync.py` | 生产 (P1 新增) | HTTP GET api-server → CSV (增量) |
| `config.yaml` | 通用 | Broker 配置、输出路径、验证规则 |
| `training_data.csv` | 通用 | 训练数据（两种模式共享相同文件路径和格式） |

## Training Data Sync — PC 端设计

### 决策 1: 客户端形式 (2026-05-29)

**选择**: 独立脚本 `data_collection/http_sync.py`，与 `mqtt_subscriber.py` 并列

**用法**:
```bash
python data_collection/http_sync.py --api http://192.168.1.1:8080
python prepare_and_train.py
```

**增量同步**:
- 游标: `data_collection/last_sync.txt`，存 ISO8601 时间戳
- 首次运行 (无游标): `?from=1970-01-01T00:00:00Z` → 全量拉取
- 后续: `?from=<last_sync_time>` → 追加到 CSV
- 游标更新: CSV 写入成功后覆盖 `last_sync.txt`

### 决策 2: 容错 (2026-05-29)

| 场景 | 行为 |
|------|------|
| api-server 不可达 | 退出码 1 + 不更新游标 |
| 无新数据 (空响应) | 正常退出 |
| CSV 被手动删除 | 清空游标 → 全量重拉 |
| 下载中断 | 游标不动 → 下次重跑覆盖 |

Phase 1 不做自动重试，网络在 192.168.1.x LAN 内可靠。

## 模型部署模块 (deployment/)

| 文件 | 用途 |
|------|------|
| `onnx_converter.py` | Keras .h5 → ONNX + metadata JSON + 自动 POST 到 model-deploy |
| `tflite_converter.py` | Keras .h5 → TFLite (FP32/FP16/INT8) + C 头文件生成 |
| `config.yaml` | TFLite 转换参数 + ESP32 部署目标 |

## 关键路径

```
生产模式完整流程:
  http_sync.py → CSV → prepare_and_train.py → .tflite + .onnx
  → onnx_converter.py → POST model-deploy → Orange Pi inference-engine 热重载
  → .tflite → POST model-deploy → OTA Server → ESP32 HTTP 下载 → SPIFFS 分区 → 热切换
```
