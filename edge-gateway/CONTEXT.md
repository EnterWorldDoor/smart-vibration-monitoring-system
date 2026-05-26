# Orange Pi 4 Pro Edge Gateway — Domain Context

## 硬件平台

- **SoC**: 全志 A733 (2×Cortex-A76 + 6×Cortex-A55 @ 2.0GHz, RISC-V E902 协处理器 @ 200MHz)
- **GPU**: Imagination BXM-4-64
- **NPU**: 3 TOPS (INT8)
- **内存**: 4GB LPDDR4x
- **存储**: microSD + eMMC 插座
- **网络**: Wi-Fi 6 + BT 5.4, 千兆以太网 (PoE 支持, YT8531CA PHY)
- **显示**: HDMI 2.0 (4K@60fps), 4-lane MIPI DSI
- **摄像头**: 1× 2-lane MIPI CSI, 1× 4-lane MIPI CSI
- **USB**: 1× USB 3.0 Type-A, 3× USB 2.0 Type-A
- **扩展**: 40Pin GPIO (GPIO/UART/I2C/SPI/PWM), 3-pin DEBUG UART
- **供电**: Type-C 5V 3A
- **PoE**: 千兆以太网支持 PoE 供电 (需 PoE 交换机/注入器) — 一根网线解决供电+通信, 工业现场标准

## 系统软件 (已配置)

- **OS**: Ubuntu 22.04.5 LTS (Orange Pi 1.0.8 Jammy), kernel 6.6.98-sun60iw2 aarch64
- **主机名**: edgevib-gateway
- **用户**: orangepi (sudo NOPASSWD enabled), root
- **容器**: Docker CE 29.2.1 + Docker Compose v5.1.0
- **运行时**: Go 1.22.5, Python 3.10.12, GCC 11.4.0, GNU Make 4.3
- **项目路径**: /opt/edge-gateway (monorepo edge-gateway/ 子目录)
- **远程开发**: VS Code Remote-SSH, Claude Code PC端通过SSH操作

## SSH 配置

- **认证**: ED25519 密钥 `/c/Users/lishu/.ssh/id_ed25519` (PC端), 已添加到 orangepi 和 root 的 authorized_keys
- **KeepAlive**: ClientAliveInterval=60, ClientAliveCountMax=10
- **登录命令**: `ssh orangepi@192.168.1.1` 或 `ssh root@192.168.1.1`
- **sudo**: NOPASSWD 已配置 (`/etc/sudoers.d/orangepi`)

## 网络配置 (实际部署)

### 拓扑
```
PC (192.168.1.100/24, 以太网) ←→ Orange Pi (192.168.1.1/24, eth0)
PC (10.160.35.x/17, WLAN)      ←→ 校园网/外网
```

### Orange Pi 网络
- **eth0**: 静态 IP 192.168.1.1/24, gateway 192.168.1.100
- **wlan0**: 未配置 (后续用作 WiFi AP, ESP32 直连)
- **DNS**: 通过 SSH 隧道代理 (见下方"代理链")
- **网络管理**: NetworkManager (nmcli), eth0 连接名 "Wired connection 1"

### PC 端网络
- **以太网**: 192.168.1.100/24 (静态, 通过 `netsh` 设置)
- **WLAN**: 10.160.35.x/17 (校园网 DHCP)
- **注意**: 校园网仅允许单设备登录，PC WiFi 是唯一外网通道

### Orange Pi 上网方案 (代理链) — 已简化

**新架构 (2026-05-23)**:
```
Orange Pi → SSH 反向隧道 → PC 系统代理 (127.0.0.1:7897) → 外网
Orange Pi DNS → UDP:53 → Python relay → TCP:5353 → SSH 隧道 → 8.8.8.8:53
```
不再需要 proxy.py。SSH 隧道直接将 Orange Pi 的 18888 端口转发到 PC 系统代理。

**PC 端启动代理链** (每次 PC 重启后执行):
```bash
bash D:\smartSystem\edge-gateway\scripts\proxy-start.sh
```

**Orange Pi 端配置** (已持久化):
- APT: `/etc/apt/apt.conf.d/99proxy` — `http://localhost:18888`
- Docker: `/etc/systemd/system/docker.service.d/http-proxy.conf` — `HTTP_PROXY=http://localhost:18888`
- Docker daemon: `/etc/docker/daemon.json` — registry-mirrors 使用 `docker.m.daocloud.io`
- Shell: `~/.bashrc` 已添加 `http_proxy=https_proxy=localhost:18888`

## 网络拓扑 (目标架构)

```
ESP32-S3 #1: 192.168.1.10 (静态IP)
ESP32-S3 #2: 192.168.1.11 (DHCP预留)
网段: 192.168.1.0/24 (现场工业网络, 扁平二层)
Orange Pi 开 WiFi 热点，ESP32 全部直连，不依赖外部路由器
```

## Docker 服务 (已部署运行)

### 基础设施层
- **Mosquitto MQTT Broker**: port 1883 (WebSocket 9001), 匿名访问, 持久化启用
- **TimescaleDB**: PostgreSQL 16 + TimescaleDB 扩展, port 5432, 用户 edgevib/edgevib123, 数据库 edgevib_ts
- **Grafana**: port 3000, 管理员 admin/admin123, 预置数据源连接 TimescaleDB

### 启动/停止
```bash
cd /opt/edge-gateway
docker compose -f docker/docker-compose.yml up -d mosquitto timescaledb grafana
docker compose -f docker/docker-compose.yml down
docker compose -f docker/docker-compose.yml logs -f
```

### Docker 镜像
- `eclipse-mosquitto:2.0` (~20MB)
- `timescale/timescaledb:latest-pg16` (~470MB)
- `grafana/grafana:latest` (~324MB)
- `docker.m.daocloud.io` 为镜像加速源

## MQTT Topic 命名规范

```
EdgeVib/{site_id}/{device_type}/{device_id}/{data_type}

示例:
  EdgeVib/factory1/motor/de01/data/sensor       — DE端振动数据
  EdgeVib/factory1/motor/nde01/data/sensor      — NDE端振动数据
  EdgeVib/factory1/motor/nde01/data/motor       — 电机电流/电压 (CMD 0x06)
  EdgeVib/factory1/gateway/esp32-01/status/health
  EdgeVib/factory1/camera/vision01/data/image   — 视觉快照
  EdgeVib/factory1/aggregator/orangepi/ai/report — AI分析报告
```

设备 MQTT 层使用字符串 ID (`de01`, `nde01`, `motor01`)，协议层 byte ID 通过 config_manager 映射。

## 服务架构

### 基础设施层
- **Mosquitto MQTT Broker** (port 1883): 所有 ESP32 数据入口
- **TimescaleDB** (PostgreSQL 时序扩展): 工业级时序存储，与 Grafana 配合
- **Docker Compose**: 一键编排全部服务

### 数据管线层
- **data-aggregator** (Go): MQTT 消费 → 去重合并 → TimescaleDB 写入。去重策略: (device_id, timestamp, source_path) 三元组
  - **职责边界**: 只做可靠数据管道——解析提取 (topic→site_id/device_type/device_id, JSON→timestamp) + 去重 + 写入 `sensor_data`。**不做**告警判断/AI分类，AI报告写入留给 inference-engine。单一职责：aggregator 挂了只丢数据，inference 挂不影响数据存留
  - **MQTT 订阅**: `EdgeVib/+/+/+/data/#` 通配订阅，从 topic 第2-4层提取 site_id/device_type/device_id
  - **时间提取**: ESP32 的 `timestamp_ms` 基于 boot-relative `esp_timer_get_time()`，非 Unix 绝对时间。data-aggregator 用 **Orange Pi 本地系统时间** (已 NTP 同步) 写入 `sensor_data.time` 分区键。JSON 内原始 `timestamp_ms` 保留在 payload 中用于溯源
  - **并发模型**: 单 goroutine 串行处理 (MQTT OnMessage → chan → parse → dedup → batch insert)。ESP32 每 2s 一条，10 设备仅 5 msg/s，无需并行。简单性优先，去重无锁。DB 写入攒批刷入
  - **依赖选型**: `eclipse/paho.mqtt.golang` (MQTT 3.1.1) + `jackc/pgx` (pgxpool 连接池) + `gopkg.in/yaml.v3` (配置解析)。轻量、工业稳定、TimescaleDB 官方推荐 pgx
  - **去重算法**: 用 JSON 内 `timestamp_ms` 做**精确匹配**，key = `(device_id, timestamp_ms, source_path)`。5s 窗口是 cache 条目的 TTL 存活时间。QoS 1 重传消息 `timestamp_ms` 不变，可精确去重。cache 用 in-memory map + mutex + 定时清理过期条目
  - **错误处理**: MQTT 断连→paho 自动重连 + `CleanSession=false` 持久会话保底。DB 写入失败→丢弃该批次 + ERROR 日志 + 计数，不缓存重试（MQTT 持久会话保底）。JSON 解析失败→丢弃 + WARN 日志 + 计数，不阻塞管道。SIGTERM→停止订阅→flush 最后批次→关闭连接池→退出
  - **批次写入**: pgxpool + 多值 INSERT 单语句写入。100 rows 或 5s 间隔，任一触发即 flush。2 设备场景每批约 5 行，Grafana 数据延迟 ≤5s
  - **健康上报**: 内部维护消息计数、去重命中、解析错误、DB 错误、运行时长等指标。30s 周期发布到 `EdgeVib/{site_id}/aggregator/orangepi/status/health` MQTT topic。日志用 Go `log/slog` 结构化 JSON 输出至 stdout，docker logs 可读
  - **数据模型**: 极简两层。解析层只从 topic 提取 `site_id/device_type/device_id/data_type` + 从 JSON 提取 `timestamp_ms` 做去重。JSON payload 以 `json.RawMessage` 全透传进 JSONB 列，不做解构。避免与 ESP32 JSON schema 耦合
  - **测试策略**: 单元测试 (mock `Subscriber`/`Writer` 接口，测 topic 解析 + 去重逻辑) + 集成测试 (`docker compose up mosquitto timescaledb`，paho 客户端发模拟消息验证端到端)。去重逻辑纯函数无依赖，优先单测覆盖
  - **Module 路径**: `edgevib/data-aggregator` (简洁，Docker 内编译无外部引用)。入口 `cmd/aggregator/main.go`，internal 分包 `mqtt`/`topic`/`dedup`/`store`/`model`/`health`
- **rs232-gateway** (C): UART5/RS232 串口守护进程。复用现有 CRC16-MODBUS 协议解析，F407 直连备份通道。通过 systemd 自启动
- **edge-router** (Go): 多设备路由转发，模拟同级节点数据交换

### AI 推理层
- **inference-engine** (Python/ONNX Runtime): 复杂模型 — 趋势预测、剩余寿命(RUL)、多设备聚合分析
- **llm-analyzer** (Python/llama.cpp): 本地大模型故障报告生成，提示词模板驱动
- **vision-service** (Python/OpenCV + V4L2): USB 摄像头定时拍照 + 存档，V4L2 DMA 零拷贝采集。后期扩展 OpenCV 运动检测/异常识别

#### inference-engine 详细设计 (2026-05-24)

##### 职责边界

inference-engine **不是训练模型的**。模型训练在 Edge-AI PC (`edge-ai/`) 上完成。inference-engine 的职责：
1. 加载 PC 端预转换的 ONNX 模型
2. 在 Orange Pi 上用 ONNX Runtime 做推理（CPU 或 NPU）
3. 消费 TimescaleDB 历史数据做批量推理
4. 结果写回 TimescaleDB / MQTT

与 ESP32 `ai_service.c` 的互补关系：
| 维度 | ESP32 ai_service | Orange Pi inference-engine |
|------|------------------|---------------------------|
| 模型 | TFLite Micro (CNN-LSTM, <200KB) | ONNX Runtime (趋势预测/RUL, 更大) |
| 推理类型 | 实时 4 分类 (normal/imbalance/misalignment/bearing_fault) | 批量趋势分析 + 剩余寿命 + 多设备聚合 |
| 输入 | 32×24 特征窗口 (实时特征提取) | TimescaleDB 查询历史窗口 |
| 延迟 | <80ms 实时 | 10s 间隔批量 |
| 硬件 | Xtensa LX7 双核 240MHz | 全志 A733 (2×A76 + 6×A55) + NPU 3TOPS |

##### 数据输入路径 (决策 1)

**混合模式**：定时从 TimescaleDB 读数据做趋势/RUL 分析 + MQTT 订阅紧急事件触发即时推理。

```
主路径: TimescaleDB (vibration_view 等 VIEW) → 定时查询(10s) → ONNX 推理 → 结果写入 TimescaleDB
触发路径: MQTT EdgeVib/+/+/+/status/health (异常事件) → 即时触发推理
```

选择理由：
- 与 data-aggregator 无功能重叠（aggregator 只做透传写入，inference-engine 做分析）
- TimescaleDB 已有 vibration_view 等 VIEW，无需重复解析 JSON
- 紧急告警不等待 10s 轮询周期

##### ONNX 模型转换流水线 (决策 2)

**Edge-AI PC 端预转换**（主路径），不在 Orange Pi 上做 Keras → ONNX 转换。

```
Edge-AI PC: Keras .h5 → tf2onnx → .onnx → scp/rsync → Orange Pi
Orange Pi:  ONNX Runtime 加载 .onnx → 推理
```

选择理由：
- PC 端已有完整 TensorFlow 训练管线，加一个 ONNX 导出脚本即可
- Orange Pi Docker 镜像保持轻量（`python:3.11-slim` + `onnxruntime`，不含 TensorFlow）
- 避免 4GB 边缘设备上装 ~1GB TensorFlow 的镜像膨胀

##### 特征维度

inference-engine 与 ESP32 共享相同的 24 维特征向量定义：
```
[rms_x, rms_y, rms_z, overall_rms, peak_freq_x, peak_amp_x,
 skewness_x, kurtosis_x, crest_factor_x,
 band_energy_x[0..7],
 peak_freq_y, peak_amp_y, crest_factor_y,
 peak_freq_z, peak_amp_z, crest_factor_z,
 temperature_c]
```
来源：`ai_service.c:push_feature_vector()` 与 `ai_service.h:AI_NUM_FEATURES=24`

##### 特征来源 (决策 3)

**直接用 ESP32 预提取特征**（通过 TimescaleDB VIEWs），不在 Orange Pi 端重新做 DSP。

```
ESP32: raw ADC → FFT/RMS/Peak → 特征向量 → MQTT JSON → data-aggregator → TimescaleDB JSONB
Orange Pi: vibration_view (JSONB 解构) → 时间序列特征 → ONNX 模型推理
```

选择理由：
- ESP32 每 2s 发送的 MQTT JSON 中已包含 RMS/峰值频率/FFT 频带能量等特征，不发送原始 400Hz ADC 波形
- `vibration_view` 已从 JSONB 解构出 `rms_x/y/z, overall_rms, peak_frequency_hz, peak_amplitude_g, fft_peaks` 等字段
- 趋势预测/RUL 分析使用 RMS 走向和频率漂移即可，不需要原始波形
- Autoencoder 异常检测如需原始数据重构，后续可要求 ESP32 新增 raw data topic（Phase 2）

##### MVP 范围 (决策 4)

**统计趋势 + Autoencoder 异常检测** 同时上线。

**Phase 1 (MVP)**:
- **统计趋势分析** (Python numpy/scipy，无需 ONNX): RMS 趋势斜率、频带能量漂移、温度-振动相关性、DE/NDE RMS ratio
- **Autoencoder 异常检测** (ONNX Runtime): 24 维特征向量 → 重构误差 → 异常分数，捕捉训练数据中未见的故障模式

**Phase 2** (后续):
- RUL 剩余寿命预测 (ONNX)：需额外训练数据（运行至故障的完整生命周期数据）
- 多设备对比分析

现有模型资产：`edge-ai/models/saved_models/autoencoder_best.h5` 已训练，可直接 tf2onnx 转换部署。

##### 多设备聚合策略 (决策 5)

**分层聚合**：设备级独立推理 → Python 层 DE/NDE 对比 → 电机级健康评分。

```
Layer 1 (设备级): DE 端独立统计趋势 + Autoencoder 异常检测
                 NDE 端独立统计趋势 + Autoencoder 异常检测
Layer 2 (电机级): Python 代码做 DE/NDE 对比 — RMS ratio 趋势、频谱相似度、相位一致性
                 加权计算电机综合健康评分
Layer 3 (车间级): 多台电机健康排序 (Phase 2)
```

选择理由：
- 设备级独立推理复用同一个 24 维 Autoencoder 模型，不引入 48 维联合模型
- `dual_channel_view` 已提供 RMS ratio 和 spectral_similarity，Python 直接消费
- 电机综合健康评分逻辑在 Python 代码中，可调且不依赖 ML

##### 结果输出策略 (决策 6)

**双写**：TimescaleDB 存全量 + MQTT 发关键发现。

```
inference-engine
  ├── TimescaleDB: inference_results 表 (全量持久化)
  │    列: time, site_id, device_type, device_id,
  │         anomaly_score, health_score, trend_slope_rms,
  │         de_nde_rms_ratio, model_name, model_version,
  │         inference_time_ms, details(JSONB)
  │    Grafana 直接 SQL 查询 → 趋势图/历史对比
  │
  └── MQTT: EdgeVib/{site_id}/inference/{device_id}/ai/report (关键发现)
       内容: { anomaly_detected, health_score, trend_direction, summary }
       QOS 1, Retained=false
       用途: 即时告警通知、llm-analyzer 触发报告生成
```

选择理由：TimescaleDB 保证数据持久可查询，MQTT 解耦即时通知消费方。

##### MQTT 触发条件 (决策 7)

**AI 分类异常 + RMS 超阈值** 即时触发推理，绕过 10s 轮询。

| 触发条件 | MQTT 来源 | 动作 |
|----------|-----------|------|
| AI 分类 = bearing_fault 或 confidence < 0.85 (fallback) | `.../data/sensor` JSON 中 `ai_class_id`/`cascade_source` | Autoencoder 二次验证 + 趋势回溯 |
| overall_rms > 7.1 mm/s (ISO 10816 Zone D) | `.../data/sensor` JSON 中 `overall_rms` | 回溯过去 5 分钟数据，判断恶化速率 |

设备离线不触发推理（无新数据），只记录状态变更日志。

订阅 topic: `EdgeVib/+/+/+/data/sensor` (QoS 1)，解析 JSON 中 `ai_class_id`、`cascade_source`、`overall_rms` 字段做触发判断。

##### ONNX Runtime 后端 (决策 8)

**CPU (OpenBLAS)**，Docker 镜像保持 `python:3.11-slim` + `libopenblas0`。

选择理由：
- Autoencoder 输入 24 维，推理只需几次矩阵乘法，CPU 微秒级完成
- Allwinner VIP NPU 驱动在 Ubuntu 22.04 上可用性未知，ONNX Runtime 无官方支持
- NPU 留到 Phase 2 大模型（RUL、GGUF）时再评估

##### 统计趋势指标 (决策 9)

对每个设备，维护过去 30 点（≈1 分钟 @ 2s 间隔）滑动窗口，每 10s 更新：

| 指标 | 计算方法 | 预警阈值 | 说明 |
|------|----------|----------|------|
| RMS 趋势斜率 | overall_rms 线性回归斜率 | > 0.05 mm/s/点 (上升) | 振动持续恶化 |
| 峰值频率漂移 | peak_frequency 标准差 | > 5 Hz | 故障模式可能改变 |
| 温度-振动相关性 | RMS vs 温度 Pearson r | > 0.7 | 热膨胀导致的振动 |
| DE/NDE RMS Ratio | DE_rms / NDE_rms | 偏离 1.0 > 30% | 可能不对中 |
| 波峰因子趋势 | crest_factor 线性回归斜率 | > 0.02/点 (上升) | 轴承早期缺陷 |
| 频带能量迁移 | 高频段(200-400Hz)能量占比趋势 | > 5%/天 | 轴承磨损进展 |

##### 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 数据输入路径 | TimescaleDB 轮询 + MQTT 触发 |
| 2 | ONNX 转换位置 | Edge-AI PC 端预转换 |
| 3 | 特征来源 | ESP32 预提取特征 (via VIEWs) |
| 4 | MVP 范围 | 统计趋势 + Autoencoder 异常检测 |
| 5 | 多设备聚合 | 分层: 设备→电机→车间 |
| 6 | 结果输出 | TimescaleDB 全量 + MQTT 关键发现 |
| 7 | MQTT 触发 | AI 异常 + RMS 超阈值 |
| 8 | ONNX 后端 | CPU (OpenBLAS) |
| 9 | 统计指标 | 6 项指标, 30 点滑动窗口 |

##### 下一步实现

```
services/inference-engine/
├── Dockerfile                        — 已有骨架, 需添加 onnxruntime
├── requirements.txt                  — numpy, scipy, onnxruntime, paho-mqtt, psycopg2, pyyaml
├── src/
│   ├── __init__.py
│   ├── __main__.py                   — 入口: 加载配置 → 初始化 → 主循环
│   ├── config.py                     — YAML 配置加载
│   ├── db/
│   │   ├── __init__.py
│   │   ├── client.py                 — TimescaleDB 连接池 + 查询(VIEWs) + 写入(inference_results)
│   │   └── schema.sql                — inference_results 表 DDL
│   ├── mqtt/
│   │   ├── __init__.py
│   │   ├── subscriber.py             — MQTT 订阅 + 触发判断 (AI异常/RMS超阈值)
│   │   └── publisher.py              — MQTT 发布推理报告
│   ├── inference/
│   │   ├── __init__.py
│   │   ├── autoencoder.py            — ONNX Runtime 加载 + 推理 + 重构误差
│   │   ├── trend_analysis.py         — 统计趋势 (RMS斜率/频漂/波峰因子/频带迁移)
│   │   └── aggregation.py            — 设备级→电机级→车间级聚合
│   ├── health.py                     — 自身健康上报 (MQTT)
│   └── models/                       — ONNX 模型存放目录 (gitignored, 部署时拷贝)
├── config/
│   └── inference.yaml                — 已存在, 需更新
└── tests/
    ├── test_autoencoder.py
    ├── test_trend_analysis.py
    └── test_aggregation.py
```

同时 Edge-AI PC 端 (`edge-ai/deployment/`) 需新增 ONNX 转换脚本。

### 应用层
- **Grafana**: 实时振动频谱 + 设备健康仪表盘，预置 JSON dashboard。支持 HDMI 直连显示器做工厂现场看板 (Kiosk 全屏模式)
- **OTA Server** (Go): 固件分发 (ESP32 + STM32)。大文件走 Ethernet 第三通道
- **Web API** (Go/FastAPI): REST + WebSocket 对外接口
- **OPC UA Server** (open62541, C, 可选增强): 将 TimescaleDB 聚合数据暴露为标准 OPC UA 节点模型，SCADA/组态软件直连

### 增强服务 (第二阶段)
- **audio-monitor** (Python + PyAudio + FFT): USB麦克风或3.5mm音频接口采集环境声音 → FFT频谱 → 异常声纹识别。工业声学监测: 轴承磨损时高频分量增加、转子碰磨时的周期性冲击特征
- **ntp-server** (chrony): 局域网时间同步基准。所有设备 (ESP32/STM32/PC) 时间对齐，保证 TimescaleDB 时序数据时间戳一致性
- **Prometheus + Node Exporter**: Orange Pi 宿主机指标采集 (CPU/内存/磁盘/网络)，可接入 Grafana 统一展示

### 数据上行
- **MQTT Bridge**: 训练数据转发到 Edge-AI PC 端 Kafka

## 数据流路径

```
路径 A (主通道):
  F407(UART4) → ESP32-S3 → WiFi/MQTT → Orange Pi (Mosquitto)
  承载: 振动数据、DE特征、NDE特征、温度、电流/电压

路径 B (备份通道):
  F407(UART5/RS232) → TPT3232E → DB9 → USB-RS232 → Orange Pi (rs232-gateway)
  承载: OTA固件包、关键告警、ESP32离线时保底数据
  切换策略: F407主动监测ESP32心跳, 丢失>3s切换UART5, 恢复后切回

路径 C (视觉):
  USB Camera → Orange Pi (vision-service, V4L2)
  承载: 定时拍照存档、异常帧检测

路径 D (第三备份, 可选):
  F407(Ethernet/LAN8720) → RJ45 → 千兆交换机 → Orange Pi (lwIP TCP)
  承载: 大文件OTA固件 (>1MB) 高速传输
  优先级: UART4(WiFi主) > UART5(RS232备) > Ethernet(TCP第三)
```

## AI 模型分层

| 层级 | 设备 | 模型 | 用途 |
|------|------|------|------|
| 实时推理 | ESP32-S3 | CNN-LSTM (TFLite, <200KB) | 振动故障4分类 |
| 批量推理 | Orange Pi | ONNX 趋势预测 + RUL | 多设备聚合健康评估 |
| 语义分析 | Orange Pi | llama.cpp (GGUF) | 故障报告自然语言生成 |
| 视觉检测 | Orange Pi | OpenCV | 可见光异常检测 |

ONNX 模型用 Git LFS 管理 (<50MB), GGUF 大模型放 PC 端存储, 部署时拉取。

## OPC UA 集成 (可选增强)

- **库**: open62541 (C, LGPL)
- **角色**: OPC UA Server 运行在 Orange Pi 上
- **功能**: TimescaleDB 聚合数据 → 标准 OPC UA 节点模型暴露
- **节点示例**: `Motor01.XAxis.RMS`, `Motor01.Current.Amps`, `Motor01.Temperature.Celsius`
- **价值**: 任何 SCADA/组态软件可直接连接，工业互操作

## 现场总线扩展

- **RS485/Modbus RTU**: F407 USART3 复用为 RS485 方向控制, Orange Pi 通过 USB-RS485 接入。OPC UA Server 做 Modbus ↔ OPC UA 协议网关
- **CAN**: 已用于 F407 ↔ F103 NDE 节点 (ADXL345 振动采集)。Orange Pi 侧暂不额外接入 CAN
- **Ethernet**: F407 LAN8720 PHY + lwIP TCP, 预留直连 Orange Pi 的能力

## 开发工作流

```
PC (Claude Code + VS Code) → git push → Orange Pi git pull
                                    ↓
                              rsync scp (快速迭代)
                                    ↓
Orange Pi: make build → docker-compose up → systemd 守护
```

## 关键脚本

| 脚本 | 用途 |
|------|------|
| `edge-gateway/scripts/proxy-start.sh` | PC端启动代理链 (每次PC重启后) |
| `edge-gateway/scripts/setup-ubuntu.sh` | 系统初始化 (首次烧录镜像后) |
| `edge-gateway/scripts/deploy.sh` | 从PC部署代码到Orange Pi |
| `edge-gateway/scripts/setup-win-nat.ps1` | Windows NAT配置 (备用方案) |

## 待完成事项

- [x] **WiFi AP**: 已配置 hostapd + NetworkManager + dnsmasq，SSID: EdgeVib-AP, 子网 192.168.2.0/24
- [x] **data-aggregator**: Go 源码已完成，Docker 部署运行中 (2026-05-23)
- [x] **DNS 修复**: Python UDP→TCP relay (dns-relay.service) + SSH 隧道转发
- [ ] **时钟同步**: 配置 chrony NTP server，通过代理同步外网时间，或手动定时同步
- [ ] **Grafana dashboard**: 导入预置振动频谱仪表盘 JSON
- [ ] **rs232-gateway**: C 源码开发 (当前为骨架)
- [ ] **ESP32 固件更新**: WiFi SSID→EdgeVib-AP, 密码→edgevib2024, MQTT broker→192.168.2.1:1883
- [ ] **/etc/resolv.conf 持久化**: 重启后确保 `nameserver 127.0.0.1` 不被覆盖

## 运维操作 (每次 PC 重启后)

### 启动 SSH 隧道 (PC 端)
```bash
bash D:\smartSystem\edge-gateway\scripts\proxy-start.sh
```
该脚本建立两条 SSH 反向隧道:
- Orange Pi:18888 → PC:7897 (HTTP 代理，走系统代理上网)
- Orange Pi:5353 → 8.8.8.8:53 (DNS TCP 中继)

### 确认 DNS 中继运行 (Orange Pi 端)
```bash
ssh root@192.168.1.1 "systemctl start dns-relay && nslookup github.com 127.0.0.1"
```

### 确认 data-aggregator 运行
```bash
ssh root@192.168.1.1 "docker ps | grep aggregator && docker logs edgevib-aggregator --since 1m"
```

## 优先级规划

| 阶段 | 内容 |
|------|------|
| 第一阶段 (MVP) | Mosquitto + data-aggregator + TimescaleDB + Grafana + rs232-gateway |
| 第二阶段 (AI) | ONNX inference-engine + llm-analyzer + vision-service |
| 第三阶段 (高可用) | OTA Server + edge-router + OPC UA Server |

## 第二阶段仪表盘 (Dashboard Phase 2)

### 技术方案

**Grafana 纯前端 + 后续 API Server**（分步推进）:
- **第一步**: 纯 Grafana 仪表盘 — 直接查询 TimescaleDB（PostgreSQL JSONB 操作符），零新代码，Grafana 原生支持 HDMI Kiosk 工厂现场投屏
- **第二步**: API Server（Go REST + WebSocket）并行开发，提供实时数据推送能力。后续 Grafana 可切到 API 数据源，API 也可复用于 SCADA/组态软件等外部系统

### JSONB 字段提取

**PostgreSQL VIEW 策略**: 建 `vibration_view`、`ai_diagnosis_view`、`device_health_view` 等数据库视图，将 `sensor_data.payload` JSONB 解构成平表列。

- data-aggregator 保持现有设计——payload 以 `json.RawMessage` 全透传进 JSONB 列，不做解构（不耦合 ESP32 JSON schema）
- VIEW 层在数据库侧独立维护，可自由迭代，不碰 Go 代码
- Grafana 面板查询只需 `SELECT * FROM vibration_view`，SQL 简洁

### 仪表盘页面划分

| 页面 | 内容 | 数据源 |
|------|------|--------|
| **设备概览** | 全部设备状态卡片（在线/离线/告警），当前 RMS 值、AI 分类结果 | `sensor_data` JSONB |
| **振动详情** | 单设备 RMS 趋势图（X/Y/Z/Overall）、FFT 频谱图、峰值频率趋势 | `vibration_view` |
| **AI 故障诊断** | AI 分类历史、置信度趋势、级联来源统计（primary_cnn/fallback_rule） | `ai_diagnosis_view` |
| **双通道对比** | DE/NDE RMS ratio 趋势、频谱相似度、相位一致性 | `dual_channel_view` |
| **系统健康** | aggregator 健康指标（消息吞吐、去重率、DB 错误）、ESP32 在线状态 | `EdgeVib/+/status/health` MQTT 或 `sensor_data` 中 `service_state` |
| **环境监测** | 温度/湿度趋势，温度补偿状态 | `environment_view` |

设备概览为默认首页（HDMI 大屏 Kiosk 全屏展示），振动详情/AI 诊断/双通道对比为设备级钻取页。

### 实时数据方案

- **Grafana**: 5s auto-refresh，与 data-aggregator 批次写入间隔对齐
- **端到端延迟**: ESP32(2s) → aggregator攒批(≤5s) → Grafana拉取(5s) = 最坏12s，实际约7s
- **WebSocket 真实时**: 留待第二步 api-server 实现，主要用于告警即时推送和外部系统集成。当前工业振动监测 5s 级别足够

### 告警规则 (Grafana Alerting)

**等级体系**（基于 ISO 10816 标准，与 `config/inference.yaml` 阈值对齐）:

| 等级 | 触发条件 | 颜色 | 动作 |
|------|----------|------|------|
| **正常** | RMS < 2.8, AI=normal, confidence ≥ 0.85 | 绿 | 无 |
| **预警** | RMS 2.8~7.1, 或 AI=imbalance/misalignment 且 confidence ≥ 0.85, 或温度 > 45°C | 黄 | Grafana 面板高亮 |
| **严重** | RMS > 7.1, 或 AI=bearing_fault 且 confidence ≥ 0.85, 或 RMS ratio > 2.0 | 红 | Grafana 面板高亮 + 告警通知 |

**实现方式**: 纯 Grafana Alerting — 告警规则写在面板 SQL 的阈值列中，利用 Grafana 内置 alerting 引擎。不需新服务，后期可扩展 MQTT 通知联动 ESP32 声光报警。

### PostgreSQL VIEW 定义

存储在 `edge-gateway/docker/timescaledb/views.sql`，独立于 `init.sql`，手动导入。

| 视图 | 用途 | 提取字段 |
|------|------|----------|
| `vibration_view` | 振动详情页 | `time, site_id, device_type, device_id, rms_x, rms_y, rms_z, overall_rms, peak_frequency_hz, peak_amplitude_g, fft_peaks(JSONB)` |
| `ai_diagnosis_view` | AI 故障诊断页 | `time, site_id, device_type, device_id, ai_class_id, ai_class_name, ai_confidence, ai_cascade_source, ai_inference_time_us` |
| `dual_channel_view` | 双通道对比页 | `time, site_id, device_type, device_id, rms_ratio, spectral_similarity, phase_coherence, nde_online, nde_errors` |
| `environment_view` | 环境监测页 | `time, site_id, device_type, device_id, temperature_c, humidity_rh, compensation_active` |
| `device_status_view` | 设备概览页 | `time, site_id, device_type, device_id, service_state, data_quality, last_rms, last_temperature, last_ai_class, last_ai_confidence`（窗口函数取每个 device 最新一条） |

VIEW 层在数据库侧独立维护，aggregator 代码零改动。Grafana 面板 SQL 直接 `SELECT * FROM vibration_view WHERE ...`

### API Server 与 Dashboard 的职责边界

- **Grafana 仪表盘**: 历史数据查询 + 趋势展示 + 告警面板高亮 + HDMI Kiosk 投屏
- **API Server (后续)**: WebSocket 真实时推送 + REST API 供外部系统 (SCADA/组态软件) 消费 + 告警通知 MQTT 下发

第一步纯 Grafana 交付，api-server 骨架保持不变，第二步再并行开发。

### 待办事项 (Dashboard Phase 2)

- [x] 编写 `docker/timescaledb/views.sql`（5 个 PostgreSQL VIEW）
- [x] 编写 `docker/grafana/dashboards/device-overview.json`（设备概览）
- [x] 编写 `docker/grafana/dashboards/vibration-detail.json`（振动详情）
- [x] 编写 `docker/grafana/dashboards/ai-diagnosis.json`（AI 故障诊断）
- [x] 编写 `docker/grafana/dashboards/dual-channel.json`（双通道对比）
- [x] 编写 `docker/grafana/dashboards/system-health.json`（系统健康）
- [x] 编写 `docker/grafana/dashboards/environment.json`（环境监测）
- [x] Grafana 告警规则配置（ISO 10816 阈值 — 面板 fieldConfig.thresholds 色阶实现）
- [x] 更新 Grafana dashboard provisioning 配置（`dashboards.yml` + datasource `uid: edgevib-ts`）
- [x] 编写集成测试（`tests/integration/` — pytest + psycopg2 + Grafana API）
- [ ] 部署到 Orange Pi 4 Pro 验证

### 实现概要 (2026-05-24)

**已创建文件**:
| 文件 | 用途 |
|------|------|
| `docker/timescaledb/views.sql` | 5 个 PostgreSQL VIEW — `vibration_view`, `ai_diagnosis_view`, `dual_channel_view`, `environment_view`, `device_status_view` |
| `docker/grafana/dashboards.yml` | Dashboard provider provisioning 配置 |
| `docker/grafana/dashboards/device-overview.json` | 设备概览（首页/Kiosk），4 Stat + Device Table + Message Throughput |
| `docker/grafana/dashboards/vibration-detail.json` | 振动详情，RMS X/Y/Z/Overall 趋势 + FFT 频谱 + 峰值趋势 |
| `docker/grafana/dashboards/ai-diagnosis.json` | AI 故障诊断，置信度趋势 + 分类分布饼图 + 级联来源统计 |
| `docker/grafana/dashboards/dual-channel.json` | DE/NDE 双通道对比，RMS Ratio + Spectral Similarity + Phase Coherence |
| `docker/grafana/dashboards/system-health.json` | 系统健康，设备在线状态表 + 消息吞吐 + Data Quality Gauge |
| `docker/grafana/dashboards/environment.json` | 环境监测，温湿度趋势 + 温度补偿状态 |
| `tests/integration/conftest.py` | 测试 fixtures — DB 连接、Grafana 会话、`make_payload()` 数据生成器 |
| `tests/integration/test_dashboard_views.py` | 23 个集成测试 — VIEW 提取正确性、NULL 处理、阈值边界、Grafana API |
| `tests/integration/insert_test_data.sql` | 独立 SQL 测试数据 — 3 设备 × 多时间点，含一条 CRITICAL 告警记录 |
| `tests/integration/requirements-test.txt` | pytest, psycopg2-binary, requests |

**已修改文件**:
| 文件 | 修改 |
|------|------|
| `docker/grafana/datasources.yml` | 添加 `uid: edgevib-ts` 稳定引用 |
| `CONTEXT.md` | 更新待办事项为已完成 |

**部署步骤** (Orange Pi 4 Pro):
```bash
# 1. 同步文件到 Orange Pi
rsync -avz --exclude='.git/' /d/smartSystem/edge-gateway/ orangepi@192.168.1.1:/opt/edge-gateway/

# 2. 导入 VIEWs
ssh orangepi@192.168.1.1 "docker exec -i edgevib-timescaledb psql -U edgevib -d edgevib_ts" < docker/timescaledb/views.sql

# 3. 重启 Grafana 加载 dashboard
ssh orangepi@192.168.1.1 "cd /opt/edge-gateway && docker compose -f docker/docker-compose.yml restart grafana"

# 4. 插入测试数据
ssh orangepi@192.168.1.1 "docker exec -i edgevib-timescaledb psql -U edgevib -d edgevib_ts" < tests/integration/insert_test_data.sql

# 5. 运行集成测试
ssh orangepi@192.168.1.1 "cd /opt/edge-gateway && pip3 install -r tests/integration/requirements-test.txt"
ssh orangepi@192.168.1.1 "cd /opt/edge-gateway/tests/integration && TEST_DB_HOST=localhost python3 -m pytest test_dashboard_views.py -v"
```

## rs232-gateway 设计详情 (2026-05-24)

### 硬件链路

```
F407(UART5: PC12 TX / PD2 RX, 115200 8N1)
    → TPT3232E (TTL→RS232 电平转换)
    → DB9 母头
    → USB-RS232 转换器 (CH340/FT232)
    → Orange Pi 4 Pro (/dev/ttyUSB0)
```

### 软件架构决策

| 决策项 | 选择 | 原因 |
|--------|------|------|
| F407 端 UART 口 | **UART5** (PC12/PD2) | 已有 NVIC 中断使能; 不与 I2C2/SPI2 复用冲突 |
| 备份通道承载数据 | **全量帧** (CMD 0x04/0x06/0x07/0x10/0x17/0x18) | 115200bps 带宽充足; F407 端不做过滤降低复杂度; 下游按 topic 区分主/备来源自行去重 |

| 协议解析方案 | **移植 ESP32 protocol.c 核心解析器** (10 状态机 + CRC16-MODBUS) | 与现网验证逻辑 100% 一致; 剥离 FreeRTOS 依赖 (Mutex/Semaphore → pthread); 保留帧 dump 调试 |

| MQTT 客户端库 | **libmosquitto** (Mosquitto 自带 C 库) | 已在 Orange Pi 可用; publish-only 场景功能足够; 自带自动重连; ~100KB 零依赖 |
| MQTT topic 策略 | **相同 topic + JSON 内加 `"source": "rs232"`** (ESP32 侧对称加 `"source": "esp32"`) | data-aggregator JSONB 透传自动保留; TimescaleDB 可溯源; Grafana 无需改动 |
| F407 主/备切换 | **UART4 任一台法帧=存活; 3s 无帧→切 UART5; 5s 连续有帧→切回; 静默时不发** | 滞回 5s>3s 防乒乓; 接受切换窗口内短暂丢帧 |
| 错误恢复 | **进程永不因可恢复错误退出; open 失败每 2s 重试; read ENODEV 则 close→重试; 无数据不报错** | 让 systemd 只处理致命崩溃; libmosquitto 自带重连 |
| systemd unit | **Type=simple, Restart=always, RestartSec=5s, User=orangepi, Group=dialout** | 前台运行; 自动拉起; dialout 组访问串口; 日志走 journald |
| 项目结构 + 构建 | **6 源文件 + Makefile + libmosquitto + libyaml** (见下方源码树) | protocol.c 零依赖可单测; 配置解析与 aggregator 一致用 YAML |
| 配置解析 | **libyaml** (libyaml-dev) | 与 data-aggregator YAML 惯例一致; config/rs232-gateway.yaml 已存在 |

### 源码树

```
services/rs232-gateway/
├── Makefile                          — gcc -Wall -Wextra -O2 + -lmosquitto -lyaml
├── src/
│   ├── main.c                        — 入口 + 信号处理 + 主循环 (~150 行)
│   ├── serial.c / serial.h           — POSIX 串口 open/read/close (2s 重试) (~100 行)
│   ├── protocol.c / protocol.h       — CRC16 + 10 状态机 (ESP32 protocol.c 移植) (~400 行)
│   ├── mqtt_pub.c / mqtt_pub.h       — libmosquitto 薄封装 (~80 行)
│   └── proto_to_json.c / proto_to_json.h — 协议帧→JSON + MQTT topic 路由 (~150 行)
├── systemd/
│   └── rs232-gateway.service
└── tests/
    └── test_protocol.c               — CRC16 + 状态机单元测试
```

### F407 端待实现

- UART5 发送函数 (`HAL_UART_Transmit(&huart5, ...)`)
- ESP32 存活检测 (UART4 任一 CRC 合法帧 = alive)
- 3s/5s 滞回切换状态机
- UART5 备份模式下发送全量协议帧
- 系统状态帧 (CMD 0x07) 中增加 `backup_active` 标志位

### 待决策

- 错误恢复: USB-RS232 拔出/松动后如何自动恢复
- systemd unit 设计: 重启策略、日志轮转、依赖关系
- Orange Pi 端项目结构: 独立 Makefile + 源文件组织

## OPC UA Server — 工业标准 SCADA 互操作

### 决策 1: 实现语言与库 (2026-05-26)

**选择**: open62541 (C)

**Why**: Orange Pi 4 Pro 内存 4GB，已用 ~3.4GB，headroom 仅 600MB。open62541 编译后 ~500KB，运行时内存 <10MB（含地址空间节点），是所有选项中最低的。open62541 由 Fraunhofer IOSB 主导维护，全 OPC UA 特性支持（DA/HA/AE/订阅/Method），工业验证最充分。C 代码可复用 rs232-gateway 已验证的 Makefile + systemd 部署模式。

**Alternatives considered**:
- Go + gopcua: 开发效率更高且 pgx DB 访问已验证，但 Go 运行时 + 依赖 ~15-20MB，是 C 方案的 2-4 倍
- Python + opcua-asyncio: ~50-80MB，在 600MB headroom 约束下不可接受；py-opcua 社区活跃度下降

### 决策 2: 数据获取策略 (2026-05-26)

**选择**: 纯 TimescaleDB 定时轮询 (1s 间隔)

**Why**: OPC UA 客户端（SCADA/组态软件）自身以 1-5 秒周期轮询 Server，1s 轮询完全匹配 SCADA 的典型刷新率。单数据源（TimescaleDB）避免 MQTT 通道引入的数据一致性问题。主循环模型简单：`while(1) { poll_db(); update_nodes(); sleep(1s); }`，与 rs232-gateway 架构一致。OPC UA 协议层面的 Monitored Items 订阅机制（SamplingInterval + 变化检测）在 Server 内存中完成，不依赖外部 MQTT broker。

**Alternatives considered**:
- MQTT 订阅实时数据: <100ms 延迟优势被 SCADA 1-5s 轮询频率抹平；引入 libmosquitto 依赖增加 ~5MB 内存
- DB + MQTT 混合: 双通道复杂度高，当前 2-10 设备规模不需要事件驱动优化。未来设备数增长到 50+ 时可再加 MQTT 通道，当前简单方案足够

### 决策 3: 地址空间模型 (2026-05-26)

**选择**: 分层树状结构，与 MQTT topic / REST URL 路径对齐

```
Root/Objects/EdgeVib/{site_id}/{device_type}/{device_id}/
  ├── Vibration/     (FolderType)
  │   ├── RMS_X         (AnalogItemType, Double, mm/s)
  │   ├── RMS_Y         (AnalogItemType, Double, mm/s)
  │   ├── RMS_Z         (AnalogItemType, Double, mm/s)
  │   ├── Overall_RMS   (AnalogItemType, Double, mm/s)
  │   └── PeakFrequency (AnalogItemType, Double, Hz)
  ├── AI_Diagnosis/  (FolderType)
  │   ├── ClassName     (String)
  │   ├── Confidence    (Double, 0-1)
  │   └── Source        (String, "primary_cnn"|"fallback_rule")
  ├── MotorData/     (FolderType, 仅 motor type)
  │   ├── Voltage       (AnalogItemType, Double, V)
  │   ├── Current       (AnalogItemType, Double, A)
  │   └── Power         (AnalogItemType, Double, W)
  └── Status/        (FolderType)
      ├── Online        (Boolean)
      └── HealthScore   (Double, 0-100)
```

**Why**: 三层对齐（MQTT topic / REST URL / OPC UA NodeId）使任何熟悉其中一个接口的人能自动理解另外两个。AnalogItemType 带 EURange + EngineeringUnits（mm/s, °C, Hz），SCADA 组态软件可自动绑定仪表盘/趋势图组件。只暴露操作员关心的聚合指标，24 维原始特征向量留给 ML 模型。AccessLevel 设为 CurrentRead 只读，设备控制走 ESP32→F407 链路。地址空间在启动时从 `device_status_view` 动态构建，不硬编码设备列表。

### 决策 4: 数据库访问与节点值映射 (2026-05-26)

**选择**: libpq 直连 + 单持久连接 + 批量查询 + 静态数组映射表

**Why**:
- **libpq**: PostgreSQL 官方 C 库，rs232-gateway 的依赖链中已存在（libmosquitto 动态链接 libpq），无需新增依赖。内存 ~1MB
- **单连接**: 单线程 1s 轮询，无并发需求。连接池在 C 中的复杂度不值得收益
- **批量查询**: `SELECT * FROM device_status_view` 一次返回所有设备最新值，VIEW 已解构成平表列（`rms_x`, `health_score`, ...），libpq 的 `PQgetvalue()` 直接读列值，零 JSON 解析
- **映射表**: 启动时为每个 OPC UA 变量节点建立 `NodeId → (device_key, column_name)` 映射，存储在静态数组 `struct node_mapping[MAX_VARIABLES]` 中。1s 轮询时遍历 SQL 结果行 → 查映射表 → `UA_Server_writeValue()` 更新。无 malloc/free

**DB 查询延迟**: `device_status_view` 是窗口函数取最新一条，2-10 设备场景下 <5ms。1s 轮询间隔对 TimescaleDB 几乎无负载。

### 决策 5: 部署模式 (2026-05-26)

**选择**: systemd 直接部署（非 Docker）

**Why**: 用户为省 10-15MB 选了 C over Go，Docker 的 20MB 容器开销同理。open62541 自带 TCP Server，`systemctl start opcua-server` 即可。SCADA 客户端局域网直连 `192.168.1.1:4840`，无需 Docker 端口映射。与 rs232-gateway（C + Makefile + systemd）部署模式一致。OPC UA 协议自带加密（SecurityPolicy），不依赖 Docker TLS。

**Alternatives considered**:
- Docker: 与 api-server/inference-engine 统一编排，但 +20MB 容器开销在 600MB headroom 约束下不合理

### 决策 6: 安全策略 (2026-05-26)

**选择**: Anonymous + SecurityPolicy=None (MVP)，预留 Username/Password 配置开关

**Why**: 与现有安全模型一致（Mosquitto 匿名、api-server 无认证）。192.168.1.0/24 扁平 LAN 是信任域，攻击者如已接入物理网络可直接连 Mosquitto/TimescaleDB/Grafana，OPC UA 单点加认证不改变威胁面。open62541 `UA_ServerConfig_setMinimal()` 一行配置。配置文件 `security.anonymous: true/false` 预留开关，将来切 UserPass 只需加 AccessControl 回调，diff <20 行。端口转发暴露到公网时再启用 Username/Password + Sign。

### 决策 7: 错误处理与容错 (2026-05-26)

**选择**: rs232-gateway 模式 + OPC UA 节点质量标记

**Why**:
- **进程永不因可恢复错误退出**: DB 断连→每 2s 重试（`PQreset()`），节点标记 `UA_STATUSCODE_BADWAITINGFORINITIALDATA`，重连成功→恢复 `GOOD`。查询失败→标记 `UNCERTAINLASTUSABLEVALUE`，保留上次有效值。这些都不退出
- **systemd 只处理致命崩溃**: `Restart=always, RestartSec=5s`，SIGSEGV/OOM 时自动拉起
- **OPC UA 协议优势**: 每个变量节点有 Status 字段，SCADA 读到 Bad 自动显示"通信故障"。比 MQTT/REST 的"无响应即挂了"更优雅
- **无数据不报错**: device_status_view 返回空行表明没有在线设备，是正常状态

### 决策 8: 项目结构与构建 (2026-05-26)

**选择**: 8 源文件 + Makefile + systemd，对标 rs232-gateway 模式

```
services/opcua-server/
├── Makefile
├── src/
│   ├── main.c / main.h                     — 入口 + 信号 + 1s 主循环
│   ├── opcua_server.c / opcua_server.h     — open62541 初始化 + 地址空间构建 + 节点更新
│   ├── db_client.c / db_client.h           — libpq 连接 + 查询 + 结果解析
│   ├── node_mapping.c / node_mapping.h     — NodeId ↔ DB column 映射表
│   └── config.c / config.h                 — libyaml 配置加载
├── systemd/
│   └── opcua-server.service
└── tests/
    ├── test_opcua_server.c
    └── test_db_client.c
```

**Why**: 与 rs232-gateway 结构平行（6→8 源文件，多了 DB 和地址空间模块，少了 MQTT 和 JSON 转换）。依赖 `libopen62541-dev` + `libpq-dev` + `libyaml-dev`，后二者已在 Orange Pi 上存在。`make -j4` 编译到单二进制，systemd 部署。

### 决策 9: 配置文件 (2026-05-26)

**选择**: 4-section YAML (`server` / `timescaledb` / `security` / `logging`)

```yaml
server:
  endpoint: "opc.tcp://0.0.0.0:4840"
  app_name: "EdgeVib OPC UA Server"
  app_uri:  "urn:edgevib:opcua-server"
  site_id:  "factory1"

timescaledb:
  host: "localhost", port: 5432
  dbname: "edgevib_ts", user: "edgevib", password: "edgevib123"
  poll_interval_ms: 1000

security:
  anonymous: true

logging:
  level: "info"
```

**Why**: 与 api-server 5-section YAML 惯例对齐。`endpoint: 0.0.0.0:4840` 绑定所有接口（eth0 + wlan0 AP），SCADA 从有线和 WiFi 侧都能连。`poll_interval_ms` 可配，现场调试无需重新编译。不设 product_uri/manufacturer_name（非商用发布）。

### 决策 10: 测试策略 (2026-05-26)

**选择**: 双层测试 — C 单元测试 + Python 集成测试

**Why**: 对标 rs232-gateway 测试水准（~1200 行）。C 单元测试覆盖 node_mapping / config / db_client / opcua_server 四个模块，`make test` 一键运行。Python 集成测试用 `opcua-asyncio` 客户端连接真实 Server，验证地址空间结构 + 节点值与 TimescaleDB 数据一致性 + Bad 状态行为。测试数据复用 `tests/integration/insert_test_data.sql`（3 设备 × 多时间点）。

### 决策 11: 功能范围 (2026-05-26)

**选择**: DA-only（纯数据访问），排除 HA/A&C/Methods

**Why**: 历史数据查询 → TimescaleDB + Grafana + api-server REST 已覆盖。告警状态机 → inference-engine + Grafana Alerting + F407 声光报警已覆盖。设备控制 → ESP32→F407 链路，OPC UA 加写入口会引入安全隐患。All nodes AccessLevel = CurrentRead。在 Server app_description 中明确声明 DA-only，避免 SCADA 集成方期望错误。
