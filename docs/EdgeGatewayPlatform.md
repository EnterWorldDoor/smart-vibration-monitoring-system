# EdgeVib 边缘网关平台设计文档（Edge Gateway Platform）

> 运行平台：Orange Pi 4 Pro（全志 A733）· Ubuntu 22.04.5 LTS · kernel 6.6.98-sun60iw2 aarch64
> 权威领域上下文与逐条架构决策（ADR）见 [`edge-gateway/CONTEXT.md`](../edge-gateway/CONTEXT.md)；内核编译经验见 [`edge-gateway/KERNEL-LESSONS.md`](../edge-gateway/KERNEL-LESSONS.md)。

---

# 1. 平台概述

边缘网关是 EdgeVib 系统的"边缘大脑"，介于设备层（STM32/ESP32）与平台层（PC 训练）之间。它承担四类职责：

1. **数据汇聚与持久化** —— 统一接收 ESP32/F407 上报的遥测，去重后写入工业级时序库；
2. **二级 AI 推理** —— 在设备端实时分类之上，做趋势分析、剩余寿命（RUL）预测、电机综合健康评分，并用本地 LLM 生成自然语言故障报告；
3. **工业协议与集成** —— 对上位机/SCADA 暴露 OPC UA，对外提供 REST + WebSocket，管理固件与模型的 OTA 分发；
4. **多模态监测与联动** —— USB 摄像头视觉巡检、声学异常监测、跨车间告警广播。

平台采用**两条并行技术栈**：

- **微服务栈**（应用层）：11 个 Go/Python/C 微服务 + 基础设施，主要走 Docker Compose 编排；
- **内核驱动栈**（系统层，D1–D7）：把工业数据以标准 Linux 子系统（CAN/IIO/block/GPIO/RTC/HWMON/input/V4L2）暴露，每个驱动配套一个用户态 Go daemon。

## 1.1 硬件平台

| 项目 | 规格 |
|------|------|
| SoC | 全志 A733：2×Cortex-A76 + 6×Cortex-A55 @ 2.0GHz + RISC-V E902 协处理器 |
| NPU | 3 TOPS (INT8)，Phase 2 供 inference-engine 加速（预留）|
| 内存 | 4GB LPDDR4x |
| 网络 | Wi-Fi 6 + BT 5.4，千兆以太网（支持 PoE）|
| 摄像头 | 1× 2-lane + 1× 4-lane MIPI CSI |
| 音频 | ES8389 I2S codec + 3.5mm 音频口 |
| OS | Ubuntu 22.04.5 LTS，kernel 6.6.98-sun60iw2 |
| 运行时 | Docker CE + Compose · Go 1.22 · Python 3.10/3.11 · GCC 11.4 |

---

# 2. 基础设施层

| 组件 | 镜像/技术 | 端口 | 职责 |
|------|-----------|------|------|
| **Mosquitto** | eclipse-mosquitto:2.0 | 1883 / 9001(WS) | MQTT 事件总线，所有服务间解耦通信的枢纽 |
| **TimescaleDB** | timescale/timescaledb:pg16 | 5432 | PostgreSQL 16 + 时序扩展，统一共享状态存储 |
| **Grafana** | grafana/grafana | 3000 | 唯一 Web 可视化，7 个预置仪表盘 |
| **Prometheus** | prom/prometheus | 9090 | 指标采集（7 天保留）|
| **Node Exporter** | prom/node-exporter | — | 主机 CPU/内存/温度指标 |
| **cAdvisor** | gcr.io/cadvisor | — | 容器资源指标 |
| **Alertmanager** | prom/alertmanager | 9093 | 告警路由 |
| **alert-webhook** | 自研桥接 | 8092 | Alertmanager → MQTT 告警桥 |

> NTP 时间同步（P3）由 chrony 承担（`config/chrony.conf`），Orange Pi 作为局域网时间源，ESP32 作为 NTP 客户端。

## 2.1 数据模型（TimescaleDB Hypertables）

数据模型定义于 [`edge-gateway/docker/timescaledb/init.sql`](../edge-gateway/docker/timescaledb/init.sql)，共 **9 张 hypertable**：

| 表 | 用途 | 写入方 | 保留策略 |
|----|------|--------|----------|
| `sensor_data` | 原始遥测（JSONB payload）| data-aggregator | 90 天 |
| `ai_reports` | AI 分析结果（异常评分/健康分/RUL）| inference-engine | — |
| `llm_reports` | LLM 生成的故障报告（标题/摘要/分析/建议）| llm-analyzer | — |
| `vision_captures` | 视觉抓拍元数据 | vision-service | 60 天 |
| `audio_features` | 声学特征（~8 行/秒，128-bin 频谱）| audio-monitor | 60 天 |
| `audio_anomalies` | 声学异常事件（WAV 路径）| audio-monitor | — |
| `firmware_versions` | 固件版本（SHA256）| ota-server | — |
| `upgrade_history` | 设备升级历史 | ota-server | — |
| `model_versions` | AI 模型版本 | model-deploy | — |

`sensor_data` 表结构：`(time, site_id, device_type, device_id, data_type, payload JSONB, source_path)`。data-aggregator 用 **Orange Pi 本地系统时间**（已 NTP 同步）作分区键，ESP32 原始 `timestamp_ms`（boot-relative）保留在 payload 中溯源。

## 2.2 MQTT 主题命名规范

```
EdgeVib/{site_id}/{device_type}/{device_id}/{data_type}

示例:
  EdgeVib/factory1/motor/de01/data/sensor        — DE端振动数据
  EdgeVib/factory1/motor/nde01/data/sensor       — NDE端振动数据
  EdgeVib/factory1/inference/de01/ai/report       — 网关AI报告 (核心扇出主题)
  EdgeVib/factory1/llm/de01/report                — LLM故障报告
  EdgeVib/factory2/router/factory1/alert          — 跨车间告警广播
```

---

# 3. 微服务集群

平台共 **11 个微服务**。部署分层原则：**纯软件服务走 Docker**（依赖隔离），**需直接访问硬件设备节点的服务走 systemd**（串口/USB 摄像头/I2S 音频/OPC UA 端口直通）。

## 3.1 data-aggregator（Go）— 数据汇聚入库

- **职责边界**：只做可靠数据管道（解析 topic → 去重 → 写库），**不做**告警判断/AI 分类。单一职责：aggregator 挂了只丢数据，不影响其他服务。
- **技术栈**：Go · pgx/v5 连接池 · Paho MQTT · yaml.v3。
- **输入**：订阅 `edgevib/#`（ESP32 扁平格式）与 `EdgeVib/+/+/+/data/#`（分层格式）；`devices.yaml` 把 ESP32 的 `dev_id`(uint8) 映射为 `(site_id, device_type, device_id)`。
- **去重**：`(device_id, timestamp_ms, source_path)` 三元组 + 5s 窗口，in-memory map + mutex。
- **输出**：批量写入 `sensor_data`（100 行或 5s 触发刷入）。30s 上报健康到 `.../aggregator/orangepi/status/health`。
- **关键文件**：`services/data-aggregator/cmd/aggregator/main.go`、`internal/{topic,dedup,store}/`。

## 3.2 inference-engine（Python）— 边缘 AI 推理

- **职责**：设备端实时分类之上的**二级 AI**——自编码器重构误差异常检测 + 趋势分析 + RUL + 电机综合健康评分（0–100）+ 多设备聚合。**不训练模型**（训练在 PC）。
- **技术栈**：Python 3.11 · ONNX Runtime（CPU/OpenBLAS，NPU 预留）· numpy · structlog · asyncio。
- **与 ESP32 ai_service 互补**：

  | 维度 | ESP32 ai_service | Orange Pi inference-engine |
  |------|------------------|----------------------------|
  | 模型 | TFLite Micro 1D-CNN (<200KB) | ONNX 自编码器（更大）|
  | 类型 | 实时 4 分类 (<80ms) | 批量趋势/RUL（10s 间隔）|
  | 输入 | 实时特征窗口 32×24 | TimescaleDB 历史窗口 |

- **输入**：定时从 `sensor_data` 查历史窗口 + 订阅 `.../data/sensor` 紧急事件触发（RMS>7.1mm/s ISO10816-D 区 / bearing_fault / 置信度<0.85）。
- **输出**：写 `ai_reports`；发布 `EdgeVib/{site}/inference/{device}/ai/report`。
- **模型**：`src/models/autoencoder.onnx` + `autoencoder_metadata.json`（输入 24 维）。

## 3.3 llm-analyzer（Python）— 本地 LLM 故障报告

- **职责**：把 AI 异常告警转成中文自然语言诊断报告（标题/摘要/分析/建议）+ 定时日报。
- **技术栈**：llama.cpp + llama-cpp-python · **Qwen2.5-1.5B-Instruct Q4_K_M GGUF**（原生中文最优，面向中国工厂操作人员）· 4 线程 · 容器限 2GB。
- **输入**：订阅 `.../inference/+/ai/report`（仅 WARNING/CRITICAL 触发，5min 去重，严重度升级穿透去重）；从 DB 拉上下文。
- **输出**：写 `llm_reports`；发布 `.../llm/{device}/report`。
- **性能**：~1.5GB 内存，5–8 tok/s，200 字报告约 30s（工业非实时可接受）。
- **提示词**：`services/llm-analyzer/prompts/{alert_report,daily_summary}.yaml`。

## 3.4 edge-router（Go）— 跨车间告警广播

- **核心价值**：把某车间的电机故障告警 + 振动快照广播给其他车间，实现**跨车间对比诊断**——把 AI 诊断从"时序维度"扩展到"空间维度"（同型号健康电机 vs 异常电机实时对比，检测共因故障如供电母线异常）。
- **技术栈**：Go · pgx · 30s 去重（三元组 `source_site + source_device + alert_source`）。
- **输入**：双源消费 `.../inference/+/ai/report`（网关趋势）+ `.../motor/+/data/sensor`（ESP32 实时分类）。
- **输出**：查 TimescaleDB 拿振动上下文快照 → 组装 `AlertPayload` → 广播到除源车间外的所有 `sites` → `EdgeVib/{target}/router/{source}/alert`。
- **Phase 1**：单机同一 Mosquitto broker 模拟 2 车间×2 电机（factory1/factory2），`sites` 来自 `config/edge-router.yaml` 静态列表。Phase 2 换 broker 地址即可接真实多台 Pi。

## 3.5 api-server（Go）— REST + WebSocket 网关

- **职责**：对外统一 API 与实时推送。
- **技术栈**：Go · chi/v5 · gorilla/websocket · pgx/v5 · Auth/Logging/Recovery 中间件。
- **端点**（`:8080`）：
  - `GET /api/v1/sites` · `/sites/{id}/overview` · `/sites/{id}/devices`
  - `GET .../devices/{type}/{id}` · `/sensor` · `/environment` · `/ai-reports` · `/llm-reports`
  - `GET /api/v1/data/export`（CSV，供 edge-ai 训练数据同步）
  - `GET /api/v1/ws/events`（WebSocket 实时事件桥接 MQTT）
  - `GET /api/v1/health`

## 3.6 rs232-gateway（C，systemd）— 串口备份链路

- **职责**：主/备通信冗余。ESP32 WiFi 主链失效时，通过 RS232（F407 UART5）直连采集。
- **技术栈**：C · libyaml · CRC16-MODBUS 协议栈 · Paho C。
- **链路**：串口 `/dev/ttyUSB0` @115200，帧头 `0xAA55`/帧尾 `0x0D`；主链 3s 无有效帧切备用，5s 恢复切回（滞回防抖）。
- **输出**：解析后转发到 MQTT。

## 3.7 opcua-server（C，systemd）— OPC UA 工业协议

- **职责**：向上位机/SCADA 暴露 OPC UA 地址空间。
- **技术栈**：C · open62541 · 静态数组节点映射表（MAX 256，线性扫描）。
- **行为**：每 1s 轮询 TimescaleDB，DB 列映射为 OPC UA 节点，`opc.tcp://0.0.0.0:4840`（匿名安全策略）。

## 3.8 vision-service（Python，systemd）— 视觉巡检

- **职责**：定时基线抓拍（60s，640×480）+ AI 告警触发的高清事件抓拍（1920×1080）。
- **技术栈**：Python · OpenCV（单主线程，非线程安全）· queue 桥接 MQTT。
- **输入**：订阅 `.../inference/+/ai/report`。
- **输出**：图像落盘 `/opt/edge-gateway/data/vision`；元数据写 `vision_captures`；发布 `.../vision/{device}/capture`。
- **硬件**：默认 USB UVC 摄像头（即插即用）；A733 VIN/CSI 框架已实测可用，MIPI CSI 为 D7 预备方案。

## 3.9 audio-monitor（Python，systemd）— 声学异常监测

- **职责**：连续声学流式监听 + 异常触发保存原始音频（前 3s + 后 2s WAV）。捕获轴承撞击、转子碰磨等 <100ms 瞬态事件。
- **技术栈**：Python · sounddevice(PortAudio) · 16kHz 16-bit 单声道 · ES8389 I2S codec (hw:1,0) · numpy FFT。
- **管线**：连续录音 → 滑窗 FFT（2048 点 Hann 窗 50% overlap）→ 声学特征（频谱质心/频带能量/峰值频率/谱峭度）→ 动态阈值（3σ 预警 / 5σ 严重）→ 异常触发保存 WAV + 写库 + MQTT。
- **输出**：`audio_features`（~8 行/秒）+ `audio_anomalies`；WAV 存 `/var/lib/edgevib/audio`；发布 `.../audio/{device}/alert`。

## 3.10 ota-server（Go）— 固件 OTA 分发

- **职责**：ESP32/F407 固件版本管理与升级分发。
- **端点**（`:8090`）：`POST /firmware/upload` · `GET /firmware/versions` · `/firmware/upgrade-history` · `POST /firmware/trigger-upgrade` · `GET /firmware/{platform}/*`（下载）。
- **DB**：`firmware_versions`（SHA256）+ `upgrade_history`。

## 3.11 model-deploy（Go）— AI 模型分发

- **职责**：向 inference-engine 分发/回滚 AI 模型（保留 3 版）。
- **端点**（`:8091`）：`POST /models/deploy` · `GET /models` · `POST /models/{name}/rollback`。
- **热加载**：发布 `.../inference/{device}/model/reload` 通知推理引擎热加载。DB 表 `model_versions`。

---

# 4. 服务间数据流

```
[ESP32/F407 传感器]
   │ WiFi/MQTT                              │ RS232 备份链路
   ▼                                        ▼
   └────────────┐              [rs232-gateway] ──► MQTT
                ▼
        [data-aggregator] ──► TimescaleDB.sensor_data
                │ (MQTT sensor 原始流被下游直接消费)
                ▼
        [inference-engine] ──► ai_reports + 发布 .../ai/report ◄── 核心扇出主题
                │
   ┌────────────┼──────────────┬──────────────┬─────────────┐
   ▼            ▼             ▼              ▼             ▼
[llm-analyzer][vision-service][audio-monitor][edge-router][api-server]
 llm_reports  vision_captures audio_*表       跨车间广播    REST+WS 推送

TimescaleDB ──1s轮询──► [opcua-server] ──► OPC UA :4840 (上位机/SCADA)
[model-deploy] ──► .../model/reload ──► inference-engine 热加载
[ota-server]   ──► 固件分发 ──► ESP32/F407
Prometheus/Alertmanager ──► [alert-webhook] ──► MQTT
```

**核心枢纽**：Mosquitto（事件总线）+ TimescaleDB（共享状态）。`.../ai/report` 被 llm-analyzer、vision-service、audio-monitor、edge-router、api-server **五个服务同时订阅**。

---

# 5. Linux 内核驱动栈（D1–D7）

所有驱动位于 `edge-gateway/drivers/<模块名>/`，统一结构：单文件 `.c`（SPDX GPL-2.0 + 详细块注释）+ `Makefile`（`obj-m` + `KERNEL_DIR`）+ `test/test_module.sh`（Orange Pi 上跑，PASS/FAIL 统计）+ `systemd/*-load.service` + `*-d.service` + Go daemon 子目录。目标内核 **6.6.98-sun60iw2**。

> **设计意图**：把工业数据以标准 Linux 子系统设备呈现，可被通用工具链（candump/iio-tools/sensors/evtest/v4l2-ctl）直接消费。这是"用真实内核驱动开发范式承载工业 IoT 数据"的系统级练习，兼具工程价值与内核学习价值。

| # | 模块 | Linux 子系统 | 内核侧 | Go daemon 交互 | 作用 |
|---|------|--------------|--------|----------------|------|
| **D1** | `vcan_edgevib` | CAN / SocketCAN | 虚拟 CAN 接口 `vcan_edgevib`，回环 can_frame，sysfs `crc_errors`/`fifo_overruns` | `can-d`：AF_CAN raw socket 注入 NDE 传感器帧 | 工业 CAN 流量以标准 SocketCAN 暴露（candump/Wireshark 可见）|
| **D2** | `iio_vibration` | IIO（sysfs-only）| 24 通道 IIO 设备 + `/dev/edgevib-iio-inject` chardev，`float×1000` s32 存储 | `iio-d`：2s 轮询 `vibration_view` → write 96 字节特征 | 振动特征以标准 IIO 设备暴露 |
| **D3a** | `edgevib_buffer` | block（blk-mq）| `/dev/edgevib-buffer` 1MB RAM 环形块设备，4096B 扇区含 magic/crc32/ts | `flush-d`：双角色生产者(MQTT→write)+消费者(read→批量INSERT) | 入库前内核级暂存缓冲，抗 daemon 重启 |
| **D3b** | `edgevib_gpio` | GPIO + IRQ chip | 6 线虚拟 gpio_chip（4 输出健康信号 + 2 输入 ESTOP/PSU_FAIL 带 IRQ），sysfs `inject_irq` 模拟中断 | `gpio-d`：MQTT → gpioset 操作输出线，1Hz 心跳 | 网关级健康信号与急停/掉电检测 |
| **D4** | `rtc-edgevib` | RTC | DT overlay 匹配 platform bus → `/dev/rtcN`，独立时间计数器 + hrtimer 模拟 alarm 中断 | `rtc-d`：ioctl(RTC_SET/RD_TIME)，30s 落盘 `/var/lib/edgevib/last_time` | 无硬件 RTC 时的掉电时间持久化 |
| **D5** | `edgevib_hwmon` | hwmon | N 台电机 hwmon 通道（temp/curr/volt/power + max/crit/alarm）+ inject chardev | `hwmon-d`：轮询 DB → write 20 字节小端结构 | 电机健康以标准 hwmon 暴露，`sensors` 可读 |
| **D6** | `edgevib_input` | input / evdev | 虚拟 input 设备 `edgevib-estop`，急停 3 态映射为键事件（KEY_STOP/KEY_WAKEUP）| `input-stop-d`：MQTT 急停状态 → write input_event | 急停按钮以标准 evdev 暴露，`evtest` 可读 |
| **D7a** | `edgevib_video` | V4L2 | N 个 `/dev/videoX`（videobuf2-vmalloc），完整 V4L2 ioctl + inject chardev | `video-d`：轮询 `vision_captures` → JPEG 解码 → YUYV → write | 视觉抓拍以标准 V4L2 摄像头回放，`v4l2-ctl` 可读 |
| **D7b** | `edgevib_csi` | V4L2 subdev + media controller | OV5640 5MP MIPI CSI-2 传感器驱动模板，probe→v4l2_subdev→media entity→link sunxi VIN | 无 daemon（需真实硬件）| 真实 MIPI 相机接入的框架模板（pending 硬件验证）|

> **D8 不是内核驱动**：即 §3.4 的 edge-router（纯 Go + Docker）。
>
> **内核 6.6 兼容坑**（详见 KERNEL-LESSONS.md）：`iio_device_alloc` 签名变更、`gendisk.dev` 字段消失、虚拟 gpiochip 无 parent IRQ（改用 sysfs `inject_irq`）等。

---

# 6. 部署编排

## 6.1 Docker（纯软件服务）

主编排文件 `edge-gateway/docker/docker-compose.yml`，网络 `edgevib-net`（bridge）。容器化服务：mosquitto、timescaledb、grafana、data-aggregator、edge-router、inference-engine、llm-analyzer、api-server、ota-server、model-deploy + Prometheus 栈。均 `restart: unless-stopped`，`depends_on: mosquitto + timescaledb`。

```bash
cd edge-gateway
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml logs -f
```

## 6.2 systemd（硬件直通服务）

`rs232-gateway`、`opcua-server`、`vision-service`、`audio-monitor` 各含 `systemd/` unit 与 `Makefile`，直接跑在主机上访问串口/USB/I2S/端口，配置用 `localhost` 而非 Docker 服务名。

## 6.3 内核驱动加载

```bash
cd edge-gateway/drivers/<模块>
make && sudo make load          # 编译 + insmod
# 或用 systemd: <mod>-load.service (modprobe) + <mod>-d.service (daemon)
```

## 6.4 对外端口一览

| 端口 | 服务 |
|------|------|
| 1883 / 9001 | Mosquitto MQTT / WebSocket |
| 3000 | Grafana |
| 4840 | OPC UA |
| 5432 | TimescaleDB |
| 8080 | api-server REST + WS |
| 8090 | ota-server |
| 8091 | model-deploy |
| 8092 | alert-webhook |
| 9090 / 9093 | Prometheus / Alertmanager |

---

# 7. 配置约定

所有服务/驱动配置集中于 `edge-gateway/config/*.yaml`，Docker 以只读卷挂载为 `/app/config.yaml`。站点模型默认 `factory1`，edge-router 模拟 `factory1`/`factory2` 两车间。设备 ID 映射由 `config/devices.yaml` 维护（MQTT 层字符串 ID ↔ 协议层 byte ID）。
