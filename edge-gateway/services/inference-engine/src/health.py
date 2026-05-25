"""Health reporter — periodic status telemetry via MQTT."""

import json
import time
import structlog

import paho.mqtt.client as mqtt

from .config import MQTTConfig

logger = structlog.get_logger(__name__)


class HealthReporter:
    def __init__(self, cfg: MQTTConfig, site_id: str):
        self.cfg = cfg
        self.site_id = site_id
        self.client = mqtt.Client(
            client_id=f"{cfg.client_id}-health", protocol=mqtt.MQTTv311,
        )
        self.start_time = time.time()
        self.total_inferences = 0
        self.total_anomalies = 0
        self.total_reports = 0
        self.last_inference_time_ms = 0.0
        self.errors = 0

    def connect(self):
        self.client.connect(self.cfg.broker, self.cfg.port, 60)
        self.client.loop_start()

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()

    def report(self):
        uptime = time.time() - self.start_time
        topic = f"EdgeVib/{self.site_id}/aggregator/orangepi/status/health"
        payload = json.dumps({
            "service": "inference-engine",
            "uptime_seconds": int(uptime),
            "total_inferences": self.total_inferences,
            "total_anomalies": self.total_anomalies,
            "total_reports": self.total_reports,
            "last_inference_time_ms": round(self.last_inference_time_ms, 2),
            "errors": self.errors,
            "model_loaded": True,
        })
        self.client.publish(topic, payload, qos=1, retain=False)
        logger.debug("health reported", inferences=self.total_inferences,
                     anomalies=self.total_anomalies)
