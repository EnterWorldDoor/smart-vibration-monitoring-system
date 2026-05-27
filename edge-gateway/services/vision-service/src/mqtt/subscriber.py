"""MQTT subscriber for inference-engine WARNING/CRITICAL events."""

import json
import structlog
from dataclasses import dataclass, field
from typing import Callable, Optional

import paho.mqtt.client as mqtt

from src.config import MQTTConfig, DeviceConfig

logger = structlog.get_logger(__name__)


@dataclass
class TriggerEvent:
    site_id: str = ""
    device_id: str = ""
    severity: str = ""
    trigger_reason: str = ""
    payload: dict = field(default_factory=dict)


class MQTTSubscriber:
    """Subscribes to inference-engine ai/report, parses topic, matches devices."""

    def __init__(self, mqtt_cfg: MQTTConfig, devices: list[DeviceConfig]):
        self.mqtt_cfg = mqtt_cfg
        self.devices = devices
        self._device_map: dict[tuple[str, str], DeviceConfig] = {
            (d.site_id, d.device_id): d for d in devices
        }
        self.client = mqtt.Client(
            client_id=mqtt_cfg.client_id, protocol=mqtt.MQTTv311,
        )
        self._trigger_callback: Optional[Callable[[TriggerEvent, DeviceConfig], None]] = None
        self._connected = False

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

    def set_trigger_callback(self, cb: Callable[[TriggerEvent, DeviceConfig], None]):
        self._trigger_callback = cb

    def connect(self):
        try:
            self.client.connect(self.mqtt_cfg.broker, self.mqtt_cfg.port, 60)
            self.client.loop_start()
            logger.info("mqtt subscriber connecting", broker=self.mqtt_cfg.broker)
        except Exception:
            logger.warning("mqtt subscriber connect failed",
                           broker=self.mqtt_cfg.broker)

    def disconnect(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass

    def is_connected(self) -> bool:
        return self._connected

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            client.subscribe(self.mqtt_cfg.subscribe_topic, qos=self.mqtt_cfg.qos)
            logger.info("mqtt subscribed", topic=self.mqtt_cfg.subscribe_topic)
        else:
            logger.error("mqtt connect failed", rc=rc)

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        if rc != 0:
            logger.warning("mqtt disconnected unexpectedly", rc=rc)

    def _on_message(self, client, userdata, msg):
        """Called in paho daemon thread. Parse topic, match device, fire callback."""
        try:
            payload = json.loads(msg.payload)
        except json.JSONDecodeError:
            return

        topic_parts = msg.topic.split("/")
        if len(topic_parts) < 6:
            return
        site_id = topic_parts[1]
        device_id = topic_parts[3]

        dev = self._device_map.get((site_id, device_id))
        if dev is None:
            return

        severity = payload.get("severity", "")
        if severity not in ("WARNING", "CRITICAL"):
            return

        event = TriggerEvent(
            site_id=site_id,
            device_id=device_id,
            severity=severity,
            trigger_reason=payload.get("trigger_reason", ""),
            payload=payload,
        )
        if self._trigger_callback:
            self._trigger_callback(event, dev)
