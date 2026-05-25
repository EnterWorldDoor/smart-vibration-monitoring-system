"""MQTT publisher for inference reports."""

import json
import structlog

import paho.mqtt.client as mqtt

from src.config import MQTTConfig

logger = structlog.get_logger(__name__)


class MQTTPublisher:
    def __init__(self, cfg: MQTTConfig):
        self.cfg = cfg
        self.client = mqtt.Client(
            client_id=f"{cfg.client_id}-pub", protocol=mqtt.MQTTv311,
        )
        self._connected = False

    def connect(self):
        self.client.connect(self.cfg.broker, self.cfg.port, 60)
        self.client.loop_start()
        logger.info("mqtt publisher connecting", broker=self.cfg.broker)

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()

    def publish_report(self, site_id: str, device_id: str, report: dict):
        topic = self.cfg.publish_topic.format(site_id=site_id, device_id=device_id)
        payload = json.dumps(report)
        info = self.client.publish(topic, payload, qos=self.cfg.qos, retain=False)
        if info.rc == mqtt.MQTT_ERR_SUCCESS:
            logger.debug("report published", topic=topic)
        else:
            logger.warning("publish failed", topic=topic, rc=info.rc)
