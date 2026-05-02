# EdgeVib Raspberry Pi 边缘推理节点 - AGENTS.md (未来规划)

> **版本**: 0.1.0-draft
> **最后更新**: 2026-05-01
> **状态**: 🚧 未来规划 — 架构待定，尚未实施
> **目标硬件**: Raspberry Pi 4B (4GB/8GB) / Raspberry Pi 5
> **操作系统**: Raspberry Pi OS (Debian 12 Bookworm, 64-bit)

---

## ⚠️ 重要说明

Raspberry Pi 推理节点是 **EdgeVib 系统的未来扩展**，当前阶段 (v2.1) 的双模型（主模型 + 兜底规则引擎）全部部署在 **ESP32-S3** 上。

Raspberry Pi 的角色定位:
- **不做** ESP32 双模型的推理（那是在 ESP32 上运行的）
- **未来做** 更高性能模型的推理（如 ResNet、EfficientNet、更大 LSTM 等）
- **未来做** Web Dashboard 可视化面板
- **未来做** 多 ESP32 节点的数据汇聚与联合分析

当前 edge-ai/ 目录下的 `raspberry_pi/` 文件夹是为将来预留的目录结构，`edge_server.py` 和 `deploy.sh` 仅为参考模板。

---

## 🏗️ 未来架构 (草案)

```
┌──────────────────────────────────────────────────────────────┐
│                 Raspberry Pi 推理节点 (未来)                  │
│                                                              │
│  ┌──────────┐  ┌──────────────────┐  ┌───────────────────┐  │
│  │ MQTT     │  │ 高性能推理引擎     │  │ Web Dashboard     │  │
│  │ Bridge   │──▶│ (ResNet/GPT等)   │──▶│ (Streamlit/       │  │
│  │          │  │                  │  │  Grafana)         │  │
│  └──────────┘  └──────────────────┘  └───────────────────┘  │
│       │                                                      │
│       ▼                                                      │
│  ESP32 × N ──▶ 每个ESP32自己做双模型推理                      │
│                 树莓派做更高维度的分析                         │
└──────────────────────────────────────────────────────────────┘
```

---

## 📋 未来待定事项

- [ ] 确定高性能模型架构 (ResNet18/50, EfficientNet-B0, 或 Transformer)
- [ ] 确定是否需要 GPU 推理 (Pi 5 支持?)
- [ ] Web Dashboard 技术选型 (Streamlit / Grafana / FastAPI+React)
- [ ] 多 ESP32 数据汇聚方案
- [ ] 模型更新与 OTA 策略

---

## 📞 联系方式

- **技术支持**: edge-vib-support@example.com
- **Bug报告**: [GitHub Issues](https://github.com/EdgeVib/edge-ai/issues)

---

**文档维护者**: EnterWorldDoor (AI System Architect)
**最后审核**: 2026-05-01
**状态**: 未来规划
