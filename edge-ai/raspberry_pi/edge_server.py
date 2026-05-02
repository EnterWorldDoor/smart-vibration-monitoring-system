#!/usr/bin/env python3
"""
EdgeVib Raspberry Pi 边缘推理服务器 — 🚧 未来规划 (参考模板)

⚠️ 当前版本 (v2.1) 双模型部署在 ESP32-S3 上，
   本文件仅为 Raspberry Pi 未来高性能模型预留的参考模板。

   未来定位: 运行 ResNet/EfficientNet 等更高性能模型 + Web Dashboard
   当前用途: 参考代码，不实际部署

   启动方式 (未来):
       python raspberry_pi/edge_server.py --broker localhost:1883
"""

import argparse
import sys
import os
import signal
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from inference.inference_engine import InferenceEngine
from inference.real_time_processor import MQTTInferenceBridge
from utils.logger import get_logger

logger = get_logger(__name__)

g_bridge = None
g_engine = None


def signal_handler(sig, frame):
    """处理终止信号"""
    global g_bridge
    logger.info("Shutting down...")
    if g_bridge:
        g_bridge.stop()
    sys.exit(0)


def main():
    global g_bridge, g_engine

    parser = argparse.ArgumentParser(description='EdgeVib Raspberry Pi Inference Server')
    parser.add_argument('--broker', default='localhost:1883',
                       help='MQTT Broker address (host:port)')
    parser.add_argument('--model-dir', default='models/saved_models',
                       help='Model directory path')
    parser.add_argument('--confidence', type=float, default=0.85,
                       help='Primary model confidence threshold')
    parser.add_argument('--http-port', type=int, default=0,
                       help='HTTP API port (0=disabled)')
    parser.add_argument('--hotspot-ssid', default='EdgeVib_Hotspot',
                       help='WiFi hotspot SSID to connect to')
    parser.add_argument('--hotspot-password', default='1234567890',
                       help='WiFi hotspot password')

    args = parser.parse_args()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    broker_host, broker_port = args.broker.rsplit(':', 1)
    broker_port = int(broker_port)

    logger.info("EdgeVib Raspberry Pi Inference Server")
    logger.info("  MQTT Broker: %s:%d", broker_host, broker_port)
    logger.info("  Model Dir: %s", args.model_dir)
    logger.info("  Confidence Threshold: %.2f", args.confidence)

    engine = InferenceEngine(
        confidence_threshold=args.confidence,
        model_dir=args.model_dir
    )

    primary_path = os.path.join(args.model_dir, 'primary_best.h5')
    if os.path.exists(primary_path):
        engine.load_primary(primary_path)
    else:
        logger.warning("Primary model not found at %s, using fallback only", primary_path)

    fallback_path = os.path.join(args.model_dir, 'fallback_config.yaml')
    if os.path.exists(fallback_path):
        engine.load_fallback_config(fallback_path)

    g_engine = engine

    bridge = MQTTInferenceBridge(
        engine=engine,
        broker_host=broker_host,
        broker_port=broker_port,
        input_topic='edgevib/train/#',
        output_topic_prefix='edgevib/inference/rpi'
    )
    g_bridge = bridge

    bridge.start()

    if args.http_port > 0:
        try:
            from flask import Flask, jsonify, request
            app = Flask(__name__)

            @app.route('/health')
            def health():
                return jsonify({'status': 'ok', 'timestamp': time.time()})

            @app.route('/inference/status')
            def inference_status():
                return jsonify(engine.get_status())

            @app.route('/inference/alerts')
            def alerts():
                return jsonify({'alerts': [], 'count': 0})

            import threading
            flask_thread = threading.Thread(
                target=lambda: app.run(
                    host='0.0.0.0', port=args.http_port,
                    debug=False, use_reloader=False
                ),
                daemon=True
            )
            flask_thread.start()
            logger.info("HTTP API started on port %d", args.http_port)
        except ImportError:
            logger.warning("Flask not installed, HTTP API disabled")

    logger.info("Inference server running. Press Ctrl+C to stop.")

    while True:
        try:
            status = engine.get_status()
            stats = status.get('cascade_stats', {})
            logger.info(
                "Status: inferences=%d, primary_rate=%.1f%%, latency=%.1fms",
                stats.get('total_inferences', 0),
                (stats.get('primary_used', 0) / max(stats.get('total_inferences', 1), 1)) * 100,
                status['stats'].get('avg_inference_time_ms', 0)
            )
            time.sleep(60)
        except KeyboardInterrupt:
            break

    bridge.stop()
    logger.info("Server stopped")


if __name__ == '__main__':
    main()
