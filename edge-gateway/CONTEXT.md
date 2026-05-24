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
- **rs232-gateway** (C): USART3/RS232 串口守护进程。复用现有 CRC16-MODBUS 协议解析，F407 直连备份通道。通过 systemd 自启动
- **edge-router** (Go): 多设备路由转发，模拟同级节点数据交换

### AI 推理层
- **inference-engine** (Python/ONNX Runtime): 复杂模型 — 趋势预测、剩余寿命(RUL)、多设备聚合分析
- **llm-analyzer** (Python/llama.cpp): 本地大模型故障报告生成，提示词模板驱动
- **vision-service** (Python/OpenCV + V4L2): USB 摄像头定时拍照 + 存档，V4L2 DMA 零拷贝采集。后期扩展 OpenCV 运动检测/异常识别

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
