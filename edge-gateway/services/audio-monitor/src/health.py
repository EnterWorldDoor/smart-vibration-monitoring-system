"""Health reporter — periodic status telemetry via MQTT.

Pattern mirrored from vision-service src/health.py.
Publishes to EdgeVib/{site_id}/audio/orangepi/status/health every 30 s.
"""

import json
import time

import paho.mqtt.client as mqtt
import structlog

from src.config import MQTTConfig

logger = structlog.get_logger(__name__)


class HealthReporter:
    def __init__(self, mqtt_cfg: MQTTConfig, site_id: str):
        self.cfg = mqtt_cfg
        self.site_id = site_id
        self.client = mqtt.Client(
            client_id=f"{mqtt_cfg.client_id}-health", protocol=mqtt.MQTTv311,
        )
        self.start_time = time.time()

        # Counters (incremented by main thread, read by report())
        self.frames_processed: int = 0
        self.anomalies_detected: int = 0
        self.wav_files_written: int = 0
        self.db_write_failures: int = 0
        self.mqtt_publish_failures: int = 0
        self.capture_errors: int = 0
        self.errors: int = 0

    def connect(self) -> None:
        try:
            self.client.connect(self.cfg.broker, self.cfg.port, 60)
            self.client.loop_start()
        except Exception:
            pass

    def disconnect(self) -> None:
        self.client.loop_stop()
        try:
            self.client.disconnect()
        except Exception:
            pass

    def report(self) -> None:
        uptime = int(time.time() - self.start_time)
        topic = f"EdgeVib/{self.site_id}/audio/orangepi/status/health"
        payload = json.dumps({
            "service": "audio-monitor",
            "uptime_seconds": uptime,
            "frames_processed": self.frames_processed,
            "anomalies_detected": self.anomalies_detected,
            "wav_files_written": self.wav_files_written,
            "db_write_failures": self.db_write_failures,
            "mqtt_publish_failures": self.mqtt_publish_failures,
            "capture_errors": self.capture_errors,
            "errors": self.errors,
        })
        try:
            self.client.publish(topic, payload, qos=1, retain=False)
            logger.debug("health reported", frames=self.frames_processed,
                         anomalies=self.anomalies_detected)
        except Exception:
            self.mqtt_publish_failures += 1
