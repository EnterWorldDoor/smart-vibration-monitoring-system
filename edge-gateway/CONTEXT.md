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

## 服务架构总览

### 已完成并合并到 dev (10 个)

| # | 服务 | 语言/运行时 | 部署 | 职责 |
|----|------|-----------|------|------|
| 1 | data-aggregator | Go | Docker | MQTT → TimescaleDB 数据管道，去重+批量写入 |
| 2 | rs232-gateway | C | systemd | UART5/RS232 串口转 MQTT，F407 备份通道 |
| 3 | inference-engine | Python/ONNX | Docker | ONNX Runtime 推理：统计趋势+Autoencoder 异常检测 |
| 4 | llm-analyzer | Python/llama.cpp | Docker | Qwen2.5-1.5B 本地大模型故障报告生成 |
| 5 | vision-service | Python/OpenCV | systemd | USB 摄像头：定时基线+MQTT 事件触发拍照 |
| 6 | audio-monitor | Python/sounddevice | systemd | 连续声学监听：FFT 特征+异常触发保存 WAV |
| 7 | opcua-server | C (open62541) | systemd | OPC UA DA 标准节点暴露，SCADA 互操作 |
| 8 | api-server | Go (chi) | Docker | REST + WebSocket 对外数据服务 |
| 9 | ota-server | Go | Docker | 固件分发：上传/版本管理/HTTP 下载/MQTT 触发 |
| 10 | Grafana | — | Docker | 实时仪表盘 + SQL VIEW + HDMI Kiosk 现场看板 |

### Edge-AI PC 端（独立系统）

| 组件 | 语言 | 职责 |
|------|------|------|
| mqtt_subscriber | Python | 订阅 ESP32 训练数据 → CSV 落盘 |
| prepare_and_train | Python | 端到端训练管线：特征提取→1D-CNN→TFLite→C头文件 |
| onnx_converter | Python | Keras .h5 → ONNX 转换（供 inference-engine 消费） |
| tflite_converter | Python | .h5 → .tflite → C header（供 ESP32 ai_service 消费） |

Edge-AI PC 产出两个模型：
- **model_data.h** — CNN-LSTM 4 分类器 (TFLite Micro)，手动复制到 ESP32 固件
- **autoencoder.onnx** — 异常检测自编码器，手动 scp 到 Orange Pi

当前缺口：Edge-AI PC 与 Edge-Gateway 之间完全是手工操作——训练数据靠 ESP32 开发模式直连 PC Mosquitto，模型部署靠人肉 scp 和复制 C 头文件。

---

## 待开发模块优先级 (2026-05-28)

### P0: Model Deploy Service（模型部署服务）✅ 已完成 (2026-05-28)

**状态**: 已开发完成、Docker 部署、API 全端点验证通过。13 个架构决策见上方"Model Deploy Service 设计"章节。

**实现**: Go Docker 服务 (port 8091)，PC HTTP POST 推送 ONNX 模型 → 文件存储 + TimescaleDB 版本元数据 + MQTT 触发 inference-engine 热重载。

---

### P1: Training Data Sync（训练数据同步）

**问题**: 生产环境中 ESP32 连接 Orange Pi Mosquitto，训练数据困在 TimescaleDB 里，无法回流到 Edge-AI PC 做持续训练。当前"ESP32 直连 PC Mosquitto"仅在开发环境可行。

**目标**: 将 Orange Pi TimescaleDB 中的生产数据同步到 Edge-AI PC，打通 ML 训练闭环。

```
Edge-AI PC (训练) ←── HTTP 拉取 ──→ Orange Pi (api-server)
       │                                    │
       │  模型部署下行 (model-deploy)         │  数据采集上行 (data-aggregator)
       │  ─────────────────────────→         │  ←─────────────────────────
       │                                    │
       └─ 训练闭环 ──────────────────────────┘
```

**与 Model Deploy Service 的关系**: 一个管数据上行（Training Data Sync），一个管模型下行（Model Deploy），完成 ML 持续训练闭环。

预估内存: ~0MB（在现有 api-server 中新增端点，不增加独立服务）

#### 决策 1: 运行位置 (2026-05-29)

**选择**: PC 端拉取 (pull)，不在 Orange Pi 上运行推送服务

**Why**:
- Orange Pi 4GB 内存已跑 11 个服务，不要再加独立同步进程
- PC 端训练管线是主动方——它知道什么时候需要数据（训练开始前），Orange Pi 无从得知
- PC 端不可达不影响 Orange Pi 核心业务（数据采集不能停）

**Alternatives considered**:
- Orange Pi 推送: 增加 Orange Pi 内存压力，且需要常驻进程监控 TimescaleDB 变更

#### 决策 2: 传输协议 (2026-05-29)

**选择**: HTTP API 数据导出端点，在现有 api-server 中新增 `GET /api/v1/data/export`

**Why**:
- api-server 已有 pgxpool + TimescaleDB 连接 + chi 路由，加一个导出 handler 改动最小
- PC 端 `requests` 定时拉取 → CSV → 直接喂给 `prepare_and_train.py`，无缝接入现有训练管线
- 支持 `?from=&to=&sites=&devices=&format=csv` 过滤参数，避免无范围全量查询
- HTTP 响应流式写入 CSV 文件，内存峰值可控

**Alternatives considered**:
- PC 直连 TimescaleDB (port 5432): 暴露数据库到 LAN，耦合 DB schema，PC 需持有 DB 密码
- MQTT 桥接: 只做实时消息转发不做批量历史数据，data-aggregator 已消费消息不会二次转发

#### 决策 3: 数据格式 (2026-05-29)

**选择**: CSV 格式，与 edge-ai `training_data.csv` 列结构一致

```
timestamp_ms, dev_id, rms_x, rms_y, rms_z, overall_rms, peak_freq, peak_amp,
temperature_c, humidity_rh, label
```

**Why**: api-server 查询 `vibration_view` 已解构出全部所需字段，输出 CSV 后 PC 端 `prepare_and_train.py` 可零改动消费。ESP32 的 24 维特征由 `prepare_and_train.py` Step 1 的 `extract_features_from_rows()` 从 RMS 序列窗口内重新计算（FFT/偏度/峭度/频带能量），不需要 api-server 端预提取特征向量。

**Why CSV 而非 JSON/Parquet**: `prepare_and_train.py` 已硬编码 `csv.DictReader` 解析，保持兼容。数据量在模拟阶段 KV 级别，CSV 行式追加天然支持增量同步。

#### 决策 4: 同步策略 (2026-05-29)

**选择**: 增量同步 — PC 端维护 `last_sync_time` 游标，每次只拉取新数据追加到 CSV

**Why**: 增量同步在面试场景中可展示对大规模数据扩展性的思考。PC 端游标文件 `data_collection/last_sync.txt` 记录上次同步的最大 `timestamp_ms`，每次 `GET /api/v1/data/export?from=<last_sync_time>&format=csv`。即便当前模拟数据量小，增量架构一次性设计好，数据量增长后无 schema 变更。

**游标管理**:
- 位置: `edge-ai/data_collection/last_sync.txt`，纯文本存 ISO8601 时间戳
- 规则: HTTP 请求成功后更新游标，请求失败保留旧游标（下次自动重试覆盖）
- 首次同步: `last_sync.txt` 不存在 → `?from=1970-01-01T00:00:00Z` → 全量拉取

**Alternatives considered**:
- 全量同步: 简单但每次重新传输全部数据，大规模时带宽和时间浪费不可接受

#### 决策 5: API 端点设计 (2026-05-29)

**选择**: `GET /api/v1/data/export?from=<ISO8601>&to=<ISO8601>&sites=<csv>&devices=<csv>&format=csv`

**端点规格**:
| 参数 | 必填 | 默认 | 说明 |
|------|------|------|------|
| `from` | 否 | `1970-01-01T00:00:00Z` | 增量同步游标 |
| `to` | 否 | `NOW()` | 截止时间 |
| `sites` | 否 | 全部 | 逗号分隔 site_id 列表 |
| `devices` | 否 | 全部 | 逗号分隔 device_id 列表 |
| `format` | 否 | `csv` | `csv` 或 `json` |

**响应**: `Content-Type: text/csv`，`Content-Disposition: attachment`，流式写入，不在内存中攒全量结果。逐行 `SELECT ... FROM vibration_view WHERE time >= $1 ORDER BY time` → `csv.Writer.Write()`，未来数据量增长到 10 万行+时内存峰值仍为 O(1)。

**Why**: 端点放在 api-server 现有 `handler/` 下新增 `export.go`，复用 pgxpool + chi 路由。查询 `vibration_view` 已解构的平表列，CSV 列头直接对齐 edge-ai `training_data.csv`。不分页——模拟阶段数据量 KV 级别，`?from=` 增量查询天然限制单次返回量。

#### 决策 6: PC 端客户端 (2026-05-29)

**选择**: 独立脚本 `edge-ai/data_collection/http_sync.py`，与 `mqtt_subscriber.py` 并列，训练前手动运行

```bash
python data_collection/http_sync.py --api http://192.168.1.1:8080
python prepare_and_train.py
```

**Why**:
- `prepare_and_train.py` 是纯计算管线，不应混入网络 IO。分离关注点：数据获取 vs 模型训练
- 失败隔离：sync 失败（网络断开）不影响已有 CSV 和后续重试
- 与开发模式 `mqtt_subscriber.py` 并列——开发用 MQTT 直收，生产用 HTTP 拉取。`prepare_and_train.py` 不感知数据来源，始终读同一个 CSV 路径
- 增量游标 `last_sync.txt` 由 `http_sync.py` 管理，其他模块不关心

**Alternatives considered**:
- 嵌入 `prepare_and_train.py`: 网络 IO 耦合进计算管线，失败时整个训练中断
- 替换 `mqtt_subscriber.py`: 开发模式仍需 MQTT 直收（本地调试），不能删

#### 决策 7: 错误处理与容错 (2026-05-29)

**选择**: 独立脚本模式，Phase 1 不做自动重试，容错策略如下

| 场景 | 行为 |
|------|------|
| api-server 不可达（连接超时 5s） | WARN 日志 + 退出码 1 + 不更新 `last_sync.txt` |
| HTTP 200 但响应体为空（无新数据） | 正常退出 + 不更新游标 |
| HTTP 500/503 | ERROR 日志 + 退出码 1 + 不更新游标 |
| 下载中途连接断开 | CSV 不完整 → 不更新游标 → 下次重跑覆盖不完整行 |
| CSV 文件被手动删除 | `http_sync.py` 检测到 `last_sync.txt` 存在但 CSV 不存在 → 清空游标 → 全量拉取 |

**游标更新原子性**: 先完整写入 CSV，成功后更新 `last_sync.txt`。如果 CSV 写入中途失败，游标不动，下次重跑自动覆盖不完整数据。

**Why Phase 1 不做自动重试**: 独立脚本模式，重试交给操作员或 cron 定时任务。`http_sync.py` 自身保持简单——跑一次、成功或失败、退出。网络在 192.168.1.x LAN 内可靠，HTTP 超时是小概率事件。Phase 2 可加 `--retry 3 --retry-delay 30` 参数。

#### 决策 8: ML 训练闭环 — 与 Model Deploy Service 的关系 (2026-05-29)

**选择**: Training Data Sync（数据上行）和 Model Deploy（模型下行）各自独立，通过 `prepare_and_train.py` 串联

```
Edge-AI PC:
  http_sync.py          → training_data.csv          (数据上行，拉取)
  prepare_and_train.py  → .h5 + .tflite + .onnx      (训练+转换)
  onnx_converter.py     → POST /api/v1/models/deploy  (模型下行，推送)
                          └── model-deploy → MQTT → inference-engine 热重载
```

**Why**: 两者没有直接代码依赖，互不感知。`http_sync.py` 不知道有 model-deploy，`model-deploy` 不知道有 training data sync。唯一的耦合点是 `prepare_and_train.py` 的成功执行——数据拉取和模型推送都以此为前提。各组件可独立测试、独立部署。

**完整自动化链路** (PC 端一键执行):
```bash
python data_collection/http_sync.py --api http://192.168.1.1:8080
python prepare_and_train.py
python deployment/onnx_converter.py        # 训练后自动 POST 到 model-deploy
```
Orange Pi 侧 inference-engine 收到 MQTT `model/reload` → `Ort::Session` 热替换 → 新模型生效。全过程不需要操作员介入。

#### 决策 9: ESP32 模型自动部署 — 模型与固件分离 (2026-05-29)

**选择**: ESP32 TFLite 模型不嵌入固件 (`model_data.h`)，改为运行时从 SPIFFS/FAT 分区加载。模型文件通过 OTA Server HTTP 独立分发

**Why**:
- 当前"编译时嵌入模型"模式每次模型更新需要重新编译固件 → 1.2MB 全量 OTA 烧录，模型仅 200KB 却要整个固件重传
- "模型与固件分离"后模型更新只需 HTTP 下载 200KB `.tflite` 到 models 分区，ESP32 运行时 `TfLiteModelCreate(file_buffer)` 从分区加载并热切换
- ESP32-S3 有 8MB PSRAM，TFLite arena + 模型可全放 PSRAM，不占 SRAM
- 这是完整 ML 闭环的最后一块拼图——不解决则"自动化训练闭环"讲不通

**ESP32 固件改动**:
- **分区表**: 新增 `models` FAT/SPIFFS 分区 (0.7MB)，与 OTA 的分区表改造一起做
- **ai_service**: `model_data.h` 替换为分区文件加载 (`esp_spiffs_init()` + `fopen` + `fread`)
- **模型更新任务**: 新 FreeRTOS 任务（低优先级），轮询 OTA Server `version.json` 发现新 TFLite 版本 → HTTP 下载到 models 分区 → 校验 SHA256 → 通知 `ai_service` 热切换
- **MQTT 上报**: 模型版本随 health heartbeat 上报到 `EdgeVib/{site_id}/ota/{device_id}/version`

**model-deploy 扩展**:
- 新增 `.tflite` 文件接收 + 存储（现有 `filestore/store.go` 支持任意文件类型，无需改动）
- OTA Server `version.json` 中 `esp32` section 的 `file` 字段指向 model-deploy 存储的 `.tflite` 路径
- 或更简单：model-deploy POST 接收 `.tflite` + metadata → 存储到 `/opt/edge-gateway/firmware/esp32/` → OTA Server 已有的 HTTP 文件服务直接暴露

**数据流**:
```
Edge-AI PC:
  prepare_and_train.py → deployment/models/edgevib_classifier.tflite
  → POST /api/v1/models/deploy (model_name=esp32_classifier, file=.tflite)
  → model-deploy 存储到 /opt/edge-gateway/firmware/esp32/
  → OTA Server version.json 更新 esp32.latest_version + file 字段

ESP32-S3:
  模型更新任务 → GET /firmware/version.json (每小时轮询)
  → 发现新版本 → GET /firmware/esp32/<version>.tflite
  → SHA256 校验 → 写入 /models/ 分区
  → MQTT 通知 ai_service 热切换新模型
```

**Why 现在做**: Training Data Sync + Model Deploy 都已自动化，ESP32 模型是唯一手动的环节。不做则闭环缺一环，面试时故事不完整。OTA Server 决策 8 已经规划了分区表改造，这次一起做避免二次返工。

**Alternatives considered**:
- 留在 Phase 2: 本次 P1 范围过窄，只做数据上行不解决 ESP32 模型自动部署，闭环不完整
- 保持 `model_data.h` 嵌入式: 每次模型更新 = 重新编译固件 + 全量 OTA，太重

#### 决策 10: 测试策略 (2026-05-29)

**选择**: 双层测试 — api-server Go 单元测试 + edge-ai Python 测试，ESP32 集成测试延后

| 层级 | 测试对象 | 验证点 |
|------|---------|--------|
| **Go 单元** | `handler/export.go` | mock pgxpool: SQL 参数化正确性、CSV 格式化、`?from=`/`?to=` 参数校验、空结果集处理 |
| **Go 单元** | `handler/deploy.go` (扩展) | `.tflite` 文件接收、`platform` 字段解析、文件落入 OTA 共享目录 |
| **Python** | `http_sync.py` | mock `requests.get`: 增量游标读写、CSV 追加、断点恢复（游标+CSV不一致时全量重拉）、空响应处理 |

**Why 只做两层**:
- api-server export 端点逻辑简单（VIEW→CSV 流式输出），mock pgxpool 的单元测试覆盖核心路径即可
- `http_sync.py` 是独立脚本，输入（HTTP 响应）和输出（CSV 文件）都是文件系统操作，mock HTTP 即可在 PC 端完整测试
- ESP32 集成测试需物理硬件+场地，当前不具备条件，延后到有实验环境时再做

#### 决策 11: 安全策略 (2026-05-29)

**选择**: 沿用 api-server 现有安全模型 — 无认证 + 预留 `X-API-Key` middleware

**Why**: Training Data Sync 在 192.168.1.x 私有 LAN 内运行，攻击者如已接入内网可直接连 Mosquitto/TimescaleDB/Grafana。`GET /api/v1/data/export` 是纯只读端点，暴露的数据与 Grafana 面板查询一致。安全加固留在整个系统功能完备后统一做（TLS、认证、固件签名），现在逐点加认证不改变威胁面但增加开发摩擦。

---

### Training Data Sync 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 运行位置 | PC 端 pull（不增加 Orange Pi 负担） |
| 2 | 传输协议 | HTTP API（api-server 新增 `GET /api/v1/data/export`） |
| 3 | 数据格式 | CSV（与 edge-ai `training_data.csv` 一致） |
| 4 | 同步策略 | 增量（PC 端 `last_sync.txt` 游标） |
| 5 | API 端点 | `?from=&to=&sites=&devices=&format=csv`，流式响应 |
| 6 | PC 端客户端 | 独立脚本 `http_sync.py`，与 `mqtt_subscriber.py` 并列 |
| 7 | 错误处理 | 游标原子性（先写 CSV 后更新游标），Phase 1 无自动重试 |
| 8 | ML 闭环 | 数据上行（sync）和模型下行（deploy）独立，通过训练脚本串联 |
| 9 | ESP32 模型部署 | 模型与固件分离（SPIFFS 分区 + 运行时加载），自动分发 |
| 10 | 测试策略 | 双层：Go 单测 + Python mock 测试，ESP32 集成测试延后 |
| 11 | 安全 | 无认证 + 预留 middleware，系统完备后统一加固 |

---

### P2: System Monitoring（基础设施监控）

**问题**: 10 个服务运行在 Orange Pi 4GB 内存约束下，没有统一的 metrics 采集和告警。

**目标**: Prometheus + Node Exporter 采集宿主机 + 容器资源指标，Grafana 统一告警。

**预估内存**: ~80MB (Prometheus ~50MB + Node Exporter ~10MB + cAdvisor ~20MB)。

#### 决策 1: 监控范围 — 基础设施层 vs 应用层 (2026-05-30)

**选择**: Phase 1 只做**基础设施层**监控（CPU/内存/磁盘/网络/容器资源），应用层健康（服务心跳/错误计数/推理延迟）延后。

**Why**:
- 应用层已有 MQTT `status/health` topic 周期性上报（data-aggregator: 30s, inference-engine: 30s, llm-analyzer: 30s, vision-service: 30s），Grafana 设备状态面板已消费这些 MQTT 消息
- 应用层监控需要定义统一 metrics schema（service_name, metric_name, labels, value），这涉及跨 10 个服务的代码改动，不是纯配置工作
- 先解决最紧迫的——Orange Pi 4GB 内存 OOM 风险、磁盘爆满、CPU 过热——这些是基础设施告警，不需要改应用代码

**监控分层**:
| 层 | 采集器 | 监控对象 | Phase |
|----|--------|---------|-------|
| 宿主机 | Node Exporter | CPU/内存/磁盘/网络/温度/系统负载 | **P2 (本次)** |
| 容器 | cAdvisor | 每个 Docker 容器的 CPU/内存/IO/网络 | **P2 (本次)** |
| 应用 | 未来统一 metrics SDK | 服务心跳/推理延迟/错误率/MQTT 消息速率 | P4 (后续) |

**Alternatives considered**:
- 应用层一并做: 跨 10 个服务改代码，范围过大。且 Prometheus 内存开销随 time-series 数量线性增长，应用 metrics 一上 Prometheus 内存可能从 50MB 涨到 150MB+，在 4GB 约束下需要先观察基础设施基线再做

#### 决策 2: 部署方式 — 全 Docker Compose 统一编排 (2026-05-30)

**选择**: Prometheus + Node Exporter + cAdvisor 全部 Docker 部署，加入 `docker/docker-compose.yml`

**Why**:
- 三个组件都不需要硬件设备节点（与 systemd 的硬件直通分界线一致）。Node Exporter 只需 `-v /:/host:ro` bind mount 读宿主机 `/proc`/`/sys`，cAdvisor 只需 `-v /var/run/docker.sock:/var/run/docker.sock`
- Docker 统一编排方便一键启停、日志采集走 `docker logs`、内存限制 `--memory` 防 OOM
- Prometheus 数据目录通过 Docker volume 持久化，与 TimescaleDB/Grafana/Mosquitto 数据管理一致
- 腾讯云 Docker 镜像加速已有配置，无需额外拉取源

**Alternatives considered**:
- 混搭 (Prometheus Docker + Node Exporter systemd): Node Exporter 裸机安装可减少一层 bind mount，但增加一个 systemd unit 的运维负担。bind mount `/:/host:ro` 在 Docker 中已足够获取宿主机指标
- 全 systemd: 与现有 Docker 服务管理方式割裂，且 cAdvisor 是 Go 二进制直接监控 Docker daemon，systemd 部署无优势

#### 决策 3: 数据保留 & 内存预算 (2026-05-30)

**选择**: Prometheus TSDB 7 天保留，P2 新增内存上限 80MB，全量 headroom 从 ~1000MB 降至 ~920MB

**Why**:
- Orange Pi 4GB 总内存，P2 后 15 个组件合计 ~3080MB，剩余 headroom ~920MB，安全
- 7 天保留对 Prometheus TSDB 足够——超过一周的趋势分析走 Grafana 查 TimescaleDB（已有 30 天+历史数据），Prometheus 只做短期告警
- SD 卡 32GB，Prometheus 7 天数据约 ~100-150MB 磁盘占用，不构成约束

**Docker memory limit**: Prometheus `--memory=80m`（含 TSDB head block），cAdvisor `--memory=30m`，Node Exporter `--memory=15m`

**Alternatives considered**:
- 15 天保留 (Prometheus 默认): 磁盘 ~200-300MB 可接受但 TSDB head block 随 active series 增长，3000 series × 15d 可能 80-100MB 内存，在 4GB 约束下偏激进

#### 决策 4: 采集频率 & Phase 1 告警规则 (2026-05-30)

**选择**: 30s scrape interval + 7 条告警规则

**Why 30s**: 工业场景基础设施告警（OOM/磁盘满/CPU 过热）不需要 15s 级响应。30s 间隔将 TSDB head block 内存从 ~50MB 降至 ~30MB。Prometheus evaluation_interval 同样 30s，与 scrape interval 对齐。

**Phase 1 告警规则**:

| 规则 | PromQL 条件 | 级别 | 说明 |
|------|------------|------|------|
| 内存 > 85% | `node_memory_MemAvailable / node_memory_MemTotal < 0.15` | Warning | 4GB 中 < 600MB 空闲 |
| 内存 > 95% | 同上 `< 0.05` | Critical | OOM 前最后警告 |
| 磁盘 > 80% | `node_filesystem_avail / node_filesystem_size < 0.20` | Warning | SD 卡 32GB，< 6.4GB 空闲 |
| 磁盘 > 90% | 同上 `< 0.10` | Critical | |
| CPU 温度 > 80°C | `node_thermal_zone_temp` | Warning | A733 Tj=105°C，主动散热前预警 |
| 容器 OOM | `increase(container_oom_events_total[5m]) > 0` | Critical | Docker OOM killer 触发 |
| Prometheus 自身 | `up{job="prometheus"} == 0` | Critical | 监控系统自身存活 |

**不做 CPU 使用率告警**: A733 2×A76 + 6×A55，llama.cpp 推理时短期 spike 到 100% 是预期行为。持续高负载用 load average 趋势替代阈值告警。

**Alternatives considered**:
- 15s scrape: 告警延迟更低但 TSDB head block 多占 ~20MB 内存，在 4GB 约束下不划算

#### 决策 5: 告警通知通道 — 双通道 (2026-05-30)

**选择**: Grafana Dashboard 展示 + MQTT 推送，双通道并行。不做邮件/短信（无外部通道配置）

**Why**:
- **Grafana Dashboard 展示**: 主视觉通知通道，Prometheus alert rules 直接在 Grafana 面板显示 Warning/Critical 状态，零额外组件
- **MQTT 推送**: Prometheus Alertmanager → webhook → 轻量脚本 → MQTT `EdgeVib/system/monitoring/alert`，api-server WebSocket hub 消费并推送到 Web 前端
- 双通道给后续自动化留接口：声光报警器（digital_io alarm_service 已预留 GPIO 控制）、LLM 自动生成故障报告
- MQTT alert payload: `{severity, alert_name, description, value, threshold, timestamp}`

**Alternatives considered**:
- 纯 Grafana Dashboard: 零改动但告警不进入 MQTT 消息总线，自动化闭环无法消费
- 邮件/钉钉/企业微信: 需要 Orange Pi 有外网通道 + 配置 SMTP/API token，环境不具备

#### 决策 6: 告警引擎 — Prometheus Alertmanager (2026-05-30)

**选择**: Prometheus Alertmanager（与 Prometheus 同镜像自带），告警规则文件 `config/prometheus.rules.yml` Git 版本控制

**Why**:
- Alertmanager 与 Prometheus 同镜像内运行，零额外容器进程，~10MB 内存增量
- 告警规则以 YAML 文件版本控制，code review 可审计，`docker compose restart prometheus` 即生效
- 支持 silence 窗口（维护时段静音）+ inhibit 规则（如"主机 down 时抑制磁盘告警，避免告警风暴"）
- Phase 1 不需要 HA 模式 cluster（单节点 Orange Pi）

**Alternatives considered**:
- Grafana Alerting: 内置已有 Grafana，但告警规则在 UI 中配置不利于 Git 版本管理和 code review，且 inhibit/silence 功能不如 Alertmanager 成熟
- 独立 Alertmanager 容器: 功能更全面但多一个容器进程（~15MB），在 4GB 约束下无必要

---

### P2 System Monitoring 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 监控范围 | 基础设施层（Node Exporter + cAdvisor），应用层延后 P4 |
| 2 | 部署方式 | 全 Docker Compose 统一编排 |
| 3 | 数据保留 & 内存 | Prometheus TSDB 7 天，P2 新增 ~90MB，headroom ~910MB |
| 4 | 采集频率 | 30s scrape + 7 条告警规则 |
| 5 | 通知通道 | Grafana Dashboard + Alertmanager → MQTT webhook 双通道 |
| 6 | 告警引擎 | Prometheus Alertmanager（YAML rules，Git 版本控制） |

**源码结构** (P2 纯配置，无新增 Go/Python 代码):
```
edge-gateway/
├── config/
│   ├── prometheus.yml              — 主配置: scrape targets + alertmanager
│   └── prometheus.rules.yml        — 7 条告警规则
├── docker/
│   ├── docker-compose.yml          — 新增 prometheus, cadvisor, node-exporter 服务
│   └── alertmanager/
│       └── config.yml              — Alertmanager: 路由 + MQTT webhook receiver
├── scripts/
│   └── alert-webhook.py            — Alertmanager webhook → MQTT 转发 (≤30 行)
└── CONTEXT.md                      — 本文档
```

---

### P3: NTP Server（时间同步）

**问题**: ESP32 使用 boot-relative timestamp，data-aggregator 用 Orange Pi 本地时间做分区键。Orange Pi 时间不准则所有时序数据偏移。

**目标**: Orange Pi 做局域网 NTP Server，ESP32/STM32/PC 全部时间对齐。

**预估内存**: ~5MB (chrony systemd 服务)。主要是配置工作。

#### 决策 1: Orange Pi NTP 上游时间源 (2026-05-30)

**选择**: PC (192.168.1.100) 作为 Orange Pi 的唯一 NTP upstream

**Why**:
- PC 双网卡（WLAN 外网 NTP 已同步 + 以太网 192.168.1.100 直连 Orange Pi），时间天然可靠
- PC ↔ Orange Pi 是以太网直连，不经过 SSH 隧道、不经过代理链——NTP UDP 123 端口天然可达
- 不需要改造代理链（SSH 隧道只转发 TCP，NTP 是 UDP），零额外网络配置
- PC 开机→Windows NTP 自动同步→Orange Pi chrony 立即可用，零人工介入
- 即使 PC WLAN 断开（校园网故障），PC 本地 CMOS RTC 时钟仍可提供分钟级精度，chrony 的 drift file 机制可容忍上游短期不可达

**Orange Pi chrony 角色**: NTP client + NTP server 双角色
- **client**: upstream 指向 PC `192.168.1.100`（唯一上游）
- **server**: 监听所有接口（eth0 + wlan0），为局域网内 ESP32/STM32 提供时间

**网络拓扑对应关系**:
```
外网 NTP (pool.ntp.org)
    ↓ WLAN
PC (192.168.1.100) ──NTP server──→ Orange Pi (192.168.1.1 eth0)
                                      ↓ NTP server (wlan0 192.168.2.1)
                                   ESP32-S3 (WiFi AP 客户端)
```
PC 自身是 NTP client（从外网同步）+ NTP server（向 Orange Pi 提供时间）。Orange Pi chrony 是唯一局域网时间权威。

**Alternatives considered**:
- Orange Pi 通过 SSH 代理同步外网 NTP: SSH 隧道只转发 TCP，NTP 用 UDP 123，需要 socat/iptables 做 UDP→TCP 转换，复杂性远超收益
- 手动设置时间 + chrony free-run: A733 无电池 RTC，断电时钟归零；晶振漂移累积误差不可控
- 多个外网 NTP upstream: Orange Pi 无独立外网通道，eth0 gateway 是 PC 不转发

**部署验证 (2026-05-30)**:
- Orange Pi chrony 4.2 部署成功，PC upstream NTP 同步正常
- 同步精度: 偏差 -110μs，stratum 4，Reach=17（连续可达）
- `maxdistance 15.0` 必须: Windows w32time NTP server 默认 root dispersion 5-10s，chrony 默认 `maxdistance 1.0` 会拒绝此源
- chrony 监听 `0.0.0.0:123`（eth0+wlan0），等待 ESP32 客户端连接

#### 决策 2: ESP32 NTP 目标 (2026-05-30)

**选择**: ESP32 SNTP 客户端指向 Orange Pi chrony — `192.168.2.1:123`（WiFi AP 网关地址）

**Why**:
- ESP32 通过 WiFi 连接 Orange Pi 热点 `EdgeVib-AP`（子网 192.168.2.0/24），chrony 在 wlan0 接口监听
- 局域网一跳 UDP 123，零路由、零 NAT、零代理链依赖
- ESP32 不直接出外网——Orange Pi 外网通道是 SSH 隧道（TCP only），ESP32 无法独立到达外网 NTP
- `time_sync` 组件已有 fallback：SNTP 不可达时 `time_sync_get_timestamp_us()` 回退到 `esp_timer_get_time()`（boot-relative），系统正常运行。开发模式下 ESP32 连 PC 网络时 SNTP 自然超时，不影响数据采集

**ESP32 config_manager 默认值变更**:
| 字段 | 旧默认值 | 新默认值 | 说明 |
|------|---------|---------|------|
| `sntp_server1` | `pool.ntp.org` | `192.168.2.1` | Orange Pi chrony |
| `sntp_server2` | `time.google.com` | 空字符串（禁用） | 局域网单上游，不依赖外网 |

**Why 单上游**: 局域网只有一个 chrony 实例。ESP32 `time_sync` 组件支持最多 4 个 NTP 服务器，但多服务器设计是为外网容灾——同一局域网内配置多个 chrony 实例没有意义。chrony 自身不可达 = Orange Pi 挂了 = 整个系统离线，ESP32 时间戳退化为 boot-relative 不影响安全性。

**Alternatives considered**:
- ESP32 继续指向外网 NTP (pool.ntp.org): Orange Pi 外网走 SSH TCP 隧道，UDP 123 不通；即使配 IP forwarding，校园网限制单设备登录使 ESP32 无法直接出网

#### 决策 3: STM32（F407 + F103）时间同步范围 (2026-05-30)

**选择**: Phase 1 (P3) 不做 STM32 时间同步，只覆盖 Orange Pi + ESP32

**Why**:
- P3 核心目标是"保证 TimescaleDB 时序数据时间戳一致性"，ESP32 + Orange Pi 时间对齐已达成——数据经 ESP32 打时戳后入 TimescaleDB，STM32 无时间不影响数据完整性
- STM32F407/F103 在 ESP32 上游——它们的数据由 ESP32 在 MQTT 发布时提供时间戳，不依赖 STM32 本地时钟
- F407 TFT LCD 显示设备状态（安全状态/健康等级/运行模式），不是时钟应用，显示 uptime 足够
- 新增 UART4 时间下发协议命令（ESP32→F407）增加约 50 行协议代码，Phase 1 不做
- STM32 时间同步延后到 P4（应用层监控），届时统一评估是否需要新增 `CMD_TIME_SYNC`

**Alternatives considered**:
- 新增 UART4 CMD 0x19（时间广播）: ESP32 每 60s 向 F407 下发 Unix 时间，F407 再通过 CAN 转发给 F103。功能完整但为 LCD 时钟显示引入协议变更，性价比低，延后

#### 决策 5: ESP32 MQTT `timestamp_ms` 保持 boot-relative 不变 (2026-05-30)

**选择**: ESP32 MQTT JSON 中 `timestamp_ms` 继续使用 `esp_timer_get_time()` 的 boot-relative 值，不切换为 Unix epoch

**Why**:
- data-aggregator 去重依赖 `(device_id, timestamp_ms, source_path)` 三元组精确匹配，`timestamp_ms` 的单调性至关重要
- ESP32 SNTP 同步丢失时（Orange Pi 宕机/WiFi 断开），`time_sync` 组件 fallback 到 `esp_timer_get_time()`——如果 MQTT 中已切换为 Unix epoch，数值会从 ~1.7e12 跳变到 ~2e6，去重行为不确定
- TimescaleDB 的正确时间戳由 data-aggregator 用 Orange Pi 本地时间保证——Orange Pi 现在有 chrony（决策 1+4），写入 `sensor_data.time` 分区键的时间是准的
- ESP32 JSONB payload 中 `timestamp_ms` 的语义是"数据产生时刻相对于本设备启动的时间"，用于去重和溯源；绝对时间由 Orange Pi 侧记录

**ESP32 获得 NTP 同步后的实际使用**:
- 自身日志系统输出人类可读时间戳
- MQTT `status/health` topic 上报 `ntp_synced: true/false` 和 `unix_time` 字段
- 定时任务调度（如 OTA 固件每小时轮询）
- MQTT 数据 payload 格式不变，与 data-aggregator 零耦合

**Alternatives considered**:
- 新增独立字段 `unix_timestamp_ms`: 与 `timestamp_ms` 并存，JSON payload 增大 13 字节（字段名 + 值），但下游无消费者需要，不是现在加的理由

#### 决策 6: NTP 时间同步状态监控 (2026-05-30)

**选择**: ESP32 MQTT health heartbeat 上报 `ntp_synced` 字段 + chrony 进程存活由 P2 Node Exporter systemd collector 覆盖

**Why**:
- ESP32 `time_sync` 组件已有完整同步状态管理（`SYNC_STATUS_SYNCED/FAILED`），在 `status/health` MQTT 消息中加一个 bool 字段 ~5 行代码
- Grafana 设备状态面板直接读取 `ntp_synced` 展示——ESP32 时间未同步时显示黄色警告
- chrony 进程存活由 P2 Node Exporter 的 `--collector.systemd` 自动采集 `node_systemd_unit_state{name="chrony.service"}`，不需要额外 exporter
- 不新增 Prometheus 告警规则——P3 只做配置工作，告警规则延后到 P4（应用层监控统一做）

**ESP32 health JSON 变更**:
```json
{
  "device_id": "esp32-01",
  "uptime_s": 3600,
  "ntp_synced": true,        // 新增: time_sync_is_synchronized()
  "ntp_server": "192.168.2.1" // 新增: 当前连接的 NTP 服务器
}
```

**Alternatives considered**:
- 安装 chrony Prometheus exporter: 增加一个 Python/Go 进程（~5MB 内存），P3 是配置工作不引入新二进制
- 零监控: 时间同步是数据一致性的基础，不监控会导致"Orange Pi 时钟漂移了几小时才发现"的运维事故

---

### P3 NTP Server 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | Orange Pi NTP 上游 | PC (192.168.1.100) — 以太网直连，零代理链依赖 |
| 2 | ESP32 NTP 目标 | Orange Pi chrony (192.168.2.1) — WiFi AP 网关一跳 |
| 3 | STM32 时间同步 | Phase 1 不做 — 不影响 TimescaleDB 数据一致性 |
| 4 | chrony 配置 + PC NTP Server | `server 192.168.1.100 iburst` + `allow 192.168.2.0/24` + PC w32time |
| 5 | ESP32 MQTT timestamp_ms | 保持 boot-relative 不变 — 去重兼容 |
| 6 | 监控 | ESP32 health heartbeat `ntp_synced` + Node Exporter systemd collector |

### P3 实施清单

**Orange Pi 侧（1 个新增文件 + 1 个脚本追加）**:

| 文件 | 操作 | 用途 |
|------|------|------|
| `config/chrony.conf` | **新增** | chrony 配置（upstream PC + allow LAN） |
| `scripts/setup-ubuntu.sh` | **追加** | `apt install chrony` + 拷贝配置 + enable 服务 |

**ESP32 侧（2 个文件修改）**:

| 文件 | 操作 | 用途 |
|------|------|------|
| `components/config_manager/config_manager.c` | **修改** | `sntp_server1` 默认值 `"pool.ntp.org"` → `"192.168.2.1"` |
| `main/esp32-gateway.c` | **修改** | health heartbeat 加 `ntp_synced` + `ntp_server` 字段 |

**PC 侧（1 个新增脚本）**:

| 文件 | 操作 | 用途 |
|------|------|------|
| `scripts/setup-win-ntp.ps1` | **新增** | 启用 Windows w32time NTP Server |

**不需要改动的服务**: data-aggregator, inference-engine, llm-analyzer, vision-service, audio-monitor, api-server, ota-server, model-deploy, opcua-server, rs232-gateway — 它们使用 Orange Pi `datetime.now(UTC)`，chrony 安装后全系统时间自动生效。

**部署验证**:
```bash
# Orange Pi
chronyc sources | grep 192.168.1.100    # PC upstream 可达
chronyc clients                          # ESP32 已连接
# ESP32 串口
grep "SNTP synchronized"                 # 同步成功日志
# MQTT
mosquitto_sub -t "EdgeVib/+/gateway/+/status/health" | jq .ntp_synced  # true
```

**完整时间同步链路**:
```
外网 NTP (pool.ntp.org)
    ↓ WLAN
PC w32time (192.168.1.100) ──NTP──→ Orange Pi chrony (192.168.1.1 eth0)
                                        ├── NTP server (wlan0 192.168.2.1)
                                        │       ↓ SNTP
                                        └──── ESP32-S3 (time_sync)
                                               ├── ntp_synced=true (MQTT health)
                                               └── timestamp_ms 保持 boot-relative (MQTT data)
```

---

## Linux 内核驱动开发路线图 (2026-05-27)

EdgeVib 当前架构中 Orange Pi 4 Pro 所有 I/O 均走内核现成驱动（USB UVC、USB-Serial、TCP/IP）。以下为紧贴业务的 Linux 内核驱动开发入口，全部**零额外硬件成本**（除 #7）。

| 优先级 | 驱动模块 | 子系统 | 知识点 | 硬件 | 工作量 |
|--------|---------|--------|--------|------|--------|
| **D1** | Virtual CAN | 网络设备 | net_device_ops + SocketCAN + sk_buff + NAPI | 零 | 2-3天 |
| **D2** | IIO Vibration Device | IIO + 字符 | iio_dev + sysfs + trigger + buffer | 零 | 2-3天 |
| **D3** | RAM Buffer Block Device | 块设备 | gendisk + blk-mq + kmap | 零 | 2-3天 |
| **D4** | Software RTC Driver | RTC + 平台 | rtc_device + platform_driver + of_match | 零 | 1-2天 |
| **D5** | HWMON Motor Health | hwmon + sysfs | hwmon_device + thermal_zone | 零 | 1-2天 |
| **D6** | E-Stop Input Device | input | input_dev + evdev | 零 | 1天 |
| **D7** | MIPI CSI V4L2 Driver | V4L2 + DT + DMA | v4l2_subdev + media_controller + DT overlay | CSI摄像头 | 5-7天 |

### D1: Virtual CAN (SocketCAN) — 详细设计

**业务关联**: NDE CAN 特征帧到达 Orange Pi 后无法被 Linux CAN 工具链消费。

**核心架构**:
```
F103 NDE 节点
  └→ CAN Bus (500kbps, SN65HVD230)
      └→ F407 CAN1 RX → 现有: 17帧重组→特征向量→ESP32 (数据主线)
                       → 新增: 逐帧旁路 CMD 0x1A→ESP32→MQTT→Orange Pi (诊断旁路)
                                                              ↓
                                              edgevib-can-d(aemon) → SocketCAN
                                                              ↓
                                              vcan_edgevib (自写内核模块)
                                                              ↓
                                        candump / cansniffer / Wireshark
```

**三个面试业务价值故事**:
1. **远程 CAN 诊断**: 工程师在 SCADA 终端 `candump vcan_edgevib`，实时看到电机 NDE 端 CAN 流量，替代手持 CAN 分析仪去现场
2. **CAN 总线健康监控**: SocketCAN 统计帧间隔抖动、总线负载趋势 → 预判 CAN 收发器老化 → 计划停机更换
3. **独立验证与安全审计**: 原始 CAN 帧回放验证 F407 重组逻辑正确性 → 定位是固件 bug 还是传感器真实故障

#### 决策 1: 内核模块 vs 内置 vcan (2026-05-30)

**选择**: 自写内核模块 (`.ko`)，不使用 Linux 内置 `vcan.ko`

**Why**:
- D1-D7 首要目标是**学习价值 + 业务价值并存**。自写内核模块覆盖 `net_device_ops` + `alloc_can_skb` + `NAPI` + SocketCAN 子系统全部知识点；内置 vcan 一行 `ip link add type vcan` 命令即可创建但学不到内核编程
- 自定义 sysfs 属性暴露 CAN 总线健康指标（帧速率、CRC 错误率、总线负载百分比），这是面试时可深入讲的"设备模型"
- 接口命名 `vcan_edgevib` 而非 `vcan0`，与 EdgeVib 系统命名体系一致，`ip link show` 可识别

**Alternatives considered**:
- Linux 内置 `vcan.ko`: 零代码、零维护，但失去全部学习价值和自定义能力。`CONFIG_CAN_VCAN=m` 是编译时选项，Orange Pi 内核可能未开启

#### 决策 2: CAN 帧数据注入方案 (2026-05-30)

**选择**: 方案 A — F407 CAN ISR 逐帧旁路上行

**Why**:
- F407 `HAL_CAN_RxFifo0MsgPendingCallback` 每收到 CRC8 校验通过的 CAN 帧后，通过 UART4 新增 CMD 0x1A 把原始 `{can_id, dlc, data[8], timestamp}` 旁路发送
- ESP32 收到 CMD 0x1A 后不做解析，透传到新 MQTT topic `EdgeVib/{site_id}/can/{device_id}/raw`
- Orange Pi `edgevib-can-d` daemon 订阅此 topic → `socket(AF_CAN)` → `write(fd, can_frame)` 注入内核模块
- 不改动 F407 现有 17 帧重组 → 特征向量 → CMD 0x17 的主数据管线

**业务完整性**: 逐帧 CRC8 校验结果也旁路（新增 1 字节 status: bit0=CRC pass/fail, bit1=seq duplicate），这样 Orange Pi 侧能看到 F407 实际接收质量——面试时可以展示"CRC 错误率趋势 → 预判 CAN 收发器老化"的完整闭环

**Alternatives considered**:
- B. ESP32 逆向重构: 从 96 字节特征向量"拆回" 17 个 CAN 帧 → 逆向算法不完美（帧边界信息 lossy），失去 CRC8 校验历史的真实语义
- C. 物理 CAN 嗅探器 (CANable): 需要额外硬件（~$30），且有了物理接口就不需要虚拟 CAN 了，D1 失去存在意义

#### 决策 3: 数据方向 — Phase 1 单向 (2026-05-30)

**选择**: Phase 1 纯单向（F103 → Orange Pi），Phase 2 再做反向（Orange Pi → F103）

**Why**:
- 单向已支撑远程诊断、总线健康监控、独立验证三个核心业务场景
- 反向引入的复杂度：MQTT 反向订阅 topic、虚拟注入帧与真实 CAN 帧的仲裁、`cansend` 误操作导致 F103 行为异常的安全风险
- 反向留给 D1 Phase 2 — 与 OTA 系统的 rs232-gateway 下行改造一起做（两者都是"用户空间→MQTT→串口/CAN→设备"模式，可共享架构方案）

#### 决策 4: UART 协议 CMD 0x1A — 攒批发送 (2026-05-30)

**选择**: ISR 写 ring buffer + 主循环 100ms 攒批 flush，不直接在 ISR 内调 UART TX

**CMD 0x1A 帧格式** (F407 → ESP32):
```
[AA55] [LEN_H LEN_L] [DEV=0x01] [CMD=0x1A] [SEQ] [N_frames(1B)] [frame_entry × N] [CRC16] [0D]

frame_entry (每帧 14 字节):
  [can_id:4B LE] [dlc:1B] [data:8B] [flags:1B]
    flags bit0: CRC8 pass(1)/fail(0)
    flags bit1: seq_duplicate
    flags bit2-7: reserved
```

**Why**:
- **ISR 纯洁性**: CAN RX ISR 只需 `ring_buf[wr++] = frame_entry` + 更新计数，<2μs，不碰 UART DMA 寄存器。17 帧背靠背到达时无重入竞态
- **带宽利用**: 100ms 攒批期内最多累积 ~2 帧（CAN 总流量 ~10 帧/s），一帧 UART 协议帧承载 2 个 CAN 帧 ≈ 40 字节，仍远低于 `PROTO_PAYLOAD_MAX_SIZE=128`
- **时序可选**: 主循环在 `app_enterprise` 任务 (1s 周期) 内，复用其 tick。或者单开 100ms 定时器 — 精确到时序决策时再定

**Ring buffer 规格**: 64 槽位（17 帧 MAX × 3 批余量），`can_frame_entry[64]` 静态分配 ~900 字节，不占 malloc

**Alternatives considered**:
- ISR 直接 UART DMA TX: ISR 执行时间被 DMA 寄存器配置拉长，背靠背帧有竞态风险
- 每 1s flush (复用 app_enterprise): 延迟过大，`candump` 用户感知 1s 卡顿

#### 决策 5: MQTT Topic 与 Payload 格式 (2026-05-30)

**选择**: JSON 批量数组 + hex 字符串编码 CAN data 字段

**MQTT Topic**: `EdgeVib/{site_id}/can/{device_id}/raw` (QoS 1, Retained=false)

**JSON Payload**:
```json
{
  "device_id": "nde01",
  "batch_seq": 123,
  "t_esp_ms": 42000,
  "frames": [
    {"id": 513, "dlc": 8, "data": "AABBCCDDEEFF0011", "flags": 1},
    {"id": 514, "dlc": 4, "data": "01020304", "flags": 1}
  ]
}
```

**字段说明**:
| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | int | CAN 标准 11-bit ID (0x201/0x202) |
| `dlc` | int | 数据长度 0-8 |
| `data` | hex string | CAN payload 8 字节，hex 编码 (16 字符) |
| `flags` | int | bit0=CRC8通过, bit1=seq重复, bits2-7=reserved |
| `t_esp_ms` | int | ESP32 `esp_timer_get_time()` 毫秒戳，daemon 端用 arrival time 替代 |

**Why hex string**:
- 人类可读：`mosquitto_sub -t 'EdgeVib/+/can/+/raw'` 直接看到 CAN payload，与 `candump` 输出格式可互译
- 紧凑：8 字节 payload → 16 字符，比整数数组 `[0,1,...]` 少 50%，比 base64 可读性好
- daemon 端 `sscanf(data_str, "%2hhx...", &frame.data[i])` 解析开销 <1μs/帧
- 系统一致性：ESP32 端 JSON 序列化用 `cJSON` 已有依赖，hex 编码只需 8 字节循环 + `sprintf("%02X")`

**Why 批量数组**: ESP32 收到 CMD 0x1A 攒批帧（含 1-2 个 frame_entry），一条 MQTT 消息发完整数组。`batch_seq` 单调递增用于 daemon 端去重（QoS 1 重传场景）。不做逐帧单独 MQTT publish——减少 WiFi 竞争和 TCP ACK 开销

**Alternatives considered**:
- 二进制 payload (protobuf/msgpack): 带宽最优但破坏"mosquitto_sub 直接调试"原则，且系统内所有 MQTT 全用 JSON，不一致
- 整数数组 data: `[170,187,221,204,238,255,0,17]` — JSON 数组每个元素 4 字符，8 字节 = 30+ 字符，hex 仅 16 字符
- 逐帧 topic: `.../raw/0x201` — MQTT wildcard 订阅可过滤 CAN ID，但 10 帧/s 下 MQTT broker topic 树膨胀不划算

#### 决策 6: 内核模块接口设计 (2026-05-30)

**6a. 帧注入: 纯 `netif_rx()`，放弃 NAPI**

**Why**: CAN 帧到达速率 ~10 帧/s。NAPI 的"中断缓解+轮询"机制是为千兆网卡中断风暴设计的（10^5 interrupts/s 级别）。10 帧/s 用 NAPI 的 poll queue 反而引入不必要的软中断调度延迟抖动。`netif_rx()` 一行代码，sk_buff 直接进协议栈。面试时展示的是"选择了匹配场景的技术，而非盲目搬模式"——这是加分项。

**NAPI 知识点留存**: NAPI 理解已在 D1 学习过程中完成——读了 `napi_schedule()` / `netif_napi_add()` 源码，知道原理。选择不用恰恰证明了判断力。

**6b. sysfs 自定义属性: 2 个**

| 属性 | 宏 | 面试故事 |
|------|-----|---------|
| `crc_errors` | `DEVICE_ATTR_RO` | flags bit0=0 的帧数 → "暴露 NDE→F407 物理链路的信噪比退化，这是标准 SocketCAN 接口不具备的 EdgeVib 特有指标" |
| `fifo_overruns` | `DEVICE_ATTR_RO` | daemon 注入过快导致内核丢帧→"软件注入速率限流器的熔断告警，证明驱动考虑了异常流控" |

位置: `/sys/devices/virtual/net/vcan_edgevib/`，与标准 netdev sysfs 同目录。`rx_can_frames` 用标准 `netdev->stats.rx_packets`，不重复造轮子。

**6c. 设备命名: 统一接口 `vcan_edgevib`**

**Why**: CAN 协议本身设计为一条总线多节点靠 ID 区分。`candump vcan_edgevib` + CAN ID filter (`candump vcan_edgevib,201:1FF`) 即可按节点过滤。多 NDE 节点扩展时（`nde02`, `nde03`）不改架构，daemon 只需订阅多个 MQTT wildcard topic。避免每节点独立网络接口导致的 SocketCAN socket 爆炸和配置复杂度。

**内核模块骨架**:
```
drivers/can/vcan_edgevib.c
  ├── vcan_edgevib_netdev_ops  (struct net_device_ops)
  │     .ndo_open  = vec_open      → netif_carrier_on
  │     .ndo_stop  = vec_stop      → netif_carrier_off
  │     .ndo_start_xmit = vec_xmit → netif_rx(skb)   ← 核心注入点
  │     .ndo_get_stats64 = vec_stats → 自定义 + 标准统计
  ├── sysfs: crc_errors, fifo_overruns (DEVICE_ATTR_RO)
  ├── module_init: alloc_candev() → register_candev()
  └── module_exit: unregister_candev() → free_candev()
```

#### 决策 7: 用户空间 daemon — Go 语言 (2026-05-30)

**选择**: Go 实现 `edgevib-can-d`，`paho.mqtt.golang` + `encoding/json` + `syscall` (AF_CAN)

**Why**:
- JSON 解析是 daemon 最易出错的环节。Go `encoding/json` → `struct rawCanFrame` 3 行代码类型安全；C `cJSON` 手动遍历 + 空指针检查需 30+ 行
- paho 已在 data-aggregator 验证，MQTT 断连自动重连 + QoS 1 持久会话，零额外代码
- 内核模块才是 D1 的 C 学习重点，daemon 用 Go 不稀释学习密度
- 与 data-aggregator (Go MQTT→TimescaleDB) 同一语言，团队技能不分裂
- 二进制 8MB + 运行时 20MB，headroom 920MB 完全容纳

**源码结构** (4 源文件, ~200 行核心逻辑):
```
drivers/vcan_edgevib/
├── can-d/
│   ├── main.go              — 入口: 配置加载 → MQTT 订阅 → 主循环
│   ├── mqtt.go              — paho 客户端 + JSON→can_frame 解析
│   ├── socketcan.go         — AF_CAN socket 封装 (bind/write/ioctl)
│   └── health.go            — 30s MQTT health 上报
└── vcan_edgevib.c           — 内核模块源码
```

**主循环模型**: paho `OnMessage` 回调 → JSON 解析 → `can_frame` 组装 → `unix.Sendmsg(fd, frame, 0)` → 非阻塞注入。paho 后台 goroutine 处理 MQTT 重连，主 goroutine 做健康上报。单 goroutine 写 SocketCAN（单 socket），无并发竞态。

#### 决策 8: 部署模式 — systemd (2026-05-30)

**选择**: systemd 直接部署 `edgevib-can-d`，Go 二进制 + systemd unit

**Why**:
- daemon 无硬件依赖（MQTT TCP + AF_CAN 网络 socket），不需要 Docker 的 `--device` 透传
- `After=mosquitto.service modprobe-vcan-edgevib.service` — systemd unit ordering 保证启动顺序
- 内核模块加载 (`modprobe vcan_edgevib`) 也走 systemd oneshot unit，统一编排
- 对标 rs232-gateway 和 opcua-server 的 systemd 部署模式：简单 IO 桥接服务走 systemd
- 省 Docker 20MB 容器开销，4GB 约束下每 MB 都算

**systemd units**:
```
/etc/systemd/system/
├── vcan-edgevib-load.service    — Type=oneshot: modprobe vcan_edgevib
├── edgevib-can-d.service        — Type=simple, After=vcan-edgevib-load.service + mosquitto.service
│     User=orangepi, Restart=always, RestartSec=5s
└── edgevib-can-d-health.timer   — (可选) 周期性健康检查
```

#### 决策 9: 测试策略 — 三层测试 (2026-05-30)

**选择**: Go 单测 (CI-able) + Shell 内核模块测试 (Orange Pi) + Python MQTT→CAN 端到端测试 (Orange Pi)

| 层 | 运行环境 | 测试对象 | 工具 |
|----|---------|---------|------|
| **Go 单测** | PC/CI | JSON hex→can_frame 解析、topic 解析、hex 边界（空/奇数字符/非法字符）、空帧列表 | `go test` mock MQTT message |
| **内核模块 test** | Orange Pi | `insmod`→`cansend`→`candump` 验证、sysfs 读取、`rmmod` 清理、统计计数器 | shell 脚本 30 行 |
| **E2E 集成** | Orange Pi | Mosquitto→daemon→AF_CAN→`candump` 完整链路、crc_errors 计数 | Python pytest + `paho.mqtt.publish` + `subprocess` |

**F407 + ESP32 固件测试**: 需物理硬件（F407 + ESP32 + CAN bus），延后到手动集成测试

**Why 不 mock 内核**: `candump`/`cansend`/`insmod`/`rmmod` 就是内核 CAN 子系统的标准测试工具。Shell 脚本验证模块完整生命周期是内核驱动开发的标准做法，面试时这是加分项——"我用 Linux 标准工具链测试了我的内核模块，零外部依赖"

#### D1 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 内核模块 vs 内置 vcan | 自写内核模块 (学习+业务价值并存) |
| 2 | CAN 帧数据注入方案 | F407 逐帧旁路上行 (CMD 0x1A) |
| 3 | 数据方向 | Phase 1 单向 (F103→Orange Pi) |
| 4 | UART 协议 | 攒批发送 (ISR→ring buffer→100ms flush) |
| 5 | MQTT Payload | JSON 批量数组 + hex 字符串 data |
| 6a | 帧注入机制 | 纯 `netif_rx()`，放弃 NAPI |
| 6b | sysfs 属性 | 2 个: `crc_errors` + `fifo_overruns` |
| 6c | 设备命名 | 统一接口 `vcan_edgevib` |
| 7 | daemon 语言 | Go (paho + encoding/json + syscall) |
| 8 | 部署模式 | systemd (2 units: 模块加载 + daemon) |
| 9 | 测试策略 | 三层: Go 单测 + Shell 内核测试 + Python E2E |

### D2: IIO Vibration Device — 详细设计

**业务关联**: 振动数据在 TimescaleDB 中，无法通过标准 Linux 传感器接口读取（`cat /sys/bus/iio/...`）。

**核心架构**:
```
TimescaleDB (vibration_view)
    │
    └── edgevib-iio-daemon (Go, 定时2s查询DB/或MQTT触发)
          ├── 从 DB 或 MQTT 获取最新 24 维特征向量
          ├── write() → /dev/edgevib-iio-inject (自定义字符设备, 注入 24×float32)
          │
    ┌───── 内核态 ─────┐
    │  edgevib_iio.ko  │
    │       ↓          │
    │  cdev write 回调: copy_from_user → 私有存储 (24通道值)  │
    │       ↓          │
    │  iio_trigger fire (软件触发, daemon write后自动触发)    │
    │       ↓          │
    │  trigger handler: 读私有存储 → iio_push_to_buffers()    │
    │       ↓          │
    │  IIO kfifo buffer (96字节/scan: 24×float32 LE)        │
    └──────┬───────────┘
           │
    cat /dev/iio:device0 → 96字节二进制 scan 输出
    iio_readdev -b 256 -s 96 iio:device0 → 标准 IIO 工具消费
    sysfs: /sys/bus/iio/devices/iio:device0/in_accel_x_raw → 单通道读取
```

**设计哲学**: 对标 D1 的架构模式——轻量内核 module（暴露标准接口）+ 用户空间 daemon（做业务逻辑），边界清晰。内核模块不感知 TimescaleDB/MQTT，只接收注入的数据并通过 IIO 标准接口暴露。

#### 决策 1: 数据注入架构 — 完整 IIO trigger + buffer (2026-06-01)

**选择**: IIO trigger + kfifo buffer (B 方案)，daemon 通过自定义字符设备注入数据

**Why**:
- 覆盖 IIO 子系统完整知识面：`struct iio_dev` + `iio_chan_spec` + `iio_trigger` + `iio_buffer` + `iio_device_register()`
- trigger + buffer 是工业传感器驱动的事实标准，真实工作场景中高频出现
- 学习价值 > 实现简洁性（D2 定位与 D1 不同——D1 追求极简匹配场景，D2 追求知识广度）
- 为未来接入物理加速度计（ADXL345 via SPI/I²C）预留架构位——更换 trigger 类型即可，无需重构

**Alternatives considered**:
- A. 纯 sysfs 回写: 简单但与 D1 知识重叠（都是 sysfs store 模式），IIO trigger/buffer 知识点丢失
- C. 纯字符设备: 偏离 IIO 框架，工具链 (iio_readdev/iio_info) 不可用

#### 决策 2: 用户空间注入机制 — 自定义字符设备 + IIO trigger (2026-06-01)

**选择**: 自定义字符设备 `/dev/edgevib-iio-inject` (B2 方案)，daemon 一次 `write()` 注入 24×float32 (96字节)

**注入流程**:
```
daemon:
  struct iio_inject {
    float rms_x, rms_y, rms_z, overall_rms,      // 4×4 = 16B
          peak_freq_x, peak_amp_x,                // 2×4 = 8B
          skewness_x, kurtosis_x, crest_factor_x, // 3×4 = 12B
          band_energy_x[8],                        // 8×4 = 32B
          peak_freq_y, peak_amp_y, crest_factor_y,// 3×4 = 12B
          peak_freq_z, peak_amp_z, crest_factor_z,// 3×4 = 12B
          temperature_c;                           // 1×4 = 4B
  };  // 总计 96 字节, 与 ESP32 push_feature_vector() 严格 1:1 对齐

  write(fd, &inject, 96) → 内核态
    ├── copy_from_user → 私有存储
    ├── iio_trigger_fire() → trigger handler
    └── iio_push_to_buffers() → kfifo
```

**Why**:
- 一次 `write()` 原子注入 24 维向量，避免 24 次 sysfs 写入的碎片化
- 内核学习覆盖 `file_operations` + `cdev_init` + `class_create` + `device_create`，与 IIO 框架组合
- MQTT 回调或 DB 轮询触发一次 `write()`，数据已在进程内存中，直接 `copy_from_user` 入内核——全程一次系统调用
- 与 D1 的 AF_CAN socket `write()` 模式对称——都是用户空间打开文件描述符写二进制 struct，降低项目整体理解成本

**Alternatives considered**:
- B1. 24 个独立 sysfs store: 24 次文件写入开销大；`in_xxx_raw` 的 store 用于注入历史数据语义不标准（raw 值语义上是传感器直接读数，不应被用户空间设置）
- B3. IIO buffer 双向化: IIO buffer 设计上 kfifo 是驱动→用户空间单向；hack IIO 核心偏离学习目标

#### 决策 3: IIO Channel 拓扑 — 单 device + 混合类型 + 24 维全量 scan (2026-06-01)

**选择**: 一个 IIO device 包含 24 个 channel，按物理量纲映射到最接近的 IIO type，未覆盖类型用 `extend_name` 标签区分

**设备数选择**: **单 device**。对标 D1 一个 `vcan_edgevib` 接口。多 IIO device 留给 Phase 2（多电机接入时），与 inference-engine 的"设备→电机→车间"三层聚合模型对齐。

**Channel 映射表**:

| 通道索引 | 特征值 | IIO Type | sysfs 路径 | 量纲 |
|---------|--------|----------|-----------|------|
| 0 | `rms_x` | `IIO_ACCEL` | `in_accel_x_raw` | mm/s² |
| 1 | `rms_y` | `IIO_ACCEL` | `in_accel_y_raw` | mm/s² |
| 2 | `rms_z` | `IIO_ACCEL` | `in_accel_z_raw` | mm/s² |
| 3 | `overall_rms` | `IIO_ACCEL` | `in_accel_envelope_raw` | mm/s² |
| 4 | `peak_freq_x` | `IIO_VOLTAGE` | `in_voltage0_peak_freq_x_raw` | Hz |
| 5 | `peak_amp_x` | `IIO_VOLTAGE` | `in_voltage1_peak_amp_x_raw` | g |
| 6 | `skewness_x` | `IIO_VOLTAGE` | `in_voltage2_skewness_x_raw` | — (无量纲) |
| 7 | `kurtosis_x` | `IIO_VOLTAGE` | `in_voltage3_kurtosis_x_raw` | — (无量纲) |
| 8 | `crest_factor_x` | `IIO_VOLTAGE` | `in_voltage4_crest_factor_x_raw` | — (无量纲) |
| 9-16 | `band_energy_x[0..7]` | `IIO_VOLTAGE` | `in_voltage5..12_band0..7_raw` | g²/Hz |
| 17 | `peak_freq_y` | `IIO_VOLTAGE` | `in_voltage13_peak_freq_y_raw` | Hz |
| 18 | `peak_amp_y` | `IIO_VOLTAGE` | `in_voltage14_peak_amp_y_raw` | g |
| 19 | `crest_factor_y` | `IIO_VOLTAGE` | `in_voltage15_crest_factor_y_raw` | — |
| 20 | `peak_freq_z` | `IIO_VOLTAGE` | `in_voltage16_peak_freq_z_raw` | Hz |
| 21 | `peak_amp_z` | `IIO_VOLTAGE` | `in_voltage17_peak_amp_z_raw` | g |
| 22 | `crest_factor_z` | `IIO_VOLTAGE` | `in_voltage18_crest_factor_z_raw` | — |
| 23 | `temperature_c` | `IIO_TEMP` | `in_temp_raw` | ℃ |

**Why**:
- 三轴 RMS 是加速度量纲（mm/s²），用 `IIO_ACCEL` 语义正确，`iio_info` 工具直接按加速度通道识别
- 非加速度特征（峰频/峭度/频带能量）用 `IIO_VOLTAGE` 做 fallback，配 `extend_name` 区分——避免污染 `IIO_ACCEL` 语义空间
- 温度用原生 `IIO_TEMP`，`iio_readdev` 输出时带温度量纲标记
- 24 维与 ESP32 `push_feature_vector()` 的 `AI_NUM_FEATURES=24` 严格 1:1 对齐——buffer scan 输出 96 字节可直接与 ESP32 端数据做二进制比对
- 与 inference-engine（`edge-gateway/CONTEXT.md` 第 196-207 行）共享同一特征定义，三端（ESP32 + Orange Pi IIO + inference-engine）特征语义无歧义

**Alternatives considered**:
- 全 `IIO_VOLTAGE` 映射: 三轴加速度信息丢失，`iio_info` 显示全是 voltage 通道使操作员困惑
- 自定义 IIO type: 需修改 IIO 核心枚举 + 工具链适配，工作量大且通用工具不识别

**知识点**: `struct iio_dev` / `iio_device_register()` / `struct iio_chan_spec` / `struct iio_trigger` / `iio_trigger_register()` / `struct iio_buffer` / `iio_push_to_buffers()` / `struct cdev` / `file_operations` / `class_create` / `device_create` / `copy_from_user()`

#### 决策 4: Daemon 数据源 — TimescaleDB 固定间隔轮询 (2026-06-01)

**选择**: Daemon 每 2s 查询 `vibration_view` 最新一条记录，写入内核 IIO 模块

**Why**:
- ESP32 每 2s 发布振动数据，data-aggregator 攒 100 行或 5s 间隔 flush。2s 轮询在节奏上自然对齐（最坏延迟 0~7s），对 IIO 消费者（`iio_readdev`/Grafana）完全可接受
- `vibration_view` 是现成的 SQL VIEW，已从 JSONB 解构出 24 维特征——daemon 只需 `SELECT * FROM vibration_view WHERE device_id = $1 ORDER BY time DESC LIMIT 1`，零额外解析
- 0.5 QPS 对 TimescaleDB 可忽略（单条 SELECT 走 time 索引，<1ms）
- PostgreSQL LISTEN/NOTIFY 方案要求 data-aggregator INSERT 后发 NOTIFY——破坏"不修改现有服务代码"的约束（CONTEXT.md 实施策略）
- 简单可靠：timer → query → write /dev/edgevib-iio-inject → sleep 2s，对标 D1 can-d 的 MQTT callback 驱动模式（推模型 vs 拉模型，但都是单数据源单向注入）

**Alternatives considered**:
- MQTT subscriber 直读: 实时性高（<100ms），但 D2 不需要推理级延迟；且需 JSON 解析 24 维特征，重复 data-aggregator 的提取逻辑
- 混合模式（MQTT + DB 互补）: 对标 inference-engine/llm-analyzer 的混合架构，但 IIO 消费者场景不需要双通道保障，过度设计

#### 决策 5: 单/多设备支持策略 — 配置文件驱动 (2026-06-01)

**选择**: Phase 1 默认单 IIO device（通过 `device_id` 配置），架构上预留多 device 切换

**Why**:
- 当前只有 DE01+NDE01 两个设备，Phase 1 单 device 匹配实际部署规模
- 配置文件 `device_mode: "single"` / `device_mode: "multi"` 控制行为：
  - `single` 模式：1 个 IIO device (`iio:device0`)，daemon 查询指定 `device_id`
  - `multi` 模式：N 个 IIO device（`iio:device0..N-1`），daemon `SELECT DISTINCT device_id` → 每个设备独立注入
- 对标 inference-engine 的"设备级独立推理→电机级聚合"（决策 5），多设备 IIO 复用了同一套分层模型
- 与 D1 的一致性：D1 一个 vcan_edgevib 对所有 CAN 帧——但 CAN 帧自带 `can_id` 天然区分设备；IIO 通道无类似标识，多设备必须拆 device

**配置示例** (`config/vcan-edgevib.yaml` 同级新增 `config/iio-vibration.yaml`):
```yaml
device:
  mode: "single"           # single | multi
  device_id: "de01"        # single 模式下使用

timescaledb:
  host: "localhost"
  port: 5432
  dbname: "edgevib_ts"
  user: "edgevib"
  password: "edgevib123"

iio:
  device_name: "edgevib-iio"
  poll_interval_s: 2
  health_interval_s: 30

health:
  topic: "EdgeVib/system/health/iio_vibration"
```

#### 决策 6: 部署模式 — Go daemon + systemd (2026-06-01)

**选择**: Go 编译为单二进制 + systemd 部署，内核 module 通过 oneshot service 加载。对标 D1 的部署架构。

**Why**:
- D2 daemon 与 D1 can-d 是同形架构：**数据源订阅 → 解析 → 写入内核接口**。D1 用 Go 已验证（encoding/json + paho MQTT → SocketCAN write），D2 复用同一模式（pgx → 字符设备 write）
- `jackc/pgx` 已在 data-aggregator + api-server 验证，DB 连接池 + SQL 查询代码可直接复用模式
- Go `time.Ticker` 2s 轮询 + `os.File.Write()` 写字符设备——核心逻辑 <150 行 Go
- 二进制 8MB + 运行时 20MB，与 can-d 等同，Orange Pi 4GB headroom 充足
- 部署架构对标 D1：2 个 systemd unit — `edgevib-iio-load.service` (oneshot modprobe) + `edgevib-iio-d.service` (simple daemon)
- systemd `User=orangepi`，与 vision-service/rs232-gateway/opcua-server 同居 systemd 层，保持"硬件直通→systemd、Docker→复杂依赖隔离"的分层一致性

**部署架构** (对标 D1):
```
/etc/systemd/system/
├── edgevib-iio-load.service      (oneshot: modprobe edgevib_iio)
│     After=network.target
│     Before=edgevib-iio-d.service
│
└── edgevib-iio-d.service         (simple: Go daemon)
      After=timescaledb.service edgevib-iio-load.service
      Wants=timescaledb.service
      ExecStart=/usr/local/bin/edgevib-iio-d -config /opt/edge-gateway/config/iio-vibration.yaml
      Restart=always, RestartSec=5
      MemoryMax=50M
      User=orangepi
```

**D1 vs D2 daemon 对比**:

| 维度 | D1 can-d | D2 iio-d |
|------|---------|---------|
| 数据源 | MQTT (推模型, onMessage callback) | TimescaleDB (拉模型, time.Ticker) |
| 输出 | AF_CAN socket `write()` | `/dev/edgevib-iio-inject` `write()` |
| 数据格式 | JSON → hex decode → `canFrame` binary (16B) | DB row → `iio_inject` binary (96B) |
| 触发频率 | MQTT 消息驱动 (~10帧/s) | 2s 定时轮询 |
| 运行时内存 | ~20MB | ~20MB |
| 内核模块 | vcan_edgevib.ko (~170行) | edgevib_iio.ko |

**Alternatives considered**:
- Python + systemd: 对标 inference-engine/vision-service 的 Python venv 部署，但 Python 运行时 ~50MB 是 Go 的 2.5x，且 DB 查询无异步优势（单次 SELECT <1ms 不需要 asyncio）
- C daemon: <5MB 最轻量，但 libpq 直接编程复杂度高，且 `pgx` 的自动重连/连接池在 C 中需手写，不匹配 2-3 天工期

#### 决策 7: sysfs 用户接口 — IIO 标准属性 + 2 个自定义计数器 (2026-06-01)

**选择**: IIO `read_raw` 回调覆盖 `RAW`/`SCALE`/`SAMP_FREQ` 三种查询 + 2 个自定义 sysfs 属性

**IIO 标准属性** (在 `iio_info` 回调 `read_raw` 中处理):

| 查询类型 | 返回值 | 说明 |
|---------|--------|------|
| `IIO_CHAN_INFO_RAW` | 私有存储中对应 channel 值 (float32 → int, 保留 3 位小数) | `cat in_accel_x_raw` 读单通道 |
| `IIO_CHAN_INFO_SCALE` | `1.000` (float, 原始值=物理值) | `in_accel_x = raw × scale` |
| `IIO_CHAN_INFO_SAMP_FREQ` | `0.5` (Hz, ESP32 2s 采集周期) | `iio_info` 显示采样率 |

`raw` 值的存储格式：float32 × 1000 转为 int，sysfs 输出 `1230` 即 1.230 mm/s²。IIO 不直接输出浮点——这是 IIO 框架约定（`read_raw` 返回 int，scale 做浮点转换）。

**自定义 sysfs 属性** (对标 D1 的 2 个计数器):

| 属性 | 路径 | 语义 | 对标 D1 |
|------|------|------|---------|
| `injection_count` | `/sys/bus/iio/devices/iio:device0/injection_count` | daemon 累计注入次数（每次 `write()` → cdev → trigger fire → +1） | `crc_errors`（sysfs 可见的业务计数器） |
| `last_injection_time_ms` | 同上路径 | 最后一次注入的内核时间戳（`jiffies_to_msecs()`），用于判断数据新鲜度 | 无直接对标（D1 帧自带时间戳，D2 的内核时间戳是补充） |

**Why**:
- IIO `read_raw` 回调覆盖 3 种查询类型是 IIO 驱动的核心知识点——面试时展示"我理解 IIO channel 体系的 RAW→SCALE 转换语义"
- 2 个自定义属性对标 D1 的 2 个 sysfs attr，遵循项目约定的 sysfs 数量模式
- `injection_count` 提供了业务级可观测性——操作员 `cat` 一下就知道数据是否在流动
- `last_injection_time_ms` 提供数据新鲜度判断——`echo $(($(date +%s) - $(cat /sys/.../last_injection_time_ms) / 1000))` 得"距上次数据多少秒"

**Alternatives considered**:
- 更多自定义属性（db_errors、buffer_overruns 等）：db_errors 放 daemon 内部 `slog` 日志，不属于内核模块关注；buffer_overruns 由 IIO 框架 `buffer/length` 和 `buffer/watermark` 已覆盖

#### 决策 8: 项目结构与测试策略 — 对标 D1 三层体系 (2026-06-01)

**选择**: 目录结构对标 D1，源文件数对标 D1，测试策略对标 D1 三层体系

**源码树** (对标 D1 `drivers/vcan_edgevib/`):

```
drivers/iio_vibration/
├── edgevib_iio.c                    # 内核模块: iio_dev + iio_trigger + kfifo buffer + cdev
├── Makefile                         # Kbuild (对标 vcan_edgevib Makefile)
├── iio-d/                           # Go daemon (3文件, 对标 can-d/ 4文件)
│   ├── main.go                      # 入口: 配置加载 → DB连接 → 主循环
│   ├── inject.go                    # /dev/edgevib-iio-inject 写操作封装
│   ├── health.go                    # 健康上报 (30s MQTT, 对标 can-d/health.go)
│   └── go.mod                       # module edgevib/iio-d
├── systemd/
│   ├── edgevib-iio-load.service     # oneshot modprobe (对标 vcan-edgevib-load.service)
│   └── edgevib-iio-d.service        # simple daemon (对标 edgevib-can-d.service)
└── test/
    ├── test_module.sh               # 内核模块集成测试 (对标 D1 test_module.sh)
    └── test_e2e.py                  # 端到端测试: DB→daemon→IIO→iio_readdev

config/
└── iio-vibration.yaml               # 服务配置 (同级 config/vcan-edgevib.yaml)
```

**daemon 源文件数**: D1 can-d 是 4 个 .go（main + socketcan + mqtt_client + health）。D2 iio-d 是 3 个（main + inject + health）——不需要 MQTT client 文件（DB 轮询替代 MQTT 订阅），health 直接走 MQTT 发布。

**测试策略** (对标 D1 三层体系):

| 层 | D2 测试内容 | 对标 D1 | 环境要求 |
|----|-----------|---------|---------|
| **Shell 内核测试** | `insmod`→`iio_info` 验证 24 channel→手动 `echo` 写 cdev→`cat /dev/iio:device0` 验证 96B buffer→`rmmod` 完整生命周期 | `test_module.sh` (7 项) | Orange Pi 实机 |
| **Python E2E** | DB 插测试数据→启动 daemon→`iio_readdev` 消费 buffer→验证 96 字节 scan 与注入数据一致 | `test_e2e.py` (4 项) | Orange Pi 实机 + TimescaleDB |
| **Go 单元测试** | `inject.go` (cdev open/write/close 正常流程 + 错误路径)、DB 查询 mock (pgx mock) | D1 无 Go 单测 | PC 端 (go test) |
| **内核 mock 测试** | 可选: KUnit 框架测试 IIO channel 注册/注销、trigger fire 回调；UML (User Mode Linux) 上跑 edgevib_iio.ko 验证 sysfs 属性生成 | D1 无 | PC 端 (UML/KUnit) |

**Why daemon 3 文件而非 4 文件**:
- D1 can-d 的核心复杂度在 `mqtt_client.go`（JSON 解析 + hex decode + SocketCAN write），需要独立文件
- D2 iio-d 不需要 MQTT subscriber——`main.go` 直接 `time.Ticker` + `db.QueryRow` + `inject.Write()`，逻辑比 D1 更薄
- `inject.go` 封装 `os.OpenFile("/dev/edgevib-iio-inject", O_WRONLY)` 和 `binary.Write()`，与 D1 `socketcan.go` 的 `syscall.Socket(AF_CAN)` 模式对称——都是"打开特殊文件描述符 + 写二进制 struct"
- 对标项目"每个 C 模块 <500 行、每个 Go 包 <200 行"的代码规模惯例

**关键测试点** (优先级排序):
1. `test_module.sh` Test 1: module load → `iio_info` 输出含 24 个 channel + trigger
2. `test_module.sh` Test 2: `echo <96B> > /dev/edgevib-iio-inject` → `cat /dev/iio:device0` 96 字节一致
3. `test_e2e.py` Test 1: DB 写入 → daemon 2s 轮询 → `iio_readdev` 读到数据 → 浮点值验证
4. `test_module.sh` Test 3: `cat /sys/bus/iio/devices/iio:device0/injection_count` 自动递增

**内核 mock 方向** (Phase 2 探索):
- KUnit: Linux 内核内置单元测试框架，可测 `iio_device_register()` 返回值、`iio_chan_spec` 字段填充
- UML (User Mode Linux): 编译内核模块为 x86_64 UML 目标，在 PC 上 `insmod`→`rmmod`，无需 Orange Pi 硬件
- mock cdev: 在 Go 单测中创建 `os.Pipe()` 模拟 `/dev/edgevib-iio-inject`，隔离内核依赖

### D2 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 数据注入架构 | IIO trigger + kfifo buffer (完整 IIO 子系统) |
| 2 | 用户空间注入机制 | 自定义字符设备 `/dev/edgevib-iio-inject`，96 字节二进制 `write()` |
| 3 | IIO Channel 拓扑 | 单 device + 混合 IIO type (ACCEL+VOLTAGE+TEMP) + 24 维全量 scan |
| 4 | Daemon 数据源 | TimescaleDB vibration_view 固定 2s 轮询 (0.5 QPS) |
| 5 | 单/多设备支持 | 配置文件驱动 `device_mode: "single"/"multi"`，Phase 1 单 device |
| 6 | 部署模式 | Go daemon + systemd binary deploy，2 个 systemd units |
| 7 | sysfs 接口 | IIO read_raw (RAW+SCALE+SAMP_FREQ) + 2 自定义计数器 (injection_count/last_injection_time_ms) |
| 8 | 项目结构与测试 | 3 文件 Go daemon + 3 层测试(Shell内核+Python E2E+Go单测) + 内核 mock 方向预留 |

### D3: EdgeVib Digital I/O — 虚拟 GPIO Controller + IRQ Chip（2026-06-03 重构）

> **2026-06-03 子系统变更**: 原 blk-mq 块设备方案（`edgevib_buffer.c`）在 Orange Pi 4 Pro 内核 6.6.98-sun60iw2 上 `device_add_disk()` 反复 kernel panic（3 个版本均失败，详见 KERNEL-LESSONS.md）。经硬件审计确认 GPIO/IRQ 子系统在该内核上稳定可用（`/dev/gpiochip0`、`/dev/gpiochip1`、`sunxi_pio_edge`/`sunxi_pio_level` IRQ 控制器均正常）。**改用 gpio_chip + irq_chip 方案**，学习 GPIO 和 IRQ 两个内核子系统。原 blk-mq 代码保留在 `test/edgevib_minimal.c` 供参考。

**业务关联**: 填补软件告警→物理信号的最后一公里。Orange Pi 作为车间级边缘网关，通过虚拟 GPIO 芯片输出系统级健康信号（绿灯=一切正常、黄灯=需关注），接收急停按钮和 UPS 掉电中断。与 F407 的电机级报警（单机保护）形成**正交的两层**——F407 管电机，Orange Pi 管网关自身。

**硬件审计结论** (2026-06-03):
- Orange Pi 4 Pro 有 2 个 GPIO 控制器: `/dev/gpiochip0` (sunxi PIO)、`/dev/gpiochip1`
- 中断支持完整: `sunxi_pio_edge`（边沿触发）+ `sunxi_pio_level`（电平触发），挂载在 GICv3
- gpiolib cdev 工具链可用: `gpiodetect` / `gpioinfo` / `gpiomon` / `gpioset`
- **结论**: GPIO + IRQ 子系统在 Orange Pi 内核上是成熟稳定的，不会有 D3 blk-mq 的兼容性问题

**设计**: 注册虚拟 gpio_chip（`gpiochip_add()`），6 条线——4 输出 + 2 输入含 IRQ。不依赖真实硬件，纯软件模拟。gpiolib 自动创建 `/dev/gpiochipN` 设备节点。Go daemon 通过 gpiolib cdev（`/dev/gpiochipN`）或 sysfs 操作 GPIO 线。

**与现有服务冲突审计** (2026-06-03):
- data-aggregator / inference-engine / llm-analyzer / api-server / rs232-gateway / vision-service / audio-monitor / Prometheus+Grafana / opcua-server / alert-webhook — **零冲突**
- `alert-webhook.py` 第 10 行预留了 `digital_io alarm_service` 占位符——D3 恰好填补

**核心架构**:
```
MQTT System Events:
  ├── EdgeVib/system/monitoring/alert (Prometheus Alertmanager)
  ├── EdgeVib/+/inference/+/ai/report (inference-engine WARNING/CRITICAL)
  │
  └── edgevib-gpio-d (Go daemon, systemd)
        ├── 订阅 MQTT → 解析健康状态 → 控制 GPIO 输出
        ├── gpiomon 监听急停/UPS 中断 → MQTT 广播
        └── 30s MQTT health 上报
              │
              └── /dev/gpiochipN (gpiolib cdev)
                    ├── Line 0: SYSTEM_OK       (输出)
                    ├── Line 1: GATEWAY_ALERT   (输出)
                    ├── Line 2: HEARTBEAT       (输出, 1Hz)
                    ├── Line 3: RESERVED        (输出)
                    ├── Line 4: ESTOP_MASTER    (输入 + IRQ)
                    └── Line 5: PSU_FAIL_IN     (输入 + IRQ)
```

**为什么用内核 GPIO 芯片而非 Go `gpiozero` / Python RPi.GPIO**:
1. **进程生命周期解耦**: GPIO 线状态保存在内核，Go daemon 重启时输出状态不跳变——不会因为 daemon 重启导致报警灯误闪
2. **gpiolib cdev 提供标准接口**: `/dev/gpiochipN` 是 Linux 4.8+ 的 GPIO 用户空间标准接口，`gpioset`/`gpiomon` 可直接调试
3. **IRQ 在内核处理**: 急停按钮中断在内核层被捕获（<1ms），不依赖用户空间线程调度——比 Go select 轮询 `/sys/class/gpio/gpioXX/value` 快 2–3 个数量级
4. **标准工具链**: `gpiodetect` 列出芯片、`gpioinfo` 查线状态、`gpiomon` 监听中断，运维无需专用工具

**与 D1/D2 的内核子系统矩阵**:
| 维度 | D1 vcan_edgevib | D2 edgevib_iio | D3 edgevib-gpio |
|------|----------------|---------------|-------------------|
| 设备类型 | 网络设备 (CAN) | IIO 设备 | **GPIO Controller** |
| 数据方向 | 用户空间→内核→用户空间 (echo) | DB→内核→IIO 消费者 | **MQTT→内核→物理世界（输出）/ 物理世界→内核→MQTT（输入）** |
| pipe 模型 | MQTT→daemon→SocketCAN→candump | DB→daemon→IIO→iio_readdev | MQTT→daemon→gpio_chip→物理IO / 物理IO→irq_chip→daemon→MQTT |
| 控制接口 | socket(AF_CAN) + write() | /dev/edgevib-iio-inject write() | `/dev/gpiochipN` + gpiolib cdev ioctl() |
| 消费接口 | candump / AF_CAN socket | iio_readdev / sysfs | `gpioset` / `gpiomon` / `gpioinfo` |
| 内核子系统 | SocketCAN + net_device_ops | IIO + cdev | **gpio_chip + irq_chip + gpiolib cdev** |
| 统计计数器 | sysfs: crc_errors, fifo_overruns | sysfs: injection_count, last_injection_time_ms | sysfs: irq_count, last_irq_time_ms |

**知识点**: `struct gpio_chip` / `gpiochip_add()` / `struct irq_chip` / `irq_domain_create_linear()` / `irq_create_mapping()` / `handle_edge_irq()` / `handle_level_irq()` / `gpiolib cdev` / `gpiodetect` / `gpiomon` / `gpioinfo`

#### 决策 1: 注册模型 — 直接 gpiochip_add（2026-06-03）

**选择**: 直接调用 `gpiochip_add()` 注册，不使用 platform_driver 或 misc_register 包装

**Why**:
- GPIO 子系统自 4.8 以后有 gpiolib cdev，`gpiochip_add()` 自动创建 `/dev/gpiochipN`，不需要额外设备节点
- 跟 D1 (`register_candev`) 和 D2 (`iio_device_register`) 一样，子系统框架自己创建设备
- 少一层 platform_driver 的 boilerplate，代码更紧凑
- Linux 内核内置的 `gpio-mockup` 测试驱动也用这个模式——这是虚拟 GPIO 芯片的标准做法
- Orange Pi 上 `gpiodetect` 已可列出 `gpiochip0`/`gpiochip1`，再加一个 `gpiochip2`（edgevib-gpio）不会冲突

**Alternatives considered**:
- platform_driver: 标准内核模型但没有真实硬件时需要手动创建 platform_device，增加测试复杂度。D4 (RTC) 会用这个模式，D3 保持简洁
- misc_register: 创建 `/dev/edgevib-gpio`，但 gpio_chip 已经通过 gpiolib cdev 提供了 `/dev/gpiochipN`，多一个接口无增量价值

#### 决策 2: GPIO 线定义 — 6 线虚拟 GPIO（网关值守 + 环境安全）（2026-06-03）

**选择**: 6 条虚拟 GPIO 线，4 输出 + 2 输入含 IRQ。Orange Pi 管理系统级健康信号，F407 管理电机级报警——两层正交。

| Line | 名称 | 方向 | 触发类型 | 业务语义 |
|------|------|------|---------|---------|
| 0 | `SYSTEM_OK` | 输出 | — | 全部核心服务正常=高。物理意义：绿灯常亮=系统OK |
| 1 | `GATEWAY_ALERT` | 输出 | — | 任一核心服务异常=高。物理意义：黄灯=需人工干预 |
| 2 | `HEARTBEAT` | 输出 | — | 1Hz 方波，daemon 定时器翻转。外部看门狗监测 Orange Pi 存活 |
| 3 | `RESERVED` | 输出 | — | 预留扩展，默认低电平 |
| 4 | `ESTOP_MASTER` | 输入 | 下降沿 IRQ | 车间级紧急停机按钮。按下→低电平→irq→MQTT 广播停采 |
| 5 | `PSU_FAIL_IN` | 输入 | 上升沿 IRQ | UPS/电源模块市电掉电信号。拉高→irq→30s 优雅关机 |

**与 F407 的分工**:
| 维度 | F407 GPIO | D3 GPIO |
|------|----------|---------|
| 管辖范围 | 单台电机 | 网关 + 车间环境 |
| 报警对象 | 电机轴承/振动故障 | 网关服务健康/供电安全 |
| 典型动作 | 电机旁红灯亮 | 网关旁绿灯灭+黄灯亮 |
| 多电机场景 | 每台独立报警 | 任一异常即聚合告警 |

**Why 6 线而非 4 或 8**:
- 4 线不够展示 gpio_chip 的完整方向控制（至少需要 2 输出 + 2 输入才能覆盖全部方向 ops）
- 8 线过多，超出网关实际业务需求，强行造故事
- 6 线刚好对标 D1 64 槽、D2 24 通道——"小但够"的 EdgeVib 哲学

**Why 输入线走 IRQ 而非轮询**:
- 急停按钮：硬实时需求，下降沿到 MQTT 广播延迟应 <10ms。轮询 `/sys/class/gpio/gpioX/value` @ 100ms 间隔延迟不可接受
- UPS 掉电：提前 30s 收到通知可优雅关机（sync + docker compose stop + unmount），每 100ms 的轮询浪费 1/300 的关机时间窗
- IRQ 展示 `irq_chip` 和 `irq_domain` 的使用——这是 GPIO+IRQ 子系统的核心学习目标

**输出线初始状态**:
- `SYSTEM_OK`: 低（daemon 启动后拉高——主动确认机制，避免未初始化时误报健康）
- `GATEWAY_ALERT`: 低（正常时不高，告警时拉高——故障安全原则：断线=不告警而非误告警）
- `HEARTBEAT`: 低（daemon 启动后开始翻转）
- `RESERVED`: 低

**Alternatives considered**:
- 4 线（去掉 RESERVED + GATEWAY_ALERT）: 太接近业务最少集，失去扩展灵活性
- 8 线纯虚拟教学: 线号脱离业务，面试故事稀薄。gpiomockup 模式已被内核自己实现

#### 决策 3: IRQ Chip 设计 — 独立 irq_chip + 线性 IRQ 域（2026-06-03）

**选择**: 为输入线（4, 5）独立注册 `struct irq_chip` + `irq_domain_create_linear()`。支持边沿触发，从 GPIO 软件模拟中断注入。

**IRQ 域配置**:
```c
// 为 gpio_chip 的 2 条输入线创建 IRQ 域
struct irq_domain *domain = irq_domain_create_linear(
    fwnode,              // 固件节点 (NULL = 虚拟设备)
    2,                   // 2 条 IRQ 线 (line 4, 5)
    &edgevib_irq_domain_ops,  // domain ops (map/unmap/xlate)
    priv                 // 私有数据
);
```

**irq_chip ops 实现**:
| 操作 | 实现方式 | 说明 |
|------|---------|------|
| `irq_mask` | 设置 `priv->irq_masked[line] = true` | 禁止中断（GPIO 线仍可读） |
| `irq_unmask` | 设置 `priv->irq_masked[line] = false` | 恢复中断 |
| `irq_set_type` | 验证触发类型（`IRQ_TYPE_EDGE_RISING`/`FALLING`/`BOTH` 或 `IRQ_TYPE_LEVEL_HIGH`/`LOW`） | gpiolib cdev 用户空间通过 ioctl 指定 |
| `irq_bus_lock` / `irq_bus_sync_unlock` | 空实现（虚拟设备无硬件总线） | 满足 irq_chip 接口要求 |

**中断注入机制**: 通过自定义 sysfs 属性 `inject_irq` 模拟：
```bash
# 模拟 ESTOP 下降沿
echo "4 0" > /sys/devices/virtual/gpiochip/edgevib-gpio/inject_irq
# gpiomon 监听端立即输出事件
```

内部调用:
```c
irq = irq_find_mapping(domain, line_offset);
handle_edge_irq(&irq_desc);  // 或 handle_level_irq，取决于 irq_set_type 配置
```

**gpio_chip.to_irq 回调**:
```c
static int edgevib_gpio_to_irq(struct gpio_chip *chip, unsigned offset) {
    // line 4, 5 映射到 IRQ 域
    if (offset < 4) return -ENXIO;  // 输出线无 IRQ
    return irq_create_mapping(chip->irq.domain, offset - 4);
}
```
这样用户空间 `gpiofind edgevib-gpio 4 | gpiomon` 自动走 gpiolib → to_irq → irq_domain 链路。

**Why**:
- **irq_domain 是内核 IRQ 子系统的核心抽象**——不实现它等于只学了 GPIO 没学 IRQ
- 虚拟 IRQ 通过 sysfs 注入可精确重现：写入 `"4 0"` 产生下降沿、写入 `"5 1"` 产生上升沿 → gpiomon 验证
- 与 D1 (SocketCAN TX→RX echo) 的对称测试思想一致——"从用户空间注入，到内核处理，再回到用户空间观测"

**IRQ 注入实现方式** (子决策):

**选择**: `generic_handle_irq()` 内核 API + 内部对比旧值判断边沿方向

```c
// sysfs write: echo "4 0" > inject_irq → 模拟 line 4 下降沿
static ssize_t inject_irq_store(struct device *dev, ...) {
    int line, new_value;
    sscanf(buf, "%d %d", &line, &new_value);
    
    // 判断边沿类型
    int old_value = priv->line_values[line];
    if (old_value == 1 && new_value == 0)  // 下降沿
        irq_type = IRQ_TYPE_EDGE_FALLING;
    else if (old_value == 0 && new_value == 1)  // 上升沿
        irq_type = IRQ_TYPE_EDGE_RISING;
    
    // 更新内部状态 + 触发中断
    priv->line_values[line] = new_value;
    int virq = irq_find_mapping(chip->irq.domain, offset);
    generic_handle_irq(virq);  // 标准 API，EXPORT_SYMBOL_GPL
}
```

**Why `generic_handle_irq()` 而非 `handle_edge_irq()`**:
- `generic_handle_irq()` 是 `include/linux/irqdesc.h` 的公开 EXPORT_SYMBOL_GPL API，内核模块安全可用
- `handle_edge_irq()` 需要直接操作 `struct irq_desc`——内核内部结构体，EXPORT_SYMBOL 不可用
- 内核 6.6 官方的 `drivers/gpio/gpio-mockup.c` 虚拟 GPIO 测试驱动也使用 `generic_handle_irq()`
- 不需要 include 额外的内部头文件，兼容性最好

**Alternatives considered**:
- 不实现 irq_chip，仅 gpio_chip 输出: 丢失 IRQ 子系统学习，D3 退化为纯 GPIO，价值减半
- 用 gpio-mockup.ko 代替自写: 内核已有但那是调别人代码，无学习增量
- `handle_edge_irq()` 直接调用: API 不可导出，跨内核版本兼容性差

#### 决策 4: 自定义 sysfs 属性 — 2 个计数器 + 1 个注入接口（2026-06-03）

**选择**: 3 个自定义 sysfs 属性，对标 D1/D2 的计数器模式 + D3 独特的中断注入需求

| 属性 | 路径 | 语义 |
|------|------|------|
| `irq_count` | `/sys/devices/virtual/gpiochip/edgevib-gpio/irq_count` | 自模块加载以来 IRQ 触发总次数 |
| `last_irq_time_ms` | 同上路径 | 最近一次 IRQ 触发时刻（ms since boot） |
| `inject_irq` | 同上路径 | **写入**: `"<line> <value>"` 模拟中断（写-only）。值 0/1 分别模拟下降沿/上升沿 |

**Why 3 个而非 2 个**:
- D1 和 D2 各 2 个 sysfs 属性（计数器 + 时间戳），模式成熟
- D3 额外增加 `inject_irq` 是因为虚拟 GPIO 没有真实硬件触发中断——必须提供注入入口才能测试 IRQ 路径
- `inject_irq` 对标 D2 的 `/dev/edgevib-iio-inject`——两者都是"软件模拟真实物理事件"的注入点。D2 用 cdev write，D3 用 sysfs write——因为注入只传两个整数，不需要 96 字节结构体

**Alternatives considered**:
- 用 cdev ioctl 注入: 对标 D2 模式更统一但 IRQ 注入只需两个 int（线号+值），sysfs 更轻量。且 D3 已有 gpiolib cdev 作为主接口，再开一个 cdev 困惑用户
- 只保留 `irq_count`: 对标 D1/D2 的 2 属性惯例但不能测试——`gpiomon` 监听需要 sysfs 注入才能触发生成中断事件

#### 决策 5: Go daemon 架构 — edgevib-gpio-d，3 文件（2026-06-03）

**选择**: 独立 Go daemon `edgevib-gpio-d`，MQTT 订阅消费 → gpiolib cdev 写输出 + gpiomon 监听输入 IRQ

**数据流**:
```
输入路径 (MQTT → GPIO 输出):
  MQTT 订阅: EdgeVib/system/monitoring/alert (Prometheus)
             EdgeVib/+/inference/+/ai/report (inference-engine)
             EdgeVib/+/gateway/+/status/health (设备在线状态)
      ↓
  evaluate_health(): 解析 severity + 检查各服务心跳时间戳
      ↓
  规则引擎:
    - 任一 CRITICAL → gpioset gpiochip2 0=0 (SYSTEM_OK 拉低)
    - 任一 WARNING  → gpioset gpiochip2 1=1 (GATEWAY_ALERT 拉高)
    - 全部清除      → gpioset gpiochip2 0=1 (SYSTEM_OK 恢复)
    - 定时器 500ms  → gpioset gpiochip2 2=!prev (HEARTBEAT 翻转)
      ↓
  /dev/gpiochip2 (gpiolib cdev ioctl)

输出路径 (GPIO 输入 IRQ → MQTT):
  gpiomon -r /dev/gpiochip2 4 5 (daemon 内 exec 或 Go gpiolib bindings)
      ↓
  line 4 下降沿 → MQTT publish EdgeVib/system/emergency/estop {"action":"shutdown"}
  line 5 上升沿 → MQTT publish EdgeVib/system/emergency/psu_fail {"graceful_shutdown_ms":30000}
```

**Go daemon 文件规划**:
| 文件 | 职责 | 对标 |
|------|------|------|
| `main.go` | 入口 + 配置加载 + 信号处理 | D1 can-d main.go |
| `gpio_ctrl.go` | gpiolib cdev 封装：线 get/set/direction + gpiomon 子进程管理 | D2 iio-d inject.go |
| `health.go` | MQTT 订阅 + 健康评估规则引擎 + 30s health 上报 | D1 can-d health.go |

**Why 3 文件而非 4 文件**: D3 daemon 比 D2 iio-d 多 MQTT 订阅，但 `health.go` 同时承载 MQTT 订阅和健康评估——因为两者逻辑紧密相关（订阅 topic = 评估输入），拆成 mqtt_client + health 反而增加文件间耦合

**语言选择**: Go 而非 Python
- gpiolib cdev Go bindings 通过 `cgo` 或直接 ioctl 操作 `/dev/gpiochipN`——Go 的二进制部署与 systemd 完美配合
- 对标 D1 (can-d) 和 D2 (iio-d) 的 Go daemon 模式
- Python `gpiod` 库需额外安装，不符合项目 systemd 层的最小依赖原则

**内存预算**: Go 二进制 ~8MB + 运行时 ~10MB ≈ 18MB，systemd `MemoryMax=40M` 充足

**gpiolib 交互方式** (子决策):

**选择**: 子进程 exec（`os/exec` 调用 gpiod 工具链），不手写 ioctl，不引入 cgo

```go
// 输出: 设置 GPIO 线电平
exec.Command("gpioset", "gpiochip2", "0=1").Run()       // SYSTEM_OK 拉高
// 输入: 监听中断事件
cmd := exec.Command("gpiomon", "-r", "gpiochip2", "4")  // ESTOP 上升沿
cmd.Start()
```

**Why**:
- 场景频率极低：HEARTBEAT 每秒 2 次翻转，告警一年可能几次，IRQ 监听一条 gpiomon 常驻
- 每秒 2 个 fork 对 A733 8 核完全不可感知
- gpiod 工具链已通过 `apt install gpiod` 安装在 Orange Pi，零额外依赖
- 对标 D1 can-d——它没有手写 AF_CAN ioctl，而是用 Go 标准 `os/exec` 调用 `cansend`
- 代码量最少：`gpio_ctrl.go` ~50 行 vs 手写 ioctl ~300 行
- 可调试性最好：出问题直接 ssh 手动跑 `gpioset` / `gpiomon` 验证，无需专用工具
- 不引入 cgo（`libgpiod`）污染纯 Go 构建链，与项目所有 Go daemon 一致

**gpiomon 子进程管理**:
- 启动时 `exec.Command("gpiomon", "-r", chip, lines...).Start()` 常驻后台
- 通过 `cmd.StdoutPipe()` 逐行解析 JSON 格式事件 `{"event":"FALLING_EDGE","line":4}`
- goroutine 监控 cmd.Wait()，异常退出时 `RestartSec=5` systemd 自动拉起
- SIGTERM → `cmd.Process.Signal(syscall.SIGTERM)` → 等待 2s → `cmd.Process.Kill()`

**Alternatives considered**:
- Go ioctl 直调: 零依赖、最低延迟，但手写 `struct gpio_v2_line_request` 等内核 ABI 约 300 行，且 gpiolib cdev v1/v2 版本兼容需额外处理。D3 场景不足够高频不值得
- cgo + libgpiod: API 语义清晰但引入 cgo 编译链→交叉编译痛苦，且破坏项目纯 Go daemon 一致性

**Alternatives considered** (daemon 层面):
- Python daemon (gpiod 库): 与 audio-monitor/vision-service 统一但 Python gpiod binding 非标准库，需 `pip install python3-gpiod`。Go 的 ioctl 直调更简洁
- 合并到 api-server: api-server 的 WebSocket hub 已有 MQTT 订阅，但违反"每内核模块一 daemon"原则，且 api-server 是 Docker 部署而 GPIO 需内核设备访问

**物理输出说明** (2026-06-03): GPIO 线输出为 3.3V 逻辑电平（Orange Pi 40-pin 排针），不是板载 LED。实际部署需外接 LED（串 220Ω 限流电阻）或继电器模块做功率驱动。D3 模块只负责逻辑信号——这是工业 I/O 的标准分层：控制器给信号，驱动电路做功率放大。

**健康评估规则引擎** (子决策):

**选择**: 纯代码规则引擎（Go switch-case 硬编码），不引入 YAML 规则文件

```go
func evaluateHealth(alerts []Alert, heartbeats map[string]time.Time) (sysOK, gwAlert bool) {
    hasCritical := false
    hasWarning := false
    
    for _, a := range alerts {
        if a.Severity == "CRITICAL" { hasCritical = true; break }
        if a.Severity == "WARNING"  { hasWarning = true }
    }
    // 检查心跳超时 >120s
    for id, lastHB := range heartbeats {
        if time.Since(lastHB) > 120*time.Second { hasWarning = true }
    }
    
    if hasCritical  { return false, true  }  // 绿灯灭+黄灯亮
    if hasWarning   { return true,  true  }  // 绿灯亮+黄灯亮=需关注
    return true, false                      // 绿灯亮=一切OK
}
```

**Why**:
- 4 条规则非常稳定，不太可能需要非开发人员调阈值
- 对标 D1 can-d——topic 路由也是硬编码在 Go 代码里
- 约 50 行 vs YAML 方案 150 行，保持 `health.go` 简洁
- 未来如需规则外部化，从 switch-case 重构到 YAML 约 1 小时，不需要提前设计

**告警去重**: 同一 (source, severity) 在 30s 窗口内只触发一次 GPIO 状态变更，避免抖动。与 llm-analyzer 决策 8 的 5 分钟去重窗口思路一致但时间更短（GPIO 是物理输出，闪灯比报告更明显需要更快恢复）。

#### 决策 6: 部署架构 — 2 systemd units（2026-06-03）

**选择**: 内核模块 + Go daemon 走 systemd，对标 D1/D2。内核模块通过 `modprobe` 加载

**systemd units**:
```ini
# /etc/systemd/system/edgevib-gpio-load.service
[Unit]
Description=EdgeVib Digital I/O — Virtual GPIO Controller
After=network.target
Before=edgevib-gpio-d.service

[Service]
Type=oneshot
ExecStart=/sbin/modprobe edgevib-gpio
ExecStartPost=/bin/chmod 666 /dev/gpiochip2
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target

# /etc/systemd/system/edgevib-gpio-d.service
[Unit]
Description=EdgeVib Digital I/O — GPIO Daemon
After=mosquitto.service edgevib-gpio-load.service
Wants=mosquitto.service

[Service]
Type=simple
User=orangepi
SupplementaryGroups=gpio           # /dev/gpiochip2 访问权限
ExecStart=/usr/local/bin/edgevib-gpio-d --config /opt/edge-gateway/config/edgevib-gpio.yaml
Restart=always
RestartSec=5
MemoryMax=40M

[Install]
WantedBy=multi-user.target
```

**启动顺序**:
```
edgevib-gpio-load.service (modprobe + chmod)
        │
        └── edgevib-gpio-d.service
              After=mosquitto.service
              (不依赖 TimescaleDB — 纯 GPIO+MQRTT 操作)
```

**Why systemd 而非 Docker**:
- 与 D1/D2 内核模块部署一致
- GPIO `/dev/gpiochip2` 是内核创建的设备节点，直接访问无需 Docker `--device` 透传
- Systemd `SupplementaryGroups=gpio` 一行搞定权限，比 Docker `--device /dev/gpiochip2 --group-add gpio` 更简洁
- 符合"硬件直通→systemd"的架构分层（vision-service/audio-monitor/rs232-gateway 同属此层）

**配置** (`config/edgevib-gpio.yaml`):
```yaml
gpio:
  chip_path: "/dev/gpiochip2"
  lines:
    system_ok: 0
    gateway_alert: 1
    heartbeat: 2
    reserved: 3
    estop_master: 4
    psu_fail_in: 5
  heartbeat_interval_ms: 500

mqtt:
  broker: "localhost"
  port: 1883
  client_id: "edgevib-gpio-d"
  subscribe_topics:
    - "EdgeVib/system/monitoring/alert"          # Prometheus Alertmanager
    - "EdgeVib/+/inference/+/ai/report"           # inference-engine
    - "EdgeVib/+/gateway/+/status/health"         # ESP32 在线状态
  publish_estop_topic: "EdgeVib/system/emergency/estop"
  publish_psu_topic: "EdgeVib/system/emergency/psu_fail"

health:
  interval_s: 30
  topic: "EdgeVib/system/health/edgevib-gpio-d"
  service_timeout_s: 120    # 核心服务心跳超时（大于此值判定为离线）

logging:
  level: "info"
```

**Alternatives considered**:
- Docker 部署: 与 inference-engine/llm-analyzer 统一编排但需内核设备透传，且不符合硬件直通分层
- 合并 GPIO daemon 与现有的 can-d 或 iio-d: 违反 SRP，每个内核模块一个 daemon 是已形成的清晰模式

#### 决策 7: 测试策略 — 三层测试 + 硬件无关（2026-06-03）

**选择**: Shell 内核模块测试 + Go 单元测试 + Python E2E 集成测试，均可无真实 GPIO 硬件执行

**测试层级**:

| 层 | 测试内容 | 环境 | 对标 |
|----|---------|------|------|
| **Shell 内核测试** | 7 项: module lifecycle / gpiodetect / gpioinfo / GPIO set+get / inject_irq + gpiomon / sysfs counters / rmmod | Orange Pi | D2 `test_module.sh` (7 项) |
| **Go 单元测试** | 配置加载、健康评估规则引擎（MQTT severity→GPIO 输出逻辑）、MQTT topic 解析 | PC (go test) | D1/D2 Go 单测 |
| **Python E2E** | MQTT→daemon→GPIO 完整链路: paho 发模拟告警 → ssh exec gpioinfo 验证输出 → sysfs 读回确认 | Orange Pi | D2 `test_e2e.py` (4 项) |

**Shell 测试清单** (7 项):
```bash
# Test 1: module lifecycle
insmod edgevib_gpio.ko && gpiodetect | grep edgevib-gpio

# Test 2: gpioinfo 查看线定义
gpioinfo gpiochip2 | grep -E "SYSTEM_OK|GATEWAY_ALERT|ESTOP_MASTER"

# Test 3: 输出方向 set+get 闭环
gpioset gpiochip2 0=1 && gpioget gpiochip2 0 | grep "1"
gpioset gpiochip2 0=0 && gpioget gpiochip2 0 | grep "0"

# Test 4: 注入 IRQ + gpiomon 监听
timeout 2 gpiomon -r gpiochip2 4 &
sleep 0.1
echo "4 0" > /sys/devices/virtual/gpiochip/edgevib-gpio/inject_irq
wait  # gpiomon 应打印 falling edge 事件

# Test 5: 上升沿 IRQ 注入
timeout 2 gpiomon -r gpiochip2 5 &
sleep 0.1
echo "5 1" > /sys/devices/virtual/gpiochip/edgevib-gpio/inject_irq
wait  # gpiomon 应打印 rising edge 事件

# Test 6: sysfs 计数器验证
cat /sys/devices/virtual/gpiochip/edgevib-gpio/irq_count   # >= 2
cat /sys/devices/virtual/gpiochip/edgevib-gpio/last_irq_time_ms  # > 0

# Test 7: rmmod cleanup
rmmod edgevib_gpio && test ! -e /dev/gpiochip2
```

**Why 可无硬件测试**: 虚拟 GPIO 芯片的所有状态在内核内存中。`gpioset` 写输出→`gpioget` 读回形成软件自闭环。`inject_irq` sysfs 注入中断→`gpiomon` 监听验证。不需要接真实的继电器或按钮。

**Why 不用真实 GPIO 测试**: Orange Pi 40-pin 接真实硬件虽可做但风险高——GPIO 电压 3.3V，接错烧板子。虚拟芯片零风险。且 D1(vcan)/D2(sysfs iiio)/D3(gpio) 都是纯软件方案，保持一致。

**Alternatives considered**:
- 4 层 (加 KUnit): 与 D2 决策 8 一致——Phase 1 Shell 测试覆盖内核模块完整生命周期，KUnit 留到 Phase 2 探索

#### 决策 8: 项目结构 — 对标 D1/D2 目录布局（2026-06-03）

**选择**: 内核模块单文件 + Go daemon 3 文件 + 2 systemd units + 2 测试文件

**源码树**:
```
drivers/edgevib_gpio/
├── edgevib_gpio.c               # 内核模块: gpio_chip + irq_chip + sysfs (~350行)
├── Makefile                     # Kbuild (对标 D1/D2)
├── gpio-d/                      # Go daemon: MQTT→GPIO 双向
│   ├── main.go                  # 入口: 配置加载 → 信号处理 → 主循环
│   ├── gpio_ctrl.go             # gpiolib cdev 封装: get/set/direction + gpiomon
│   ├── health.go                # MQTT 订阅 + 健康评估 + 30s health 上报
│   └── go.mod                   # module edgevib/edgevib-gpio-d
├── systemd/
│   ├── edgevib-gpio-load.service   # oneshot: modprobe + chmod
│   └── edgevib-gpio-d.service      # simple: daemon, After=mosquitto+load
└── test/
    ├── test_module.sh            # 7 项 Shell 内核测试
    └── test_e2e.py               # Python E2E: MQTT→GPIO→sysfs 完整链路

config/
└── edgevib-gpio.yaml             # gpio-d 配置 (同级 vcan-edgevib.yaml, iio-vibration.yaml)
```

**源码行数对标**:
| 组件 | D1 (CAN) | D2 (IIO) | D3 (GPIO) |
|------|---------|---------|-----------|
| 内核模块 | 170 行 | 375 行 | ~350 行 |
| Go daemon | 4 文件 | 3 文件 | 3 文件 |
| systemd units | 2 | 2 | 2 |
| Shell 测试 | 7 项 | 7 项 | 7 项 |

**一级目录命名**: `drivers/edgevib_gpio/` — 下划线分隔，与 D1 `vcan_edgevib/` 和 D2 `iio_vibration/` 一致，不被废弃的 `edgevib_buffer/` 目录名因历史原因混用连字符。

**存量代码处理**:
- `drivers/edgevib_buffer/` — 保留为 blk-mq 历史参考，不移除
- flush-d Go daemon 与本 daemon 无共享逻辑——前者是 MQTT→块设备→DB 管道，后者是 MQTT→GPIO 双向控制

**Alternatives considered**:
- 重名 `edgevib_digital_io/`: 名字过长。gpio 是内核标准术语，清晰度最高
- 复用 `edgevib_buffer/` 目录: 改名混淆历史。新旧 D3 是两个完全不同的子系统，独立目录更干净

### D3 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 0 | **子系统变更** (2026-06-03) | **gpio_chip + irq_chip**（`gpiochip_add`），替代 blk-mq 块设备 |
| 1 | 注册模型 | 直接 `gpiochip_add()`，无 platform/misc 包装 |
| 2 | GPIO 线定义 | 6 线: SYSTEM_OK + GATEWAY_ALERT + HEARTBEAT + RESERVED + ESTOP_MASTER (IRQ) + PSU_FAIL_IN (IRQ) |
| 3 | IRQ Chip 设计 | 独立 `irq_chip` + 线性 IRQ 域 (2 线)，边沿触发，sysfs inject_irq 注入 |
| 4 | sysfs 属性 | 3 个: `irq_count` + `last_irq_time_ms` + `inject_irq` |
| 5 | Go daemon | 3 文件 (`main` + `gpio_ctrl` + `health`)，gpiolib cdev + gpiomon |
| 6 | 部署架构 | 2 systemd units (load oneshot + daemon simple)，硬件直通分层 |
| 7 | 测试策略 | 三层: Shell 内核测试 (7项) + Go 单测 + Python E2E，硬件无关 |
| 8 | 项目结构 | `drivers/edgevib_gpio/` — 内核单文件 ~350行 + Go 3 文件 + 2 units + 2 测试 |
| — | blk-mq 遗留 | 原 `edgevib_buffer/` 目录和 `test/edgevib_minimal.c` 保留为历史参考 |

### D4: Software RTC Driver

**业务关联**: Orange Pi 无电池 RTC，断电时钟归零→TimescaleDB 分区键乱序。同时 `systemd-time-wait-sync.service` 无 RTC 设备可等待，Grafana/TimescaleDB 等依赖 `time-sync.target` 的服务提前启动、写入错误时间戳。

**核心价值**: 注册真正的 `rtc_device` 到内核 RTC 子系统 → `hwclock` 命令可用、`/sys/class/rtc/rtcN/` 暴露 RTC 状态可监控、内核 11 分钟自动回写保持 RTC 准确、未来加装电池 RTC 硬件（I2C DS3231）只需换驱动上层不变。

**知识点**: `struct rtc_device` / `struct rtc_class_ops` / `devm_rtc_device_register()` / `struct platform_driver` / `module_platform_driver()` / DT overlay / `platform_device_register_simple()`

#### 数据流概览

```
Boot:  rtc-d (Go daemon) 读 /var/lib/edgevib/last_time
         → ioctl(RTC_SET_TIME) 设置内核 RTC
         → 内核启动流程 hwclock --hctosys → RTC read_time → 设系统时钟初值

Runtime: chrony 逐步修正系统时钟 (不碰 RTC)
         内核 RTC 子系统每 11 分钟自动 set_time 回写 (NTP 时间→RTC)
         rtc-d 每 30s 周期持久化: ioctl(RTC_READ_TIME) → 写 /var/lib/edgevib/last_time

Shutdown: systemd → SIGTERM → rtc-d 捕获
            → ioctl(RTC_READ_TIME) 最后一次读 RTC → 写文件 → 退出
```

#### 决策 1: 内核模块 vs 用户空间方案 (2026-06-03)

**选择**: 内核模块，注册 `rtc_device` 到 RTC 子系统

**Why**:
- `fake-hwclock` 纯用户空间方案只能恢复时间，无法让 `hwclock`/`systemd-time-wait-sync.service`/`/sys/class/rtc/` 识别 RTC 设备
- 注册到 RTC 子系统后内核每 11 分钟自动回写，NTP 同步后的准确时间自动传播到 RTC
- 与 D1-D7 统一学习目标——覆盖 `rtc_class_ops` + `platform_driver` + DT overlay
- 未来加装 I2C DS3231 电池 RTC，只需换内核模块，上层工具链不变

**Alternatives considered**:
- `fake-hwclock`: 零内核代码，但失去全部学习价值和 RTC 子系统集成能力
- 纯 systemd timer + shell 脚本: 最轻量但无 sysfs 接口，无 `hwclock` 支持

#### 决策 2: Platform Device 匹配方案 (2026-06-03)

**选择**: DT overlay（`.dts` → `.dtbo`），通过 `of_match_table` 匹配 `compatible = "edgevib,rtc-edgevib"`

**Why**:
- 标准内核开发姿势——`platform_driver` + `of_match_table` + DT overlay 是嵌入式 Linux 驱动的核心流程
- 面试展示价值：能讲清楚 DT → platform bus → driver probe 全链路
- 与 D1-D3 差异化——D4 是 driver roadmap 中第一个引入 platform_driver 的模块
- 保留未来硬件扩展接口：加装 I2C DS3231 时只需写新的 DT overlay 而不改驱动代码

**风险**: Orange Pi 4 Pro 内核 6.6.98 可能未开启 `CONFIG_OF_OVERLAY`，需通过 U-Boot `fdtoverlays` 路径加载（Allwinner 主线支持），或重新编译内核。在实施阶段验证。

**Alternatives considered**:
- `platform_device_register_simple()`: 自注册 device，不需要 DT overlay，更简单但失去 DT 学习价值
- 不走 platform bus，纯 misc 设备: 更简单但 RTC 子系统本就是用 platform_driver 建模的，不匹配

#### 决策 3: Go Daemon 职责边界 (2026-06-03)

**选择**: Daemon 全管——开机恢复时间 + 运行中周期性持久化 + 关机保存

**Why**:
- 内核模块不做文件 I/O（反模式），只管理虚拟 RTC 芯片内部计数器（`rtc_base + jiffies 自增偏移`）
- Daemon 用标准 Linux 接口与内核交互：`ioctl(RTC_SET_TIME)` 写时间、`ioctl(RTC_READ_TIME)` 读时间
- 周期性持久化（30s）防止突然断电损失过多时间——这是 D4 核心场景（Orange Pi 会断电）的针对性设计
- 与 D1-D3.5 统一长期运行 Go 服务 + 健康上报模式

**Alternatives considered**:
- 内核模块直接读写文件: 内核文件 I/O 反模式，Linus 会 nack
- Daemon 只做一次性设置后退出: 失去周期持久化保护，突然断电最多丢失整个运行时段的时间

#### 决策 4: 关机保存触发机制 (2026-06-03)

**选择**: systemd SIGTERM → daemon 捕获 → 末次 RTC 读取 + 文件写入 + `TimeoutStopSec=15`

**为什么**:
- 与 D1-D3.5 的 daemon 统一用 `signal.NotifyContext(ctx, SIGINT, SIGTERM)` + `<-ctx.Done()` 优雅退出
- 双重保险：30s 周期持久化已有，关机时只是做最后一次 flush。systemd 来不及发 SIGTERM 也只丢最多 29 秒
- `/var/lib/edgevib/last_time` 存 Unix epoch (UTC)，`date +%s` 格式，10 位 ASCII，与 fake-hwclock 兼容

**Alternatives considered**:
- systemd `ExecStop` 独立 shell 脚本: 多一个维护点，与 D1-D3.5 统一模式不一致
- 内核 `platform_driver.shutdown` 回调: 内核文件 I/O 反模式 + shutdown 时文件系统可能已卸载

#### 决策 5: RTC 时间模型 (2026-06-03)

**选择**: 独立时间源——RTC 内部维护 `rtc_base + jiffies 自增偏移`，与系统时钟分离

**Why**:
- 语义正确：真实 RTC 芯片 `read_time` 读的是芯片内部寄存器，不是系统时钟。`rtc_base + 自增偏移` 模拟了这个硬件行为
- 时间链清晰：Boot 时 daemon 设 RTC → hwclock --hctosys 读 RTC 设系统时钟 → chrony 修正系统时钟 → 内核每 11 分钟自动把 NTP 准确时间写回 RTC
- 内核 11 分钟自动回写有了真实意义——如果 RTC 直接返回系统时钟，`set_time` 改系统时钟会形成死循环
- 可观测性：`cat /sys/class/rtc/rtc-edgevib/since_epoch` 与 `date +%s` 输出可能不同，运维能看到 RTC 偏差量

**Alternatives considered**:
- 系统时钟镜像（`ktime_get_real_seconds()` 直接返回）: RTC 无独立性，`hwclock --show` 与 `date` 永远相同，失去"真实 RTC 行为模拟"的学习价值

#### 决策 6: `rtc_class_ops` 实现范围 (2026-06-03)

**选择**: 实现全部 8 个 ops：`read_time` + `set_time` + `read_alarm` + `set_alarm` + `alarm_irq_enable` + `read_offset` + `set_offset` + `proc`

**Why**:
- 为未来加装真实电池 RTC 硬件（I2C DS3231 等）做全知识储备——每个 ops 都是面试可讲的知识点
- `set_alarm` 用内核 `hrtimer` 模拟硬件闹钟中断，到期回调中调 `rtc_update_irq(dev, 1, RTC_AF | RTC_IRQF)` — **面试高光点**，展示 Linux 内核高精度定时器 + RTC 子系统中断模型
- `rtcwake -d /dev/rtcN -s 10 -m no` 可验证整个闹钟链路正常触发，不需要真实待机硬件
- `read_offset`/`set_offset` 返回 0/空操作——面试时能清楚解释"软件 RTC 基于 jiffies，不需要独立振荡器 ppm 级别的频率补偿"，理解为什么硬件 RTC 需要 offset 功能

**实现细节**:
| ops | 实现 | 关键技术点 |
|-----|------|-----------|
| `read_time` | `rtc_base + jiffies_to_timespec64()` | 核心逻辑 |
| `set_time` | 重新校准 `rtc_base` 偏移 | 核心逻辑 |
| `read_alarm` | 返回存储的 alarm 时间 | getter |
| `set_alarm` | `hrtimer_init()` + `hrtimer_start(HRTIMER_MODE_ABS)` → 回调中 `rtc_update_irq()` | hrtimer + RTC 中断信号 |
| `alarm_irq_enable` | 1→重启 hrtimer, 0→`hrtimer_cancel()` | IRQ mask 语义 |
| `read_offset` | 返回 0（无独立振荡器频率偏差） | 诚实 stub |
| `set_offset` | 空操作（jiffies 已随 NTP 修正） | 同上 |
| `proc` | 暴露自定义行: last_daemon_sync, save_count | /proc/driver/rtc 增强 |

**验证测试**: `rtcwake -d /dev/rtcN -s 10 -m no` → 10 秒后 `/sys/class/rtc/rtcN/wakealarm` 事件触发，设备文件变为可读。

**Alternatives considered**:
- 只做 read_time/set_time 两个最小 ops: 简单但失去 hrtimer + rtc_update_irq 的学习深度，且真实硬件 RTC 有 alarm 功能时应具备
- 省略 offset: 同样可接受，但这 2 个 stub 只加 ~20 行代码，面试价值 / 代码行数比极高

#### 决策 7: 自定义 sysfs 属性 (2026-06-03)

**选择**: 4 个自定义属性，挂在 platform device 节点下（`/sys/devices/platform/rtc-edgevib/`）

**Why**:
- 与 D1-D3.5 模式一致——counter + timestamp 对（set_time_count + last_set_time_ms, alarm_fired_count + last_alarm_time_ms）
- 挂在 platform device 节点是有意为之——D1-D3 都没用过 platform bus 的设备模型，D4 首次引入，完整的 "platform device → drvdata → sysfs group" 链路是面试知识点
- 运维可监控："set_time_count 增长慢 → 内核 11 分钟自动回写可能异常" / "alarm_fired_count 不增 → hrtimer 链路故障"

**属性清单**:
| 属性 | 类型 | 含义 | 对标 |
|------|------|------|------|
| `set_time_count` | RO, atomic | set_time 调用总次数 | D1 `crc_errors` |
| `last_set_time_ms` | RO, atomic64 | 最后一次 set_time 时间戳 | D2 `last_injection_time_ms` |
| `alarm_fired_count` | RO, atomic | hrtimer 闹钟触发次数 | D3.5 `irq_count` |
| `last_alarm_time_ms` | RO, atomic64 | 最后一次闹钟触发时间 | D3.5 `last_irq_time_ms` |

**实现**: `platform_set_drvdata(pdev, priv)` + `devm_device_add_groups(&pdev->dev, edgevib_rtc_groups)` 在 probe 中挂载

**Alternatives considered**:
- 挂在 RTC 设备的 `dev` 下: 路径更直观(`/sys/class/rtc/rtc-edgevib/device/`)但失去 platform bus 设备模型的学习价值

#### 决策 8: 项目结构与文件布局 (2026-06-03)

**选择**: 12 文件布局，DT overlay 与模块同目录，daemon 配置走代码默认值 + 可选 YAML

**Why**:
- 与 D1-D3.5 保持统一目录结构：内核模块 .c + Go daemon 子目录 + systemd 单元 + 测试脚本
- DT overlay 放在 `drivers/rtc-edgevib/` 而非新建 `dts/` 顶层目录——目前无 dts 目录，D4 的 overlay 是唯一 DT 文件，就近放置
- Daemon 配置沿用现有模式：代码中写死合理默认值（RTC 设备 `/dev/rtc0`、持久化路径 `/var/lib/edgevib/last_time`、保存间隔 30s、健康间隔 30s），可选 `-config` YAML 覆盖

**源码树**:
```
edge-gateway/drivers/rtc-edgevib/
├── Makefile                              — 内核模块编译 (KERNEL_DIR ?= /lib/modules/$(uname -r)/build)
├── edgevib_rtc.c                         — 内核模块 (~350行): platform_driver + rtc_class_ops + hrtimer + sysfs
├── rtc-edgevib.dts                       — DT overlay 源: compatible="edgevib,rtc-edgevib"
├── rtc-d/
│   ├── go.mod                            — module edgevib/rtc-d
│   ├── main.go                           — 入口: 配置加载 → ioctl 设时间 → 30s周期持久化 → SIGTERM优雅退出
│   ├── rtc_io.go                         — /dev/rtcN + RTC_RD_TIME/RTC_SET_TIME ioctl 封装
│   ├── persist.go                        — /var/lib/edgevib/last_time 文件读写
│   └── health.go                         — 30s MQTT 健康上报 (对标 D1-D3.5)
├── systemd/
│   ├── edgevib-rtc-load.service          — oneshot: DT overlay 加载 + modprobe
│   └── edgevib-rtc-d.service             — simple: After=edgevib-rtc-load.service
├── test/
│   ├── test_module.sh                    — 内核模块测试 (insmod→sysfs→hwclock→rtcwake alarm→rmmod)
│   └── test_e2e.py                       — 端到端测试 (MQTT→daemon→RTC→文件持久化)
└── config/
    └── rtc-edgevib.yaml                  — Go daemon 可选配置 (rtc_device, persist_path, period_save_s, health)
```

**Daemon 默认值**: RTC 设备 `/dev/rtc0`, 持久化路径 `/var/lib/edgevib/last_time`, 保存间隔 30s, 健康间隔 30s。所有默认值可通过 `-config` 覆盖，对标 D1 can-d / D2 iio-d 模式。

**Alternatives considered**:
- 配置文件外置到 `edge-gateway/config/`: 目前 D1-D3.5 config 文件未提交 git（代码默认值已够用），D4 遵循同一模式

#### 决策 9: 测试策略 (2026-06-03)

**选择**: 两层测试——`test_module.sh`（10 项内核模块测试）+ `test_e2e.py`（4 项端到端集成测试）

**Why**:
- 对标 D1-D3.5 测试分层模式
- 内核模块测试覆盖 D4 特有的 DT overlay 加载 + `hwclock`/`rtcwake` 标准工具链验证
- E2E 测试覆盖完整生命周期：时间恢复 → 周期持久化 → SIGTERM 末次保存 → MQTT 健康上报
- Alarm 测试用 `rtcwake -s 3 -m no` 验证 hrtimer + `rtc_update_irq()` 全链路——允许 ±1s 抖动（hrtimer 非硬实时），这是**面试高光测试用例**

**test_module.sh** (bash, 10 项):
| # | 测试 | 验证点 |
|---|------|--------|
| 1 | DT overlay 加载 | `/sys/devices/platform/rtc-edgevib/` 出现 |
| 2 | insmod | `lsmod` + `dmesg` 无 error |
| 3 | RTC 标准 sysfs | `/sys/class/rtc/rtc-edgevib/since_epoch` 可读 |
| 4 | 自定义 sysfs (×4) | `set_time_count`, `last_set_time_ms`, `alarm_fired_count`, `last_alarm_time_ms` 均可 cat |
| 5 | `hwclock --show` | 能读 RTC 时间 |
| 6 | `hwclock --systohc` → `hwclock --show` | 写入后读取 ±1s 误差内 |
| 7 | `rtcwake -d /dev/rtcN -s 3 -m no` | `alarm_fired_count` +1, 设备文件变为可读 |
| 8 | `hwclock --set` 链路 | set_time → set_time_count +1 |
| 9 | 无 DT overlay 时 probe 不触发 | 模块加载但设备不创建 |
| 10 | rmmod | 资源清理完整, `/sys/class/rtc/rtc-edgevib` 消失 |

**test_e2e.py** (Python, 4 项):
| # | 测试 | 验证点 |
|---|------|--------|
| 1 | 完整生命周期 | daemon 启动→从文件恢复时间→`hwclock --show` 验证→35s 后文件被周期性更新→SIGTERM→文件最终写入 |
| 2 | 断电模拟 | 写旧时间到 `last_time`→启动 daemon→`hwclock --show` 为旧时间(非 epoch 0) |
| 3 | MQTT 健康上报 | `mosquitto_sub` 监听→30s 内收到健康消息含 `rtc_device`, `persist_path`, `save_count` |
| 4 | systemd restart 恢复链 | daemon crash→systemd `Restart=always`→时间从文件恢复→日志验证 |

**Alternatives considered**:
- 不做 alarm 测试: 失去 hrtimer + rtc_update_irq 链路的验证覆盖
- E2E 不做 systemd restart 链: 丢失 D4 核心业务价值"—断电后时间能恢复"的正向验证

#### D4 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 内核模块 vs 用户空间 | 内核模块 — 注册 rtc_device 到 RTC 子系统 |
| 2 | Platform Device 匹配 | DT overlay + of_match_table |
| 3 | Go Daemon 职责 | Daemon 全管 — 开机恢复 + 30s 周期持久化 + 关机保存 |
| 4 | 关机保存触发 | SIGTERM 优雅退出 + 末次 flush |
| 5 | RTC 时间模型 | 独立时间源 — rtc_base + jiffies 自增 |
| 6 | rtc_class_ops 范围 | 全部 8 个 ops — hrtimer 模拟 alarm |
| 7 | 自定义 sysfs | 4 个属性, 挂在 platform device 节点 |
| 8 | 项目结构 | 12 文件, DT overlay 同目录, 代码默认值 |
| 9 | 测试策略 | test_module.sh (10 项) + test_e2e.py (4 项) |

### D5: HWMON Motor Health — 车间级多电机电气健康监测（2026-06-03 设计）

**业务定位**: D2 IIO 管机械域（振动），D5 HWMON 管电气域（电流/电压/温度）。车间有多台电机，每台电机的 F407 ADC1 采集电流(PA0/IN8)、电压(PB1/IN9)、温度(PA0/IN0)通过 CMD 0x06 上报。D5 通过标准 Linux hwmon 接口暴露这些参数，`sensors` 命令一键查看全车间电机健康，Prometheus hwmon collector 零代码采集，hwmon 框架自带的 `_alarm` 实现过温/过流自动告警。

**核心架构**:
```
车间 (factory1)
  motor01: F407 ADC1 → CMD 0x06(电流/电压/功率) + NTC(温度)
  motor02: F407 ADC1 → CMD 0x06 + NTC
  ...

Orange Pi 4 Pro (车间网关)
  │
  TimescaleDB: environment_view / motor data → edgevib-hwmon-d (Go)
  │                                                  │
  │                              write(/dev/edgevib-hwmon-inject)
  │                                                  │
  │                              ┌─── 内核态 ────────┤
  │                              │  edgevib_hwmon.ko │
  │                              │                   │
  │                              │  motor[0] → hwmon0 (motor01)
  │                              │  motor[1] → hwmon1 (motor02)
  │                              │  motor[N] → hwmonN (motor0N)
  │                              └───────────────────┘
  │                                        │
  $ sensors                          /sys/class/hwmon/hwmon*/
  Prometheus hwmon collector         Grafana hwmon plugin
```

**D5 在 EdgeVib 内核子系统矩阵中的位置**:
| 维度 | D1 (vcan) | D2 (iio) | D3 (gpio) | D4 (rtc) | **D5 (hwmon)** |
|------|-----------|----------|-----------|----------|----------------|
| 物理域 | 通信 | 机械 | 控制 | 时间 | **电气** |
| 设备数量 | 1 | 1 | 1 | 1 | **N (车间多电机)** |
| 框架告警 | 无 | 无 | 无 | 无 | **_alarm 自动** |
| 消费工具 | candump | iio_readdev | gpioinfo | hwclock | **sensors** |

**Linux 内核知识点** (12 项):
| # | 知识点 | 在 D5 中的位置 | 面试价值 |
|---|--------|---------------|---------|
| 1 | `struct hwmon_chip_info` | channel 定义 + ops 填充 | ★★★ hwmon 框架核心 API |
| 2 | `devm_hwmon_device_register_with_info()` | 现代注册 API，自动资源管理 | ★★★ devm 模式在 hwmon 的应用 |
| 3 | `hwmon_temp`/`hwmon_curr`/`hwmon_in`/`hwmon_power` | 4 种 channel type，每种有不同 sysfs 属性 | ★★★ channel type 的语义区分 |
| 4 | `is_visible` 回调 | 动态控制通道可见性（`active` flag 控制） | ★★ umode_t 权限控制 |
| 5 | `read` 回调 | val 返回 s32 (millidegree/millivolt 等) | ★★ 理解 hwmon 的单位约定 |
| 6 | `_alarm` 自动管理 | 框架自动对比 `_input` vs `_max` → 触发 `_alarm` | ★★★ 零代码的告警机制 |
| 7 | 多设备注册 | 一个 .ko 注册 N 个 hwmon device (D1-D4 都没做过) | ★★★ 动态多实例设计 |
| 8 | `kcalloc` / `struct device *[]` | 管理多设备生命周期 | ★★ 内核动态内存 |
| 9 | cdev + hwmon 组合 | 一个模块同时暴露 cdev（数据入口）和 hwmon sysfs（数据出口） | ★★ 子系统组合 |
| 10 | 数据陈旧检测 | `active` flag + jiffies 超时 → is_visible 隐藏通道 | ★★ 工业级数据质量 |
| 11 | `struct device_attribute` | 自定义 sysfs 属性（对标 D1-D3） | ★★ 设备模型 |
| 12 | `module_param` | `num_motors=N` 模块参数 | ★ 基本技能 |

#### 决策 1: 多设备架构 — 模块参数 `num_motors=N`

**选择**: `insmod edgevib_hwmon.ko num_motors=4` 指定车间电机数量，内核模块预分配数组 + 为每台电机构建独立的 hwmon device

**Why**:
- 工厂车间电机数量变化 = 物理布线变更 = 系统需停机 = `rmmod` + `insmod` 可接受
- 对标 D1-D4 简单的 Phase 1 哲学，先做静态多设备，将来需要动态增减时加 sysfs `add_motor`
- 数据结构:
  ```c
  struct edgevib_motor {
      char name[32];                    // "motor01"
      struct device *hwmon_dev;         // hwmon_device 指针
      s32 temperature;                  // millidegrees Celsius
      s32 current;                      // milliamperes
      s32 voltage;                      // millivolts
      s32 power;                        // microwatts
      u64 last_update_jiffies;
      bool active;                      // daemon 是否持续注入数据
  };
  struct edgevib_hwmon_priv {
      int num_motors;
      struct edgevib_motor *motors;     // kcalloc
      struct cdev cdev;
      atomic_t injection_count;
      atomic64_t last_injection_time_ms;
      unsigned long stale_timeout_jiffies;  // 默认 10s
  };
  ```

**Alternatives considered**:
- sysfs 动态管理: 运行时增删电机但代码 ~2×，Phase 2 再加
- Go daemon 管理 (ioctl): 逻辑在用户空间但 ioctl 增加耦合

#### 决策 2: 注入机制 — cdev `/dev/edgevib-hwmon-inject`

**选择**: 对标 D2，Go daemon 一次 `write(fd, &struct, 20)` 注入一台电机数据

**注入格式** (20 字节):
```c
struct edgevib_hwmon_inject {
    u32 motor_index;          // 0..N-1
    s32 temperature;          // millidegree (e.g., 52300 = 52.3°C)
    s32 current;              // milliampere (e.g., 2450 = 2.45A)
    s32 voltage;              // millivolt (e.g., 24100 = 24.1V)
    s32 power;                // microwatt (e.g., 59000000 = 59W)
};
```

**Why**: 对标 D2 已验证的 cdev write 模式。ARM64 禁止浮点，s32 定标在 hwmon 框架中天然匹配（sysfs `temp1_input` 单位就是 millidegree）。4 电机 × 1 次 write = 4 次系统调用，替代方案 16 次 sysfs write 的碎片化。

**Alternatives considered**:
- 多个 sysfs store: 4 电机 × 4 通道 = 16 个 store 文件，hwmon 自身 sysfs 已很丰富
- netlink: 100 台设备时的方案，D5 场景过度设计

#### 决策 3: 阈值管理 — hwmon 框架自带可写 sysfs + 内核默认值

**选择**: 内核模块提供 `_max`/`_crit` 默认值，操作员通过 hwmon 框架自带的 sysfs 文件覆写

**默认阈值**:
| 通道 | max | crit | 业务含义 |
|------|-----|------|---------|
| temp1 | 80°C | 100°C | 电机绝缘等级 F (155°C)，留安全裕度 |
| curr1 | 8A | — | PD6010D 额定 10A，80% 预警 |
| in0 | 30V | — | 24V 系统 +25% 安全裕度 |

**Why**: `hwmon_device_register_with_info()` 创建的 sysfs 文件 `temp1_max`、`temp1_crit` 本身是可写的——hwmon 框架已经做好了。`echo 90000 > temp1_max` 立即生效，框架自动对比 `_input` vs `_max` 更新 `_alarm`。systemd `ExecStartPost` 恢复持久化阈值。这是 hwmon 框架的正确用法——不绕过框架自己造轮子。

**Alternatives considered**:
- 内核硬编码: 不同型号电机阈值不同需重新编译
- daemon 注入携带阈值: 每次 write 传阈值冗余（cdev struct 20B→32B）

#### 决策 4: 通道范围 — 纯电气域

**选择**: 4 通道纯电气参数，与 F407 ADC1 采集对齐。振动留给 D2 IIO

| Channel | hwmon type | sysfs 文件 | 单位 | 来源 |
|---------|-----------|-----------|------|------|
| `temp1` | `hwmon_temp` | `temp1_input`, `temp1_max`, `temp1_crit` | millidegree | F407 NTC (PA0/IN0) |
| `curr1` | `hwmon_curr` | `curr1_input`, `curr1_max` | milliampere | F407 U-phase (PB0/IN8) |
| `in0` | `hwmon_in` | `in0_input`, `in0_max` | millivolt | F407 母线电压 (PB1/IN9) |
| `power1` | `hwmon_power` | `power1_input`, `power1_max` | microwatt | 计算: V×I |

**Why**: D2 IIO 已覆盖振动域（加速度传感器属于 IIO 的自然领地）。把振动塞进 hwmon fan1 是语义污染——fan1 单位是 RPM，存 mm/s 让 `sensors` 输出混乱。D5 的独特性 = 电气域。D2(IIO)+D5(hwmon) = 电机机械+电气全貌。

**Alternatives considered**:
- 加振动 fan1_input (原 CONTEXT.md 设计): 违反 hwmon 子系统语义
- 加效率 energy1: Phase 2 计算指标

#### 决策 5: Go Daemon — 4 文件，TimescaleDB 2s 轮询

**选择**: `edgevib-hwmon-d`，pgxpool 直连 TimescaleDB，2s 定时轮询 + `/dev/edgevib-hwmon-inject` 写入

**数据流**:
```
TimescaleDB
  ├── SELECT DISTINCT device_id FROM device_status_view
  │     WHERE device_type = 'motor'
  │   → 发现车间活跃电机列表: motor01, motor02, ...
  │
  ├── 每 2s 对每台电机:
  │     SELECT * FROM environment_view WHERE device_id = $1 ORDER BY time DESC LIMIT 1
  │     → temperature_c
  │     SELECT * FROM sensor_data WHERE device_id = $1 AND data_type = 'motor'
  │     → payload JSONB 解构: current_a, voltage_v, power_w
  │
  └── write(fd, &edgevib_hwmon_inject{motor_index, temp, curr, volt, power})
```

**Why**: 对标 D2 (DB 轮询) 的拉模型。0.5 QPS × N 台电机对 TimescaleDB 零压力。与 D2 iio-d 同构降低理解成本。

**源文件**:
| 文件 | 职责 | 对标 |
|------|------|------|
| `main.go` | 入口 + 配置 + 信号处理 + 主循环 + 电机发现 | D2 main.go |
| `inject.go` | /dev/edgevib-hwmon-inject open/write/close | D2 inject.go |
| `db.go` | TimescaleDB pgxpool + 数据查询 + JSONB 解构 | 新增 |
| `health.go` | 30s MQTT 上报 | D2 health.go |

**Alternatives considered**:
- MQTT 订阅: 对标 D3 gpio-d 的推模型，但数据已在 DB 中，查 DB 更简单

#### 决策 6: 自定义 sysfs — 2 属性对标 D1-D4

**选择**: injection_count + last_injection_time_ms，挂在 cdev 关联的 device 节点

**Why**: 对标 D1-D4 的 2-counter 模式。hwmon 框架自带的 `_alarm` 已提供业务告警，自定义属性只需覆盖注入链路可观测性。

**Alternatives considered**:
- 4 属性: 不需要——hwmon 自带的 `_alarm` 覆盖了业务告警路径

#### 决策 7: 部署 — 2 systemd units

**选择**: `edgevib-hwmon-load.service` (oneshot modprobe) + `edgevib-hwmon-d.service` (simple daemon)，对标 D1-D4

**Why**: 纯 IO 服务（DB连接+内核设备节点写），不需要 Docker。硬件直通→systemd 分层一致。

**Alternatives considered**:
- Docker: Go 二进制 + DB 连接无硬件直通需求，但不符合 D1-D4 统一模式

#### 决策 8: 项目结构 — 对标 D2/D3

```
drivers/edgevib_hwmon/
├── edgevib_hwmon.c              # 内核模块 (~400行)
├── Makefile                     # Kbuild
├── hwmon-d/
│   ├── main.go                  # 入口 + 主循环
│   ├── inject.go                # cdev 写封装
│   ├── db.go                    # TimescaleDB 查询 + JSONB 解构
│   ├── health.go                # 30s MQTT 上报
│   └── go.mod
├── systemd/
│   ├── edgevib-hwmon-load.service
│   └── edgevib-hwmon-d.service
├── test/
│   ├── test_module.sh           # 7 项内核测试
│   └── test_e2e.py              # 4 项端到端测试
└── config/
    └── edgevib-hwmon.yaml
```

**源码行数对标**:
| 组件 | D1 | D2 | D3 | **D5** |
|------|-----|-----|------|------|
| 内核模块 | 170行 | 375行 | 350行 | **~400行** |
| Go daemon | 4文件 | 3文件 | 3文件 | **4文件** |
| systemd units | 2 | 2 | 2 | 2 |
| Shell 测试 | 7项 | 7项 | 7项 | 7项 |

#### 决策 9: 测试策略 — 三层

| 层 | 测试内容 | 验证点 | 环境 |
|----|---------|--------|------|
| **Shell 内核** (7项) | insmod num_motors=2 → sensors 列出 2 个 hwmon → cdev write 注入 → sensors 验证值 → rmmod | module lifecycle, hwmon sysfs, cdev write→read, stale 超时, _alarm 触发 | Orange Pi |
| **Go 单元** | 配置加载、DB mock、inject 正常/错误路径 | 纯函数 + mock pgxpool | PC (`go test`) |
| **Python E2E** (4项) | DB 写入模拟数据 → 启动 daemon → `sensors` 验证 → SIGTERM 优雅退出 → systemd restart | 完整链路: DB→daemon→cdev→hwmon→sensors | Orange Pi |

**关键 Shell 测试亮点**:
```bash
# Test 4: sensors 命令验证 — D5 核心业务价值
sensors | grep "edgevib-hwmon-motor01"
# 应输出: Motor Temp: +52.3°C (high = +80.0°C, crit = +100.0°C)
#         Current: +2.45 A (high = +8.00 A)
#         Voltage: +24.10 V
#         Power: +59.00 W

# Test 6: _alarm 自动告警 — hwmon 框架零代码特性
echo 50000 > /sys/class/hwmon/hwmon0/temp1_max   # 设低阈值
echo "0 60000 0 0 0" | xxd -r > /dev/edgevib-hwmon-inject  # 注 60°C
cat /sys/class/hwmon/hwmon0/temp1_alarm  # 应输出 1
```

#### D5 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 多设备架构 | `num_motors=N` 模块参数 + 内部数组 + 每电机一 hwmon device |
| 2 | 注入机制 | `/dev/edgevib-hwmon-inject` cdev, 20 字节 binary struct |
| 3 | 阈值管理 | hwmon 框架自带可写 sysfs + 内核默认值 |
| 4 | 通道范围 | 4 通道纯电气域 (temp1 + curr1 + in0 + power1) |
| 5 | Go daemon | 4 文件，TimescaleDB 2s 轮询，pgxpool 直连 |
| 6 | 自定义 sysfs | 2 属性: injection_count + last_injection_time_ms |
| 7 | 部署 | 2 systemd units: oneshot load + simple daemon |
| 8 | 项目结构 | `drivers/edgevib_hwmon/` — 内核 ~400行 + 4文件 Go + 2 units + 2 测试 |
| 9 | 测试策略 | 三层: Shell (7项+_alarm) + Go单元 + Python E2E (4项) |

### D6: E-Stop Input Device

**业务关联**: F407 急停状态通过 MQTT (CMD 0x07) 到达，暴露为 input 设备后 `evtest` 直接监控。

**设计**: 注册虚拟 input 设备。daemon 从 MQTT 收到急停事件后 `input_report_key(dev, KEY_STOP, 1); input_sync(dev);`。

**知识点**: `struct input_dev` / `input_register_device()` / `input_report_key()` / `input_sync()`

### D7: MIPI CSI V4L2 Driver (需硬件)

**业务关联**: 为 Orange Pi 4 Pro 的 MIPI CSI 接口编写 V4L2 子设备驱动，使树莓派兼容 CSI 摄像头可用。USB UVC 为默认方案不变，CSI 为备选增强。

**知识点**: DT overlay + `v4l2_subdev_ops` + Media Controller pipeline + DMA buffer

### 实施策略

前 6 个模块独立为 `.ko` 内核模块，存放在 `edge-gateway/drivers/` 目录。内核模块通过 sysfs/netlink/input 等标准接口与用户空间交互，伴随 daemon 从 MQTT/TimescaleDB 获取业务数据后注入驱动。**不修改现有服务代码**。D7 需额外 CSI 摄像头硬件。

---

## 总体优先级汇总

```
已完成: 10 services + model-deploy = 11 个应用服务
服务开发: P1 Training Data Sync → P2 System Monitoring → P3 NTP Server
驱动开发: D1 Virtual CAN → D2 IIO → D3 Block Dev → D4 RTC → D5 HWMON → D6 Input → D7 CSI
          (服务开发和驱动开发可并行推进)
```

---

## 数据流路径

```
路径 A (主通道):
  F407(UART4) → ESP32-S3 → WiFi/MQTT → Orange Pi (Mosquitto) → data-aggregator → TimescaleDB

路径 B (备份通道):
  F407(UART5/RS232) → rs232-gateway → MQTT → Orange Pi

路径 C (视觉):
  USB Camera → vision-service (V4L2) → 文件存储 + TimescaleDB 元数据

路径 D (声学):
  MEMS 麦克风 → audio-monitor (sounddevice) → FFT 特征 → TimescaleDB + WAV 存档

路径 E (固件分发, OTA):
  ota-server (HTTP) → ESP32 ota_update (自更新 A/B 分区)
  ota-server (HTTP) → ESP32 ota_relay → UART4 → F407 Bootloader (SPI Flash → 内部 Flash)

路径 F (模型部署, P0 待实现):
  Edge-AI PC → Model Deploy Service → ESP32 (TFLite via OTA) + Orange Pi (ONNX 热替换)

路径 G (训练数据回流, P1 待实现):
  Orange Pi TimescaleDB → Training Data Sync → Edge-AI PC (CSV)
```

## AI 模型分层

| 层级 | 设备 | 模型 | 格式 | 用途 |
|------|------|------|------|------|
| 实时推理 | ESP32-S3 | CNN-LSTM | TFLite Micro | 振动故障 4 分类 |
| 批量推理 | Orange Pi | Autoencoder | ONNX Runtime | 异常检测（重构误差） |
| 统计趋势 | Orange Pi | numpy/scipy | Python | RMS 斜率/频漂/DE-NDE ratio |
| 语义分析 | Orange Pi | Qwen2.5-1.5B | GGUF (llama.cpp) | 故障报告自然语言生成 |
| 视觉检测 | Orange Pi | OpenCV | Python | 画面异常检测 (Phase 2) |

## 部署分层

- **Docker**: 复杂依赖隔离 — TimescaleDB, Grafana, inference-engine (ONNX), llm-analyzer (llama.cpp 编译链), data-aggregator (pgx), api-server, ota-server
- **systemd**: 硬件直通 — rs232-gateway (dialout), vision-service (video), audio-monitor (audio), opcua-server

---

## Model Deploy Service 设计

### 决策 1: 服务边界 (2026-05-28)

**选择**: 独立服务，不与 ota-server 合并

**Why**: 固件和模型的生命周期不同（A/B 分区+重启 vs 文件替换+热加载）。版本维度不同（单一体 vs 多模型独立迭代）。触发源不同（运维手动 vs PC 训练自动）。回滚粒度不同（切换分区 vs 替换回旧文件）。

协作方式：Phase 2 中 ESP32 TFLite 模型更新通过 model-deploy 触发 ota-server 走固件 OTA 流程，API 调用而非内聚。

### 决策 2: Phase 1 范围 (2026-05-28, 更新 2026-05-29)

**选择**: Phase 1 同时覆盖 Orange Pi ONNX + ESP32 TFLite 模型部署

**Why (原)**: ONNX 部署是纯文件操作 + MQTT 信号热重载，本地闭环。

**Why (2026-05-29 扩展至 ESP32)**: Training Data Sync (P1) 打通了数据上行链路，model-deploy 作为模型下行链路如果不覆盖 ESP32 TFLite，则 ESP32 模型更新成为 ML 训练闭环中唯一的手动环节。现在时机成熟——与 Training Data Sync 一起交付，形成完整的"数据上行→训练→双平台自动部署"闭环。

**ESP32 TFLite 部署路径** (模型与固件分离):
```
PC: .tflite → POST /api/v1/models/deploy → model-deploy 存储
  → 文件落入 /opt/edge-gateway/firmware/esp32/ (与 OTA Server 共享 volume)
  → OTA Server version.json esp32.latest_version + file 自动更新

ESP32: 模型更新任务 → GET /firmware/version.json (轮询)
  → 发现新版本 → HTTP GET 下载 .tflite
  → SHA256 校验 → 写入 SPIFFS /models/ 分区
  → TfLiteModelCreate(file_buffer) 热切换
```

**Phase 1 完整功能**:
- PC 训练完成 → HTTP POST 推送 ONNX + TFLite 模型 + metadata → model-deploy
- 模型文件存储 + 版本元数据持久化（TimescaleDB `model_versions` 表加 `platform` 列区分 `orangepi`/`esp32`）
- ONNX: 替换 inference-engine 模型文件 + MQTT 信号热重载
- TFLite: OTA Server version.json 联动 + ESP32 HTTP 下载 + SPIFFS 分区热切换
- REST API 查询模型版本历史和当前部署状态
- ESP32 端固件改造：分区表加 `models` 分区 + `ai_service` 从分区文件加载模型（替代 `model_data.h` 嵌入）

### 决策 3: 模型存储方式 (2026-05-28)

**选择**: 文件系统存模型 + TimescaleDB 存元数据

**Why**: 与 ota-server 固件存储模式一致。ONNX 文件 100-500KB，文件系统直接 mmap/fopen 供 inference-engine 零拷贝加载。DB 不存 blob（TOAST 膨胀、WAL 压力）。ota-server `filestore/` 的 SHA256 + 路径安全校验模式可直接复用。

**存储路径**: `/opt/edge-gateway/models/{model_name}/{version}.onnx`

**TimescaleDB 表**: `model_versions(model_name, version, file_path, sha256, file_size, metrics_json, deployed_at, deployed_by)`

### 决策 4: 触发机制 (2026-05-28)

**选择**: PC HTTP POST 推送 (push)

**Why**: 训练完成事件只有 PC 端知道，Orange Pi 无从得知何时该拉取。事件驱动零延迟，不需要 PC 侧常驻 HTTP 服务。`onnx_converter.py` 训练结束后加一行 `requests.post()` 即可。PC 不可达时 model-deploy 不受影响，推送失败 PC 侧重试。

**API**: `POST /api/v1/models/deploy`，multipart form，字段 `model_file` + `model_name` + `version` + `metrics_json`

### 决策 5: 版本管理 (2026-05-28)

**选择**: 语义版本 (major.minor.patch)，独立于固件版本

**Why**: 模型独立迭代，与固件解耦。`major.minor.patch` 语义清晰（架构/训练数据/bugfix）。人类可读。PC 端在 metadata JSON 中写版本号，推送时携带。

### 决策 6: 回滚策略 (2026-05-28)

**选择**: 保留最近 3 个版本在磁盘 + REST API 一键回滚

**Why**: 3 个 ONNX 文件 ~1.5MB 对 eMMC 零压力。`POST /api/v1/models/{name}/rollback?version=x.y.z` 原子替换文件 + 信号热重载。inference-engine 加载前先 `CreateSession()` 验证，失败保留旧 session。DB `model_versions.deployed_at` 可追溯。超出 3 版本自动轮转清理。

### 决策 7: 热重载机制 (2026-05-28)

**选择**: MQTT 消息通知 inference-engine 重载

**Why**: 不挂载 Docker socket（安全风险）。无轮询开销。架构解耦。inference-engine 已有 MQTT subscriber，加 reload topic 改动小。ACK 机制保证可靠性。

**Topic**: 
- `EdgeVib/{site}/inference/{device}/model/reload` (model-deploy → inference-engine, QoS 1)
- `EdgeVib/{site}/inference/{device}/model/status` (inference-engine → model-deploy, QoS 1)

**文件共享**: 两个 Docker 容器均挂载 `/opt/edge-gateway/models:/models`

### 决策 8: REST API 设计 (2026-05-28)

**选择**: 6 端点，Go `chi` 路由，对标 api-server

**端点清单**:

| 方法 | 路径 | 用途 |
|------|------|------|
| `POST` | `/api/v1/models/deploy` | PC 推送模型 (multipart) |
| `GET` | `/api/v1/models` | 所有模型及当前部署版本 |
| `GET` | `/api/v1/models/{name}` | 单模型详情 + metrics 历史 |
| `GET` | `/api/v1/models/{name}/versions` | 该模型所有历史版本 |
| `POST` | `/api/v1/models/{name}/rollback` | 回滚到指定版本 |
| `GET` | `/api/v1/health` | 健康检查 |

**Why POST 而非 PUT**: deploy 每次触发 reload 副作用，非幂等。POST 语义更准确。

### 决策 9: 部署模式 (2026-05-28)

**选择**: Docker 容器，与 api-server/ota-server 统一编排在 docker-compose

**Why**: 纯 HTTP 服务，不需要硬件直通（不像 rs232-gateway/vision-service/audio-monitor 需要 dialout/video/audio group）。技术栈与 api-server 同构（pgxpool + paho + chi）。共享 volume `/opt/edge-gateway/models:/models` 与 inference-engine 互通模型文件。Go 多阶段构建 → alpine:3.19，镜像 ~20MB，运行时 ~25MB。

### 决策 10: 项目结构 (2026-05-28)

**选择**: 10 源文件 Go Docker 服务，对标 ota-server/api-server 布局

**源码树**: `services/model-deploy/` — cmd/server/main.go + internal/{config,db,filestore,mqtt,handler,middleware}

**Why**: 与 api-server/ota-server 同构（chi + pgxpool + paho + multi-stage Docker）。`filestore/store.go` 直接从 ota-server 复用模式（SHA256 + 路径遍历防护 + 轮转清理）。Go module: `edgevib/model-deploy`。

### 决策 11: PC 端集成 (2026-05-28)

**选择**: `onnx_converter.py` 结尾加 `requests.post` 推送模型到 model-deploy

**Why**: 改动 ~10 行，不破坏现有逻辑。`requests` 已在 edge-ai requirements.txt。LAN 内推送失败脚本报错退出，运维重跑即可。替代当前手工 scp 流程。

**容错**: Phase 1 不做重试。PC 和 Orange Pi 在 192.168.1.x 同一 LAN，网络可靠。模型文件 100-500KB，POST 耗时 <1s。Phase 2 可加指数退避重试。

### 决策 12: 测试策略 (2026-05-28)

**选择**: 双层 — Go 单测 + Python 集成测试，对标 ota-server

| 层级 | 测试对象 | 验证点 |
|------|---------|--------|
| Go 单元 | config, filestore, handler | 配置加载、SHA256、路径安全、响应格式 |
| Go Mock | db/client, mqtt | SQL 正确性、reload topic 格式 |
| Python E2E | 完整 docker-compose 管线 | PC 推送→文件落盘→DB→MQTT→回滚→轮转 |

**E2E 测试文件**: `tests/model-deploy/test_end_to_end.py`，复用 `tests/integration/insert_test_data.sql`。

### 决策 13: 安全策略 (2026-05-28)

**选择**: 无认证 + 预留 X-API-Key middleware，对标 api-server

**Why**: model-deploy 监听 `192.168.1.1:8091`，与 api-server 同在私有 LAN NAT 后面。攻击者如已接入内网可直接连 Mosquitto/TimescaleDB/Grafana，单点加认证不改变威胁面。预留 `auth.enabled: false` 配置开关，一行改 `true` 即启用。

### Model Deploy Service 设计决策汇总

| # | 决策项 | 选择 |
|---|--------|------|
| 1 | 服务边界 | 独立服务，不与 ota-server 合并 |
| 2 | Phase 1 范围 | 仅 Orange Pi ONNX 部署，ESP32 TFLite 留 Phase 2 |
| 3 | 模型存储 | 文件系统 + TimescaleDB 元数据（对标 ota-server filestore） |
| 4 | 触发机制 | PC HTTP POST 推送 (push) |
| 5 | 版本管理 | 语义版本 (major.minor.patch)，独立于固件 |
| 6 | 回滚策略 | 保留最近 3 版本 + REST API 回滚 + 自动轮转 |
| 7 | 热重载 | MQTT 消息通知 inference-engine 换模型 |
| 8 | API 设计 | 6 端点 (deploy/models/versions/rollback/health) |
| 9 | 部署模式 | Docker + docker-compose，共享 volume 与 inference-engine |
| 10 | 项目结构 | 10 源文件 Go Docker 服务 (chi + pgxpool + paho) |
| 11 | PC 端集成 | onnx_converter.py 加 requests.post (~10 行) |
| 12 | 测试策略 | 双层: Go 单测 + Python E2E |
| 13 | 安全 | 无认证 + 预留 X-API-Key middleware |

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
