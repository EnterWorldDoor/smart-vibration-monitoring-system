"""Health reporter — periodic status telemetry via MQTT."""

import json
import time
import structlog

import paho.mqtt.client as mqtt

from src.config import MQTTConfig

logger = structlog.get_logger(__name__)


class HealthReporter:
    def __init__(self, cfg: MQTTConfig, site_id: str):
        self.cfg = cfg
        self.site_id = site_id
        self.client = mqtt.Client(
            client_id=f"{cfg.client_id}-health", protocol=mqtt.MQTTv311,
        )
        self.start_time = time.time()
        self.total_baselines = 0
        self.total_events = 0
        self.errors = 0

    def connect(self):
        try:
            self.client.connect(self.cfg.broker, self.cfg.port, 60)
            self.client.loop_start()
        except Exception:
            logger.warning("health mqtt connect failed")

    def disconnect(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass

    def report(self):
        uptime = time.time() - self.start_time
        topic = f"EdgeVib/{self.site_id}/vision/orangepi/status/health"
        payload = json.dumps({
            "service": "vision-service",
            "uptime_seconds": int(uptime),
            "total_baselines": self.total_baselines,
            "total_events": self.total_events,
            "errors": self.errors,
        })
        try:
            self.client.publish(topic, payload, qos=1, retain=False)
            logger.debug("health reported", baselines=self.total_baselines,
                         events=self.total_events, errors=self.errors)
        except Exception:
            pass
