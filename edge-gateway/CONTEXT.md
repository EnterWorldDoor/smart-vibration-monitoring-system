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
| 语义分析 | Orange Pi | llama.cpp (GGUF) + llama-cpp-python | 故障报告自然语言生成 |
| 视觉检测 | Orange Pi | OpenCV | 可见光异常检测 |

ONNX 模型用 Git LFS 管理 (<50MB), GGUF 大模型放 PC 端存储, 部署时拉取。

## llm-analyzer — 本地 LLM 故障报告生成

### 决策 1: LLM 运行时引擎 (2026-05-26)

**选择**: llama.cpp + llama-cpp-python

**Why**: 与 inference-engine 的 ONNX Runtime 分离，不抢 ONNX session；GGUF 量化模型生态最大；llama-cpp-python 可在 asyncio `run_in_executor` 中调用，架构与 inference-engine 一致；Orange Pi 4GB 内存跑 1-2B 参数 Q4_K_M 量化模型可行。

**Alternatives considered**:
- Ollama: 多了 daemon 层，增加内存开销 (~200MB)，部署复杂度高于纯 Python 方案
- ONNX + SLM (Phi-3-mini): 模型生态不如 GGUF 丰富，中文支持弱

### 决策 2: 模型选型 (2026-05-26)

**选择**: Qwen2.5-1.5B-Instruct (Q4_K_M 量化 GGUF)

**Why**: 原生中文最优（阿里通义千问家族），面向中国工厂操作人员；Q4_K_M 量化后 ~1.0GB，Python 进程 + llama.cpp 上下文 ~500MB，合计 ~1.5GB；推理速度 5-8 token/s，200 字故障报告约 30 秒，工业非实时场景可接受。

**Alternatives considered**:
- Gemma-3-1B: 中文较弱，不适合中文故障报告
- DeepSeek-R1-Distill-Qwen-1.5B: 推理链开销大，token 消耗翻倍，延迟高

### 内存预算 (4GB LPDDR4x)

| 组件 | 预估占用 |
|------|---------|
| Ubuntu 22.04 + 系统服务 | ~400MB |
| Docker daemon | ~200MB |
| Mosquitto + TimescaleDB + Grafana | ~600MB |
| data-aggregator (Go) | ~50MB |
| rs232-gateway (C) | ~10MB |
| inference-engine (Python + ONNX) | ~300MB |
| **已占用小计** | **~1.5GB** |
| Qwen2.5-1.5B (Q4_K_M) 模型文件 | ~1.0GB |
| llm-analyzer Python + llama.cpp 上下文 | ~500MB |
| **llm-analyzer 合计** | **~1.5GB** |
| vision-service (预留) | ~200MB |
| audio-monitor (预留) | ~100MB |
| api-server (Go, 预留) | ~50MB |
| OPC UA Server (open62541 C, 预留) | ~30MB |
| **未来扩展预留小计** | **~380MB** |
| **总计** | **~3.4GB / 4GB** |
| **剩余 headroom** | **~600MB** |

headroom 600MB 用于应对 TimescaleDB 内存增长（高频查询缓存）、Docker 日志缓冲、以及操作系统页缓存。

### 决策 3: 输入数据源 & 触发机制 (2026-05-26)

**选择**: MQTT 事件触发 + TimescaleDB 直读 混合

**Why**:
- **MQTT 实时触发**: 订阅 `EdgeVib/+/inference/+/ai/report`，inference-engine 发布 WARNING/CRITICAL 时立即触发报告生成。事件驱动零延迟
- **TimescaleDB 上下文补充**: 收到触发后查询 `ai_reports` 完整行 + 近 10 分钟 `vibration_view` 数据做 LLM 上下文。避免 MQTT payload 过大，保持消息轻量
- **定时汇总**: 支持按 cron 表达式（如每班次 8h/12h）生成设备健康日报，与事件触发并存

**Alternatives considered**:
- 纯 MQTT: payload 中放完整上下文太重（>10KB），超出 MQTT 合理范围
- 纯 DB 轮询: 丢失事件驱动的即时性，且轮询间隔内可能堆积多条告警
- inference-engine import 调用: 强耦合，任一服务 OOM 都会影响对方

### 决策 4: 输出目标 (2026-05-26)

**选择**: TimescaleDB `llm_reports` 表持久化 + MQTT 实时推送

**Why**:
- TimescaleDB 持久化: Grafana 可新增 "AI 报告" 面板；审计可追溯；与 `ai_reports` 解耦，独立 schema
- MQTT 发布到 `EdgeVib/{site_id}/llm/{device_id}/report`: 实时推送，下游服务（api-server、通知系统）直接消费
- 与 inference-engine 输出模式一致（DB + MQTT 双通道）

### 决策 5: Prompt 模板设计 (2026-05-26)

**选择**: YAML 模板 + 2 场景 + 1-shot 示例

**Why**:
- **模板格式**: YAML 文件，每模板含 `system_prompt` + `user_template`（`{placeholder}` 变量）。与项目 `config/*.yaml` 惯例一致，非开发人员也能改
- **模板数量**: 2 个 — `alert_report`（事件触发，强调即时告警解读和处置建议）+ `daily_summary`（定时汇总，强调趋势分析和健康评分变化）
- **1-shot 示例**: 每个模板嵌 1 个示例报告。1.5B 小模型对 structured-data→text 任务，1-shot 能大幅提高输出结构一致性（标题/摘要/分析/建议 四段式），成本仅 +150 tokens
- **输出结构**: 统一四段式 — 标题/当前状态摘要/异常分析/维护建议，后期解析和 Grafana 展示友好

**Context window 策略**: Qwen2.5-1.5B 支持 32K token，本场景输入控制在 800-1200 token（数据上下文）+ 200 token（system prompt + 示例），输出 200-500 token，远低于上限

### 决策 6: 服务集成方式 (2026-05-26)

**选择**: Docker + volume mount (模型文件 bind mount 进容器)

**Why**: 与 inference-engine 部署一致，docker-compose 统一编排；Python 依赖隔离（llama-cpp-python 需编译 C++ 扩展）；模型放 `/opt/edge-gateway/models/` 不污染镜像；可用 Docker `--memory` 限制内存防 OOM。

### 决策 7: 模型加载策略 (2026-05-26)

**选择**: 始终加载 (always-loaded)

**Why**: 内存预算已预留 1GB；故障报告是核心价值，10-30s 懒加载延迟影响用户体验；llama.cpp 空闲时 CPU 用量为零（纯内存驻留）；工业可靠性 > 省内存。

### 决策 8: 告警去重 & 频率控制 (2026-05-26)

**选择**: 去重窗口 + 升级穿透

**Why**:
- 同 (device_id, severity) 在 **5 分钟窗口内**只生成一篇报告，避免同一故障每 10s 重复触发
- severity **升级 (WARNING→CRITICAL)** 或**降级 (CRITICAL→WARNING)** 穿透去重，立即生成新报告——状态变化值得报告
- 去重状态用内存 dict 维护，进程重启清空（可接受，最多重复一篇）
- 与 inference-engine 的 MQTT 事件驱动衔接，抑制噪音

### llm-analyzer 架构概览

```
inference-engine (10s cycle)
    │
    ├── WARNING/CRITICAL → MQTT EdgeVib/+/inference/+/ai/report
    │                              ↓
    │                       llm-analyzer MQTT subscriber (trigger)
    │                              ↓
    │                       去重检查 (内存dict, 5min窗口, 升级穿透)
    │                              ↓
    │                       DB: 查询 ai_reports + vibration_view (上下文)
    │                              ↓
    │                       report_builder: 组装数据 → prompt
    │                              ↓
    │                       llama.cpp: Qwen2.5-1.5B 推理 (~30s)
    │                              ↓
    │                       DB: INSERT INTO llm_reports
    │                       MQTT: publish EdgeVib/{site}/llm/{device}/report
    │
    ├── 定时日报 (cron 8h/12h)
    │       ↓
    │   DB: 查询过去窗口内 ai_reports → prompt → LLM → llm_reports + MQTT
    │
    └── Health: 30s MQTT status/health (与 inference-engine 一致)
```

### 待实现清单

| 文件 | 用途 |
|------|------|
| `src/__main__.py` | asyncio 入口 + 主循环 |
| `src/config.py` | YAML 配置 dataclass |
| `src/db/client.py` | TimescaleDB: 读 ai_reports + vibration_view, 写 llm_reports |
| `src/llm/engine.py` | llama-cpp-python 封装: load/unload/generate |
| `src/llm/templates.py` | YAML prompt 模板加载 + 变量填充 |
| `src/mqtt/subscriber.py` | MQTT 触发订阅 + 去重逻辑 |
| `src/mqtt/publisher.py` | MQTT 报告发布 |
| `src/report_builder.py` | 数据上下文组装 → prompt → LLM 调用 → 报告解析 |
| `src/health.py` | 健康遥测 (推理计数、错误计数、延迟统计) |
| `prompts/alert_report.yaml` | 事件告警模板 |
| `prompts/daily_summary.yaml` | 定时日报模板 |
| `docker/timescaledb/init.sql` | 追加 `llm_reports` 建表 DDL |
| `docker/docker-compose.yml` | 追加 llm-analyzer 服务定义 |
| `Dockerfile` | Python 3.11 + llama-cpp-python + psycopg2 + paho-mqtt |
| `requirements.txt` | llama-cpp-python, psycopg2-binary, paho-mqtt, pyyaml, structlog |
| `tests/` | 单元测试: prompt 模板填充、去重逻辑、report_builder 数据组装 |

## vision-service — USB 摄像头视觉监测

### 决策 1: 硬件接口 (2026-05-27)

**选择**: USB UVC 摄像头 + OpenCV VideoCapture

**Why**: 用户手头已有 USB 2.0 摄像头，即插即用。Linux UVC 驱动成熟，OpenCV `cv2.VideoCapture(0)` 打开即用，零驱动适配。Orange Pi 4 Pro 虽有两个 MIPI CSI 接口，但全志 A733 的 CSI 驱动在 Ubuntu 22.04 上可用性未知，且用户无 MIPI 摄像头硬件。

**Alternatives considered**:
- MIPI CSI: 低 CPU 开销、高帧率、专用带宽，但需要驱动适配且用户无对应硬件

### 决策 2: 部署模式 (2026-05-27)

**选择**: systemd 直接部署（非 Docker）

**Why**: 与 rs232-gateway、opcua-server 相同的 IO 密集型轻量服务模式。vision-service 仅依赖 `opencv-python-headless + numpy`，在 Python venv 中 `pip install` 即可，无需 Docker 隔离复杂依赖链。Docker 的 20MB 容器开销 + 400MB 镜像在 Orange Pi 4GB 内存约束下不合理。

**全量内存预算（更新，含 api-server + llm-analyzer）**:

| 组件 | 运行时内存 | 部署方式 |
|------|-----------|---------|
| Ubuntu 22.04 系统 | ~600 MB | 裸机 |
| llm-analyzer (Qwen2.5-1.5B Q4_K_M) | ~1300 MB | Docker |
| TimescaleDB | ~400 MB | Docker |
| Grafana | ~180 MB | Docker |
| inference-engine (Python + ONNX) | ~180 MB | Docker |
| api-server (Go) | ~20 MB | Docker |
| data-aggregator (Go) | ~20 MB | Docker |
| Mosquitto | ~15 MB | Docker |
| rs232-gateway (C) | ~5 MB | systemd |
| opcua-server (C) | ~10 MB | systemd |
| Docker daemon 自身 | ~50 MB | — |
| **vision-service (Python + OpenCV)** | **~120 MB** | **systemd** |
| **已用合计** | **~2900 MB** | |
| **剩余 headroom** | **~1100 MB** | |

llm-analyzer 的 1.5B GGUF 模型（q4_k_m 量化，权重 ~0.84GB + KV Cache ~0.35GB + Python 运行时 ~0.1GB）是最大单一内存消费者（~1.3GB）。系统部署遵循简单分界线：**Docker = 复杂依赖隔离**（llama.cpp C++ 编译链、ONNX Runtime、PostgreSQL），**systemd = 简单 IO 服务**（串口读→MQTT 写、DB 读→OPC UA 暴露、摄像头→存图）。

**Alternatives considered**:
- Docker: 与 inference-engine/llm-analyzer 统一编排，但 +20MB 容器开销 + ~400MB Python-OpenCV 镜像，在 4GB 内存约束下不合理。USB `/dev/video0` 设备透传也需要额外 `--device` 配置

### 决策 3: 采集策略 — 混合模式 (2026-05-27)

**选择**: 低频基线拍照 + MQTT 事件触发拍照，Phase 1 不做运动检测

**Why**:
- **工业场景 ≠ 安防监控**：电机一直在转动，传统帧差分"运动检测"永远触发，无效。工业场景需要的是"画面异常检测"（histogram drift / SSIM），需要累积基线数据后才能做
- **Phase 1 (MVP) 只做采集**: 先把图像数据采回来积攒基线，Phase 2 再加画面异常分析管线。拆分降低 MVP 复杂度，不改动 Phase 1 架构即可平滑升级
- **双模式互补**: 低频缩略图做长期视觉回溯（"上周电机什么样子"），事件触发全分辨率做故障瞬间证据保全。存储可控，关键瞬间不丢画质
- **与振动域解耦**: inference-engine 负责振动异常检测，vision-service 负责视觉证据采集。通过 MQTT 事件驱动协作，视觉不参与异常判断（Phase 1）

**Phase 1 采集参数**:

| 参数 | 定时基线 | 事件触发 |
|------|---------|---------|
| 间隔 | 60s | inference-engine WARNING/CRITICAL |
| 分辨率 | 640×480 | 1920×1080 (全分辨率) |
| 保留期 | 7 天 (自动轮转) | 30 天 |
| 用途 | 长期视觉趋势回溯 | 故障瞬间高清证据 + 辅助 llm-analyzer 报告 |

**Phase 2 扩展（本迭代不做）**:
- 基线缩略图 → OpenCV 直方图对比 / SSIM 结构相似度 → 画面显著偏离→视觉告警
- 新增 `frame_analyzer.py` 消费基线图片流，不改动 Phase 1 采集管线

**Alternatives considered**:
- 纯定时拍照: 事件驱动零延迟优势丢失，告警发生后要等最长 60s 才拍
- 纯事件触发: 无基线数据，事后无法回溯故障前画面，Phase 2 画面异常检测无数据可用
- Phase 1 就做运动检测: 电机转动场景下帧差分无效，需先累积至少 1 周基线才能做异常检测

### 决策 4: 存储策略 — 文件系统主记录 + DB 元数据索引 + MQTT 通知 (2026-05-27)

**选择**: 三级存储 — 文件系统为主记录、TimescaleDB 为查询索引、MQTT 为实时通知

**文件系统结构**:
```
/opt/edge-gateway/data/vision/{site_id}/{device_id}/{YYYY-MM-DD}/{type}_{HHMMSS}.jpg

示例:
  data/vision/factory1/motor01/2026-05-27/baseline_143005.jpg
  data/vision/factory1/motor01/2026-05-27/event_143522_WARNING.jpg
```

按 site/device/date 三级目录，文件名自编码 type 和 timestamp。按日期分目录的优势：自动轮转只需 `rm -rf` 过期日期目录，一条命令。

**TimescaleDB 元数据表** (`vision_captures`):
```sql
CREATE TABLE vision_captures (
    time           TIMESTAMPTZ NOT NULL,
    site_id        TEXT NOT NULL,
    device_id      TEXT NOT NULL,
    capture_type   TEXT NOT NULL,   -- 'baseline' | 'event'
    trigger_src    TEXT,            -- 'timer' | 'mqtt_inference'
    resolution     TEXT,            -- '640x480' | '1920x1080'
    file_path      TEXT NOT NULL,
    file_size_bytes BIGINT
);
```

**Why**: 文件是主记录——DB 不可用时照样存图，只写 WARN 日志。DB 行是便利索引：Grafana 面板可列出最近截图、api-server REST 可查询过滤、Phase 2 画面分析可直接按时间范围批量查基线。

**MQTT 通知**: 每次拍照后发布轻量消息到 `EdgeVib/{site_id}/vision/{device_id}/capture`，内容仅含文件路径 + 元数据（不含图像数据）。api-server WebSocket hub 推送实时截图通知。

**自动轮转策略**:
- 定时基线 (640×480): 保留 7 天，每天清理过期日期目录
- 事件触发 (1920×1080): 保留 30 天
- 容量估算: 基线 ~50KB/张 × 1440张/天 ≈ 72MB/天 → 7天 ≈ 500MB；事件按每天 5 次 ≈ 1MB/天

### 决策 5: 数据管线集成 (2026-05-27)

待定

### 决策 5: 数据管线集成 — 车间级架构 (2026-05-27)

**选择**: vision-service 定位为车间级服务，支持多设备多摄像头配置，MQTT 事件驱动 + 定时双采集模式

**Why — Orange Pi 是车间网关不是单设备附属**:

系统已在代码层面确立了车间级架构：
- 单一 Mosquitto Broker 做车间消息总线，所有 ESP32 直连
- 单一 TimescaleDB 实例，通过 `site_id`/`device_id` 列区分设备
- Grafana 设备概览页展示全部设备
- inference-engine: 设备级推理 → 电机级聚合 → 车间级排序（决策 5）
- WiFi AP: Orange Pi 开热点 `EdgeVib-AP`，所有 ESP32 直连

F407+ESP32 是一对一绑定电机（数据采集），Orange Pi 4 Pro 是车间级边缘网关（数据汇聚 + AI + 可视化 + OPC UA）。vision-service 应继承这一模式——一台 Orange Pi 可接多台 USB 摄像头，每台对准一台电机。

**MQTT 集成（输入）**:
- 订阅 `EdgeVib/+/inference/+/ai/report` (QoS 1)
- 从 topic 解析 `site_id` 和 `device_id`，匹配配置文件中 `devices` 列表
- 事件 body 包含 WARNING/CRITICAL severity → 触发对应设备摄像头全分辨率拍照

**MQTT 集成（输出）**:
- 发布到 `EdgeVib/{site_id}/vision/{device_id}/capture` — 轻量通知（文件路径 + 元数据，不含图像）
- 后续 api-server WebSocket hub 或 Grafana 可消费此 topic 做实时截图展示

**TimescaleDB 集成**:
- 写入 `vision_captures` 元数据表（best effort，DB 不可用时文件系统主记录不受影响）
- 查询可选：读取 `device_status_view` 确认设备在线状态（Phase 2 优化）

**摄像头-设备映射配置**:

```yaml
# config/vision-service.yaml — 车间级多设备设计
devices:
  - site_id: "factory1"
    device_type: "motor"
    device_id: "motor01"
    camera_index: 0          # /dev/video0 对准 motor01
    label: "1号电机(驱动端)"

  # 未来扩展
  # - site_id: "factory1"
  #   device_type: "motor"
  #   device_id: "motor02"
  #   camera_index: 1        # /dev/video1 对准 motor02
  #   label: "2号电机(驱动端)"

capture:
  baseline_interval_s: 60
  baseline_resolution: "640x480"
  baseline_quality: 75
  baseline_retention_days: 7
  event_resolution: "1920x1080"
  event_quality: 90
  event_retention_days: 30
  storage_path: "/opt/edge-gateway/data/vision"

mqtt:
  broker: "localhost"
  port: 1883
  client_id: "edgevib-vision-service"
  subscribe_topic: "EdgeVib/+/inference/+/ai/report"
  publish_topic: "EdgeVib/{site_id}/vision/{device_id}/capture"

timescaledb:
  host: "localhost"
  port: 5432
  dbname: "edgevib_ts"
  user: "edgevib"
  password: "edgevib123"
```

**数据流**:

```
定时器 (60s)
    │
    └── for each device in config.devices:
          ├── cv2.VideoCapture(camera_index).read()
          ├── resize 640×480 → JPEG encode (quality 75)
          ├── fs: write /vision/{site_id}/{device_id}/{date}/baseline_{time}.jpg
          ├── DB: INSERT INTO vision_captures (best effort)
          └── MQTT: publish capture notification (best effort)

MQTT 事件 (inference-engine WARNING/CRITICAL)
    │
    ├── 解析 topic → site_id/device_id → 匹配 config.devices[device_id]
    ├── cv2.VideoCapture(camera_index).read()
    ├── 1920×1080 → JPEG encode (quality 90)
    ├── fs: write .../{device_id}/{date}/event_{time}_{severity}.jpg
    ├── DB: INSERT INTO vision_captures
    └── MQTT: publish capture notification
```

**错误处理**: 对标 rs232-gateway 的"永不因可恢复错误退出"原则：
- 摄像头采集失败（ENODEV）→ WARN 日志 + 跳过本轮 + 继续循环
- DB 写入失败 → WARN 日志 + 文件已存，数据不丢失
- MQTT 发布失败 → WARN 日志 + 不影响主循环
- 进程级致命错误 → systemd `Restart=always, RestartSec=5s` 自动拉起

### 决策 6: 项目结构与构建 (2026-05-27)

**选择**: 8 源文件 + Makefile + systemd，对标 inferenc-engine (Python) 和 rs232-gateway (systemd) 的混合模式

**源码树**:

```
services/vision-service/
├── Makefile                          — venv 创建 + pip install + 一键安装 systemd unit
├── requirements.txt                  — opencv-python-headless, numpy, psycopg2-binary, paho-mqtt, pyyaml
├── src/
│   ├── __init__.py
│   ├── __main__.py                   — 入口: 配置加载 → 初始化 → 主循环 (定时+事件双模式)
│   ├── config.py                     — YAML 配置 dataclass (对标 llm-analyzer src/config.py)
│   ├── camera/
│   │   ├── __init__.py
│   │   └── capture.py                — cv2.VideoCapture 封装 (open/read/release/reconnect)
│   ├── storage/
│   │   ├── __init__.py
│   │   └── file_store.py             — 目录创建 + JPEG 写入 + 自动轮转清理
│   ├── db/
│   │   ├── __init__.py
│   │   └── client.py                 — psycopg2 vision_captures 元数据写入 (best effort)
│   ├── mqtt/
│   │   ├── __init__.py
│   │   ├── subscriber.py             — MQTT 触发订阅 + topic 解析 + 设备匹配
│   │   └── publisher.py              — MQTT capture 通知发布
│   └── health.py                     — 健康上报 (30s MQTT, 对标 inference-engine)

config/
└── vision-service.yaml               — 车间级多设备配置 (与 inference.yaml 同目录)

systemd/
└── vision-service.service
```

**Why 8 源文件**: 对标 inference-engine (10 源文件) 和 rs232-gateway (6 源文件) 之间。每个模块职责单一：camera 只管硬件、storage 只管文件、db 只管元数据写入、mqtt 管订阅和发布。未来 Phase 2 加 `frame_analyzer.py` 不改动现有模块。

**构建与安装**:

```bash
make install  # python3 -m venv venv → pip install -r requirements.txt → sudo cp systemd/*.service
make test     # pytest tests/
```

**systemd unit** (对标 rs232-gateway):

```ini
[Unit]
Description=EdgeVib Vision Service — USB Camera Capture
After=network.target mosquitto.service

[Service]
Type=simple
User=orangepi
Group=video                          # /dev/video0 访问权限
ExecStart=/opt/edge-gateway/services/vision-service/venv/bin/python \
          -m src --config /opt/edge-gateway/config/vision-service.yaml
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

`Group=video` 给 `/dev/video0` 访问权限，不需要 root。日志走 journald，`journalctl -u vision-service -f` 查看。Makefile 提供 `install`/`test` 统一入口，与 rs232-gateway/opcua-server 保持一致操作习惯。

### 决策 7: 测试策略 — 分层测试 (2026-05-27)

**选择**: 单元测试 (纯逻辑) + Mock 测试 (外部依赖隔离) + 集成测试 (端到端，mock 摄像头)

**单元测试** (纯逻辑，无硬件依赖，pytest):

| 模块 | 测试内容 | 方式 |
|------|---------|------|
| `config.py` | YAML 解析、dataclass 校验、多设备配置加载 | 纯函数 |
| `file_store.py` | 路径生成 `"{site}/{device}/{date}/{type}_{time}.jpg"`、轮转清理逻辑（过期日期目录删除） | 纯函数 + tempdir |
| `mqtt/subscriber.py` | topic 解析 `EdgeVib/+/inference/+/ai/report` → site_id/device_id 提取、设备列表匹配 | 纯函数 |

**Mock 测试** (外部依赖 mock，无需真实硬件/DB):

| 模块 | Mock 对象 | 测试内容 |
|------|-----------|---------|
| `camera/capture.py` | mock `cv2.VideoCapture` | open 失败重连、read 返回 None→跳过、release 后 resource 清理 |
| `db/client.py` | mock `psycopg2.connect` | INSERT 生成正确性、DB 不可用→best effort→WARN 日志不抛异常 |
| `mqtt/publisher.py` | mock `paho.mqtt.client` | publish 消息格式验证、断连自动重连 |

**集成测试** (端到端，mock 摄像头):

| 测试 | 覆盖 |
|------|------|
| `test_end_to_end.py` | 完整管线: 配置加载 → mock 摄像头返回固定帧 → 存文件到 tempdir → 验证文件路径/命名/JPEG 编码 → 验证 DB 元数据写入 |

**Why**: 对标 inference-engine 测试分层（conftest.py fixtures + 模块级测试 + 集成测试）。纯逻辑模块优先单测覆盖——config/file_store/subscriber 是无外部依赖的纯函数。cv2 和 paho 通过 mock 隔离，不需要真实硬件即可在 PC 端跑全部测试。集成测试用 mock 摄像头 + tempdir，一键 `pytest tests/`。

**测试数据**: 复用 `tests/integration/insert_test_data.sql`（已有 3 设备 × 多时间点），vision-service 测试不需额外插入数据。

**关键路径覆盖**: topic 解析（MQTT 输入）、路径生成（文件检索）、轮转清理（磁盘爆满防护）三个关键路径必须有测试。

### 决策 8: 主循环编程模型 — 同步主循环 + Queue 桥接 (2026-05-27)

**选择**: 同步 `while+sleep` 主循环 + `queue.Queue` 桥接 MQTT 事件，不使用 asyncio

**主循环架构**:

```python
event_queue = queue.Queue(maxsize=32)  # 线程安全队列

# paho on_message 回调（在 paho 后台线程执行）
def on_trigger(client, userdata, msg):
    event = parse_trigger(msg.topic, msg.payload)
    try:
        event_queue.put_nowait(event)
    except queue.Full:
        pass  # 队列满丢弃，不影响采集主循环

# 主循环（主线程，唯一操作摄像头的线程）
def main_loop():
    last_baseline = time.time()
    while running:
        # 1. 消费 MQTT 事件（非阻塞）
        while not event_queue.empty():
            handle_event_capture(event_queue.get_nowait())

        # 2. 定时基线拍照
        if time.time() - last_baseline >= cfg.capture.baseline_interval_s:
            for dev in cfg.devices:
                capture_baseline(dev)
            last_baseline = time.time()

        # 3. 自动轮转清理（每小时一次）
        if need_rotation():
            rotate_expired_files()

        time.sleep(1)
```

**Why 同步循环**:
- 只有两个并发点（60s 定时器 + 低频 MQTT 事件），asyncio 的 `gather(task1, task2)` 过度抽象
- paho-mqtt Python 客户端是线程模型（`loop_start()` daemon thread），不是 asyncio-native。用 asyncio 反而要多写 `loop.call_soon_threadsafe` 桥接代码
- 同步循环更易调试——完整调用栈，无协程状态，`Ctrl+C` 直接中断
- 省 asyncio event loop 约 5MB 内存
- 对标 rs232-gateway 和 opcua-server 的 `while(1) { poll(); sleep(1s); }` 模式——C 服务怎么做，Python 服务也怎么做

**Why Queue 桥接而非回调线程直接拍照**:
cv2.VideoCapture 不是线程安全的。paho `on_message` 在子线程触发，主循环的定时拍照也在用同一个 VideoCapture 对象——两个线程争一个摄像头必出 race condition。`queue.Queue` 将事件串行到主线程处理，保证摄像头操作全程单线程。

**Alternatives considered**:
- asyncio 模型: 与 inference-engine/llm-analyzer 一致但 paho-mqtt 线程桥接增加复杂度，且 asyncio 的全双工 I/O 优势在此场景用不上
- 回调线程直接拍照: cv2 线程不安全，race condition 风险

---

## vision-service 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 硬件接口 | USB UVC 摄像头 + OpenCV VideoCapture |
| 2 | 部署模式 | systemd (`Group=video`) |
| 3 | 采集策略 | 混合: 60s 定时基线 (640×480) + MQTT 事件触发全分辨率 (1920×1080) |
| 4 | 存储策略 | 文件系统主记录 + TimescaleDB `vision_captures` 元数据索引 + MQTT 通知 |
| 5 | 数据管线集成 | 车间级多设备设计，MQTT 事件驱动 + 定时双采集 |
| 6 | 项目结构 | 8 源文件 + Makefile + systemd unit |
| 7 | 测试策略 | 三层: 单元测试 (纯逻辑) + Mock 测试 + 集成测试 (mock 摄像头) |
| 8 | 主循环模型 | 同步 `while+sleep` + `queue.Queue` 桥接 MQTT 事件 |
| — | Phase 2 预留 | 画面异常检测 (OpenCV 直方图/SSIM)，新增 `frame_analyzer.py` 不影响 Phase 1 架构 |

### 关键文件清单

| 文件 | 用途 |
|------|------|
| `services/vision-service/src/__main__.py` | 入口 + 主循环 |
| `services/vision-service/src/config.py` | YAML 配置 dataclass |
| `services/vision-service/src/camera/capture.py` | cv2.VideoCapture 封装 (open/read/release/reconnect) |
| `services/vision-service/src/storage/file_store.py` | 文件系统写入 + 自动轮转清理 |
| `services/vision-service/src/db/client.py` | TimescaleDB `vision_captures` 元数据写入 (best effort) |
| `services/vision-service/src/mqtt/subscriber.py` | MQTT 触发订阅 + topic 解析 + 设备匹配 |
| `services/vision-service/src/mqtt/publisher.py` | MQTT capture 通知发布 |
| `services/vision-service/src/health.py` | 健康上报 (30s MQTT) |
| `services/vision-service/systemd/vision-service.service` | systemd unit |
| `services/vision-service/Makefile` | venv + pip install + 一键安装 |
| `services/vision-service/requirements.txt` | opencv-python-headless, numpy, psycopg2-binary, paho-mqtt, pyyaml |
| `config/vision-service.yaml` | 车间级多设备配置 |
| `docker/timescaledb/init.sql` | 追加 `vision_captures` 建表 DDL |
| `tests/test_config.py`, `test_file_store.py`, `test_subscriber.py`, `test_capture.py`, `test_db_client.py`, `test_end_to_end.py` | 三层测试覆盖 |

---

## API Server 设计

### 决策 1: 语言与框架 (2026-05-26)

**选择**: Go（编译为单二进制部署）

**Why**: 内存预算紧张（Orange Pi 4GB，仅 600MB headroom）。Go 编译二进制 ~50MB vs Python/FastAPI ~200MB；`jackc/pgx` 已在 data-aggregator 验证，性能优于 psycopg2；单二进制 `scp` 部署，无需 Docker 依赖链路；与 data-aggregator 共享 pgxpool + YAML 配置 + 结构化日志模式。

**Alternatives considered**:
- Python/FastAPI: 开发效率高、async WebSocket 原生支持，但 Python 运行时 + uvicorn + psycopg2 内存开销约 150MB 额外成本，在 600MB headroom 约束下风险较大

### 决策 2: HTTP 路由库 (2026-05-26)

**选择**: `chi` (github.com/go-chi/chi/v5)

**Why**: 标准 `net/http` Handler 接口，零侵入；middleware 链（logging/recovery/CORS）开箱即用；路由分组天然支持 API 版本化；仅 ~50KB，无额外依赖。Go 1.22 `http.ServeMux` 虽支持路径参数但缺少 middleware chaining 和 route grouping。

**Alternatives considered**:
- `gin`: 最流行但 `gin.Context` 与 `net/http` 不兼容，后续扩展受限
- 纯 `net/http` (Go 1.22): 缺少 middleware chaining，路由分组需手写

### 决策 3: REST 资源建模 (2026-05-26)

**URL 规范**: `sites/{site_id}/devices/{device_type}/{device_id}/...` 层级路径，与 MQTT topic `EdgeVib/{site_id}/{device_type}/{device_id}/...` 结构一致。

**端点清单**:

| 方法 | 路径 | 用途 |
|------|------|------|
| `GET` | `/api/v1/health` | 系统健康（DB 连通性、MQTT broker 状态、各服务心跳汇总） |
| `GET` | `/api/v1/sites` | 所有站点列表（`SELECT DISTINCT site_id FROM sensor_data`） |
| `GET` | `/api/v1/sites/{site_id}/overview` | 站点下所有设备当前状态概览（`device_status_view` 聚合） |
| `GET` | `/api/v1/sites/{site_id}/devices` | 站点下设备列表 |
| `GET` | `/api/v1/sites/{site_id}/devices/{device_type}/{device_id}` | 单个设备当前状态 |
| `GET` | `/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/sensor` | 振动时序数据（`vibration_view`, `?from=&to=` 必传, `?limit=&offset=`） |
| `GET` | `/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/environment` | 环境数据（`environment_view`, `?from=&to=` 必传） |
| `GET` | `/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/ai-reports` | AI 推理报告（`ai_reports`, `?from=&to=&severity=&limit=`） |
| `GET` | `/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/llm-reports` | LLM 故障报告（`llm_reports`, `?from=&to=&severity=&limit=`） |
| `WS` | `/api/v1/ws/events` | 实时事件推送（inference WARNING/CRITICAL、LLM 报告完成） |

**设计约束**:
- API 纯只读，不提供写入端点。数据写入统一经过 MQTT → data-aggregator → TimescaleDB 链路
- 时序类端点（sensor/environment）`?from=&to=` 必传，不做默认时间窗口。避免无范围查询打爆内存
- 报告类端点 `?limit=` 默认 20，上限 100
- 所有端点读 VIEW 不读裸 JSONB，VIEW 层已做解构，API 层不做重复提取

### 决策 4: 数据访问与 WebSocket 实时推送 (2026-05-26)

**TimescaleDB 访问**: `pgxpool` 直连只读。与 data-aggregator 复用同一套 `jackc/pgx` 模式，连接池配置偏只读（pool_max_conns=5, 更短超时）。SQL 直接查 VIEW，不用 ORM。

**WebSocket 实时推送**: API Server 作为 MQTT subscriber（paho Go 客户端），订阅以下 topic：

| MQTT Topic | 事件类型 | 用途 |
|------------|----------|------|
| `EdgeVib/+/inference/+/ai/report` | `ai_alert` | inference-engine WARNING/CRITICAL 即时推送 |
| `EdgeVib/+/llm/+/report` | `llm_report` | LLM 故障报告完成通知 |
| `EdgeVib/+/+/+/status/health` | `device_status` | 设备上下线状态变更 |

MQTT 消息到达 → 内部 `chan Event` → WebSocket hub → broadcast 所有客户端。不做客户端过滤（事件频率每分钟几条），不做历史回放（历史数据走 REST）。

**降级策略**:
- DB 不可用 → REST 端点返回 503，进程不退出，自动重连
- MQTT 不可用 → WS 端点返回 503，REST 端点正常工作
- 两个通道独立降级，互不影响

### 决策 5: 认证与安全 (2026-05-26)

**选择**: 默认无认证，预留可选 API Key middleware

**Why**: API Server 运行在扁平工业 LAN (192.168.1.0/24)，NAT 后面无公网暴露。现有 Mosquitto/TimescaleDB/Grafana 均为内网信任模型，单点加认证不改变威胁面。

预留 `X-API-Key` header 校验 middleware，配置文件 `auth.enabled: false` 默认关闭。未来如需端口转发或云端上行，一行配置即可开启。

---

### 决策 6: 部署模式 (2026-05-26)

**选择**: Docker 容器（multi-stage 构建，`golang:1.22-alpine` → `alpine:3.19`）

**Why**: 与 data-aggregator/inference-engine/llm-analyzer 统一编排在 docker-compose 中；最终镜像 ~25MB（Go 静态二进制 + alpine）；docker network 内部 DNS 访问 timescaledb/mosquitto；`docker logs` 统一日志采集。

对外端口: `8080:8080`。

### 决策 7: 配置与模块结构 (2026-05-26)

**配置文件**: `config/api-server.yaml`，YAML 格式，`gopkg.in/yaml.v3` 解析。包含 server/timescaledb/mqtt/auth/log 五个 section，与 data-aggregator 惯例一致。

**源码树**:

```
services/api-server/
├── Dockerfile                          — multi-stage: golang:1.22-alpine → alpine:3.19
├── go.mod                              — module edgevib/api-server
├── go.sum
├── cmd/server/main.go                  — 入口: 配置加载 → pgxpool → MQTT subscriber → chi router → http.Server
└── internal/
    ├── config/config.go                — YAML 配置 struct
    ├── db/client.go                    — pgxpool + 查询方法 (QueryOverview, QuerySensorData, QueryAIReports, ...)
    ├── mqtt/subscriber.go              — paho MQTT 订阅 → chan Event → WS hub
    ├── ws/
    │   ├── hub.go                      — WebSocket hub (register/unregister/broadcast)
    │   └── client.go                   — 单连接 read/write pump
    ├── handler/
    │   ├── health.go                   — GET /api/v1/health
    │   ├── sites.go                    — GET /api/v1/sites, /sites/{id}/overview, /sites/{id}/devices
    │   ├── devices.go                  — GET .../devices/{type}/{id}, /sensor, /environment
    │   ├── reports.go                  — GET .../ai-reports, /llm-reports
    │   └── ws.go                       — WS /api/v1/ws/events
    └── middleware/
        ├── logging.go                  — 请求日志 (method, path, status, duration)
        ├── recovery.go                 — panic recovery
        └── auth.go                     — 可选 X-API-Key 校验

config/
└── api-server.yaml                     — 服务配置
```

共 12 个 .go 文件，与 data-aggregator 规模相当。

---

- **库**: open62541 (C, LGPL)
- **角色**: OPC UA Server 运行在 Orange Pi 上
- **功能**: TimescaleDB 聚合数据 → 标准 OPC UA 节点模型暴露
- **节点示例**: `Motor01.XAxis.RMS`, `Motor01.Current.Amps`, `Motor01.Temperature.Celsius`
- **价值**: 任何 SCADA/组态软件可直接连接，工业互操作

## 现场总线扩展

## audio-monitor — 工业声学监测

### 决策 1: 核心工作模式 (2026-05-28)

**选择**: 连续流式监听 + 异常触发保存原始音频片段

**Why**: 工业声学监测的核心价值是捕获突发异常——轴承撞击声、转子碰磨等事件持续时间通常 <100ms。周期性采样（如 vision-service 的 60s 定时抓拍模式）大概率错过瞬态声学事件。Orange Pi 4 Pro 板载麦克风 + 3.5mm 音频插孔提供双输入通道，全志 A733 的 2×A76 + 6×A55 算力在 16kHz 单声道 FFT 下 <1ms，完全支持实时流处理。

**工作流**:
```
持续录音 → 滑动窗口 FFT (如 2048 点, Hann 窗, 50% overlap)
    → 声学特征提取 (频谱质心/频带能量/峰值频率/谱峭度)
    → 统计基线比对 (动态阈值)
    → 异常触发 → 保存前 3s + 后 2s 原始 PCM/WAV 到本地文件
    → 特征 + 元数据 → TimescaleDB 写入
    → 关键告警 → MQTT 发布
```

**Alternatives considered**:
- 周期性采样: 对标 vision-service 模式，CPU 占用低但丢瞬态事件
- 纯阈值触发: 无持续特征记录，事后无法回溯趋势

### 决策 2: 音频采集参数与 Python 库 (2026-05-28)

**选择**: sounddevice + 16kHz + 16-bit PCM 单声道

**Why**:
- **sounddevice**: `InputStream(callback=callback_fn)` 直接将音频数据以 numpy 数组传入回调，后续 FFT/特征提取零拷贝。后端 portaudio 是 Linux 音频事实标准，ALSA/PulseAudio/PipeWire 全兼容
- **16kHz 采样率**: 0-8kHz Nyquist 覆盖轴承故障特征频段 (1-5kHz)、齿轮啮合冲击、转子碰磨谐波。与 ESP32 I2S 麦克风采样率一致，便于跨端特征对齐。FFT 2048 点 <1ms，留足 headroom 给特征提取
- **16-bit PCM 单声道**: ALSA 硬件原生格式，`np.int16`→`np.float32` 一次转换。异常片段 WAV 标准格式，业界通用。Orange Pi 板载 MEMS 麦克风是单声道
- **默认参数，可配置覆盖**: `audio.sample_rate: 16000`, `audio.block_size: 2048`, `audio.channels: 1`

**Alternatives considered**:
- PyAudio: 同样 portaudio 后端但 API 是阻塞式 `read()`，需要手动管理线程。sounddevice 的 callback API 更干净
- python-alsaaudio: 最轻量但 API 偏底层，缺少 numpy 集成，回调模型需手写
- 44.1kHz/48kHz: 超声谐波检测有价值但板载 MEMS 频响上限 ~10kHz，高采样率无实际收益。保留配置项后续外接麦克风可用

### 决策 3: 声学特征提取 (2026-05-28)

**选择**: 5 项工业声学指标，与振动特征体系对齐

每 2048 点滑动窗口 (50% overlap, ~64ms@16kHz) 提取：

| # | 特征 | 计算 | 业务含义 |
|---|------|------|---------|
| 1 | **RMS 能量** | `sqrt(mean(signal²))` | 总体声压级，持续增大 = 磨损进展 |
| 2 | **频谱质心** (Hz) | `Σ(f·|X(f)|) / Σ|X(f)|` | 声音"亮度"，向高频漂移 = 轴承缺陷 |
| 3 | **频谱峭度** | 频带能量的四阶矩/二阶矩² | 冲击性检测，>3 表示瞬态冲击（碰磨/轴承剥落） |
| 4 | **高低频能量比** | `ΣE(2-8kHz) / ΣE(0-500Hz)` | 高频占比增大 = 早期故障信号 |
| 5 | **主导频率 + 幅值** | max(|X(f)|) 的 f 和 dB | 跟踪特征频率漂移，与 RPM 相关 |

**Why**: 与 inference-engine 振动 6 项指标 (RMS 趋势/峰值频率/频带能量/DE-NDE ratio/波峰因子/温度相关) 形成 **跨模态互补**：

| 振动指标 | 声学对应 | 联合诊断价值 |
|---------|---------|------------|
| RMS 趋势斜率 | RMS 能量趋势 | 振动+声学双升 = 确认恶化，单升 = 传感器异常或工况变化 |
| 峰值频率漂移 | 主导频率漂移 | 两个物理通道追踪同一故障频率，交叉验证 |
| 频带能量迁移 | 高低频能量比 | 振动高频↑+声学高频↑ = 轴承磨损；仅振动↑ = 传感器松动 |
| 波峰因子趋势 | 频谱峭度 | 冲击性信号检测，DE 端振动 + 空气声学双重确认碰磨 |

**特征存储**: TimescaleDB `audio_features` 表 (time, site_id, device_id, rms_energy, spectral_centroid, spectral_kurtosis, hf_lf_ratio, dominant_freq, dominant_amp_db, feature_vector JSONB)。`feature_vector` JSONB 保留完整 128-bin 降采样频谱用于 Grafana 热力图和后续 ML 模型。

**Alternatives considered**:
- MFCC (13 维): 语音/音乐识别标准特征，但工业声学场景中物理可解释性差——操作员无法理解"第 3 个倒谱系数为什么是 2.7"
- 仅 RMS: 信息量太少，无法区分"电机启动了"和"轴承坏了"

### 决策 4: 异常检测策略 (2026-05-28)

**选择**: 动态基线 + 多 σ 阈值 (MVP)，预留 Autoencoder 可插拔接口 (Phase 2)

**Why**: MVP 阶段无历史声学训练数据，AE 无法冷启动。动态基线自适应环境噪声变化（白班/夜班背景声差异），与 inference-engine 的 TrendAnalyzer 使用相同的统计方法论（滑动窗口 + 标准差检测）。`audio_features` 表持续积累特征数据后，Phase 2 可训练 AE 模型——代码预留 `anomaly_detector.py` 的可插拔接口（`Detector(ABC)` 基类，`ThresholdDetector` / `AutoencoderDetector` 实现）。

**触发规则**:

| 条件 | 动作 |
|------|------|
| `rms_energy > baseline_rms + 3σ` 或 `spectral_centroid > baseline_centroid + 3σ` | 保存异常音频片段 (前 3s + 后 2s WAV) + MQTT 发布 warning 级别报告 |
| `rms_energy > baseline_rms + 5σ` 或 `spectral_kurtosis > 5.0` | 立即触发保存 + MQTT 发布 critical 级别报告 + 联动 inference-engine 即时推理 |
| 以上条件持续 > 30s | MQTT 发布持续异常告警，建议现场检查 |

**基线更新**: 60s 滑动窗口，仅在未触发异常时更新（避免异常值污染基线）。启动后前 60s 为热机学习期，不触发异常。

**Alternatives considered**:
- 静态阈值: 无法适应环境变化，工厂白班/夜班背景噪声差异大
- Autoencoder 先行: 无训练数据冷启动不可行。预留 `Detector` ABC 接口，Phase 2 切换只需改一行实例化代码

### 决策 5: 部署模式 (2026-05-28)

**选择**: systemd 直接部署（对标 vision-service），`User=orangepi`, `SupplementaryGroups=audio`

**Why**: 三个维度分析——

**1. 硬件直通（最关键）**: `/dev/snd/*` 是 ALSA 音频设备节点。systemd `SupplementaryGroups=audio` 一行搞定。Docker 需要 `--device /dev/snd --group-add audio`，且 portaudio 在容器内枚举 ALSA 设备时如设备节点映射不完整会初始化失败。vision-service 选 systemd 同理（`/dev/video0`→`Group=video`）。

**2. 内存预算**: Orange Pi 4 Pro 4GB 总内存，当前格局——
- Docker 组件: TimescaleDB (~350MB) + Grafana (~120MB) + inference-engine (~120MB) + llm-analyzer (~300MB, 含 GGUF 模型) + Mosquitto (~25MB) + data-aggregator (~25MB) + api-server (~35MB) ≈ **~975MB**
- systemd 组件: opcua-server (~4MB) + rs232-gateway (~5MB) + vision-service (~70MB) + dns-relay (~10MB) ≈ **~90MB**
- Ubuntu 22.04 系统基座 ≈ **~800MB**
- 合计已用 ≈ **~1.9GB**，剩余 ≈ 2.1GB headroom

audio-monitor Python 运行时 ~50MB，Docker overhead ~15MB（overlayfs + 容器守护进程）。50MB vs 65MB 绝对值不大，但 llm-analyzer 加载大模型时 headroom 宝贵，每 MB 都算。

**3. 架构分层一致性**: 当前部署已形成自然分工——
- **Docker**: 基础设施 + 无硬件依赖的 AI 服务
- **systemd**: 需要直接访问硬件设备的服务（`dialout`/`video`/`audio` group）

这不是偶然的，是演化出来的正确分层。audio-monitor 属于第二类。

**Alternatives considered**:
- Docker: 与 inference-engine/llm-analyzer 统一编排，但 ALSA 硬件访问在容器中有已知兼容性问题（设备节点映射、PulseAudio socket 挂载等），Docker overhead 10-15MB，且破坏"硬件直通→systemd"的架构分层

### 决策 6: 项目结构与构建 (2026-05-28)

**选择**: 7 源文件 + venv + Makefile + systemd unit，对标 vision-service

```
services/audio-monitor/
├── Makefile                              — venv + pip install + install systemd
├── requirements.txt                      — sounddevice, numpy, scipy, paho-mqtt, psycopg2-binary, pyyaml
├── src/
│   ├── __init__.py
│   ├── __main__.py                       — 入口 + 信号处理 + 主循环
│   ├── config.py                         — YAML 配置 dataclass (对标 vision-service config.py)
│   ├── audio/
│   │   ├── __init__.py
│   │   ├── capture.py                    — sounddevice InputStream callback 封装
│   │   └── processor.py                  — FFT + 5 项特征提取 + 基线管理
│   ├── anomaly/
│   │   ├── __init__.py
│   │   ├── detector.py                   — Detector ABC + ThresholdDetector 实现
│   ├── storage/
│   │   ├── __init__.py
│   │   └── file_store.py                 — 异常 WAV 文件保存 + 自动轮转清理
│   ├── db/
│   │   ├── __init__.py
│   │   └── client.py                     — TimescaleDB audio_features 表写入 + 基线查询
│   ├── mqtt/
│   │   ├── __init__.py
│   │   ├── publisher.py                  — 异常报告发布 (EdgeVib/{site_id}/audio/{device_id}/alert)
│   │   └── subscriber.py                 — inference-engine 联动触发订阅
│   └── health.py                         — 健康上报 (30s MQTT)
├── systemd/
│   └── audio-monitor.service             — User=orangepi, SupplementaryGroups=audio
└── tests/
    ├── __init__.py
    ├── conftest.py
    ├── test_config.py
    ├── test_capture.py                   — mock sounddevice.InputStream
    ├── test_processor.py                 — FFT + 特征提取正确性
    ├── test_detector.py                  — 阈值检测逻辑
    ├── test_file_store.py
    └── test_end_to_end.py                — 集成测试: 特征提取 → 异常判定 → 文件保存
```

**Why**: 源文件数与 vision-service (8 个) 相当，结构平行可互读。所有文件在 `src/` 下，`Makefile` 管理 venv 生命周期（`make venv && make install`）。psycopg2-binary 省去 pg_config 编译依赖。sounddevice 是唯一新增系统依赖（`apt install libportaudio2`）。

### 决策 7: 数据管线集成 (2026-05-28)

**选择**: 双写 TimescaleDB + MQTT，复用现有数据模型

**数据流**:
```
板载麦克风/3.5mm Line-In
    → sounddevice InputStream callback (16kHz, 2048 block, 50% overlap)
    → processor.py: FFT → 5 项特征 → 基线比对 → 异常判定
    ├── TimescaleDB: audio_features 表 (全量特征, 每 ~64ms 一行) + audio_anomalies 表 (异常事件, 含 WAV 路径)
    └── MQTT: EdgeVib/{site_id}/audio/{device_id}/alert (仅异常, QOS 1)
```

**MQTT Topic**: `EdgeVib/{site_id}/audio/{device_id}/alert`

**alert JSON**:
```json
{
  "device_id": "motor01",
  "timestamp_utc": "2026-05-28T10:30:15.123Z",
  "severity": "warning",
  "trigger_reason": "rms_energy_exceeded",
  "rms_energy": 0.023,
  "baseline_rms": 0.008,
  "sigma_level": 3.5,
  "spectral_centroid_hz": 2340,
  "spectral_kurtosis": 2.1,
  "wav_path": "/var/lib/edgevib/audio/anomalies/motor01_20260528T103015.wav"
}
```

**TimescaleDB 新表**:

| 表 | 用途 | 关键列 |
|----|------|--------|
| `audio_features` | 全量声学特征时序 | `time, site_id, device_id, rms_energy, spectral_centroid_hz, spectral_kurtosis, hf_lf_ratio, dominant_freq_hz, dominant_amp_db, feature_vector(JSONB)` |
| `audio_anomalies` | 异常事件记录 | `time, site_id, device_id, severity, trigger_reason, rms_energy, baseline_rms, sigma_level, wav_path, duration_ms, metadata(JSONB)` |

**Why 双写**: TimescaleDB 保证全量特征可查询（Grafana 声学频谱趋势图、与振动数据 JOIN 做跨模态分析），MQTT 解耦即时告警消费方（api-server WebSocket 推送、llm-analyzer 触发故障报告生成）。`feature_vector` JSONB 保留 128-bin 降采样频谱，`audio_features` 表与 `sensor_data` 表的透传策略一致（保留原始信息不退化为固定字段）。

### 决策 8: 音频片段存储 (2026-05-28)

**选择**: 本地文件系统 WAV + TimescaleDB 元数据索引，自动轮转清理

- **存储路径**: `/var/lib/edgevib/audio/anomalies/{device_id}_{ISO8601}.wav`
- **片段长度**: 异常触发点前 3s + 后 2s = 5s (16-bit 16kHz 单声道 = 160KB/片段)
- **轮转策略**: 保留最近 100 个片段或 7 天，超出的自动删除（`file_store.py` 中 `rotate_expired_files()`，对标 vision-service 的轮转逻辑）
- **baseline WAV**: 每 10 分钟保存 2s 正常片段到 `audio/baselines/`，仅保留最新 6 个，用于事后对比

**Why**: 对标 vision-service 的 JPEG 文件存储模式。5s 片段大小 ~160KB，100 个仅 16MB，对 eMMC 几乎无压力。TimescaleDB 不存 blob（`TOAST` 机制在大 blob 场景性能差），存路径指针更高效。与 vision-service 的 `vision_captures` 元数据表模式一致。

### 决策 9: 主循环模型 (2026-05-28)

**选择**: 同步 `while+sleep(0.1)` + `queue.Queue` 桥接 portaudio C 线程

```
sounddevice callback (portaudio C 线程, 每 64ms):
  2048 samples → FFT (<1ms) → 特征 → queue.put(features)

主线程 (Python):
  while running:
    features = queue.get(timeout=1.0)
    基线更新 + 异常判定
    每 1s: 攒批 flush 到 TimescaleDB
    异常时: WAV 保存 + MQTT 告警发布
    每 30s: health.report()
```

**Why**: sounddevice callback 在 portaudio 内部高优先级 C 线程运行，只做快速操作（FFT + 入队）。DB 写入/MQTT 发布等可能阻塞的操作统一在主线程处理。与 vision-service 决策 8 相同理由：只有 2 个数据源（音频流 callback + MQTT 订阅），不需要 asyncio 的全双工 I/O 复杂度。`queue.Queue` 作为 C 线程→Python 主线程的线程安全桥接。

**Alternatives considered**:
- asyncio: 需要 `loop.call_soon_threadsafe` 桥接 portaudio callback，增加复杂度
- callback 内直接 DB 写: 阻塞 portaudio 线程导致音频丢帧，不可接受

### 决策 10: 配置文件 (2026-05-28)

**选择**: 4-section YAML (`audio` / `timescaledb` / `mqtt` / `alarm`)，对标 vision-service

```yaml
audio:
  device_index: -1              # -1 = 系统默认麦克风, 0/1/... = 指定设备
  sample_rate: 16000            # Hz, 可配置 8000/16000/44100/48000
  block_size: 2048              # 每次回调的采样点数
  channels: 1                   # 单声道
  overlap_ratio: 0.5            # FFT 窗口重叠比例

timescaledb:
  host: "localhost", port: 5432
  dbname: "edgevib_ts", user: "edgevib", password: "edgevib123"

mqtt:
  broker: "localhost", port: 1883
  client_id: "edgevib-audio-monitor"
  qos: 1

alarm:
  sigma_warning: 3.0            # warning 级别 σ 倍数
  sigma_critical: 5.0           # critical 级别 σ 倍数
  baseline_window_s: 60         # 基线计算窗口 (秒)
  anomaly_sustain_s: 30         # 持续异常 > 此值时升级告警
  pre_trigger_s: 3              # 异常前保留音频秒数
  post_trigger_s: 2             # 异常后保留音频秒数
  learning_period_s: 60         # 启动后热机学习期

logging:
  level: "info"
```

**Why**: `audio` section 替代其他服务的 `server`/`camera` section，所有硬件参数可配置，现场调试无需重编译。`alarm` section 将所有阈值集中管理，与 inference-engine `config.yaml` 的 `trend` section 模式一致。`device_index: -1` 默认用系统默认麦克风，调试时随时换设备。

### 决策 11: 测试策略 (2026-05-28)

**选择**: 三层测试 — 单元测试 + Mock 测试 + 端到端测试

| 层级 | 测试对象 | mock 对象 | 验证点 |
|------|---------|-----------|--------|
| **单元** | `processor.py` (FFT/特征提取/基线管理) | 无 (纯 numpy 计算) | FFT 正确性 (已知正弦波输入→预期频谱)、基线统计算法、异常判定逻辑 |
| **单元** | `detector.py` (ThresholdDetector) | 无 (纯数据输入→布尔输出) | σ 阈值边界、持续时长计数、学习期抑制 |
| **Mock** | `capture.py` (InputStream) | `sounddevice.InputStream` | callback 调用次数、数据格式正确、异常恢复 |
| **Mock** | `db/client.py` | `psycopg2` | SQL 正确性、重连逻辑、攒批 flush |
| **Mock** | `mqtt/publisher.py`, `mqtt/subscriber.py` | `paho.mqtt.client` | topic 格式化、JSON 序列化、重连 |
| **Mock** | `file_store.py` | 文件系统 (tmpdir) | WAV 写入、轮转清理、异常场景 (磁盘满) |
| **E2E** | 完整管线 | sounddevice (用 WAV 文件回放替代真实麦克风), TimescaleDB, MQTT | 音频输入→特征提取→异常判定→WAV 保存→MQTT 告警完整性 |

**Why 三层而非两层**: 对标 vision-service（5 test_*.py + test_end_to_end.py）。`processor.py` 和 `detector.py` 是纯函数模块，单测覆盖核心算法正确性。`capture.py` 的 sounddevice mock 比真实麦克风更可靠——用已知频率的合成正弦波 WAV 作为输入，验证 FFT 输出精确值。E2E 测试用 WAV 文件回放替代真实麦克风，在 CI 中可运行。

### 关键文件清单

| 文件 | 用途 |
|------|------|
| `services/audio-monitor/src/__main__.py` | 入口 + 主循环 |
| `services/audio-monitor/src/config.py` | YAML 配置 dataclass |
| `services/audio-monitor/src/audio/capture.py` | sounddevice InputStream callback 封装 |
| `services/audio-monitor/src/audio/processor.py` | FFT + 5 项特征提取 + 基线管理 |
| `services/audio-monitor/src/anomaly/detector.py` | Detector ABC + ThresholdDetector 实现 |
| `services/audio-monitor/src/storage/file_store.py` | WAV 文件保存 + 自动轮转清理 |
| `services/audio-monitor/src/db/client.py` | TimescaleDB `audio_features` + `audio_anomalies` 表写入 |
| `services/audio-monitor/src/mqtt/publisher.py` | 异常报告 MQTT 发布 |
| `services/audio-monitor/src/mqtt/subscriber.py` | inference-engine 联动触发订阅 |
| `services/audio-monitor/src/health.py` | 健康上报 (30s MQTT) |
| `services/audio-monitor/systemd/audio-monitor.service` | systemd unit, `SupplementaryGroups=audio` |
| `services/audio-monitor/Makefile` | venv + pip install + 一键安装 |
| `services/audio-monitor/requirements.txt` | sounddevice, numpy, scipy, paho-mqtt, psycopg2-binary, pyyaml |
| `config/audio-monitor.yaml` | 4-section 配置 |
| `docker/timescaledb/init.sql` | 追加 `audio_features` + `audio_anomalies` 建表 DDL |
| `tests/test_config.py`, `test_processor.py`, `test_detector.py`, `test_capture.py`, `test_file_store.py`, `test_db_client.py`, `test_end_to_end.py` | 三层测试覆盖 |

### audio-monitor 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 核心工作模式 | 连续流式监听 + 异常触发保存原始音频 |
| 2 | 音频采集 | sounddevice + 16kHz + 16-bit PCM 单声道 |
| 3 | 声学特征 | 5 项工业指标 (RMS/频谱质心/频谱峭度/高低频能量比/主导频率) |
| 4 | 异常检测 | 动态基线 + σ 阈值 (MVP)，预留 AE 可插拔接口 |
| 5 | 部署模式 | systemd, `User=orangepi`, `SupplementaryGroups=audio` |
| 6 | 项目结构 | 11 源文件 + venv + Makefile (对标 vision-service) |
| 7 | 数据管线 | 双写 TimescaleDB (audio_features/audio_anomalies) + MQTT |
| 8 | 文件存储 | WAV 文件系统 + 元数据索引 + 自动轮转清理 |
| 9 | 主循环模型 | 同步 `while+sleep` + `queue.Queue` 桥接 portaudio C 线程 |
| 10 | 配置文件 | 4-section YAML (audio/timescaledb/mqtt/alarm) |
| 11 | 测试策略 | 三层: 单元 + Mock + E2E (WAV 文件回放替代真实麦克风) |

---

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

---

## OTA Server — 跨平台固件分发系统

### 系统范围

OTA Server 负责三个平台的固件分发：ESP32-S3、STM32F407 (DE 振动节点)、STM32F103 (NDE 传感器节点)。三个平台的传输通道、固件格式、Flash 布局各不相同，需要统一版本管理和分发策略。

### 平台 OTA 就绪状态 (2026-05-28 基线)

| 平台 | OTA 客户端 | 传输通道 | Flash 现状 | 缺口 |
|------|-----------|---------|-----------|------|
| ESP32-S3 | 已完成(1200行C)，未集成 | WiFi/HTTP | 单 factory 分区 | 分区表需加 ota_0/ota_1，主程序集成 |
| STM32 F407 | 零代码 | UART4(经ESP32), UART5(RS232), Ethernet(LAN8720) | 单区 1MB + 外挂 16MB SPI Flash | bootloader、Flash编程、协议OTA命令 |
| STM32 F103 | 零代码 | CAN(经F407中继) | 单区 64KB (仅剩 ~26KB) | Flash空间极紧、CAN带宽有限 |
| Orange Pi | OTA Server 零代码 | — | — | HTTP文件服务、版本管理、固件存储 |

### 决策 1: ESP32 OTA 传输通道 (2026-05-28)

**选择**: HTTP 直连 (Orange Pi 提供 HTTP 文件服务 + version.json 端点)

**Why**: ESP32 现有 `ota_update` 模块已实现完整的 HTTP OTA 客户端——`ota_update_check_version()` 轮询 version.json、`perform_ota_download()` HTTP GET 下载固件、SHA256 校验、`esp_ota_set_boot_partition()` 切换启动分区。选 HTTP 直连意味着 ESP32 侧 OTA 代码零改动，只需：
- 分区表加 `ota_0`/`ota_1` 分区
- 主程序 `app_main()` 调用 `ota_update_init()` + 定时 `ota_update_check_version()`
- 配置 `ota_server_url` 指向 Orange Pi AP 侧 HTTP 服务

Orange Pi 侧提供轻量 HTTP 文件服务（Go 内嵌 `net/http` 或 nginx），暴露 `/firmware/esp32/version.json` 和 `/firmware/esp32/<version>.bin` 两个端点。ESP32 每小时轮询一次 version.json，发现新版本则自动下载升级。MQTT 仅用于升级状态上报（进度/成功/失败），不参与文件传输。

**Alternatives considered**:
- MQTT 通知 + HTTP 下载: 增加 MQTT→OTA 桥接代码但收益有限，OTA 对实时性无要求，每小时轮询足以覆盖
- MQTT 传输固件: MQTT 单消息最大 ~256MB(Mosquitto 配置)，但 ESP32 端需将固件分片→MQTT payload→重组→写 Flash，协议复杂度远超 HTTP Range 请求，且占用 MQTT 通道带宽影响实时数据

### 决策 2: STM32 F407 OTA 传输通道 (2026-05-28)

**选择**: 主通道 ESP32 UART4 中继 + 备通道 UART5 RS232 直连，复用现有主/备切换策略

**Why**: 与数据采集路径的主/备架构完全一致:
```
OTA 主通道 (ESP32正常): Orange Pi → HTTP/WiFi → ESP32 → UART4(115200bps) → F407
OTA 备通道 (ESP32挂机): Orange Pi → USB-RS232 → UART5(115200bps) → F407
```

F407 端复用现有心跳监测: ESP32 UART4 任一台法帧=存活，丢失 >3s 切换 UART5，恢复后切回。固件更新是低频操作（几周一次），1MB 固件 115200bps 约 90 秒传输，期间 UART4 传感器数据暂停可接受——升级期间 ESP32 缓存或丢弃传感器数据。

ESP32 中继角色: 从 Orange Pi HTTP 下载 F407 固件 → 通过 UART4 协议透传给 F407。ESP32 端 UART 协议栈（protocol.c 10 状态机 + CRC16-MODBUS）已验证稳定，新增 OTA 透传 CMD 仅需 2-3 个命令码。

备通道复用 rs232-gateway：当前 rs232-gateway 仅做上行（serial read → MQTT publish），需新增下行能力（MQTT subscribe → serial write）以支持固件下发。这是纯软件改动，USB-RS232 硬件链路已存在。

**Alternatives considered**:
- 纯 RS232 直连: 需额外 USB-RS232 线缆，RS232 线缆长度受限（~15m），不适合 Orange Pi 远程部署场景
- Ethernet/LAN8720 主通道: 速度最快（100Mbps）但 `MX_LWIP_Init` 当前因 PHY 无链路挂死，需先修复 lwIP 初始化逻辑才能使用。Phase 2 可启用为第三通道

### 决策 3: STM32 F103 NDE 节点 OTA 排除 (2026-05-28)

**选择**: F103 不支持 OTA，固件一次性烧录

**Why**: F103 64KB Flash 中固件占用 ~37KB（58%），剩余 ~27KB。加入 bootloader（4-8KB CAN接收 + Flash擦写 + 跳转）后剩余仅 ~19KB，未来扩展空间极紧。F103 固件职责极其固定——ADXL345 驱动、定点 FFT（dsp_fft_q15）、CAN CRC8 组帧发送——这些工业传感器基础算法不会随业务需求变化。真正的业务逻辑变更（电机控制、AI模型执行、报警策略）发生在 F407 和 ESP32，这两者已有 OTA 通道。F103 绑在电机 NDE 端，物理 USB 烧录在工厂现场可接受，且 USB 口已预留用于供电+调试。

**Alternatives considered**:
- CAN OTA via F407 中继: 技术可行但 64KB Flash 余量太紧，收益不抵复杂度。CAN 500kbps 传输 OK，但 F103 加 bootloader 意味着 STM32CubeIDE 工程需双固件（bootloader + app），维护成本翻倍
- F407 SWD 编程 F103: F407 可通过 GPIO 模拟 SWD 协议烧录 F103，但需额外 2 根 GPIO 线（SWCLK/SWDIO），当前硬件无预留。且 SWD 编程期间 F103 需保持复位状态，影响电机实时监测

### 决策 4: OTA Server 部署模式 (2026-05-28)

**选择**: Go Docker 独立服务，对标 api-server 模式

**Why**: 与现有服务架构一致——单一 Go 二进制、multi-stage Docker 构建（`golang:1.22-alpine` → `alpine:3.19`）、`docker-compose` 编排。职责分两层：
1. **HTTP 文件服务**: Go `net/http` 内嵌 `http.FileServer`，暴露 `/firmware/esp32/version.json` 和 `/firmware/esp32/<version>.bin`、`/firmware/f407/<version>.bin` 端点，供 ESP32 HTTP GET 下载
2. **固件管理 REST API**: `POST /api/v1/firmware/upload` 上传、`GET /api/v1/firmware/versions` 版本列表、`GET /api/v1/firmware/upgrade-history` 升级历史

固件二进制文件通过 volume mount 存储在 `/opt/edge-gateway/firmware/` 目录（不打包进 Docker 镜像）。版本元数据写入 TimescaleDB 新表 `firmware_versions`。MQTT 订阅 ESP32/F407 升级状态上报 topic `EdgeVib/+/ota/+/status`。

**Alternatives considered**:
- 集成到 api-server: 复用 pgxpool + MQTT + chi router，但 api-server 定位是"对外数据服务"，OTA 固件分发是不同职责。api-server 挂了不应影响 OTA，反之亦然
- nginx + 独立 systemd 服务: 两段式部署增加运维复杂度，且 nginx 容器额外占用 ~10MB 内存

### 决策 5: STM32 F407 Bootloader 设计 (2026-05-28)

**选择**: Bootloader 32KB (Sector 0-1) + 外部 SPI Flash (W25Q128 16MB) 暂存固件镜像

**Flash 布局**:
```
内部 Flash 1MB (0x08000000):
  Sector 0-1 (32KB):  Bootloader (IAP)
  Sector 2-11 (~992KB): App 固件

外部 SPI Flash 16MB (SPI2, PI0=CS/PI1=SCK/PI3=MOSI/PI2=MISO):
  固件暂存区 (完整 .bin 镜像)
```

**升级流程**:
```
1. App 运行中 ← ESP32 UART4 CMD_OTA_BEGIN(size, crc32)
2. App: 备份 SRAM 写升级标志 → 关中断 → NVIC_SystemReset()
3. Bootloader: 检测升级标志 → 初始化 UART4 (最小,无 RTOS) + SPI2
4. Bootloader: UART4 逐块接收固件 → SPI Flash 写入
5. 全部收完 → CRC32 校验
6. 校验通过 → 擦除内部 Flash App 区 (Sector 2-11)
7. SPI Flash 逐页拷贝到内部 Flash
8. 清除备份 SRAM 升级标志 → NVIC_SystemReset()
9. Bootloader: 无升级标志 → 校验 App 栈顶地址 → 跳转
```

**Why SPI Flash 暂存而非直接写内部 Flash**:
- **断电安全**: 如果在擦除/编程内部 Flash 时断电，SPI Flash 上保留完整固件镜像，重新上电后 bootloader 自动重试
- **校验前置**: 固件全部收完并 CRC32 校验通过后才擦除内部 Flash，避免写入损坏固件
- **UART4 速率匹配**: 115200bps (~11.5KB/s) 写入 SPI Flash 绰绰有余，无需 DMA 流控
- SPI2 外设已 CubeMX 配置（当前仅 `MX_SPI2_Init()` + GPIO），SPI Flash 驱动复用 F407 现有 BSP 模式

**Why 不用 Dual-Bank**:
- STM32F407 无硬件 dual-bank 功能（F7/H7 才有），软件模拟需两个 512KB 分区
- 当前固件 ~200-400KB，但未来可能超过 512KB，且 HAL/lwIP 代码持续膨胀
- SPI Flash 暂存方案无分区大小限制，更灵活

**备份 SRAM 升级标志**: 使用 STM32F407 4KB 备份 SRAM（`BKP_SRAM_BASE`，VBAT 供电域）。升级标志 = 魔数 `0x0TAF407`，复位后 bootloader 检查此魔数决定进入升级模式还是直接跳转 App。

**Alternatives considered**:
- 直接写内部 Flash: 省去 SPI Flash 步骤，但 UART4 传输中断电→固件损坏→变砖。工业现场不允许
- Dual-Bank (512KB×2): F407 无硬件支持，软件模拟需维护两份中断向量表偏移，且单分区最大 512KB 限制未来扩展

### 决策 6: UART 协议 OTA 命令扩展 (2026-05-28)

**选择**: 新增 3 个下行命令 (0x20-0x22) + 1 个上行命令 (0x23)，复用现有帧格式

**帧格式不变**: `[AA55][LEN:2B][DEV:1B][CMD:1B][SEQ:1B][DATA:≤128B][CRC16-MODBUS:2B][0D]`

**命令定义**:

| CMD | 方向 | 名称 | 数据 | 响应 |
|-----|------|------|------|------|
| 0x20 | ESP32→F407 | `CMD_OTA_BEGIN` | `[size:4B][crc32:4B]` | ACK/NACK |
| 0x21 | ESP32→F407 | `CMD_OTA_DATA` | `[seq:2B][payload:≤128B]` | ACK+seq / NACK+seq |
| 0x22 | ESP32→F407 | `CMD_OTA_END` | `[final_crc32:4B]` | ACK/NACK |
| 0x23 | F407→ESP32 | `RESP_OTA_STATUS` | `[state:1B][progress:1B][error:2B]` | — |

**Why 每帧 128B**: 复用协议现有 `PROTO_PAYLOAD_MAX_SIZE=128`，不做改动。小帧重传粒度好——CRC 错误只需重传 128B 而非整批。F407 bootloader 无 RTOS，极简轮询接收，无需 DMA。

**Why 双重校验 (CRC16+CRC32)**: CRC16-MODBUS 保证帧级传输不出错，CRC32 保证 SPI Flash 暂存的完整固件镜像与 Orange Pi 原始 .bin 逐位一致。两层校验覆盖传输 + 存储两个环节。

**ESP32 中继流程**: ESP32 先从 Orange Pi HTTP 下载 F407 固件到 PSRAM（ESP32-S3 有 8MB PSRAM，1MB 固件绰绰有余），校验 CRC32，然后 UART4 逐帧发送给 F407。每 10 帧通过 MQTT 上报一次进度到 `EdgeVib/{site}/ota/esp32/status`。

**Alternatives considered**:
- 更大的 payload 每帧: 需改 PROTO_PAYLOAD_MAX_SIZE，影响所有现有协议帧内存分配。不值为 OTA 单独改动
- 无 CRC32 仅帧 CRC16: 传输无错但无法保证 SPI Flash 暂存文件完整性（断电/Flash bit rot 无法检测）
- ESP32 流式转发不缓存: 如果 HTTP 下载中断，F407 会卡在升级中状态。完整缓存→校验→再发送更安全

### 决策 7: 固件版本管理与 version.json 格式 (2026-05-28)

**选择**: 语义版本号 (maj.min.patch) + 单一 `version.json` 文件包含所有平台

**version.json 格式** (Orange Pi HTTP 服务 `/firmware/version.json`):
```json
{
  "esp32": {
    "latest_version": "1.2.3",
    "build_date": "2026-05-28",
    "file": "esp32-gateway-1.2.3.bin",
    "size": 2097152,
    "sha256": "<hex>",
    "min_hardware_rev": "v1.0",
    "release_notes": "Fix MQTT reconnect; optimize FFT window"
  },
  "f407": {
    "latest_version": "1.0.1",
    "build_date": "2026-05-28",
    "file": "f407-node-1.0.1.bin",
    "size": 262144,
    "sha256": "<hex>",
    "min_hardware_rev": "v1.0",
    "release_notes": "Fix CAN RX ISR priority; add ADC calibration"
  }
}
```

**Why 单一 version.json**: ESP32 一次 HTTP GET 就知道自身 + F407 的固件是否需要更新。ESP32 自身用 `ota_update_check_version()`，F407 版本通过 UART4 `CMD_OTA_BEGIN` 时附带当前版本号，Orange Pi 侧可追踪。`min_hardware_rev` 防止新版固件刷到不兼容的旧硬件上。

**升级触发**:
- ESP32 自身: 每小时定时轮询 `version.json`，发现 `latest_version` > 自身 → HTTP 下载 → `esp_ota_*` 升级
- F407: ESP32 轮询时对比 `f407.latest_version` vs 上次记录 → 如有更新 → HTTP 下载 F407 .bin → UART4 中继

**TimescaleDB 持久化**: `firmware_versions` 表（平台+版本+文件元数据）+ `upgrade_history` 表（设备级升级记录：从哪版→到哪版→状态→耗时）。Grafana 可按平台/设备过滤升级历史。

**Why 不用强制升级**: 工业现场设备不能随意重启。升级由运维人员在 OTA Server REST API 手动触发（`POST /api/v1/firmware/upgrade`），版本检查是告警通知（"有新固件"），不是自动执行。

### 决策 8: ESP32 OTA 集成 — 分区表 + 主程序改造 (2026-05-28)

**选择**: 标准双 OTA 分区 + HTTP 明文 + 主程序集成

**分区表改造** (`partitions.csv`):
```
otadata,  data, ota,     0x9000,  0x2000
ota_0,    app,  ota_0,   0x20000, 0x1E0000
ota_1,    app,  ota_1,   0x200000,0x1E0000
```
每分区 ~1.9MB，当前 ESP32 固件约 1.2MB，留 ~700KB 余量。`otadata` 分区由 ESP-IDF OTA 子系统管理，记录当前启动分区和回滚状态。

**SDK Config**:
- `CONFIG_PARTITION_TABLE_CUSTOM=y` → 保持，使用自定义 partitions.csv
- `CONFIG_OTA_ALLOW_HTTP=y` → 允许 HTTP OTA（Orange Pi 在内网 192.168.2.0/24，TLS 无安全收益）
- Flash 大小需确认：sdkconfig.defaults 标注 16MB vs 分区表注释 4MB 存在矛盾，以实际焊接的 ESP32-S3 模块 Flash 为准（预期 16MB，PSRAM 8MB）

**主程序集成** (`esp32-gateway.c`):
- `app_main()`: 调用 `ota_update_init()` 初始化 OTA 模块
- 新增 OTA 检查任务（低优先级，1KB 栈）: 每小时 GET `version.json` → 比较版本 → 自动下载升级 ESP32 → 检查 F407 版本 → 如有新版本下载到 PSRAM → UART4 中继
- F407 中继子模块: `ota_relay.c` — 封装 CMD_OTA_BEGIN/DATA/END 的 UART4 发送 + ACK/NACK 处理 + 重传逻辑
- MQTT 上报升级状态到 `EdgeVib/{site_id}/ota/{device_id}/status`

### 决策 9: OTA Server 项目结构与 Phase 划分 (2026-05-28)

**选择**: 10 源文件 Go Docker 服务，Phase 1 仅 ESP32 中继主通道，rs232-gateway 下行备用延后

**源码树**:
```
services/ota-server/
├── Dockerfile                          — multi-stage: golang:1.22-alpine → alpine:3.19
├── go.mod                              — module edgevib/ota-server
├── cmd/server/main.go                  — 入口
└── internal/
    ├── config/config.go                — YAML 配置 struct
    ├── db/client.go                    — pgxpool + firmware_versions/upgrade_history CRUD
    ├── mqtt/subscriber.go              — 订阅 OTA 状态上报
    ├── filestore/store.go              — /opt/edge-gateway/firmware/ 固件文件管理
    ├── handler/
    │   ├── health.go                   — GET /api/v1/health
    │   ├── firmware.go                 — POST upload, GET versions, GET history, POST upgrade
    │   └── version.go                  — GET /firmware/version.json (动态生成), GET /*.bin (下载)
    └── middleware/
        ├── logging.go                  — 请求日志
        └── recovery.go                 — panic recovery
```

**Why Phase 1 不做 rs232-gateway 下行**: rs232-gateway 当前仅做上行（serial read→MQTT publish），新增下行需：MQTT subscribe → 解析 OTA 命令 → serial write → 等待 F407 响应 → MQTT publish 结果。这是双向串口协议，与现有单向架构有本质差异。且 ESP32 正常运行时 UART4 主通道完全覆盖，备通道只在 ESP32 挂机时用——工业场景 ESP32 挂机率极低。Phase 2 再补齐。

**配置**: `config/ota-server.yaml`，4-section YAML (server/firmware/timescaledb/mqtt)，对标 api-server 配置惯例。

### 决策 10: OTA 状态上报 MQTT Topic + TimescaleDB 持久化 (2026-05-28)

**选择**: 每个设备两个 topic (status + version) + 两张 TimescaleDB 表

**MQTT Topics**:
| Topic | 方向 | 用途 | Payload 要点 |
|-------|------|------|-------------|
| `EdgeVib/{site_id}/ota/{device_id}/status` | ESP32→Orange Pi | 升级进度上报 | `{device_id, platform, from_ver, to_ver, state, progress_pct, error}` |
| `EdgeVib/{site_id}/ota/{device_id}/version` | ESP32→Orange Pi | 当前版本周期性上报 | `{device_id, platform, current_version, build_date}` |

**Why 两个 topic**: `status` 是事件驱动（升级时密集发送，平时不发），`version` 是周期心跳（每 10 分钟，与 health heartbeat 同频）。拆分避免 status topic 被 version 心跳淹没，consumer 可单独订阅关心的 topic。QoS 1，Retained=false。

**TimescaleDB 新表**:
```sql
CREATE TABLE firmware_versions (
    time            TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    platform        TEXT NOT NULL,        -- 'esp32' | 'f407'
    version         TEXT NOT NULL,        -- '1.2.3' (semver)
    file_name       TEXT NOT NULL,
    file_size_bytes BIGINT NOT NULL,
    sha256          TEXT NOT NULL,
    min_hardware_rev TEXT,
    release_notes   TEXT,
    uploaded_by     TEXT,
    PRIMARY KEY (platform, version)
);

CREATE TABLE upgrade_history (
    time            TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    platform        TEXT NOT NULL,
    device_id       TEXT NOT NULL,
    from_version    TEXT,
    to_version      TEXT NOT NULL,
    status          TEXT NOT NULL,        -- 'downloading'|'installing'|'success'|'failed'|'rolled_back'
    progress_pct    INT,
    error_message   TEXT,
    duration_ms     BIGINT,
    triggered_by    TEXT DEFAULT 'auto'   -- 'auto'|'manual'
);
```

**Why `firmware_versions` 用 (platform, version) 复合主键而非自增 ID**: 同一平台同一版本只能有一条记录，复合主键天然去重。升级历史用时间分区键 + `device_id` 索引，Grafana 可按设备过滤升级记录。

**OTA Server 消费**: MQTT subscriber → `handleStatusReport()` → `INSERT INTO upgrade_history`。`version.json` 端点从 `firmware_versions` 表动态生成（`SELECT DISTINCT ON (platform) * ORDER BY platform, time DESC`）。

### 决策 11: 端到端升级流程 + 安全 + 测试 (2026-05-28)

**选择**:

**升级流程 — ESP32 自身**:
```
Orange Pi                     ESP32-S3
  ← GET /firmware/version.json   每小时轮询
  → 200 JSON                     比较 esp32.latest_version > current → 触发
  ← GET /firmware/esp32/*.bin    HTTP Range 分片下载（支持断点续传）
  → stream .bin                  SHA256 校验
                                  MQTT: status="downloading"
                                  esp_ota_begin/write/end
                                  MQTT: status="installing"
                                  esp_ota_set_boot_partition()
                                  MQTT: status="success"
                                  vTaskDelay(3s) → esp_restart()
                                  (新固件启动)
                                  MQTT: version={new_ver}
  ← MQTT status ───────────────── ota-server → upgrade_history
```

**升级流程 — F407 (ESP32 中继)**:
```
Orange Pi      ESP32-S3                    STM32F407
  ← GET version.json  每小时轮询
  → 200 JSON          f407.latest_version > known → 触发
  ← GET f407/*.bin    → PSRAM, CRC32
                       MQTT: status="downloading" (F407)
                       ── CMD_OTA_BEGIN ──→  (App) → 备份SRAM → 复位
                       ←── ACK ───────────  (Bootloader 就绪)
                       ── DATA ×8192 ────→  → SPI Flash
                       ←── ACK/NACK ──────
                       MQTT: status="installing", progress=X%
                       ── CMD_OTA_END ────→
                                             CRC32 → 擦除内部Flash → 拷贝
                       ←── ACK + STATUS ──  → 复位 → App
                       MQTT: status="success"
  ← MQTT status ─────── ota-server → upgrade_history
```

**安全策略**: Phase 1 SHA256 校验，不做固件代码签名。Orange Pi 在私有 LAN (192.168.x/24)，固件由运维人员通过 REST API 上传。攻击者如已进入内网可直接操作 Mosquitto/TimescaleDB/Grafana，固件签名不改变威胁面。`min_hardware_rev` 字段防固件版本刷到不兼容硬件。`CONFIG_OTA_ALLOW_HTTP=y` 因内网 TLS 无安全收益。

**测试策略**: 对标 api-server 分层 —— Go 单元测试 (config/filestore/handler 纯逻辑) + Mock 测试 (MQTT subscriber mock + pgx mock) + Python 集成测试 (真实 ota-server Docker + TimescaleDB + Mosquitto，模拟 ESP32 HTTP 轮询 version.json + MQTT 状态上报)。测试数据复用 `tests/integration/insert_test_data.sql`。

### OTA Server 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | ESP32 OTA 传输 | HTTP 直连 (Orange Pi 提供 version.json + .bin) |
| 2 | F407 OTA 传输 | 主: ESP32 UART4 中继 + 备: UART5 RS232 直连 |
| 3 | F103 OTA | 排除，一次性 USB 烧录 |
| 4 | OTA Server 部署 | Go Docker 独立服务 (net/http + chi + pgx + paho) |
| 5 | F407 Bootloader | 32KB (Sector0-1) + 外部 SPI Flash 暂存 |
| 6 | UART 协议扩展 | 3 下行 (0x20-0x22) + 1 上行 (0x23)，CRC16+CRC32 双重校验 |
| 7 | 版本管理 | 语义版本 + 单一 version.json 含所有平台 |
| 8 | ESP32 集成 | 双 OTA 分区 + HTTP 明文 + 主程序 OTA 检查任务 + 中继模块 |
| 9 | 源码结构 | 10 源文件 Go Docker，Phase 1 仅主通道，rs232-gateway 下行 Phase 2 |
| 10 | MQTT + DB | status/version 双 topic + firmware_versions/upgrade_history 双表 |
| 11 | 升级流程 + 安全 + 测试 | 端到端双平台流程 + SHA256 无签名 + 分层测试 |

### OTA 系统待实现清单

#### Orange Pi 侧 (feature/ota-server — 当前分支)

| 状态 | 文件 | 用途 |
|------|------|------|
| ✅ | `services/ota-server/` (14 files) | Go Docker 服务源码 |
| ✅ | `config/ota-server.yaml` | 4-section 配置 |
| ✅ | `docker/timescaledb/init.sql` | firmware_versions + upgrade_history 表 |
| ✅ | `docker/docker-compose.yml` | ota-server 服务编排 |
| ✅ | `services/ota-server/internal/config/config_test.go` | 配置加载 + DSN + 环境变量展开 |
| ✅ | `services/ota-server/internal/filestore/store_test.go` | 固件存储 + 路径遍历防护 + 轮转 |
| ✅ | `services/ota-server/internal/handler/health_test.go` | 健康端点 (DB/MQTT 状态) |
| ✅ | `services/ota-server/internal/handler/version_test.go` | version.json + 固件下载 + 路径验证 |
| ✅ | `tests/ota-server/test_end_to_end.py` | 17 个集成测试 (Python + pytest) |
| ⬜ | 运行 Go 单元测试 | `go test ./internal/...` on Orange Pi |
| ⬜ | 运行 Python 集成测试 | `pytest tests/ota-server/test_end_to_end.py -v` |

#### ESP32-S3 侧 (firmware/esp32-gateway/)

| 状态 | 任务 | 说明 |
|------|------|------|
| ⬜ | 分区表改造 | `partitions.csv`: factory → ota_0/ota_1/otadata (每分区 ~1.9MB) |
| ⬜ | sdkconfig 修改 | `CONFIG_OTA_ALLOW_HTTP=y` (内网 HTTP 下载) |
| ⬜ | Flash/PSRAM 确认 | 确认焊装的 ESP32-S3 实际 Flash 大小 (sdkconfig.defaults 写 16MB vs 分区表注释 4MB) |
| ⬜ | 主程序集成 | `esp32-gateway.c`: `app_main()` 调用 `ota_update_init()` |
| ⬜ | OTA 检查任务 | 新建 FreeRTOS 任务 (低优先级, 1KB栈): 每小时 GET `/firmware/version.json` → 版本比较 → HTTP下载+升级 |
| ⬜ | F407 中继模块 | 新建 `components/ota_relay/ota_relay.c`: HTTP下载F407固件→PSRAM→UART4 CMD_OTA_BEGIN/DATA/END 中继 |
| ⬜ | OTA 状态 MQTT 上报 | 通过现有 MQTT 发布升级进度到 `EdgeVib/{site}/ota/{device_id}/status` |
| ⬜ | CMakeLists 依赖 | `main/CMakeLists.txt` REQUIRES 添加 `ota_update` + `ota_relay` |

#### STM32 F407 侧 (firmware/stm32_node_vibration/)

| 状态 | 任务 | 说明 |
|------|------|------|
| ⬜ | Bootloader 工程 | 新建独立 CubeIDE 工程或构建目标: Sector0-1(32KB), 无RTOS, 极简 UART4+SPI2 |
| ⬜ | SPI Flash 驱动 | 新建 `bsp/spi_flash/w25q128.c`: 读写擦除 W25Q128 (SPI2, PI0=CS/PI1=SCK/PI3=MOSI/PI2=MISO) |
| ⬜ | 备份 SRAM 升级标志 | Bootloader 入口: 检查 BKP_SRAM 魔数 `0x0TAF407` → 决定升级或跳转App |
| ⬜ | OTA 命令处理 | `Modules/protocol/` 新增: CMD 0x20(OTA_BEGIN) → 复位进Bootloader; Bootloader 内处理 0x21(OTA_DATA) + 0x22(OTA_END) |
| ⬜ | UART4 极简驱动 | Bootloader 内轮询 UART4 RX (无RTOS, 无DMA, 无中断), 与协议 10 状态机帧解析对齐 |
| ⬜ | CRC32 硬件加速 | 使用 F407 内建 CRC32 单元 (AHB1, `CRC->DR`), 校验固件完整性 |
| ⬜ | Flash 擦除+编程 | Bootloader HAL_FLASH 擦除 Sector2-11, 从 SPI Flash 逐页拷贝到内部 Flash |
| ⬜ | App 复位入口 | App 端 `main.c` 新增: 检测 CMD_OTA_BEGIN (0x20) → 写 BKP_SRAM 魔数 → `NVIC_SystemReset()` |
| ⬜ | 心跳扩展 | 系统状态帧 CMD 0x07 增加 `firmware_version` 字段 (当前8字节→扩展payload) |
| ⬜ | CubeMX IOC 修改 | 启用 SPI2 (Full Duplex Master)、确认备份 SRAM 时钟使能 (PWR 模块) |

#### rs232-gateway OTA 备通道 (Phase 2)

| 状态 | 任务 | 说明 |
|------|------|------|
| ⬜ | 下行能力改造 | `rs232-gateway` 新增 MQTT 订阅 → serial write → 等待 F407 响应 → MQTT publish |
| ⬜ | 主/备切换状态机 | F407 端监测 ESP32 UART4 心跳, 丢失>3s切UART5, 恢复后切回 (与数据路径一致) |
| ⬜ | OTA 命令路由 | rs232-gateway 根据 MQTT topic 路由 CMD_OTA_* 到串口或 HTTP |
