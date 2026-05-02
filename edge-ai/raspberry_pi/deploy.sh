#!/bin/bash
# EdgeVib Raspberry Pi 自动部署脚本 — 🚧 未来规划 (参考模板)
# 版本: 0.1.0-draft
# 状态: 未来规划，当前不实际使用
# 适用: Raspberry Pi 4B/5 (Raspberry Pi OS 64-bit)
# 说明: 当前双模型部署在ESP32-S3，此脚本为未来RPi高性能模型预留

set -e

echo "========================================="
echo " EdgeVib Raspberry Pi Deployment Script"
echo "========================================="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EDGE_AI_DIR="$SCRIPT_DIR/.."

echo "[1/5] Installing system dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    python3-pip \
    python3-numpy \
    libatlas-base-dev \
    libhdf5-dev \
    libopenblas-dev \
    python3-venv

echo "[2/5] Creating virtual environment..."
python3 -m venv "$EDGE_AI_DIR/venv" --system-site-packages
source "$EDGE_AI_DIR/venv/bin/activate"

echo "[3/5] Installing Python dependencies..."
pip install --upgrade pip setuptools wheel

pip install tensorflow-aarch64==2.15.1 \
    || pip install tensorflow==2.15.1

pip install \
    numpy==1.26.4 \
    pandas>=2.0 \
    paho-mqtt>=2.0 \
    pyyaml>=6.0 \
    scikit-learn>=1.3 \
    scipy>=1.10 \
    matplotlib>=3.7 \
    flask>=3.0 \
    psutil>=5.9

echo "[4/5] Creating required directories..."
mkdir -p "$EDGE_AI_DIR/models/saved_models"
mkdir -p "$EDGE_AI_DIR/logs"
mkdir -p "$EDGE_AI_DIR/reports"
mkdir -p "$EDGE_AI_DIR/datasets"

echo "[5/5] Generating systemd service..."
SERVICE_FILE="/etc/systemd/system/edgevib-inference.service"
sudo tee "$SERVICE_FILE" > /dev/null << EOF
[Unit]
Description=EdgeVib Edge Inference Service
After=network.target mosquitto.service
Wants=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=$EDGE_AI_DIR
Environment="TF_CPP_MIN_LOG_LEVEL=2"
Environment="TF_NUM_INTEROP_THREADS=2"
Environment="TF_NUM_INTRAOP_THREADS=2"
ExecStart=$EDGE_AI_DIR/venv/bin/python $EDGE_AI_DIR/raspberry_pi/edge_server.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable edgevib-inference

echo ""
echo "========================================="
echo " Deployment Complete!"
echo "========================================="
echo ""
echo "To start the inference service:"
echo "  sudo systemctl start edgevib-inference"
echo ""
echo "To check status:"
echo "  sudo systemctl status edgevib-inference"
echo ""
echo "To view logs:"
echo "  journalctl -u edgevib-inference -f"
echo ""
