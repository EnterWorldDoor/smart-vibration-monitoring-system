"""MQTT subscriber for cross-modal trigger events.

Subscribes to inference-engine ai/report topics.  When an inference-engine
WARNING or CRITICAL alert fires, the audio-monitor can immediately save a
baseline snapshot for cross-modal correlation.

Pattern mirrored from vision-service mqtt/subscriber.py.
"""

import json
from dataclasses import dataclass, field
from queue import Queue
from typing import Callable, Optional

import paho.mqtt.client as mqtt
import structlog

from src.config import MQTTConfig, DeviceConfig

logger = structlog.get_logger(__name__)


@dataclass
class TriggerEvent:
    """MQTT trigger event, consumed by main thread via Queue."""
    topic: str = ""
    site_id: str = ""
    device_id: str = ""
    severity: str = ""           # "WARNING" | "CRITICAL"
    trigger_reason: str = ""
    payload: dict = field(default_factory=dict)


class MQTTSubscriber:
    """Subscribes to EdgeVib/+/inference/+/ai/report for cross-modal triggers."""

    def __init__(self, mqtt_cfg: MQTTConfig, devices: list[DeviceConfig]):
        self.mqtt_cfg = mqtt_cfg
        self.devices = devices
        self.client = mqtt.Client(
            client_id=f"{mqtt_cfg.client_id}-sub", protocol=mqtt.MQTTv311,
        )
        self._connected = False
        self._event_queue: Queue = Queue(maxsize=32)
        self._trigger_callback: Optional[
            Callable[[TriggerEvent, DeviceConfig], None]
        ] = None

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

    def set_trigger_callback(
        self, cb: Callable[[TriggerEvent, DeviceConfig], None]
    ) -> None:
        self._trigger_callback = cb

    def connect(self) -> None:
        try:
            self.client.connect(self.mqtt_cfg.broker, self.mqtt_cfg.port, 60)
            self.client.loop_start()
            logger.info("mqtt subscriber connecting",
                        broker=self.mqtt_cfg.broker)
        except Exception:
            logger.warning("mqtt subscriber connect failed",
                           broker=self.mqtt_cfg.broker)

    def disconnect(self) -> None:
        self.client.loop_stop()
        try:
            self.client.disconnect()
        except Exception:
            pass

    def is_connected(self) -> bool:
        return self._connected

    def get_queue(self) -> Queue:
        return self._event_queue

    # ---- paho callbacks ----

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            topic = self.mqtt_cfg.subscribe_topic
            client.subscribe(topic, qos=self.mqtt_cfg.qos)
            logger.info("mqtt subscribed", topic=topic)
        else:
            logger.warning("mqtt connect failed", rc=rc)

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        if rc != 0:
            logger.warning("mqtt disconnected unexpectedly", rc=rc)

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
        except json.JSONDecodeError:
            return

        topic_parts = msg.topic.split("/")
        if len(topic_parts) < 5:
            return

        site_id = topic_parts[1]
        device_id = topic_parts[3]
        severity = payload.get("severity", "").upper()

        if severity not in ("WARNING", "CRITICAL"):
            return

        event = TriggerEvent(
            topic=msg.topic,
            site_id=site_id,
            device_id=device_id,
            severity=severity,
            trigger_reason=payload.get("trigger_reason", ""),
            payload=payload,
        )

        # Match against configured devices
        matched_dev = next(
            (d for d in self.devices
             if d.site_id == site_id or d.site_id == ""),
            None,
        )

        if self._trigger_callback and matched_dev:
            try:
                self._trigger_callback(event, matched_dev)
            except Exception:
                logger.exception("trigger callback error")

        # Push to queue for main thread
        try:
            self._event_queue.put_nowait((event, matched_dev))
        except Exception:
            pass  # queue full, drop
