"""MQTT publisher for vision-service capture notifications."""

import json
import structlog

import paho.mqtt.client as mqtt

from src.config import MQTTConfig

logger = structlog.get_logger(__name__)


class MQTTPublisher:
    """Publishes lightweight capture notifications (file path + metadata)."""

    def __init__(self, cfg: MQTTConfig):
        self.cfg = cfg
        self.client = mqtt.Client(
            client_id=f"{cfg.client_id}-pub", protocol=mqtt.MQTTv311,
        )

    def connect(self):
        try:
            self.client.connect(self.cfg.broker, self.cfg.port, 60)
            self.client.loop_start()
            logger.info("mqtt publisher connected", broker=self.cfg.broker)
        except Exception:
            logger.warning("mqtt publisher connect failed", broker=self.cfg.broker)

    def disconnect(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass

    def publish_capture(
        self,
        site_id: str,
        device_id: str,
        capture_type: str,
        file_path: str,
        resolution: str,
        file_size_bytes: int,
        timestamp_iso: str,
    ):
        topic = self.cfg.publish_topic.format(
            site_id=site_id, device_id=device_id)
        payload = json.dumps({
            "capture_type": capture_type,
            "file_path": file_path,
            "resolution": resolution,
            "file_size_bytes": file_size_bytes,
            "timestamp_utc": timestamp_iso,
        })
        try:
            info = self.client.publish(topic, payload, qos=self.cfg.qos, retain=False)
            if info.rc == mqtt.MQTT_ERR_SUCCESS:
                logger.debug("capture notification published", topic=topic)
            else:
                logger.warning("publish failed", topic=topic, rc=info.rc)
        except Exception:
            logger.warning("publish exception", topic=topic)
