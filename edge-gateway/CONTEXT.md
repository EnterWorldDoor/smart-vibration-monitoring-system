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
